# LXRGC — an attempt to implement LXR as a standalone CoreCLR GC

An experiment to implement the **LXR** garbage collector ("Low-Latency,
High-Throughput Garbage Collection", Zhao, Blackburn & McKinley, PLDI 2022 —
<https://arxiv.org/abs/2210.17175>) as a **standalone** CoreCLR GC —
loaded via `DOTNET_GCName=LXRGC.dll`, **without modifying the runtime** — as a
sibling to [ZeroGC](https://github.com/kkokosa/runtimelab/tree/feature/ZeroGC).

## TL;DR

**It can't be done without a runtime change, and this repo shows exactly why
and exactly how far you *can* get.** LXR is defined by its *field-logging
(coalescing) reference-counting write barrier*, which must capture the **old**
value of every mutated reference field. The CoreCLR standalone-GC ABI only lets
a plug-in GC point the JIT's fixed **card-marking** barrier at a card table — it
never surfaces the old value and never lets the GC inject its own barrier code.
Full analysis with runtime citations and a proposed minimal runtime change:
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

## What's blocked

- The **field-logging write barrier** that would feed the RC engine. It is
  implemented (`LXRCollector::LogModifiedField`) but **unreachable**: no
  standalone-ABI mechanism routes managed field writes to it, and the old value
  is destroyed by the store before the only observable side effect (a card
  dirty bit). Consequently the RC/collection machinery is present but dormant,
  so in practice LXRGC allocates and never reclaims. See FEASIBILITY.md §3.

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
