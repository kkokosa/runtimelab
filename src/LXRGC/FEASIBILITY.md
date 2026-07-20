# LXR on the CoreCLR standalone-GC ABI — feasibility analysis

**Verdict: LXR cannot be implemented as a standalone GC without changing the
runtime.** The blocker is fundamental and appears at the very heart of LXR: its
*field-logging / coalescing reference-counting write barrier*. The CoreCLR
standalone-GC ABI gives a plug-in GC no way to observe the **old** value of a
mutated reference field, and no way to install its own barrier code — the only
barrier the JIT emits is a fixed **card-marking** barrier whose behaviour the GC
can parameterise but not replace.

This document explains what LXR needs, what the ABI provides, exactly where they
diverge (with runtime source citations), and the minimal runtime change that
would unblock it. A working scaffold (`native/`) that goes *as far as the ABI
allows* accompanies this analysis and has been built and run against the locally
compiled runtime.

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

## 5. Minimal runtime change that would unblock LXR

LXR needs the JIT write barrier to optionally **log the old value**. The
smallest viable runtime change is to add an **old-value-logging barrier
variant** the standalone GC can select, mirroring how
`FEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP` already adds write-watch barrier
variants:

1. Add a `WriteBarrierOp` / barrier variant (e.g. `JIT_WriteBarrier_FieldLog`)
   in `writebarriermanager.*` and the per-arch `JitHelpers_FastWriteBarriers`
   that, before/atomically-with the store, appends `(slot, *slot)` to a
   per-thread **log buffer** whose base/limit are thread-local (like the
   allocation context) and refilled via a GC slow-path call.
2. Extend `WriteBarrierParameters` with the log-buffer configuration and a
   `WriteBarrierOp::SwitchToFieldLogging` op.
3. Expose a `GCToEEInterface` upcall for buffer overflow (flush to the GC).

This is the same barrier the runtime's own SATB/precise machinery is *close* to
but does not expose to standalone GCs. With it, the already-written
`LXRCollector` in this scaffold could be driven directly.

---

**Conclusion (per the original request): a runtime change is required — stopping
here.** The scaffold demonstrates everything achievable without it; the
field-logging write barrier is the one thing the standalone-GC ABI cannot
provide.
