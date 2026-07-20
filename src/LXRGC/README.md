# LXRGC — an attempt to implement LXR as a standalone CoreCLR GC

An experiment to implement the **LXR** garbage collector ("Low-Latency,
High-Throughput Garbage Collection", Zhao, Blackburn & McKinley, PLDI 2022 —
<https://arxiv.org/abs/2210.17175>) as a **standalone** CoreCLR GC —
loaded via `DOTNET_GCName=LXRGC.dll`, **without modifying the runtime** — as a
sibling to [ZeroGC](https://github.com/kkokosa/runtimelab/tree/feature/ZeroGC).

## TL;DR

**Originally this could not be done without a runtime change — and this repo
shows exactly why.** LXR is defined by its *field-logging (coalescing)
reference-counting write barrier*, which must capture the **old** value of every
mutated reference field. The stock CoreCLR standalone-GC ABI only lets a plug-in
GC point the JIT's fixed **card-marking** barrier at a card table.

**That wall has now been removed** by adding a small, generic, **GC-agnostic
pluggable write barrier** to the runtime fork
[`kkokosa/runtime` @ `feature/pluggable-write-barrier`] (gated behind
`FEATURE_GC_CUSTOM_WRITE_BARRIER`). The runtime exposes a neutral
`WriteBarrierKind::Callback` that captures the old value and hands
`(slot, newValue, oldValue)` to a GC-registered callback — nothing LXR- or
policy-specific enters the runtime. LXRGC selects it and its coalescing-RC engine
is now genuinely driven by real managed field stores. Full analysis:
**[FEASIBILITY.md](FEASIBILITY.md)**.

## What's here

```
src/LXRGC/
  FEASIBILITY.md          The analysis: the wall, with runtime citations
  native/
    LXRGC.h               ABI surface + Immix geometry + RC engine declarations
    dllmain.cpp           GC_VersionInfo / GC_Initialize entry points
    LXRGCHeap.cpp         IGCHeap impl: Immix allocator + full RC/trace/sweep
    LXRGCHandles.cpp      IGCHandleManager impl (reused from ZeroGC)
    build.ps1             Builds LXRGC.dll (Release/Debug, x64) via cl.exe
    bench/                Microbenchmark: inline-template vs function-ptr scanning
  samples/ConsoleApp/     Reclamation demo (allocate → drop → collect → reuse → cycle)
```

## What works (built and run against the local runtime)

- Full `IGCHeap` + `IGCHandleManager` ABI; `LXRGC.dll` **loads and runs real
  managed code** as the active GC.
- A real **Immix substrate**: 32 KiB blocks / 256 B lines, per-block metadata,
  per-thread lock-free block-run bump allocator.
- The **RC engine**: side-table reference counts, coalescing-RC replay,
  recursive zero-count freeing, and a working backup trace + Immix sweep.

## What's now unblocked (via two generic runtime facilities)

- The **field-logging write barrier** that feeds the RC engine. LXRGC selects
  the runtime's neutral `WriteBarrierKind::Callback` in `LXRGCHeap::Initialize`;
  the runtime captures the overwritten value and calls
  `LXRWriteBarrierCallback(slot, newValue, oldValue)` →
  `LXRCollector::LogModifiedField`. At `GC.Collect()`, `ProcessModifiedBuffers`
  runs the coalescing RC (increment new referent, decrement old). Requires the
  runtime fork branch `feature/pluggable-write-barrier` (FEASIBILITY.md §5).
- **Object reference scanning** — a second generic, GC-agnostic runtime header
  `src/coreclr/gc/gcobjscan.h` (the long-missing dotnet/runtime **#12809**),
  offering an inline template `GCScanObjectRefs<TVisit>` that matches the
  built-in `go_through_object` **macro** speed (≈0.42 ns/field vs ≈2.07 ns/field
  for the naive function-pointer API — ~4.5× faster; see `native/bench/`). LXRGC
  scans every object through it (FEASIBILITY.md §6).

## Full reclamation — now working (STW)

Built on the two facilities, LXRGC performs **algorithmically-full stop-the-world
LXR reclamation**: coalescing RC with recursive zero-count freeing, a periodic
**backup trace** (under `SuspendEE`, marking from handles + `GcScanRoots`) that
collects **dead cycles** pure RC cannot, and an **Immix sweep** that
`MEM_DECOMMIT`s fully-dead chunks (committed memory actually drops) and recycles
them for reuse. Verified end-to-end (`samples/ConsoleApp`):

```
[phase1] live graph rooted            = 368 MB
[phase2] dropped + GC.Collect()       =  92 MB   (reclaimed 288 MB)
[phase3] re-allocated                 = 168 MB   (reused, not doubled)
[phase4] cycle dropped + GC.Collect() = 108 MB   (a further 62 MB, cycle collected)
LXRGC-RECLAIM-OK / LXRGC-REUSE-OK / LXRGC-SMOKE-OK
```

## What still remains (a genuine, scoped runtime gap)

- **Concurrency + copying evacuation.** Real LXR is concurrent and defragments by
  moving objects, which needs generic runtime support beyond the two facilities
  above (safepoint cooperation for a concurrent collector, an object-forwarding /
  read-barrier hook). This phase is STW-only; concurrency is a flagged follow-on
  stage, not silently dropped.

## Build & run

```powershell
cd src\LXRGC\native
.\build.ps1 -RuntimeRepo C:\github\runtime -Configuration Release

$fw = "C:\github\runtime\artifacts\bin\testhost\net11.0-windows-Release-x64\shared\Microsoft.NETCore.App\11.0.0"
Copy-Item obj\Release\LXRGC.dll $fw -Force
$env:DOTNET_GCName = "LXRGC.dll"
& "$fw\corerun.exe" ..\samples\ConsoleApp\bin\ConsoleApp.dll
```

The runtime at `C:\github\runtime` is used **read-only** (headers + a locally
built `coreclr.dll`/testhost for running). Nothing under it is modified.
