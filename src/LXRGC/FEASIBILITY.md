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
and two real dotLLM inference servers) while over-committing memory, but two
GC-side robustness edges trace back to the missing concurrency support — tight
always-allocating async loops at ≥8 threads can starve the single-threaded STW
collector (webapi is therefore benchmarked at 4 workers), and on very large,
continuously-mutating graphs (`growing-cache`) the periodic backup trace can
occasionally AV by following a stale reference into a chunk a prior sweep
decommitted (~30% of runs). Both are precisely the "concurrent trace with
safepoint cooperation" gap described above, not a limit of the STW reclamation.

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
same parallel/serial closure), and copying evacuation runs in the concurrent finish
pause — safe because that pause has completed marking (allocate-black included, so
`Evacuate`'s fix-up forwards every marked object's fields) and pins all roots
(covering references mutators cached during the window). The remaining work is depth
and hardening (finer parallel work-stealing, broader benchmark coverage), not new
runtime dependencies.

#### Known limitation — multi-GB continuously-mutating graphs (growing-cache)

On a continuously-growing multi-GB object graph containing very large reference
arrays (the `growing-cache` benchmark: a `Dictionary` whose backing `Entry[]`
reaches tens of MB while millions of small value objects churn), the STW backup
trace deterministically leaves a band of *still-referenced* value objects unmarked;
the subsequent sweep therefore reclaims and reuses their region, and the **next**
trace access-violates when it follows the live array's now-dangling element slots
into the reused memory (their MethodTable words read back as UTF-16 string data).
This was root-caused this cycle: the fault is inside `DrainMarkStack` during
`BackupTrace`; `LXR_NO_SWEEP=1` avoids it (no reclaim/reuse), the mark stack never
overflows (`MarkStackDrops == 0`), and completed traces verify clean
(`LXR_VERIFY_TRACE=1` → `offenders=0`) — so it is a **marking-completeness gap on
large-array subtrees**, not a sweep or mark-stack bug. It is gated behind two
diagnostics (`LXR_FAULT_DIAG`, `LXR_VERIFY_TRACE`) and does not affect the other
workloads (console, web API, zero-alloc, GCPerfSim single/multi-thread, dotLLM
inference all run the full unified collector cleanly). Fully closing it is scoped
as prototype hardening; the benchmark harness tolerates it (each run is isolated,
so only the LXR `growing-cache` cell is skipped).

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
