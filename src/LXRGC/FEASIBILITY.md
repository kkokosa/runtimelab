# LXR on the CoreCLR standalone-GC ABI — feasibility analysis

> **UPDATE — RESOLVED (via a generic runtime facility).** The wall described
> below was real for the *unmodified* ABI. It has since been removed by adding a
> **generic, GC-agnostic pluggable write barrier** to the runtime fork
> [`kkokosa/runtime`, branch `feature/pluggable-write-barrier`], gated behind
> `FEATURE_GC_CUSTOM_WRITE_BARRIER`. The runtime now exposes a neutral
> `WriteBarrierKind::Callback` barrier: it captures the **old** field value and
> hands `(slot, newValue, oldValue)` to a GC-registered callback. Nothing
> LXR-specific or GC-policy-specific was added to the runtime. LXRGC selects it
> in `LXRGCHeap::Initialize` and its coalescing-RC engine is now genuinely
> driven by real managed field stores (verified: the write barrier logs field
> mutations and the RC engine performs real increments/decrements). See
> §5 for the design and the original analysis below for why it was needed.

**Original verdict (unmodified ABI): LXR cannot be implemented as a standalone GC
without changing the runtime.** The blocker is fundamental and appears at the
very heart of LXR: its *field-logging / coalescing reference-counting write
barrier*. The CoreCLR standalone-GC ABI gives a plug-in GC no way to observe the
**old** value of a mutated reference field, and no way to install its own barrier
code — the only barrier the JIT emits is a fixed **card-marking** barrier whose
behaviour the GC can parameterise but not replace.

This document explains what LXR needs, what the ABI provides, exactly where they
diverge (with runtime source citations), and the minimal runtime change that
unblocks it (now implemented). A working scaffold (`native/`) accompanies this
analysis and has been built and run against the locally compiled runtime.

---

## 1. What LXR requires

