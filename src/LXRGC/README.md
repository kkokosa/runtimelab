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
    LXRGCHeap.cpp         IGCHeap impl: Immix allocator + RC engine (dormant)
    LXRGCHandles.cpp      IGCHandleManager impl (reused from ZeroGC)
    build.ps1             Builds LXRGC.dll (Release/Debug, x64) via cl.exe
  samples/ConsoleApp/     Tiny allocating smoke-test app
```

## What works (built and run against the local runtime)

- Full `IGCHeap` + `IGCHandleManager` ABI; `LXRGC.dll` **loads and runs real
  managed code** as the active GC.
- A real **Immix substrate**: 32 KiB blocks / 256 B lines, per-block metadata,
  per-thread lock-free block-run bump allocator.
- The **RC engine**: side-table reference counts, coalescing-RC replay,
  recursive zero-count freeing, backup-trace / sweep skeletons.

## What's now unblocked (via the runtime's generic pluggable write barrier)

- The **field-logging write barrier** that feeds the RC engine. LXRGC selects
  the runtime's neutral `WriteBarrierKind::Callback` in `LXRGCHeap::Initialize`;
  the runtime captures the overwritten value and calls
  `LXRWriteBarrierCallback(slot, newValue, oldValue)` →
  `LXRCollector::LogModifiedField`. At `GC.Collect()`, `ProcessModifiedBuffers`
  runs the coalescing RC (increment new referent, decrement old). Verified: a
  managed run captured thousands of real field-store log entries and performed
  real RC increments/decrements. Requires the runtime fork branch
  `feature/pluggable-write-barrier` (see FEASIBILITY.md §5).

## What still remains (GC-side, not an ABI limitation)

- Recursive zero-count freeing needs `CGCDesc` field traversal, and cycle
  collection needs the backup trace — both live entirely inside LXRGC and are
  independent of the runtime facility.

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