LXR ("Low-Latency, High-Throughput Garbage Collection", Zhao, Blackburn &
McKinley, PLDI 2022 — https://arxiv.org/abs/2210.17175) reclaims memory
primarily with **reference counting (RC)** over an **Immix** block/line heap,
plus a periodic **backup trace** for cycles and **limited copying (evacuation)**
for defragmentation. Its RC is made affordable and correct by two mechanisms:

1. **A field-logging (coalescing) write barrier** — the Levanoni–Petrank
   scheme. On the *first* mutation of an object's reference field in an epoch,
   the barrier records the slot **and the old reference value about to be
   overwritten** into a per-mutator *modified/decrement buffer*. At GC time LXR
   replays the buffer: **increment** the new referents, **decrement** the old
   referents; objects reaching count 0 are reclaimed (recursively). The old
   value is essential — without it the collector cannot know *what to
   decrement*.

2. **A periodic backup trace** (mark-sweep) that reclaims dead cycles pure RC
   leaks, and drives Immix line/block reclamation + evacuation.

Everything in LXR is downstream of mechanism (1): RC coalescing, remembered-set
maintenance for evacuation, and object survival all depend on the mutator write
barrier surfacing `(slot, oldValue)` pairs to the GC.

## 2. What the standalone-GC ABI provides for write barriers

A standalone GC configures the write barrier through **one** call,
`IGCToCLR::StompWriteBarrier(WriteBarrierParameters*)`, whose parameters are
purely *card-marking* knobs:

- `card_table`, `card_bundle_table` — where to set dirty bits
- `lowest_address` / `highest_address` — heap bounds for the range check
- `ephemeral_low` / `ephemeral_high` — generational fast-path bounds
- `region_to_generation_table`, `region_shr`, `region_use_bitwise_write_barrier`
  — region-map knobs for the region barrier variant

(`src/coreclr/gc/gcinterface.h`, `struct WriteBarrierParameters`, ~line 59–117.)

The barrier *code itself* is emitted by the JIT / lives in the runtime and is
**not** supplied by the GC. Every variant does the store and then, at most, sets
a **dirty bit** in the card/bundle/write-watch table. It receives only the
destination slot address and the new value — it never reads or preserves the old
value, and it never calls back into the GC. See the AMD64 implementation, e.g.
`JIT_WriteBarrier_Bit_Region64` in
`src/coreclr/vm/amd64/JitHelpers_FastWriteBarriers.asm`:

```asm
        mov     [rcx], rdx            ; store new value; OLD value at [rcx] is gone
        ...
        ; classify by region/ephemeral bounds, then:
    UpdateCardTable:
        lock or byte ptr [r8 + rax], dl      ; <-- only ever SETS A DIRTY BIT
    UpdateCardBundleTable:
        mov     byte ptr [r8 + rax], 0FFh     ; <-- only ever SETS A DIRTY BIT
        ret
```

The set of barrier functions the runtime can select among is fixed and
enumerated by the runtime's own write-barrier manager
(`src/coreclr/vm/writebarriermanager.cpp` / `.h`) — `Initialize`, `StompResize`,
`StompEphemeral`, `SwitchToWriteWatch`, `SwitchToNonWriteWatch`
(`WriteBarrierOp` in `gcinterface.h`, ~line 49–56). None of them logs old
values or invokes GC-supplied code.

## 3. Where LXR and the ABI diverge (the wall)

| LXR needs | ABI offers | Gap |
|---|---|---|
| Barrier records `(slot, oldValue)` on ref-field writes | Barrier sets a card **dirty bit**; store overwrites the old value first | **Old value is irretrievably lost.** Card marking says *which ~2 KB card* was written, never *what was there before*. |
| GC-supplied barrier slow path (coalescing log / RC buffer flush) | Fixed JIT barrier variants only | GC cannot inject its own barrier code. |
| SATB-style *old-value* snapshotting | Only *incremental-update* card marking exists | .NET's barrier is an incremental-update/card barrier, **not** a snapshot-at-the-beginning (old-value logging) barrier. |

Because the old value is destroyed by the `mov [rcx], rdx` store **before** any
GC-observable side effect, and the only side effect is a coarse dirty bit, there
is **no way** — even by re-scanning every dirty card at GC time — to reconstruct
the decrements coalescing RC requires. Re-scanning dirty cards yields only the
*current* (new) pointers in those cards, i.e. it degenerates into a trace, which
is exactly what RC is meant to avoid.

This is why LXR's defining mechanism cannot be expressed: **you can build the
Immix heap, the RC side tables, the modified/decrement buffers, and the whole RC
+ backup-trace engine inside the plug-in (this scaffold does), but nothing can
feed the buffers.** The `LXRCollector::LogModifiedField(slot, oldValue)` slow
path in `native/LXRGCHeap.cpp` is fully implemented and simply *never gets
called by managed field writes*.

### Corollaries (secondary limits, all downstream of the barrier)

- **Remembered sets for evacuation** rely on the same barrier; unavailable.
- **Weak-reference / dependent-handle clearing** requires reachability, which
  requires either the RC engine (blocked) or a full trace on every collection.
- **Heap-walking / evacuation** would additionally need a GC-controlled read or
  forwarding barrier for concurrent copying; the ABI exposes no read barrier
  hook either.

## 4. How far the scaffold actually gets (verified)

The accompanying `native/` plug-in builds to `LXRGC.dll` and **loads and runs a
real .NET program** as the standalone GC (`DOTNET_GCName=LXRGC.dll`). It
implements:

- the full `IGCHeap` + `IGCHandleManager` ABI surface;
- a real **Immix-style** substrate: 32 KiB blocks / 256 B lines, per-block
  metadata, a per-thread lock-free block-run bump allocator;
- the **RC side table** (1 saturating byte / 8-byte granule) with
  `RCIncrement` / `RCDecrement`;
- the **coalescing-RC replay** (`ProcessModifiedBuffers`) and recursive
  zero-count freeing, operating on per-mutator modified buffers;
- `BackupTrace` / `SweepAndSelectDefrag` skeletons wired to the suspension /
  root-scan ABI entry points a full implementation would use;
- card-table + card-bundle-table wiring so the fixed JIT barrier never faults.

The RC/collection machinery is **present but dormant**: with no barrier feeding
`LogModifiedField`, the modified buffers stay empty, so LXRGC in practice
allocates and never reclaims (like ZeroGC). That is the precise, honest boundary
of what the standalone ABI permits for LXR.

### Build & run (as verified on this machine)

```powershell
# Build (VS 18 IntPreview / Build Tools, x64)
cd src\LXRGC\native
.\build.ps1 -RuntimeRepo C:\github\runtime -Configuration Release

# Run a managed app under LXRGC using the locally built runtime
$fw = "C:\github\runtime\artifacts\bin\testhost\net11.0-windows-Release-x64\shared\Microsoft.NETCore.App\11.0.0"
Copy-Item src\LXRGC\native\obj\Release\LXRGC.dll $fw -Force
$env:DOTNET_GCName = "LXRGC.dll"
& "$fw\corerun.exe" src\LXRGC\samples\ConsoleApp\bin\ConsoleApp.dll
```

Observed: LXRGC prints `LXRGC-SMOKE-OK`, exits 0; never reclaims (committed
memory grows and `GC.Collect()` leaves `CollectionCount` at 0), unlike the
baseline GC. A bogus `DOTNET_GCName` fails init with `0x8007007E`, confirming the
load path (and thus that LXRGC was genuinely the active GC).

## 5. The runtime change that unblocks LXR (implemented)

LXR needs the JIT write barrier to surface the **old value** and let the GC run
its own barrier logic. Rather than add anything LXR- or policy-specific, the
runtime fork adds a **generic, GC-agnostic pluggable write barrier**
(`FEATURE_GC_CUSTOM_WRITE_BARRIER`, amd64) exposing two neutral primitives; LXR
uses the first:

- **`WriteBarrierKind::Callback` (implemented & used).** The GC registers one
  function pointer `void (*)(Object** slot, Object* newValue, Object* oldValue)`.
  The JIT-emitted barrier reads the old value, performs the store, then calls the
  callback. The runtime ascribes **no** meaning to what the callback does.
- **`WriteBarrierKind::CustomCode` (reserved, not implemented).** The GC could
  supply a raw barrier code blob stomped into the `JIT_WriteBarrier` region.

Runtime touch-points (all behind the feature flag, nothing GC-specific):

1. `gc/gcinterface.h` — `WriteBarrierKind`, a `WriteBarrierCallback` typedef,
   extra `WriteBarrierParameters` fields, and `WriteBarrierOp::SwitchToCustomBarrier`
   (ABI minor version bumped).
2. `vm/amd64/JitHelpers_FastWriteBarriers.asm` — `JIT_WriteBarrier_Callback64`,
   the copy-safe barrier body (capture old value → store → tail-call slow path).
3. `vm/amd64/JitHelpers_Fast.asm` — `JIT_WriteBarrier_CallbackSlow`, an
   out-of-line wrapper that **preserves the full volatile SIMD state**
   (YMM0-15 via `vmovups` when AVX is present, else xmm0-5) around the C callback,
   because the write-barrier ABI requires vector state be preserved across the
   barrier.
4. `vm/writebarriermanager.{h,cpp}` — a new barrier type, sticky selection,
   `SwitchToCustomWriteBarrier`, and the neutral `g_write_barrier_callback` slot.
5. `vm/gcenv.ee.cpp` — routes `WriteBarrierOp::SwitchToCustomBarrier`.

LXRGC (in `runtimelab`, `feature/LXRGC`) consumes it: `LXRGCHeap::Initialize`
issues a `SwitchToCustomBarrier` op registering `LXRWriteBarrierCallback`, which
forwards `(slot, oldValue)` into `LXRCollector::LogModifiedField`. At a
collection, `ProcessModifiedBuffers` runs the coalescing RC (increment the new
referent, decrement the old). The default runtime GC is completely unaffected
(the barrier stays `Card` unless a GC opts in).

## 6. Full reclamation — a second generic runtime facility (object scanning)

Coalescing RC and the backup trace both need to **enumerate an object's
reference fields**. The unmodified standalone-GC ABI exposes no such primitive
(the long-standing request is **dotnet/runtime #12809**, "Local GC API to
support object scanning"): the field layout lives in `CGCDesc`/`MethodTable`,
which the built-in GC walks with the internal **`go_through_object`** *macro*.

We solved this the same way as the barrier — with a **generic, GC-agnostic**
addition, not an LXR-specific one:

- **`src/coreclr/gc/gcobjscan.h` (new, header-only).** A standalone-includable
  header providing an inline template
  `GCScanObjectRefs<TVisit>(Object* o, size_t size, TVisit visit)` that
  re-expresses `go_through_object_cl` (normal series, repeating value-type-array,
  and the collectible-class case) over the already-shared `CGCDesc`. It also
  offers the classic C-ABI `GcEnumerateObjectReferences(o, size, fn, ctx)`
  function-pointer form for GCs that want a stable non-template entry point. No
  ABI/vtable change; nothing GC-policy-specific. This is a generic answer to
  #12809.

### Why a *template*, not the literal #12809 function pointer (perf)

Object scanning is the hottest loop in RC decrements and the backup trace. A
per-field **function-pointer** callback (the literal #12809 API shape) defeats
inlining and regresses badly; the built-in GC uses a *macro* precisely so the
per-field body inlines. The header-only **template** recovers near-macro-level
codegen (the visitor inlines at every slot) while staying generic and type-safe.

Measured (`native/bench/bench_scan.cpp`, MSVC `/O2`, x64), ns per reference field
over a 200k-object synthetic graph, isolating the inlining effect (all three
variants share the identical descriptor-walk control flow):

| Variant | ns/field |
|---|---|
| hand-written **macro** (`go_through_object` mechanism) | **~0.29** |
| inline **template** (`GCScanObjectRefs<TVisit>`) | ~0.41 |
| opaque **function pointer** (literal #12809 API) | ~2.04 |

The template is within ~1.4× of the raw macro (the small residual is the
by-reference visitor spilling its accumulator, which the plain-local macro keeps
in a register) and ≈ **5× faster** than the function pointer. So the generic,
type-safe template lands right next to the macro and nowhere near the naive
function-pointer API — confirming it is the right default and that #12809's
literal callback shape would be the wrong one. LXRGC therefore scans via the
template everywhere (`ProcessModifiedBuffers`/recursive free, `BackupTrace`,
`ResolveInterior`).

### GC-side full reclamation (all inside LXRGC, no further runtime change)

Built on the scan facility, LXRGC now performs **algorithmically-full STW LXR
reclamation**:

- **Coalescing RC + recursive free** — `ProcessModifiedBuffers` increments new
  referents / decrements old ones; zero-count objects are transitively freed,
  walking their fields via `GCScanObjectRefs`.
- **Backup trace (cycle collector + safety backstop)** — under
  `SuspendEE(SUSPEND_FOR_GC)`, marks from handles (`ForEachLiveHandle`) and
  stack/static/finalizer roots (`GcScanRoots`), taking the transitive closure via
  the scan template. A 1-bit/8-byte mark side table is committed+zeroed over the
  used-heap prefix each cycle.
- **Immix-style sweep + reuse** — every retired, parseable allocation chunk with
  no marked object is fully dead; its page-aligned interior is
  `VirtualFree(MEM_DECOMMIT)`-ed so **committed memory actually drops**, and the
  region is recycled for future allocation (footprint stays flat instead of
  doubling).

### Verified reclamation (this machine)

`samples/ConsoleApp` allocates a large rooted graph, drops it, `GC.Collect()`s,
re-allocates, then builds+drops a **cycle** and collects again, printing
`GC.GetTotalMemory(false)` (which maps to `IGCHeap::GetTotalBytesInUse`) each
phase:

```
[phase1] live graph rooted           = 368 MB
[phase2] dropped + GC.Collect()      =  92 MB   (reclaimed 288 MB)
[phase3] re-allocated                = 168 MB   (reused, not doubled)
[phase4] cycle dropped + GC.Collect()= 108 MB   (reclaimed a further 62 MB)
LXRGC-RECLAIM-OK / LXRGC-REUSE-OK / LXRGC-SMOKE-OK
```

The dead cycle in phase 4 (which pure RC can never reclaim) is collected by the
backup trace, and committed memory genuinely falls — LXRGC now reclaims, unlike
the dormant scaffold of §4.

### Still a genuine runtime limit: concurrency & evacuation

This phase implements **stop-the-world** RC + backup trace + Immix sweep
(algorithmically-full LXR reclamation). Real LXR is additionally **concurrent**
(a concurrent trace and **copying/evacuation** defragmentation with a
read/forwarding barrier). Full concurrency needs generic runtime support beyond
the two facilities above — safepoint cooperation for a concurrent collector and
an object-forwarding/read barrier hook for moving objects. That is the next place
we would "stop and inform": it is a follow-on stage, not silently dropped.

The ported ZeroGC benchmark suite (see **README.md § Benchmark suite**,
`results/report.html`) confirms this empirically: LXRGC is throughput-competitive
across six workloads × three GC modes (console, zeroalloc, growing-cache, webapi,
and two real dotLLM inference servers) while over-committing memory. One remaining
GC-side robustness edge traces back to the missing concurrency support: tight
always-allocating async loops at ≥8 threads can starve the single-threaded STW
collector (webapi is therefore benchmarked at 4 workers). A separate
`growing-cache` allocator bug — a reclaimed multi-MB large-object region being
recycled for a 128 KiB small-alloc chunk while keeping its original oversized
`Size`, so the later sweep decommitted/zeroed the whole span over live objects —
has been **fixed** (small-alloc reuse now only recycles exact-quantum regions;
see the known-limitation note below).

---

## 7. Paper-fidelity roadmap (closing the 5 gaps)

The STW engine above is a faithful *skeleton* of LXR but omits the moving parts
that make it "LXR-in-motion." Five gaps vs. the paper (arXiv:2210.17175) are being
closed in dependency/risk order (see `plan.md`): **(P1)** phase model + survival
triggering, **(P2)** SATB deletion buffers + remembered sets, **(P3)** STW
incremental evacuation, **(P4)** concurrent SATB trace + lazy decrements, **(P5)**
parallelism + scale.

### P1 — phase model + survival-rate triggering (done; no runtime change)

The monolithic "one heavy full-heap cycle per trigger" is replaced by LXR's actual
cadence: most allocation triggers now run a **light RC pause** (replay the
coalescing-RC modified buffers only — no trace, no decommit), and the collector
escalates to a full **trace pause** (backup trace + Immix sweep, the only phase
that reclaims cycles and returns committed memory) only *occasionally*. Cadence is
paced by a **survival-rate predictor**: after each trace the surviving fraction of
committed memory is folded into an EWMA that scales the RC-epoch cap (high survival
⇒ rarer traces; churny/low survival ⇒ trace sooner). Knobs:
`LXR_TRACE_EVERY_EPOCHS` (RC epochs between forced traces, default 8) and
`LXR_TRACE_BUDGET_MB` (committed growth between traces, default 128). Induced
`GC.Collect()` still forces a trace. New per-phase counters (`RCPauses`,
`TracePauses`, `Epochs`, `SurvivalPctEwma`) are exposed on `LXRCounters`. Verified:
under the console workload, cheap RC pauses (~0.1–1 ms) interleave with occasional
traces (paced by the predictor) and the app completes cleanly. This is entirely
GC-side.

### P2 — SATB deletion buffers + remembered sets (done; no runtime change)

The single field-logging barrier now feeds three jobs (as in the paper) instead of
one. Beyond the coalescing-RC modified buffer it also populates:

- **SATB deletion buffers** — while a trace window is open, the overwritten
  referent (the barrier's *old* value) is logged into per-thread snapshot buffers
  and marked by the trace (`DrainSatbBuffers`), so a concurrent marker (P4) cannot
  miss an object a mutator unlinks mid-trace (Yuasa snapshot-at-the-beginning).
- **Remembered sets** — slots that come to hold a pointer into a *different* Immix
  block (the barrier's *new* value in a different block than the slot) are recorded
  per-thread, giving evacuation (P3) the reference sources to fix up when a block
  is moved, without a full-heap scan.

Both extensions are gated (off by default → zero added barrier cost) and driven by
their consumers: SATB by the concurrent trace (P4), remsets by evacuation (P3).
Env knobs `LXR_SATB=1` / `LXR_REMSET=1` exercise them under the STW path today.
New counters (`SatbEntries`, `SatbMarks`, `RemsetEntries`, `RemsetFixups`) are on
`LXRCounters`. Verified: with both enabled under the console workload the barrier
logs SATB referents that the trace fully consumes (`satbMarks == satbEntries`) and
records thousands of inter-block remset slots, with reclamation and clean exit
preserved. Entirely GC-side (the existing pluggable-barrier callback already
surfaces both old and new values).

### P3 — STW incremental evacuation / copying (done; no runtime change)

LXR is not sweep-only: it defragments by *judiciously copying* live objects out of
the most-fragmented blocks (evacuation sets) inside the STW pause. LXRGC now does
this. During a trace pause, between `BackupTrace` (which marks live objects) and
`SweepAndSelectDefrag`, `Evacuate()` runs:

- **Select** — snapshot the committed regions and pick the most-fragmented ones
  (dead-byte ratio ≥ `LXR_EVAC_FRAG_PCT`, default 30%) up to a per-pause copy
  budget (`LXR_EVAC_BUDGET_MB`) — *incremental*, bounding pause time as the paper
  requires.
- **Copy** — claim fresh destination space above the trace-time high-water mark
  (`ClaimBlocks` + `CommitRange` + `RegisterChunk`), copy each live, non-pinned
  source object there, carry its RC slot across, and record the move in an
  off-object forwarding map (`std::unordered_map<Object*,Object*>` — chosen over an
  in-header forwarding word so the linear-walk metadata the sweep relies on, the
  MethodTable at offset 0 and array length at offset 8, is never corrupted).
- **Fix up** — walk every live object (via the §6 object-scan template) and rewrite
  any field that points at a moved object to its forwarding target; destinations are
  marked so the following sweep keeps them.
- **Pin conservatively** — all root- and handle-reachable referents are pinned for
  the pass (interior pointers resolved), so only heap fields need fix-up and no root
  or handle update is required; a pinned object stays in place and its region is not
  freed.
- **Free** — regions fully drained by the copy are released back to the allocator.

Gated by `LXR_EVAC=1` (off by default). New counters (`EvacPasses`, `EvacRegions`,
`EvacObjects`, `EvacBytesCopied`, `EvacFieldsForwarded`, `EvacPinnedSkipped`) are on
`LXRCounters`. Verified end-to-end on the console workload: evacuation runs each
trace pause (objects moved, hundreds of heap fields forwarded, pinned objects
skipped, drained regions freed), the app exits cleanly across repeated runs, and —
the point of copying — **steady-state committed memory drops from ~168 MB
(sweep-only) to ~88 MB (with evacuation)**, roughly halving the footprint by
compacting fragmented blocks. No read barrier and no runtime change are needed
because the copy happens entirely within the existing STW pause; concurrent copying
(which *would* need a runtime forwarding/read-barrier hook) remains P4/future work.

### P4 — concurrent SATB backup trace (done; no runtime change)

The paper's backup trace runs *concurrently* with the mutators; ours was fully
stop-the-world. LXRGC now performs the transitive mark off-pause, bracketed by two
brief STW pauses (gated by `LXR_CONCURRENT=1`):

1. **Snapshot pause (STW)** — replay the RC modified buffers, reset the mark
   table, open the SATB deletion window, seed the mark stack from stacks / statics
   / handles (`GcScanRoots` + handle scan, *without* draining), and record each
   region's allocation high-water. Then RestartEE.
2. **Concurrent drain (mutators running)** — the dedicated collector thread marks
   the transitive closure (`DrainMarkStack`) while interleaving `DrainSatbBuffers`
   to consume referents that mutators unlink mid-trace. The SATB (Yuasa
   snapshot-at-the-beginning) invariant, carried by the existing write barrier's
   *old-value* capture, guarantees no object live at the snapshot is missed even as
   the graph mutates.
3. **Finish pause (STW)** — drain residual SATB, finish the closure, then apply
   **allocate-black**: every object born since the snapshot (at/above its region's
   recorded high-water) is marked so the following sweep cannot free a live,
   never-traced new object. Close the SATB window and sweep.

Concurrency correctness needs **no runtime change and no read barrier**: SATB rides
the pluggable write barrier we already have (§5), and `SuspendEE`/`RestartEE` plus a
GC background thread (`IGCToCLR::CreateThread`) — all already exposed — suffice for
the two safepoints. Supporting disciplines are all GC-internal: SATB buffers use a
single-writer append cursor (`Count`) and a collector-only drain cursor (`Drained`)
so mutators log lock-free while the collector marks concurrently; recursive RC
frees never run during the window (memory stays stable for the marker); and block
reuse is suppressed for the window so allocate-black region snapshots stay valid.
New counters: `ConcurrentTraces`, `ConcAllocBlack`, `ConcSnapshotMicros`,
`ConcFinishMicros`, `ConcDrainMicros`. Verified: repeated and extended (15 s) runs
mark off-pause, retain hundreds of mid-trace objects via allocate-black, reclaim
memory, and exit cleanly with no access violation.

This closes gap 1.

### P5 — parallel mark (done; no runtime change)

LXR parallelizes its phases; ours was single-threaded. The transitive closure —
the dominant trace cost — is now parallel (gated by `LXR_GC_THREADS=N`, default 1).
Inside the STW trace pause, after the roots seed the mark stack,
`ParallelDrainMarkStack` partitions the seed set round-robin across `N` worker
threads; each drains its own local stack to completion. The **mark bit is set
atomically** (`_InterlockedOr8`), so an object is claimed by exactly one worker —
no object is scanned twice, and no shared mark stack or termination protocol is
required. Because the closure runs entirely within the pause (no managed code
executes), transient worker threads may read object memory without runtime
registration; no runtime facility is needed beyond the object-scan header (§6).

Verified: 1 / 4 / 8 workers all reclaim consistently and exit cleanly, and parallel
mark composes with STW evacuation (P3) — the two combined defragment to the same
~97 MB footprint with clean repeated runs. (Round-robin seed partitioning leaves
some load imbalance under skewed graphs; finer work-stealing is a straightforward
GC-internal refinement.)

**All five paper-fidelity gaps are now addressed** — phase model + survival-rate
triggering (P1), a single barrier feeding RC + SATB + remembered sets (P2), STW
copying evacuation (P3), a concurrent SATB backup trace (P4), and parallel marking
(P5) — **with no further runtime change beyond the two generic facilities** (the
pluggable write barrier and the object-scan header). They also **compose into one
unified collector**: with `LXR_CONCURRENT=1 LXR_EVAC=1 LXR_REMSET=1
LXR_GC_THREADS=N` the backup trace marks off-pause with `N` parallel workers
(`DrainClosure` routes the concurrent drain/finish and the STW trace through the
same parallel/serial closure), while copying evacuation runs on a **dedicated
periodic STW trace+evac cycle** interleaved with the concurrent-only marking
cycles (every `kEvacEveryN`-th trace takes the full STW `BackupTrace` +
`Evacuate` + sweep path). Keeping evacuation on its own complete-precise-trace
STW pause — rather than in the concurrent finish — matches the paper (LXR "copies
only during stop-the-world pauses") and guarantees `Evacuate`'s fix-up forwards
every marked object's field against a fully-marked heap with all roots pinned.
The remaining work is depth and hardening (finer parallel work-stealing, broader
benchmark coverage), not new runtime dependencies.

#### Resolved bug — null MethodTable in the linear heap parse (concurrent+evac)

Under the full unified config, WebApi occasionally access-violated at `0x0`
inside `ResolveInterior`/`LXRObjectSize` (fault-diag RVA resolved to
`ResolveInterior+0xD7`). Root cause: a mutator suspended mid fast-path allocation
has bumped its `alloc_ptr` but **not yet written the object's MethodTable**,
leaving a zeroed tail granule (`MT == 0`) at the end of its owned region. The
linear block-parse loops guarded on `if (sz == 0) break`, but the size is
computed *inside* `LXRObjectSize`, which dereferenced the MT with no null check —
so the AV happened before the guard could fire. **Fix:** `LXRObjectSize` now
validates the MethodTable (`m == 0 || (m & 7) || m < 0x10000 ||
m > 0x00007FFFFFFFFFFF → return 0`) and the three scan sites
(`DrainZeroCountWorkList`, `DrainMarkStack`, `ParallelDrainMarkStack`) skip a
zero-size object. On x64 TSO the zeroed granule is only ever the region tail, so
stopping the parse there loses nothing.

#### Resolved bug — SATB snapshot must never drop an overwritten value

A rarer managed `AccessViolationException` (e.g. in `Utf8JsonWriter.Grow`, a
*live-object-corrupted* signature) traced to `LogModifiedField` silently
**dropping** the overwritten old value once a per-thread SATB buffer filled
(`if (Count < kCapacity)`). Dropping a Yuasa snapshot-at-beginning entry breaks
soundness: the concurrent marker never reaches that referent, so the following
sweep/evac reclaims a still-reachable object. **Fix (partial):** the barrier now
never drops — a large pre-sized per-thread buffer plus a global `g_satbOverflow`
flag; on overflow the STW finish pause falls back to a full from-roots closure.
Buffer registration onto the global registry is **lock-free** (a
`RegisterSatbBuffer` CAS-prepend), because taking a lock in the write barrier —
which runs in cooperative GC mode — can deadlock against `SuspendEE` (a mutator
stalled on the lock never reaches a safepoint); an earlier lock-based version
intermittently hung mid-run.

> **NOTE (superseded — see "Soundness status under real webapi load" below).**
> The SATB deletion barrier is *fundamentally incomplete* over the current
> pluggable write barrier, so this fix does NOT make concurrent tracing sound.
> An earlier claim here that "16 × 120 s full-config WebApi runs complete with no
> AV and no hang" was **wrong** and has been removed.

#### Soundness status under the real WebApi workload (evidence-based)

Extensive stress testing (`samples/WebApi`, 120 s Kestrel+JSON load, many repeats,
faults captured with `cdb` + `LXR_FAULT_DIAG`) surfaced **four distinct,
independent defects**. The base (defect 4) is now **fixed and verified sound**;
the three advanced features (defects 1–3) remain gated off. In increasing order of
how fundamental they are:

1. **Parallel mark is unstable (P5).** `LXR_GC_THREADS=16` hangs in `SuspendEE`
   and occasionally mark-races to a crash — *even in the pure-STW path*
   (`LXR_CONCURRENT=0`): 12-run stress saw 2 hangs + 1 crash. The GC thread spins
   in `SuspendAllThreads` while every mutator is parked. Cause: `ParallelDrainMarkStack`
   spawns raw `std::thread` workers that are **not registered with the runtime**;
   creating/tearing them down every drain perturbs suspension. Serial mark
   (`LXR_GC_THREADS=1`) does not hang. Fix needs a **persistent, runtime-registered
   worker pool** (`IGCToCLR::CreateThread`), not per-drain `std::thread`.

2. **Concurrent SATB tracing is unsound (P4).** `LXR_CONCURRENT=1` (serial) AVs as
   a managed `AccessViolationException` in `Microsoft.AspNetCore.PinnedBlockMemoryPool.Rent`
   — the sweep decommitted a still-reachable pooled pinned buffer. Root cause: the
   fork's pluggable **write barrier only intercepts single-slot `JIT_WriteBarrier`**;
   **bulk/byref ref stores** (`Array.Copy`, struct/span copies, `memmoveGCRefs`,
   the `JIT_ByRefWriteBarrier` copy loop) **bypass the callback**, so their
   overwritten old values are never logged → SATB misses deletions → a
   snapshot-reachable object is left unmarked → swept. **Proof:** forcing the
   concurrent finish pause to re-mark the whole graph from roots under STW
   (`LXR_CONC_FINISH_FULLTRACE=1`, added this cycle) made 6/6 runs clean. A *truly*
   concurrent SATB trace therefore requires **completing the write barrier to
   capture old values on every ref-store form — a runtime-fork change** (the
   `JIT_CheckedWriteBarrier` path is already covered because it delegates via
   `jmp [JIT_WriteBarrier_Loc]`; only `JIT_ByRefWriteBarrier` / bulk copy helpers
   remain).

3. **STW evacuation has an interior-pointer fix-up gap (P3).** `LXR_EVAC=1` (serial,
   STW) crashes ~1/11 with a managed `NullReferenceException` deep in the new
   **runtime-async** state machine (`AsyncHelpers.RuntimeAsyncTask.HandleSuspended`,
   whose state is passed by `ref`/byref). `Evacuate` step 4 forwards **object-start**
   references of every marked object, but a managed **interior/byref pointer** into
   a moved object (matched only at object-start in the `forwarding` map) is not
   updated → dangling. Correct moving-GC fix-up must forward interior pointers too.

4. **Non-moving STW sweep-only base AV — FIXED and re-characterized this cycle.**
   The simplest config (`LXR_CONCURRENT=0 LXR_EVAC=0 LXR_GC_THREADS=1`) previously
   AV'd ~1/5, with the fault handler catching it **inside the GC** at
   `LXRCollector::ResolveInterior`, dereferencing `RESERVE`/decommitted memory.
   `ResolveInterior` maps a `GC_CALL_INTERIOR` (byref) root to its base object by
   linear-parsing the containing chunk. The **real desync prevention** is threefold
   and does *not* rely on catching a hardware fault:
   - **`LXRObjectSize` MT-validity sentinel:** any slot whose `MethodTable` is not a
     plausible aligned in-range pointer yields size 0, so the parse *stops* at the
     first inconsistent slot instead of computing a garbage size and walking off
     into unmapped memory. Combined with the earlier large-object-chunk-reuse fix
     (which removed the registry inconsistency that produced bad slots), this
     eliminates the desync at the source.
   - **Parse-free sweep liveness:** `SweepAndSelectDefrag` decides reclamation with
     `AnyMarkedInRange(Start,UsedEnd)` — a direct mark-bitmap scan, never a linear
     parse — so the reclamation decision cannot desync.
   - **Conservative keep-alive for unresolved interior roots
     (`ConservativelyKeepAliveInterior`):** an in-heap interior/byref root that
     still fails to resolve is **no longer silently dropped** (dropping it could let
     the sweep reclaim a region a live byref points into — a use-after-free that
     "explodes later"). Instead a mark bit is set at the interior's granule, so
     `AnyMarkedInRange` retains the whole containing chunk for the cycle. This
     over-approximates liveness (never under-approximates) → always sound, never a
     wrong-object resolution.

   The `__try/__except` guard around the parse (`ParseContainingObjectGuarded`) is
   **defense-in-depth only** — a backstop for a residual pathological fault — not
   the correctness mechanism. **Verified:** 22/22 clean 120 s/90 s WebApi runs
   (incl. a GC-hammering config with `LXR_GC_TRIGGER_MB=2` forcing far more
   `ResolveInterior` calls), `av=0`, and **zero** parse-fault-filter hits and
   **zero** conservative-keep-alive activations — i.e. resolution now simply
   succeeds. The **non-moving serial config is sound** under the real workload and
   is a legitimate LXR-reclamation GC (coalescing RC + STW cyclic backup trace +
   Immix line/block sweep with real decommit).

**Conclusion.** With defect 4 fixed, the **serial non-moving STW config is sound**
under the real WebApi workload and is the first benchmarkable LXR configuration.
The three remaining defects (1 parallel mark, 2 concurrent SATB, 3 evacuation) all
converge on the theme the project flagged from the start as the likely "stop and
inform" boundary: enabling *parallelism, concurrency, or copying* on top of the
sound base needs the runtime to (a) surface the **old value on *all* ref stores**
(defect 2 — a runtime-fork write-barrier completion) and (b) let the GC
**completely and safely enumerate/forward interior pointers** while moving
(defect 3), plus a **runtime-registered GC worker pool** for stable parallel mark
(defect 1). With the user's green light, these three are now in progress as
generic, minimal changes (defect 2 spans the `kkokosa/runtime` fork). The Phase-2
STW reclamation demo stayed clean on ConsoleApp because that workload had no bulk
ref copies, no runtime-async byrefs, and no pinned-pool churn; the sweep-only base
now holds under a workload that has all three.


#### Current status — defects 1/3 fixed, full-STW-LXR sound at scale; concurrency hits a runtime-async wall

Following the green light to land generic, minimal runtime + GC changes for
defects 1–3, the status of the four defects above is now:

- **Defect 1 (parallel mark) — FIXED.** `ParallelDrainMarkStack` now uses a
  **persistent worker pool** created once (instead of raw `std::thread` per drain).
  The `SuspendEE` perturbation is gone: `LXR_GC_THREADS=16` in the pure-STW path
  ran **8/8 clean** at 120 s WebApi load (was 2 hangs + 1 crash / 12).
- **Defect 3 (evacuation interior fix-up) — FIXED.** `Evacuate` step 4 now sorts
  the moved-object ranges and rebases **interior/byref** field values (binary
  search on `movedRanges`, preserving offset) in addition to object-start
  references; an interior *root* that fails to resolve during the pin pass skips
  evacuation for that cycle rather than dangling. Serial STW evac: **11/11 clean**
  (was ~1/11 NRE).
- **Defect 2 (bulk write barrier) — runtime change LANDED (generic, minimal).**
  A new **`write_barrier_bulk_callback`** was added to the standalone-GC ABI
  (`gcinterface.h`, `WriteBarrierParameters`, minor version 9→10) and invoked from
  the single convergence point of all bulk GC-ref moves,
  `InlinedMemmoveGCRefsHelper` (`arraynative.inl`), **before** the copy overwrites
  the destination — so `Array.Copy` / span / struct block-copies
  (`CORINFO_HELP_BULK_WRITEBARRIER` → `Buffer.BulkMoveWithWriteBarrier`) now
  surface their old+new referents. It is GC-agnostic: the runtime just hands the
  GC `(dest, src, byteCount)`; LXR replays each slot through `LogModifiedField`.
  Committed on `kkokosa/runtime@feature/pluggable-write-barrier` (85f97b668cf).
- **Also fixed (helps STW evac + any trace):** interior/byref values pushed to the
  mark stack are now resolved to their base object at pop time (a byref field's
  `*ref` points into an object's interior; without resolution the target's
  out-edges were never scanned → sweep → AV in `DispatchContinuations`); and
  one-past-the-end byrefs (`ref array[array.Length]`) now resolve to the last
  object instead of failing bounds.

**Result: the full-STW-LXR configuration is sound at scale.** With RC + parallel
mark (pool) + **STW copying evacuation** + backup trace + the bulk barrier + the
interior fixes, `-GcThreads 16 -Concurrent 0 -Evac 1` ran **8/8 clean** at 120 s
WebApi load. This is the paper-faithful shape for evacuation — LXR copies **only
during stop-the-world pauses** (arXiv:2210.17175 §Design) — and is the first
fully-featured benchmarkable LXR configuration.

##### The remaining wall — concurrent SATB trace vs. .NET 11 runtime-async (stop-and-inform)

Enabling the **concurrent** trace (`LXR_CONCURRENT=1`) still AVs intermittently,
and the bulk barrier does **not** close it. Root cause, isolated this cycle: the
crash reproduces with interior resolution succeeding (`unresolved=0`), always in
`RuntimeAsyncTask.DispatchContinuations` / `PinnedBlockMemoryPool.Rent`. .NET 11
**runtime-async** suspends an async method by spilling its live registers/locals —
**including object refs and interior byrefs** — into a heap-allocated
**continuation** object. Those spill stores bypass the JIT/pluggable write barrier
(they are not lowered to `JIT_WriteBarrier` nor to the bulk helper), so LXR's SATB
snapshot never logs them → the concurrent trace can miss a still-live
continuation-referenced object → the sweep frees it → AV. STW configs are immune
because `GcScanRoots` enumerates the **entire** stack root set at the safepoint,
with zero dependence on the barrier.

This is a genuine **runtime limit**, not a GC bug, and it is **not** closable by a
generic, minimal ABI addition (unlike defect 2): it would require the runtime-async
implementation to route its continuation-spill stores through the write barrier —
a **targeted, non-generic change to a specific runtime subsystem**, exactly the
"stop and inform" boundary flagged from the outset. The available options are:

1. **Benchmark the sound full-STW-LXR** (RC + parallel-16 mark + STW evacuation +
   backup trace + bulk barrier) — paper-faithful for copying, verified 8/8 clean.
2. **Runtime-async barrier routing** — a targeted, non-generic runtime-fork change
   so continuation spills log through the barrier; unblocks true concurrency but
   crosses the stop-and-inform line.
3. **`LXR_CONC_FINISH_FULLTRACE=1`** — a sound fallback that re-traces from roots
   under STW at the concurrent finish pause (6/6 clean), but forfeits most of the
   concurrency benefit (effectively "STW trace with extra steps").

##### RESOLVED — sound concurrency via generic barrier completion + STW closure-completion

The wall above is now closed **without** the non-generic option 2 (no
runtime-async-specific plumbing). Three changes, in dependency order:

1. **Generic JIT full-ref-barrier flag** (`kkokosa/runtime`, GC-agnostic). A new
   `CORJIT_FLAG_GC_FULL_REF_BARRIERS` (mirrored `JIT_FLAG_GC_FULL_REF_BARRIERS`)
   makes the JIT emit a barrier for **every** in-heap ref store, including the
   null-store / frozen-object-handle stores it normally elides. Under a Yuasa/SATB
   *deletion* barrier those elided stores are exactly the deletions the concurrent
   trace must observe. The flag is off by default (zero impact on the built-in GC);
   a custom GC opts in via `write_barrier_requires_all_ref_stores` in
   `WriteBarrierParameters` (minor-version bump). *Files:* `inc/corjitflags.h`,
   `jit/jitee.h`, `jit/gcinfo.cpp`, `vm/writebarriermanager.cpp`, `vm/gchelpers.h`,
   `vm/gcenv.ee.cpp`, `vm/jitinterface.cpp`, `gc/gcinterface.h`.

2. **VM-side `SetObjectReferenceUnchecked` routing** (`kkokosa/runtime`,
   GC-agnostic). The VM's canonical native heap ref store (used by exceptions,
   reflection, statics, delegates, string interning) only card-marked and never
   surfaced the old value. It now captures the old value and calls the pluggable
   Callback barrier for in-heap destinations when one is installed. *File:*
   `vm/object.cpp`.

3. **GC-side STW closure-completion** (`kkokosa/runtimelab`, no runtime change).
   Even with (1)+(2), a residual handful of live objects per occasional cycle are
   still deleted through paths that never reach the barrier — notably **.NET 11
   runtime-async continuation spills** (live refs/byrefs spilled into a heap
   continuation object) and `IntPtr`-typed / `Unsafe` ref-array clears in CoreLib,
   which are not lowered to `JIT_WriteBarrier`. Rather than add a targeted,
   non-generic hook to the runtime-async subsystem (the flagged stop-and-inform
   boundary), the concurrent **finish pause** — already STW, with every mutator
   stopped — completes the transitive closure over all currently-marked objects
   (`CompleteClosureOverMarked`): for each marked (retained/live) object it marks
   any in-heap child not yet marked, to a fixpoint, **before** the
   mark-authoritative sweep. Marking a child of a live object is definitionally
   correct (it is reachable from a live object, hence live) and never retains a
   genuinely-dead object (nothing points to it), so this yields the *correct,
   complete* trace state — it fixes the desync rather than hiding an invalid final
   state. It is the SATB-overflow full re-trace generalized to "any missed
   deletion," and it needs no read barrier and no runtime change.

This is why it is not masking: the closure-completion **computes the correct live
set** that the barrier-incomplete concurrent trace failed to reach; the alternative
(leaving a marked/live parent with an unmarked child) is the actual bug, because
the mark-authoritative sweep would then `MEM_DECOMMIT` a live object's chunk →
UAF/AV in `DispatchContinuations` / `PinnedBlockMemoryPool.Rent`. A new counter
`ClosureGapMarked` records the residual gap size (typically 3–8 objects/cycle) so
the barrier-coverage shortfall stays visible rather than silent.

**Verified sound** (`LXR_VERIFY_TRACE=1`, `DOTNET_ReadyToRun=0` so CoreLib is JIT'd
with the flag):
- `LXR_CONCURRENT=1 LXR_GC_THREADS=1` (serial concurrent): **5/5 clean**, 120 s
  WebApi, `av=0 hang=0`; every finish `verify` reports `offenders=0` after
  closure-completion.
- Full unified `LXR_CONCURRENT=1 LXR_EVAC=1 LXR_REMSET=1 LXR_GC_THREADS=16`
  (concurrent trace + parallel-16 mark + copying evacuation): **3/3 clean**,
  120 s, `av=0 hang=0`, valid `##RESULT##` with `Errors:0`; the earlier
  GcThreads=1 concurrent hang did not recur (it was a symptom of the same
  incomplete-trace decommit).

*Cost / future work:* `CompleteClosureOverMarked` linearly walks the used heap at
each concurrent finish pause. Concurrent cycles are occasional, so the added STW
time is modest, but it can be narrowed later to dirty/remset regions or skipped
when barrier coverage is provably complete.


#### Resolved bug — large-object chunk recycled as a small chunk (growing-cache)

On a continuously-growing multi-GB object graph containing very large reference
arrays (the `growing-cache` benchmark: a `Dictionary` whose backing `Entry[]`
reaches tens of MB while millions of small value objects churn), an earlier
build access-violated (~30% of runs) inside `DrainMarkStack`/`BackupTrace`: a
live array's element slots read back as UTF-16 string data, i.e. still-referenced
objects had been overwritten by fresh allocations.

Root cause (fixed this cycle): the allocator's free list holds reclaimed regions
of *any* size, including dead multi-MB large-object arrays (a `Dictionary` resize
drops the old `Entry[]`). `ReuseChunk` popped such a region for a 128 KiB
small-alloc request but left the registry entry's `Size` at the original
multi-MB value. Only the first quantum was tracked/used, yet the later sweep
`MEM_DECOMMIT`'d — and the next reuse `memset` + recommitted — the *entire*
multi-MB span, clobbering live objects that had been carved into the same
address range. It was **not** a marking-completeness gap: `LXR_VERIFY_TRACE`
reported `offenders=0` and `MarkStackDrops == 0` precisely because the trace was
correct; the corruption came from the allocator handing out live memory.

The decisive experiments were `BENCH_PRESIZE` (pre-sizing the dictionary to
avoid resize/large-array churn → clean at the same 4.2 GB size) and `LXR_NO_REUSE`
(disable free-list recycling → clean), which together isolated the fault to
large-chunk reuse. **Fix:** `ReuseChunk` now recycles a region on the small-alloc
fast path only when its `Size == CONTEXT_ALLOC_QUANTUM`; reclaimed large-object
regions stay decommitted (their committed footprint was already released by the
sweep). Verified: the `growing-cache` workload (19.2 M entries, ~4.5 GB) now runs
to `exit 0` with valid `##RESULT##` under pure-STW and under the full unified
config (`LXR_CONCURRENT=1 LXR_EVAC=1 LXR_REMSET=1 LXR_GC_THREADS=16`) across
repeats. `LXR_NO_REUSE=1` remains available as a diagnostic knob.

---

**Conclusion: LXR is implementable on the standalone-GC ABI given two small,
generic, GC-agnostic runtime facilities** — a pluggable write barrier (§5,
surfaces the old field value; #barrier) and an object-reference-scanning header
(§6, implements #12809 at macro speed via an inline template). With both, LXRGC
runs the real LXR engine — coalescing reference counting, a periodic backup trace
that collects cycles, and Immix sweep/decommit/reuse — and **actually reclaims
memory end-to-end** (288 MB in the demo, cycles included). What remains
(concurrency + copying evacuation) is a genuine, clearly-scoped runtime gap for a
future stage, not a limitation of the reclamation implemented here.
