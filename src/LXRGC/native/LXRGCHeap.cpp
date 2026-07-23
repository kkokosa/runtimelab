// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// LXRGCHeap.cpp - IGCHeap implementation + LXR reference-counting engine.
//
// See LXRGC.h for the design overview and FEASIBILITY.md for why the RC
// engine cannot actually be driven under the standalone-GC ABI.
//
#include "LXRGC.h"
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <thread>
#include <dbghelp.h>
#include <tlhelp32.h>

// One-shot full-memory minidump (enabled via LXR_DUMP_ON_GAP=1). Called at the
// first concurrent-trace closure gap - while mutators are STW-stopped, so the
// logged offender addresses stay valid - so the offender MethodTable pointers
// can be resolved to type names offline with SOS/dotnet-dump. Diagnostic only.
static volatile LONG g_lxrDumpedOnGap = 0;
static void LXRWriteGapMiniDump()
{
    if (getenv("LXR_DUMP_ON_GAP") == nullptr)
        return;
    if (InterlockedCompareExchange(&g_lxrDumpedOnGap, 1, 0) != 0)
        return;
    if (InterlockedCompareExchange(&g_lxrDumpedOnGap, 1, 0) != 0)
        return;
    const wchar_t* path = L"C:\\temp\\lxr-gap.dmp";
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "LXRGC: [gap-dump] could not create %ls (err=%lu)\n", path, GetLastError());
        return;
    }
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                                MiniDumpWithFullMemory, nullptr, nullptr, nullptr);
    CloseHandle(h);
    fprintf(stderr, "LXRGC: [gap-dump] wrote %ls ok=%d\n", path, ok);
    fflush(stderr);
}


// GCScanObjectRefs' collectible-class branch calls this EE up-call. LXRGC does
// not compile the standalone gcenv.ee inline forwarders, so provide the single
// forwarder it needs here (routes to the runtime like every other EE up-call).
uint8_t* GCToEEInterface::GetLoaderAllocatorObjectForGC(Object* pObject)
{
    return g_theGCToCLR->GetLoaderAllocatorObjectForGC(pObject);
}

LXRCounters g_lxrCounters = {};
LXRGCHeap* g_lxrGCHeap = nullptr;
LXRCollector g_lxrCollector;

// Diagnostic VEH (enabled via LXR_FAULT_DIAG=1): on an access violation, dump
// the faulting instruction pointer + accessed address + whether that address is
// committed, so we can localize the dangling-reference crash.
static LONG CALLBACK LXRFaultDiag(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        void* accessed = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        void* rip = (void*)ep->ContextRecord->Rip;
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)rip, &mod);
        wchar_t modName[MAX_PATH] = L"?";
        if (mod) GetModuleFileNameW(mod, modName, MAX_PATH);
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQuery(accessed, &mbi, sizeof(mbi));
        const char* state = (mbi.State == MEM_COMMIT) ? "COMMIT" :
                            (mbi.State == MEM_RESERVE) ? "RESERVE" : "FREE";
        fprintf(stderr, "LXRGC: !!! AV accessing %p rip=%p (rip-off=+0x%llx in %ls) accessed-state=%s\n",
                accessed, rip, mod ? (unsigned long long)((uint8_t*)rip - (uint8_t*)mod) : 0ull,
                modName, state);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// One large reservation; blocks are carved out of it. Smaller than ZeroGC's
// 64 GiB because LXR also reserves an RC side table proportional to heap size.
static const size_t HEAP_RESERVE_SIZE = (size_t)16 << 30;   // 16 GiB
static const size_t COMMIT_CHUNK = 16 * 1024 * 1024;        // 16 MiB
static const size_t CONTEXT_ALLOC_QUANTUM = 128 * 1024;
static const size_t THREAD_BLOCK_RUN = 64 * 1024 * 1024;    // 64 MiB claimed per thread at a time

struct ThreadHeapState
{
    uint8_t* RunBase = nullptr;
    uint8_t* RunEnd = nullptr;
    uint8_t* NextFree = nullptr;
    uint8_t* CommitEnd = nullptr;
    int      CurrentChunkIndex = -1; // registry index of the context chunk being filled
};
static thread_local ThreadHeapState t_threadHeap;

struct FrozenSegment
{
    uint8_t* Base;
    uint8_t* Allocated;
    uint8_t* Committed;
    uint8_t* Reserved;
    bool InUse;
};
static const int MAX_FROZEN_SEGMENTS = 64;
static FrozenSegment g_frozenSegments[MAX_FROZEN_SEGMENTS];
static CRITICAL_SECTION g_frozenSegmentsLock;

// ===========================================================================
//        LXR reference-counting engine + coalescing write barrier
// ===========================================================================
//
// Per-mutator "modified buffer": the field-logging (coalescing RC) write
// barrier appends (slot, oldValue) pairs here on the first mutation of a slot
// in an epoch. At GC time ProcessModifiedBuffers replays them.
//
struct ModifiedEntry
{
    Object** Slot;
    Object*  OldValue;
};

struct ModifiedBuffer
{
    static const size_t kCapacity = 4096;
    ModifiedEntry Entries[kCapacity];
    size_t Count = 0;
    ModifiedBuffer* NextRegistered = nullptr;
};

static thread_local ModifiedBuffer* t_modifiedBuffer = nullptr;
static ModifiedBuffer* volatile g_registeredBuffers = nullptr;
static CRITICAL_SECTION g_buffersLock;

// Lock-free prepend of a modified buffer onto the global registry (see
// RegisterSatbBuffer). The write barrier runs in cooperative GC mode; taking a
// Win32 CRITICAL_SECTION there deadlocks SuspendEE - a mutator stalled on the
// lock whose holder SuspendEE has already OS-suspended never reaches a safepoint,
// so the suspension hangs. CAS-prepend keeps the barrier non-blocking. The
// collector only traverses the registry under STW (ProcessModifiedBuffers /
// SnapshotModifiedBuffers), so there is never a concurrent register+traverse.
static void RegisterModifiedBuffer(ModifiedBuffer* nb)
{
    for (;;)
    {
        ModifiedBuffer* head = g_registeredBuffers;
        nb->NextRegistered = head;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_registeredBuffers,
                                              nb, head) == head)
            return;
    }
}

// --- SATB (snapshot-at-the-beginning) deletion buffers (P2) ----------------
// While a trace window is open (g_satbActive), the field-logging barrier
// appends every overwritten referent here. A trace consumes them via
// DrainSatbBuffers so a concurrent marker (P4) cannot miss an object that a
// mutator unlinks mid-trace. Persist across RC epochs (unlike the RC modified
// buffer) until a trace drains them.
struct SatbBuffer
{
    static const size_t kCapacity = 8192;
    Object* Entries[kCapacity];
    size_t Count = 0;     // written only by the owning mutator (append)
    size_t Drained = 0;   // written only by the collector (mark cursor)
    SatbBuffer* NextRegistered = nullptr; // global registry chain (collector walks)
};
static thread_local SatbBuffer* t_satbBuffer = nullptr; // this thread's append buffer
static SatbBuffer* volatile g_registeredSatbBuffers = nullptr;
static CRITICAL_SECTION g_satbLock;
static volatile LONG g_satbActive = 0;   // logging gate: open while a trace window is live
static volatile LONG g_satbOverflow = 0; // a mutator dropped a SATB entry this cycle
// A mutator's coalescing-RC modified buffer filled during a trace window, so a
// written slot was dropped and the modified-set race reconciliation (below) may
// be incomplete. When set, the concurrent finish falls back to the full
// O(live-heap) closure. Reset each epoch in ProcessModifiedBuffers.
static volatile LONG g_modifiedOverflow = 0;

// Lock-free prepend of a buffer onto the global SATB registry. The write barrier
// runs in cooperative GC mode; taking a lock there can deadlock against SuspendEE
// (a mutator stalled on the lock never reaches a safepoint). CAS-prepend keeps the
// barrier non-blocking. Traversal is safe because NextRegistered is published
// before the node and the list is append-only (nodes are never removed/freed).
static void RegisterSatbBuffer(SatbBuffer* nb)
{
    for (;;)
    {
        SatbBuffer* head = g_registeredSatbBuffers;
        nb->NextRegistered = head;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_registeredSatbBuffers,
                                              nb, head) == head)
            return;
    }
}

// --- Remembered sets: inter-block pointer slots (P2) -----------------------
// While enabled (g_remsetActive), the barrier records slots that come to hold a
// pointer into a different Immix block than the slot's own block. Evacuation
// (P3) walks these to fix up references into a moved block; young collection
// (item D) walks them to find mature->young edges. Item F: the set is scoped
// per-collection (reset at each consuming pause / trace under STW) rather than
// persistent-with-reuse-tags -- because reclamation only happens at trace/evac
// pauses and consumers drain under the pause before any decommit, a recorded
// slot's source is always still committed when read, so stale entries are made
// impossible rather than filtered. On overflow the barrier sets g_remsetOverflow
// (completeness is safety-critical for freeing young).
struct RemsetBuffer
{
    static const size_t kCapacity = 4096;
    Object** Entries[kCapacity];
    size_t Count = 0;
    RemsetBuffer* NextRegistered = nullptr;
};
static thread_local RemsetBuffer* t_remsetBuffer = nullptr;
static RemsetBuffer* volatile g_registeredRemsetBuffers = nullptr;
static CRITICAL_SECTION g_remsetLock;
static volatile LONG g_remsetActive = 0; // logging gate: open while evacuation is enabled
// Item F: a mutator's per-thread remset buffer filled, so an inter-block store
// (a mature->young or inter-block edge) was dropped. Completeness is safety-
// critical for young collection (a missed mature->young edge would free a live
// young object). When set, the nursery consumer (item D) must NOT reclaim young
// this pause; it falls back to the mark-authoritative trace, which re-establishes
// a complete remset via the marked-object walk. Cleared when a trace runs.
static volatile LONG g_remsetOverflow = 0;
static volatile LONG g_evacActive = 0;   // STW evacuation gate (P3)

// Lock-free prepend of a remset buffer (see RegisterModifiedBuffer): the barrier
// must not take a CRITICAL_SECTION in cooperative mode or it can deadlock
// SuspendEE. The collector only traverses the registry under STW (evacuation).
static void RegisterRemsetBuffer(RemsetBuffer* nb)
{
    for (;;)
    {
        RemsetBuffer* head = g_registeredRemsetBuffers;
        nb->NextRegistered = head;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_registeredRemsetBuffers,
                                              nb, head) == head)
            return;
    }
}

// --- Concurrent SATB trace (P4) --------------------------------------------
// While a concurrent trace window is open, block reuse is suppressed so region
// indices/starts stay stable for the allocate-black snapshot, and any region
// claimed sits above g_concWatermark. g_snapUsedEnd[i] records region i's fill
// high-water at the snapshot pause; g_snapChunkCount is the region count then.
static volatile LONG g_concurrentEnabled = 0; // env LXR_CONCURRENT: use concurrent backup trace
static volatile LONG g_traceWindowOpen  = 0;  // a concurrent trace window is live
// LXR difference #2: the sweep is RC-authoritative unless the current cycle ran
// a COMPLETE trace. Complete cycles (STW backup trace, or a concurrent finish
// where CompleteClosureOverMarked ran) are mark-authoritative so dead cycles are
// reclaimed; other concurrent finishes skip the O(live-heap) closure and rely on
// RC to keep missed-live objects (parse-free AnyRCNonZeroInRange), collecting
// dead cycles only on the next complete cycle. 1 => mark-authoritative sweep.
static volatile LONG g_traceCompleteThisCycle = 0;
static std::vector<uint8_t*>* g_snapUsedEnd = nullptr; // per-region snapshot high-water
static size_t   g_snapChunkCount = 0;
static uint8_t* g_concWatermark  = nullptr;   // heap high-water at snapshot
static int g_gcThreads = 1; // P5: parallel mark worker count (env LXR_GC_THREADS)

// --- Phase watchdog: pinpoint an intermittent hang without perturbing the hot
//     path. Each phase boundary publishes a name + a QPC stamp (two word writes,
//     no I/O). A monitor thread prints the current phase if it stalls, so the
//     benchmark log names the exact stuck phase. Enabled by LXR_WATCHDOG.
static const char* volatile g_lxrPhase = "idle";
static volatile LONG64 g_lxrPhaseStamp = 0;
static volatile LONG64 g_lxrPhaseSeq = 0;
static volatile LONG g_lxrWatchdog = 0;
static LARGE_INTEGER g_lxrQpcFreq = {};
static void LXRWatchdogThreadProc(void*);
static LONG CALLBACK LXRAvVectoredHandler(EXCEPTION_POINTERS* ep);
static inline void LXRSetPhase(const char* name)
{
    // Single plain pointer store: negligible perturbation, readable post-mortem
    // from a dump (`g_lxrPhase`). A separate optional watchdog thread reads it.
    g_lxrPhase = name;
    if (g_lxrWatchdog)
    {
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        g_lxrPhaseStamp = n.QuadPart;
        g_lxrPhaseSeq++;
    }
}

// A tiny zero-count work list used by recursive decrements. In a full LXR
// this is a bounded work packet processed incrementally; here it is a simple
// growable stack (the engine is dormant, so simplicity beats scalability).
static Object** g_zeroCountStack = nullptr;
static size_t g_zeroCountTop = 0;
static size_t g_zeroCountCap = 0;

// --- Concurrent / lazy RC decrements (LXR difference #1) -------------------
// Paper-LXR replays the coalescing-RC decrements and runs the recursive free
// OFF the stop-the-world pause, on a background collector thread, so only the
// (bounded) buffer *snapshot* costs pause time. We approximate this in the
// concurrent trace path: at the snapshot STW pause we detach the per-thread
// modified buffers into g_rcSnap* (capturing (oldValue, newValue=*slot) while
// mutators are stopped, so the reads are stable), then replay the increments
// and decrements + zero-count cascade off-pause during the concurrent drain
// window. Actual memory reclamation is unchanged: it stays gated on the sweep,
// which is mark-authoritative every concurrent cycle (g_traceCompleteThisCycle),
// so even if a rare mutator resurrection races an off-pause decrement and
// transiently corrupts a reference count, no reachable object can be freed -
// the sweep frees by mark, not by RC, that cycle. Gated by LXR_CONC_DECREMENTS
// and only ever taken on the concurrent path.
static volatile LONG g_concDecrements = 0; // env LXR_CONC_DECREMENTS
struct RCSnapshotEntry { Object* OldValue; Object* NewValue; };
static RCSnapshotEntry* g_rcSnapEntries = nullptr;
static size_t g_rcSnapCount = 0;
static size_t g_rcSnapCap = 0;

// Backup-trace mark stack (grown on demand; STW so no locking needed while draining).
static Object** g_markStack = nullptr;
static size_t g_markTop = 0;
static size_t g_markCap = 0;

// P5 persistent parallel-mark worker pool. Raw std::thread workers spun up/torn
// down per drain (the previous approach) perturbed SuspendEE and hung/raced even
// in the STW path, because thread creation/teardown inside a suspension pause
// races the runtime's thread store. Instead we create N-1 runtime-registered,
// non-suspendable worker threads ONCE at Initialize (never during a pause) via
// IGCToCLR::CreateThread; they block on per-worker start events and only run
// inside the STW mark pause (all mutators parked), so they may read object memory
// safely. Coordination is one auto-reset start event + one auto-reset done event
// per worker; the current work partition lives in g_poolSlices. Only one drain
// runs at a time (collection is serialized), so this shared state needs no lock.
static int      g_poolWorkers = 0;              // persistent workers created (= gcThreads-1)
static HANDLE*  g_poolStart   = nullptr;        // [w] auto-reset: wake worker w
static HANDLE*  g_poolDone    = nullptr;        // [w] auto-reset: worker w finished its slice
static std::vector<std::vector<Object*>>* g_poolSlices = nullptr; // slice[w+1] is worker w's grey set
static class LXRCollector* g_poolCollector = nullptr;
static void LXRMarkWorkerProc(void* idx);
static void EnsureMarkWorkerPool();

static uint32_t g_pageSize = 4096;

// Heap bytes currently committed and in use (increased on commit, decreased when
// the sweep decommits dead regions). This is what GetTotalBytesInUse reports, so
// GC.GetTotalMemory() visibly drops when LXR reclaims.
static volatile int64_t g_committedInUse = 0;

// Allocation-triggered collection: run a STW LXR cycle once committed-in-use has
// grown by this many bytes since the last collection (0 disables; overridable via
// LXR_GC_TRIGGER_MB). This is what makes LXR behave like a real reclaiming GC
// under load instead of only reclaiming on an explicit GC.Collect().
static int64_t RunLXRCollection(int generation, bool forceTrace);
static void RequestLXRCollection(bool wait, bool forceTrace);
static void LXRCollectorThreadProc(void*);
static volatile int64_t g_gcTriggerBytes = -1; // -1 = uninitialized; resolved lazily
static volatile int64_t g_gcGrowthPct = 50;    // adaptive budget: % of live heap
static volatile LONG g_inCollection = 0;       // reentrancy guard for RunLXRCollection

// --- LXR phase model (P1) -------------------------------------------------
// Real LXR does frequent *light* RC pauses and only *occasionally* a full
// backup trace + sweep, pacing the cadence with a survival-rate prediction. We
// mirror that: an allocation trigger normally runs a cheap RC pause (replay the
// coalescing-RC modified buffers, no trace, no decommit); the collector escalates
// to a full TracePause (backup trace + Immix sweep, the only phase that actually
// returns memory and reclaims dead cycles) when either enough RC epochs have
// elapsed or committed memory has grown past a trace budget since the last trace.
enum class LXRPhase { RCPause, TracePause };
static volatile LONG g_requestTrace = 0;          // sticky: next cycle must be a full trace
static volatile int64_t g_epochsSinceTrace = 0;   // RC epochs since the last full trace
static volatile int64_t g_lastTraceCommitted = 0; // committed-in-use right after the last trace
static volatile int64_t g_traceBudgetBytes = -1;  // -1 = uninitialized; growth before forcing a trace
static volatile int64_t g_traceEveryEpochs = -1;  // force a trace at least every N epochs (0 = off)

// Young-object nursery (LXR difference #6). g_traceEpoch is a monotonic counter
// bumped once per completed trace cycle; every allocation region is stamped with
// its value at registration (BlockMeta.bornTraceEpoch). An object is "young" while
// bornTraceEpoch == g_traceEpoch (no trace has aged it since birth); young objects
// are excluded from reference counting and kept alive by the trace/allocate-black.
// Gated by LXR_YOUNG_RC (default off; part of the full-LXR configuration).
static volatile LONG g_youngRC = 0;
// Item D: nursery collection at RC pauses (env LXR_NURSERY). OFF by default -
// authoritative young reclamation needs a complete mature->young remembered set
// (a complete write barrier); see the CollectNursery gate and Initialize().
static volatile LONG g_nurseryActive = 0;
static volatile int64_t g_traceEpoch = 0;

// Dedicated collector thread. Driving SuspendEE from a random cooperative-mode
// allocating thread deadlocks under high concurrency (the initiator can end up
// waiting on threads that are in turn waiting on it). Real LXR runs its trace on
// its own GC threads; we do the same: a single non-suspendable GC thread owns
// every stop-the-world cycle, and allocation/GC.Collect just post a request to
// it. Because the collector thread is created with is_suspendable=false it is
// never itself a suspension target, so SuspendEE from it is deadlock-free.
static HANDLE g_collectRequestEvent = nullptr; // auto-reset: wake the collector
static HANDLE g_collectDoneEvent = nullptr;    // auto-reset: pulsed after each cycle
// Manual-reset GC-completion event + flag backing IGCHeap::WaitUntilGCComplete /
// IsGCInProgressHelper. RESET while a SuspendEE..RestartEE window is open, SET
// otherwise (initially signaled). Without this, a thread trapped in
// Thread::RareDisablePreemptiveGC calls our WaitUntilGCComplete, which (as a
// no-op stub) returned instantly, so the thread busy-toggled preemptive<->coop
// instead of blocking. Under a first-JIT/eviction thread storm 20+ threads spin
// this loop and ThreadSuspend::SuspendAllThreads never gets a clean snapshot ->
// SuspendEE livelocks forever. Blocking here lets trapped threads park until we
// RestartEE, so suspension converges.
static HANDLE g_gcCompleteEvent = nullptr;
static volatile LONG g_gcInProgress = 0;
static volatile LONG g_collectPending = 0;     // coalesces repeated alloc triggers
static volatile int64_t g_collectCompletedSeq = 0; // ++ after each completed cycle
static volatile LONG g_collectorShutdown = 0;

// ===========================================================================
//   Parseable allocation-region registry (enables sweep + interior pointers)
// ===========================================================================
// Every allocation context chunk handed to the runtime is recorded here with
// the high-water mark of bytes the runtime actually filled. Because the
// runtime bump-allocates objects contiguously within a chunk, [Start, UsedEnd)
// is a linearly parseable run of complete objects (walkable via LXRObjectSize).
// This is what lets the backup trace resolve interior pointers to their
// containing object and lets the sweep reclaim fully-dead chunks.
struct ChunkRegion
{
    uint8_t*          Start;
    uint8_t*          UsedEnd;   // finalized on retirement; == Owner->alloc_ptr while active
    size_t            Size;
    gc_alloc_context* Owner;     // non-null while a thread is bump-allocating into it
    bool              Committed;
    bool              FreeRun;    // Immix line reuse (LXR_LINE_REUSE): a plugged/dead
                                  // sub-run carved from a retained region, available to
                                  // hand back to an allocator context. Parseable as
                                  // [Start,UsedEnd); never decommitted while listed.
};
static ChunkRegion* g_chunks = nullptr;
static size_t g_chunkCount = 0;
static size_t g_chunkCap = 0;
static size_t* g_freeChunks = nullptr;   // stack of reclaimed (decommitted) chunk indices
static size_t g_freeChunkTop = 0;
static size_t g_freeChunkCap = 0;
static CRITICAL_SECTION g_chunkLock;

// Immix line reuse (LXR difference #2, LXR_LINE_REUSE): stack of region indices
// that are FreeRun == reusable dead line-runs carved from retained regions by
// the sweep. Popped by the allocator before it extends the heap watermark, so
// dead space inside otherwise-live blocks is reused instead of growing committed
// memory (the Immix "recycle partially-free blocks" property). Guarded by
// g_chunkLock (same as g_chunks). Kept empty unless line reuse is enabled.
static size_t* g_freeRuns = nullptr;
static size_t  g_freeRunTop = 0;
static size_t  g_freeRunCap = 0;
static int     g_lineReuse = -1; // env LXR_LINE_REUSE (-1 = not yet resolved)
static size_t  g_lineReuseMinBytes = 0; // env LXR_LINE_MIN (min carve/reuse size)
static volatile int64_t g_carveRunsTotal = 0;  // FreeRun segments carved (observability)
static volatile int64_t g_carveBytesTotal = 0; // bytes carved into FreeRun segments

// The runtime's free-object MethodTable (component size 1). Writing it over a
// byte range with NumComponents == size-baseSize makes that range parse as a
// single object, so a linear heap walk stays in sync after we overwrite part of
// a dead run with fresh allocations (the standard GC "plug" / unused-array).
static MethodTable* g_freeObjectMT = nullptr;
static size_t       g_freeObjectBaseSize = 0;

// Plug [start, start+size) as one free object so a linear parse treats it as a
// single sized object. Requires size >= g_freeObjectBaseSize; smaller tails are
// left as an unparsed inter-region gap by the caller. Returns true if plugged.
static bool PlugFreeRange(uint8_t* start, size_t size)
{
    if (g_freeObjectMT == nullptr || size < g_freeObjectBaseSize)
        return false;
    // Object layout: [MethodTable*][num components...]. Component size is 1, so
    // total size = baseSize + numComponents  =>  numComponents = size - baseSize.
    *((MethodTable**)start) = g_freeObjectMT;
    // ArrayBase stores the component count right after the MT pointer (m_NumComponents).
    *((uint32_t*)(start + sizeof(void*))) = (uint32_t)(size - g_freeObjectBaseSize);
    return true;
}

// Total object size in bytes, matching the runtime's Align(base + comps*compSize).
size_t LXRObjectSize(Object* o)
{
    MethodTable* mt = o->GetGCSafeMethodTable();
    // Guard against a null / not-yet-published / corrupt MethodTable. A mutator
    // suspended mid-allocation (alloc_ptr already bumped, MT not yet stored)
    // leaves a zeroed granule at its region tail; every linear parse below
    // (ResolveInterior, allocate-black, sweep, evac) would otherwise dereference
    // that null MT and AV @ 0x0. Returning 0 makes the callers' `sz == 0` guard
    // stop/skip safely, and object-scan sites treat it as a zero-reference
    // object. No real object has size 0, so 0 is an unambiguous sentinel.
    uintptr_t m = (uintptr_t)mt;
    if (m == 0 || (m & 7) != 0 || m < 0x10000ull || m > 0x00007FFFFFFFFFFFull)
        return 0;
    size_t size = mt->GetBaseSize();
    if (mt->HasComponentSize())
        size += (size_t)((ArrayBase*)o)->GetNumComponents() * mt->RawGetComponentSize();
    return (size + (sizeof(void*) - 1)) & ~(size_t)(sizeof(void*) - 1);
}

static size_t CommitRange(uint8_t* start, size_t size)
{
    uint8_t* pbeg = (uint8_t*)(((uintptr_t)start + g_pageSize - 1) & ~((uintptr_t)g_pageSize - 1));
    uint8_t* pend = (uint8_t*)(((uintptr_t)(start + size)) & ~((uintptr_t)g_pageSize - 1));
    if (pend > pbeg)
    {
        VirtualAlloc(pbeg, pend - pbeg, MEM_COMMIT, PAGE_READWRITE);
        return (size_t)(pend - pbeg);
    }
    return 0;
}

// Record a chunk's fill high-water and mark it retired (eligible for sweep).
static void FinalizeChunk(int index, uint8_t* usedEnd)
{
    if (index < 0)
        return;
    EnterCriticalSection(&g_chunkLock);
    if ((size_t)index < g_chunkCount)
    {
        g_chunks[index].UsedEnd = usedEnd;
        g_chunks[index].Owner = nullptr;
    }
    LeaveCriticalSection(&g_chunkLock);
}

// Register a freshly handed-out chunk; returns its registry index (or -1).
static int RegisterChunk(uint8_t* start, size_t size, gc_alloc_context* owner)
{
    EnterCriticalSection(&g_chunkLock);
    if (g_chunkCount == g_chunkCap)
    {
        size_t nc = g_chunkCap ? g_chunkCap * 2 : 1024;
        ChunkRegion* grown = (ChunkRegion*)realloc(g_chunks, nc * sizeof(ChunkRegion));
        if (grown == nullptr) { LeaveCriticalSection(&g_chunkLock); return -1; }
        g_chunks = grown; g_chunkCap = nc;
    }
    int idx = (int)g_chunkCount++;
    g_chunks[idx].Start = start;
    g_chunks[idx].UsedEnd = start;
    g_chunks[idx].Size = size;
    g_chunks[idx].Owner = owner;
    g_chunks[idx].Committed = true;
    g_chunks[idx].FreeRun = false;
    LeaveCriticalSection(&g_chunkLock);
    g_lxrCollector.StampBornEpoch(start, size); // #6: mark blocks as this window's nursery
    return idx;
}

// Reuse a previously reclaimed region (block reuse); returns its (zeroed,
// recommitted) start and sets *outIndex, or nullptr if the free list is empty.
static uint8_t* ReuseChunk(gc_alloc_context* owner, int* outIndex)
{
    uint8_t* start = nullptr;
    static int noReuse = -1;
    if (noReuse < 0) noReuse = (getenv("LXR_NO_REUSE") != nullptr) ? 1 : 0;
    if (noReuse)
        return nullptr;
    // While a concurrent trace window is open, do not recycle freed regions:
    // keeping region indices/starts stable lets the allocate-black snapshot match
    // regions by index, and forces mid-trace allocations above g_concWatermark.
    if (g_traceWindowOpen)
        return nullptr;
    EnterCriticalSection(&g_chunkLock);
    while (g_freeChunkTop > 0)
    {
        size_t idx = g_freeChunks[--g_freeChunkTop];
        if (idx >= g_chunkCount || g_chunks[idx].Committed)
            continue;
        ChunkRegion& c = g_chunks[idx];
        // Only recycle standard-size regions on the small-alloc fast path. A
        // reclaimed LARGE-object region (Size >> quantum) must NOT be handed out
        // for a 128 KiB small chunk: doing so leaves the registry entry's Size
        // at the multi-MB original, so the later sweep would MEM_DECOMMIT (and
        // this reuse would memset/recommit) the entire multi-MB span while only
        // the first quantum is tracked/used - the oversized decommit/zero then
        // clobbers live objects carved into the same span, corrupting the heap
        // (the growing-cache dictionary-resize AV). Leave large regions on the
        // free list for a matching large request (or decommitted).
        if (c.Size != CONTEXT_ALLOC_QUANTUM)
            continue;
        size_t recommitted = CommitRange(c.Start, c.Size); // recommit the decommitted interior
        InterlockedExchangeAdd64(&g_committedInUse, (int64_t)recommitted);
        memset(c.Start, 0, c.Size);         // hand back zeroed memory like a fresh commit
        c.Owner = owner;
        c.UsedEnd = c.Start;
        c.Committed = true;
        start = c.Start;
        *outIndex = (int)idx;
        break;
    }
    LeaveCriticalSection(&g_chunkLock);
    if (start != nullptr)
        g_lxrCollector.StampBornEpoch(start, CONTEXT_ALLOC_QUANTUM); // #6: recycled region hosts young objects
    return start;
}

// Immix line reuse: hand back a carved FreeRun region big enough for 'needBytes'
// before the allocator extends the committed heap. The run's pages are already
// committed (it held live+dead objects), so reuse avoids a fresh commit - this
// is where dead line space actually offsets heap growth. Returns the run start
// and its full size, or nullptr if none fits. Suppressed during a trace window
// (region indices/starts must stay stable for the allocate-black snapshot).
static uint8_t* ReuseFreeRun(gc_alloc_context* owner, size_t needBytes, size_t headerPad, int* outIndex, size_t* outSize)
{
    if (g_traceWindowOpen)
        return nullptr;
    uint8_t* start = nullptr;
    EnterCriticalSection(&g_chunkLock);
    // Scan from the top for the first run that fits; compact out consumed/invalid
    // entries lazily by swapping the taken slot with the top.
    for (size_t k = g_freeRunTop; k-- > 0; )
    {
        size_t idx = g_freeRuns[k];
        if (idx >= g_chunkCount || !g_chunks[idx].FreeRun || !g_chunks[idx].Committed)
        {
            g_freeRuns[k] = g_freeRuns[--g_freeRunTop];
            continue;
        }
        ChunkRegion& c = g_chunks[idx];
        if (c.Size < needBytes)
            continue;
        // Take it: remove from the free-run stack (swap with top).
        g_freeRuns[k] = g_freeRuns[--g_freeRunTop];
        memset(c.Start, 0, c.Size); // hand back zeroed memory like a fresh commit
        // Reserve headerPad before the first object (its sync-block/-8 header),
        // exactly like a fresh chunk (RegisterChunk registers the object start,
        // not the raw base), so the region parses cleanly from Start.
        c.Start   = c.Start + headerPad;
        c.Size    = c.Size - headerPad;
        c.Owner   = owner;
        c.UsedEnd = c.Start;
        c.FreeRun = false;          // now a normal allocating region
        c.Committed = true;
        start = c.Start;
        *outIndex = (int)idx;
        *outSize = c.Size;
        break;
    }
    LeaveCriticalSection(&g_chunkLock);
    return start;
}

// Append a region entry under g_chunkLock (already held by the caller). May
// realloc g_chunks. Returns the new index, or -1 on OOM.
static int AppendRegionLocked(uint8_t* start, uint8_t* usedEnd, size_t size, bool freeRun)
{
    if (g_chunkCount == g_chunkCap)
    {
        size_t nc = g_chunkCap ? g_chunkCap * 2 : 1024;
        ChunkRegion* grown = (ChunkRegion*)realloc(g_chunks, nc * sizeof(ChunkRegion));
        if (grown == nullptr) return -1;
        g_chunks = grown; g_chunkCap = nc;
    }
    int idx = (int)g_chunkCount++;
    g_chunks[idx].Start = start;
    g_chunks[idx].UsedEnd = usedEnd;
    g_chunks[idx].Size = size;
    g_chunks[idx].Owner = nullptr;
    g_chunks[idx].Committed = true;
    g_chunks[idx].FreeRun = freeRun;
    return idx;
}

// Push a FreeRun region index onto the reuse stack (under g_chunkLock).
static void PushFreeRunLocked(size_t idx)
{
    if (g_freeRunTop == g_freeRunCap)
    {
        size_t nc = g_freeRunCap ? g_freeRunCap * 2 : 256;
        size_t* grown = (size_t*)realloc(g_freeRuns, nc * sizeof(size_t));
        if (grown == nullptr) return;
        g_freeRuns = grown; g_freeRunCap = nc;
    }
    g_freeRuns[g_freeRunTop++] = idx;
}

static void CommitPageFor(uint8_t* addr)
{
    // The RC side table is reserved-only; commit the touched page lazily so
    // that a dormant-engine call never faults on reserved memory.
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t page = (uintptr_t)addr & ~((uintptr_t)si.dwPageSize - 1);
    VirtualAlloc((void*)page, si.dwPageSize, MEM_COMMIT, PAGE_READWRITE);
}

bool LXRCollector::Initialize(uint8_t* heapBase, size_t heapReservedBytes)
{
    m_heapBase = heapBase;
    m_heapBytes = heapReservedBytes;

    SYSTEM_INFO si; GetSystemInfo(&si);
    g_pageSize = si.dwPageSize;

    // RC side table: 1 saturating byte per 8-byte granule. Reserve only
    // (virtual); pages are committed on first touch (LXR-style side metadata).
    size_t rcTableBytes = heapReservedBytes / lxr::kObjectGranule;
    m_rcTable = (uint8_t*)VirtualAlloc(nullptr, rcTableBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_rcTable == nullptr)
        return false;

    // Mark side table for the backup trace: 1 bit per 8-byte granule. Reserved
    // only; committed on touch and decommitted wholesale after each trace to
    // reset every bit to zero.
    size_t markTableBytes = (heapReservedBytes / lxr::kObjectGranule + 7) / 8;
    m_markTable = (uint8_t*)VirtualAlloc(nullptr, markTableBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_markTable == nullptr)
        return false;

    // Coalescing "unlogged bit" side table (paper A(ii)): 1 bit per 8-byte field
    // slot. Reserved only; the covering pages are committed on the allocation path
    // (EnsureLoggedUpTo), never from the cooperative-mode barrier (which cannot
    // safely VirtualAlloc). Bits are cleared per consumed field at each RC pause
    // (and wholesale on an overflow epoch). Same geometry as the mark table (slots
    // are 8-byte aligned on amd64).
    size_t loggedTableBytes = (heapReservedBytes / lxr::kObjectGranule + 7) / 8;
    m_loggedTable = (uint8_t*)VirtualAlloc(nullptr, loggedTableBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_loggedTable == nullptr)
        return false;

    // Per-block metadata: one entry per 32 KiB block. Small enough to commit.
    m_blockCount = heapReservedBytes / lxr::kBlockSize;
    size_t metaBytes = m_blockCount * sizeof(lxr::BlockMeta);
    m_blockMeta = (lxr::BlockMeta*)VirtualAlloc(nullptr, metaBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_blockMeta == nullptr)
        return false;

    // Immix line-mark side table: 1 bit per 256 B line. Reserved only; committed
    // and zeroed per trace over the used-heap prefix (see ResetMarks). Enables
    // O(lines) free-line-run discovery for line reuse (LXR_LINE_REUSE).
    size_t lineTableBytes = (heapReservedBytes / lxr::kLineSize + 7) / 8;
    m_lineMarkTable = (uint8_t*)VirtualAlloc(nullptr, lineTableBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_lineMarkTable == nullptr)
        return false;

    InitializeCriticalSection(&m_collectLock);
    InitializeCriticalSection(&g_buffersLock);
    InitializeCriticalSection(&g_satbLock);
    InitializeCriticalSection(&g_remsetLock);
    InitializeCriticalSection(&g_chunkLock);

    // P2 barrier-extension gates. Off by default (zero barrier overhead); SATB is
    // driven by the concurrent trace (P4) and remsets by evacuation (P3). Env
    // knobs let the STW path exercise them now: LXR_SATB=1, LXR_REMSET=1.
    if (getenv("LXR_SATB") != nullptr)   g_satbActive = 1;
    if (getenv("LXR_REMSET") != nullptr) g_remsetActive = 1;
    if (getenv("LXR_EVAC") != nullptr)   g_evacActive = 1;
    if (getenv("LXR_FAULT_DIAG") != nullptr) AddVectoredExceptionHandler(1, &LXRFaultDiag);
    // P4: run the backup trace concurrently with the mutators (two brief STW
    // pauses + off-pause marking). The SATB window is opened per-trace, so
    // g_satbActive is NOT forced on here.
    if (getenv("LXR_CONCURRENT") != nullptr) g_concurrentEnabled = 1;
    // #1: replay coalescing-RC decrements + the recursive free OFF the STW
    // pause (concurrent path only). Only the bounded buffer snapshot is paused.
    if (getenv("LXR_CONC_DECREMENTS") != nullptr) g_concDecrements = 1;
    // #6: decouple young objects from reference counting (nursery). Young liveness
    // is decided by the trace/allocate-black; sound because reclamation is
    // mark-authoritative every trace cycle.
    if (getenv("LXR_YOUNG_RC") != nullptr) g_youngRC = 1;
    // Item D: young/nursery collection at RC pauses (CollectNursery). Opt-in and
    // OFF by default because reclaiming young authoritatively at an RC pause
    // requires a COMPLETE mature->young remembered set, which in turn needs a
    // write barrier that captures 100% of reference stores. Our pluggable Callback
    // barrier has residual completeness gaps (some store forms bypass it; the RC/
    // SATB paths tolerate this via the from-roots trace backstop, but the nursery
    // cannot) - proven by LXR_NURSERY_FULLSCAN (an O(heap) complete-seed mode) being
    // AV-free while the remset-only mode faults. LXR_NURSERY enables the authoritative
    // (remset) mode; add LXR_NURSERY_FULLSCAN for the sound-but-O(heap) mode.
    if (getenv("LXR_NURSERY") != nullptr) g_nurseryActive = 1;
    // P5: parallel mark worker count. Clamped to [1, 64]; 1 keeps the verified
    // single-threaded closure.
    if (const char* t = getenv("LXR_GC_THREADS"))
    {
        int n = atoi(t);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        g_gcThreads = n;
    }
    return true;
}

lxr::BlockMeta* LXRCollector::MetaForBlock(uint8_t* blockAddr)
{
    size_t idx = (size_t)(blockAddr - m_heapBase) / lxr::kBlockSize;
    if (idx >= m_blockCount)
        return nullptr;
    CommitPageFor((uint8_t*)&m_blockMeta[idx]);
    return &m_blockMeta[idx];
}

// #6 young-object nursery. Stamp every 32 KiB block spanned by a freshly
// (re)registered allocation region with the current inter-trace window id, so
// objects born in the region are recognised as young until the next trace ages
// them. Bounded (one iteration per block in the region). No-op unless enabled.
void LXRCollector::StampBornEpoch(uint8_t* start, size_t size)
{
    if (!g_youngRC || start < m_heapBase)
        return;
    int64_t epoch = g_traceEpoch;
    uint8_t* blk = (uint8_t*)((uintptr_t)start & ~(lxr::kBlockSize - 1));
    uint8_t* end = start + size;
    for (; blk < end; blk += lxr::kBlockSize)
    {
        lxr::BlockMeta* meta = MetaForBlock(blk);
        if (meta != nullptr)
            meta->bornTraceEpoch = epoch;
    }
}

bool LXRCollector::IsYoung(Object* obj)
{
    if (!g_youngRC || (uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return false;
    uint8_t* blk = (uint8_t*)((uintptr_t)obj & ~(lxr::kBlockSize - 1));
    lxr::BlockMeta* meta = MetaForBlock(blk);
    return meta != nullptr && meta->bornTraceEpoch == g_traceEpoch;
}

uint8_t* LXRCollector::RCSlot(Object* obj) const
{
    size_t idx = ((uint8_t*)obj - m_heapBase) / lxr::kObjectGranule;
    return &m_rcTable[idx];
}

void LXRCollector::RCIncrement(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return; // not our heap (frozen segment, boot object, etc.)
    if (IsYoung(obj))
        return; // #6: young objects are not reference-counted (nursery)
    uint8_t* slot = RCSlot(obj);
    CommitPageFor(slot);
    if (*slot != 0xFF) // 0xFF is the "stuck / overflowed" sentinel
        (*slot)++;
    InterlockedIncrement64(&g_lxrCounters.RCIncrements);
}

bool LXRCollector::RCDecrement(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return false;
    if (IsYoung(obj))
        return false; // #6: young objects are not reference-counted (nursery)
    uint8_t* slot = RCSlot(obj);
    CommitPageFor(slot);
    InterlockedIncrement64(&g_lxrCounters.RCDecrements);
    if (*slot == 0 || *slot == 0xFF)
        return false; // already zero, or stuck-high (resolved by backup trace)
    (*slot)--;
    return (*slot == 0);
}

// Provision this thread's write-barrier buffers OFF the barrier, on the
// allocation path (AllocateSlow) where the thread holds a proper frame and
// allocation is safe w.r.t. SuspendEE. Doing the malloc here (never in the
// leaf barrier) is what makes the barrier non-allocating and deadlock-free:
// a mutator can never be frozen inside malloc while cooperative in the barrier.
// Every thread that stores a heap reference must first obtain one to store,
// which requires an allocation, so buffers are ready before any store logs.
static void EnsureThreadBuffers()
{
    if (t_modifiedBuffer == nullptr)
    {
        ModifiedBuffer* mb = new (std::nothrow) ModifiedBuffer();
        if (mb != nullptr) { t_modifiedBuffer = mb; RegisterModifiedBuffer(mb); }
    }
    if (t_satbBuffer == nullptr)
    {
        SatbBuffer* sb = new (std::nothrow) SatbBuffer();
        if (sb != nullptr) { t_satbBuffer = sb; RegisterSatbBuffer(sb); }
    }
    if (t_remsetBuffer == nullptr)
    {
        RemsetBuffer* rb = new (std::nothrow) RemsetBuffer();
        if (rb != nullptr) { t_remsetBuffer = rb; RegisterRemsetBuffer(rb); }
    }
}

// Extend the committed logged-table prefix to cover [heapBase, addrEnd). Called
// OFF the barrier, on the allocation/heap-commit path (a proper frame where
// VirtualAlloc is safe), so the barrier itself never has to commit a reserved
// bitmap page - doing so from the cooperative-mode barrier could deadlock
// SuspendEE (VirtualAlloc takes the address-space lock). Because objects are
// always allocated before their fields are stored, every field slot's logged
// page is committed here before any barrier can touch it. Fresh MEM_COMMIT pages
// are zero-filled by the OS (bit clear == "unlogged"), so no memset is needed.
void LXRCollector::EnsureLoggedUpTo(uint8_t* addrEnd)
{
    if (m_loggedTable == nullptr || addrEnd <= m_heapBase)
        return;
    size_t usedBytes = (size_t)(addrEnd - m_heapBase);
    size_t neededBytes = (usedBytes / lxr::kObjectGranule + 7) / 8;
    neededBytes = (neededBytes + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
    size_t cap = (m_heapBytes / lxr::kObjectGranule + 7) / 8;
    if (neededBytes > cap)
        neededBytes = cap;
    if (neededBytes > m_loggedCommittedBytes)
    {
        uint8_t* from = m_loggedTable + m_loggedCommittedBytes;
        size_t delta = neededBytes - m_loggedCommittedBytes;
        if (VirtualAlloc(from, delta, MEM_COMMIT, PAGE_READWRITE) != nullptr)
            m_loggedCommittedBytes = neededBytes; // publish AFTER the commit succeeds
    }
}

// Unlogged-bit test-and-set (paper §3.4). Returns true the FIRST time a field is
// stored this epoch (so the barrier logs it exactly once), false thereafter.
// Off-heap slots always log (no coalescing metadata). If the covering bitmap page
// is not yet committed (a store racing ahead of the allocation-path commit -
// vanishingly rare), we log without coalescing rather than fault or commit here.
// Hot path: one monotonic-watermark compare + one word read + at most one atomic.
bool LXRCollector::TryFirstLogField(Object** slot)
{
    uint8_t* p = (uint8_t*)slot;
    if (p < m_heapBase || p >= m_heapBase + m_heapBytes)
        return true;
    size_t granule = (size_t)(p - m_heapBase) / lxr::kObjectGranule;
    size_t byteOff = granule >> 3;
    if (byteOff >= m_loggedCommittedBytes)
        return true; // logged-table page not committed yet: log uncoalesced (sound)
    LONG* word = (LONG*)m_loggedTable + (granule >> 5);
    LONG mask = (LONG)(1u << (granule & 31));
    if (*(volatile LONG*)word & mask)
        return false; // already logged this epoch
    return (InterlockedOr(word, mask) & mask) == 0; // win iff we flipped 0->1
}

// Clear a single field's logged bit as its modified-buffer entry is consumed at
// the RC pause (O(modified fields)). Non-atomic: called only under STW.
void LXRCollector::ClearLoggedBit(Object** slot)
{
    uint8_t* p = (uint8_t*)slot;
    if (p < m_heapBase || p >= m_heapBase + m_heapBytes)
        return;
    size_t granule = (size_t)(p - m_heapBase) / lxr::kObjectGranule;
    size_t byteOff = granule >> 3;
    if (byteOff >= m_loggedCommittedBytes)
        return;
    LONG* word = (LONG*)m_loggedTable + (granule >> 5);
    *word &= ~(LONG)(1u << (granule & 31));
}

// Wholesale-clear the entire committed logged-table prefix. Used on a
// buffer-overflow epoch, where some set bits have no consumable buffer entry, so
// per-field ClearLoggedBit cannot restore the set-bits==buffered-fields invariant.
// STW only (memset is non-atomic).
void LXRCollector::ResetLoggedTable()
{
    if (m_loggedTable != nullptr && m_loggedCommittedBytes > 0)
        memset(m_loggedTable, 0, m_loggedCommittedBytes);
}

// LXR write-barrier slow path. Reached from the runtime's generic Callback
// write barrier (WriteBarrierKind::Callback) on every in-heap reference-field
// store, carrying the OLD value the runtime just overwrote.
//
// CRITICAL: this runs in cooperative GC mode as a leaf helper the runtime cannot
// hijack while it is blocked. It must therefore NEVER allocate or take a blocking
// lock - a mutator stalled here (e.g. inside malloc holding the CRT heap lock, or
// on a CRITICAL_SECTION whose holder SuspendEE has frozen) can never reach a
// safepoint, hanging SuspendEE forever. All per-thread buffers are provisioned
// off the barrier by EnsureThreadBuffers() on the allocation path (a proper
// frame). If a buffer is somehow still absent (a store before this thread's first
// allocation - vanishingly rare), we take the sound fallback: drop the entry and
// force this cycle's trace to re-close from roots, rather than allocate here.
void LXRCollector::LogModifiedField(Object** slot, Object* oldValue, Object* newValue)
{
    // Coalescing unlogged bit (paper §3.4): log each field at most once per epoch.
    // The FIRST store this epoch captures the epoch-start referent (oldValue) for
    // both the RC decrement and the SATB snapshot; later stores to the same field
    // are elided - their intermediate old values net to zero for RC (processing
    // re-reads the slot's final value) and are post-snapshot for SATB. The bit is
    // cleared per consumed field at the RC pause. An epoch begins AT an RC pause,
    // which is also where a trace snapshot is taken, so the first post-pause
    // overwrite of a field yields exactly its snapshot-time value (Yuasa-correct).
    bool firstLog = TryFirstLogField(slot);

    // (1) Coalescing-RC modified buffer: record (slot, oldValue) on the first
    //     mutation of this field this epoch.
    if (firstLog)
    {
        ModifiedBuffer* buf = t_modifiedBuffer;
        if (buf != nullptr && buf->Count < ModifiedBuffer::kCapacity)
        {
            buf->Entries[buf->Count].Slot = slot;
            buf->Entries[buf->Count].OldValue = oldValue;
            buf->Count++;
            InterlockedIncrement64(&g_lxrCounters.ModifiedBufferEntries);
        }
        else
        {
            // First-log entry dropped (buffer absent/full): a logged bit is now
            // set with no matching buffer entry, breaking the
            // set-bits==buffered-fields invariant. Force a wholesale logged-table
            // reset (ResetLoggedTable) + the sound full closure this epoch at the
            // pause. Buffer-absent is vanishingly rare (a store before this
            // thread's first allocation).
            InterlockedExchange(&g_modifiedOverflow, 1);
        }
    }
    // A production LXR flushes a full buffer into a shared queue; omitted here.
    // Once full we simply stop recording further entries this epoch.

    // (2) SATB deletion barrier (Yuasa): while a trace window is open, retain the
    //     overwritten referent so a concurrent marker (P4) cannot miss an object
    //     unlinked mid-trace. Gated on firstLog so the retained value is the
    //     snapshot-time referent (the epoch/trace starts at the RC pause).
    //     Over-retention for one cycle is always safe; dropping an entry is not -
    //     so on absence/overflow we set g_satbOverflow and the STW finish pause
    //     falls back to a full, sound from-roots closure (ConcurrentTraceFinish).
    if (firstLog && g_satbActive && oldValue != nullptr && InHeap(oldValue))
    {
        SatbBuffer* sb = t_satbBuffer;
        if (sb != nullptr && sb->Count < SatbBuffer::kCapacity)
        {
            sb->Entries[sb->Count++] = oldValue;
            InterlockedIncrement64(&g_lxrCounters.SatbEntries);
        }
        else
        {
            // Buffer absent or full: record that a snapshot entry was lost so the
            // finish pause re-traces from roots.
            InterlockedExchange(&g_satbOverflow, 1);
        }
    }

    // (3) Remembered set: record slots that now hold an inter-block pointer, so
    //     evacuation (P3) can locate and rewrite references into a moved block.
    //     NOT coalesced on firstLog: a field's remset membership depends on the
    //     NEW value's block, which changes store-to-store, so a later inter-block
    //     store to an already-logged field must still be recorded. A dropped entry
    //     is safe: Evacuate() also forwards the fields of every marked object, so
    //     remsets are an optimization, not the sole fix-up path.
    if (g_remsetActive && newValue != nullptr && InHeap(newValue) && InHeap((Object*)slot))
    {
        uintptr_t sblk = (uintptr_t)slot     & ~(lxr::kBlockSize - 1);
        uintptr_t tblk = (uintptr_t)newValue & ~(lxr::kBlockSize - 1);
        if (sblk != tblk)
        {
            RemsetBuffer* rb = t_remsetBuffer;
            if (rb != nullptr && rb->Count < RemsetBuffer::kCapacity)
            {
                rb->Entries[rb->Count++] = slot;
                InterlockedIncrement64(&g_lxrCounters.RemsetEntries);
            }
            else
            {
                // Dropped an inter-block edge: mark the remset incomplete so the
                // young collector falls back to the authoritative trace this cycle.
                InterlockedExchange(&g_remsetOverflow, 1);
            }
        }
    }
}

void LXRCollector::SetSatbActive(bool active) { InterlockedExchange(&g_satbActive, active ? 1 : 0); }
bool LXRCollector::IsSatbActive() const { return g_satbActive != 0; }

// Mark every not-yet-drained SATB-logged referent (over-retention within a cycle
// is safe) and advance each buffer's drain cursor. Concurrency-safe: a mutator
// only appends (writes Count); the collector only advances Drained. Both are
// single-word, single-writer, so no mutator-side lock is needed. The final drain
// under the STW finish pause guarantees no residual entry is missed.
void LXRCollector::DrainSatbBuffers()
{
    EnterCriticalSection(&g_satbLock);
    for (SatbBuffer* sb = g_registeredSatbBuffers; sb != nullptr; sb = sb->NextRegistered)
    {
        size_t count = sb->Count; // snapshot the mutator's append cursor
        for (size_t i = sb->Drained; i < count; i++)
        {
            Object* o = sb->Entries[i];
            if (o != nullptr)
            {
                PushMark(o);
                InterlockedIncrement64(&g_lxrCounters.SatbMarks);
            }
        }
        sb->Drained = count;
    }
    LeaveCriticalSection(&g_satbLock);
}

// Clear the SATB buffers and drain cursors at the end of a trace. Must be called
// under STW (the finish pause) so no mutator is concurrently appending.
void LXRCollector::ResetSatbBuffers()
{
    EnterCriticalSection(&g_satbLock);
    for (SatbBuffer* sb = g_registeredSatbBuffers; sb != nullptr; sb = sb->NextRegistered)
    {
        sb->Count = 0;
        sb->Drained = 0;
    }
    InterlockedExchange(&g_satbOverflow, 0);
    LeaveCriticalSection(&g_satbLock);
}

void LXRCollector::SetRemsetActive(bool active) { InterlockedExchange(&g_remsetActive, active ? 1 : 0); }
bool LXRCollector::IsRemsetActive() const { return g_remsetActive != 0; }

void LXRCollector::EnumerateRemsetSlots(void (*visit)(Object** slot, void* ctx), void* ctx)
{
    EnterCriticalSection(&g_remsetLock);
    for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
        for (size_t i = 0; i < rb->Count; i++)
            visit(rb->Entries[i], ctx);
    LeaveCriticalSection(&g_remsetLock);
}

// Item F: reset the remembered set at the end of a collection that consumed it
// (a young/nursery pause that reclaimed young, or any complete trace). Our remset
// is scoped per-collection rather than persistent-with-reuse-tags (the paper's
// mechanism): because reclamation/decommit only happens at trace/evac pauses and
// the consumer drains the remset at the STW pause *before* any decommit, a slot's
// source is always still committed when read, so stale entries are made
// impossible instead of filtered. Clearing the overflow flag here restores
// completeness for the next window.
void LXRCollector::ResetRemsets()
{
    EnterCriticalSection(&g_remsetLock);
    for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
        rb->Count = 0;
    LeaveCriticalSection(&g_remsetLock);
    InterlockedExchange(&g_remsetOverflow, 0);
}

void LXRCollector::EnqueueZeroCount(Object* obj)
{
    if (g_zeroCountTop == g_zeroCountCap)
    {
        size_t newCap = g_zeroCountCap ? g_zeroCountCap * 2 : 1024;
        Object** grown = (Object**)realloc(g_zeroCountStack, newCap * sizeof(Object*));
        if (grown == nullptr)
            return;
        g_zeroCountStack = grown;
        g_zeroCountCap = newCap;
    }
    g_zeroCountStack[g_zeroCountTop++] = obj;
}

void LXRCollector::DrainZeroCountWorkList()
{
    // For each object that reached RC 0, its outgoing references lose a
    // reference; decrement them and cascade. The reference fields are located
    // with the generic runtime object-scanning facility (GCScanObjectRefs),
    // exactly matching the runtime's own go_through_object walk.
    //
    // RC vs trace authority: an object's RC can reach zero here for an object the
    // mark-authoritative backup trace already reclaimed (decommitted) in a prior
    // cycle - RC is advisory between traces, so the RC side table can still hold
    // a stale count (or a wild pointer can index a coincidentally-live RC slot).
    // Dereferencing such a pointer reads a decommitted page and faults. Before
    // touching any object memory, confirm the address is still committed; if not,
    // its region is gone and there is nothing to recurse into. A one-entry
    // VirtualQuery cache keeps this cheap (successive pops usually hit the same
    // committed region); this runs on the STW collection path, not the barrier.
    uint8_t* cacheBase = nullptr; size_t cacheLen = 0; bool cacheCommitted = false;
    while (g_zeroCountTop > 0)
    {
        Object* dead = g_zeroCountStack[--g_zeroCountTop];
        uint8_t* a = (uint8_t*)dead;
        if (a < m_heapBase || a >= m_heapBase + m_heapBytes)
            continue; // not our heap
        if (a < cacheBase || a >= cacheBase + cacheLen)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(a, &mbi, sizeof(mbi)) == 0)
                continue;
            cacheBase = (uint8_t*)mbi.BaseAddress;
            cacheLen = mbi.RegionSize;
            cacheCommitted = (mbi.State == MEM_COMMIT);
        }
        if (!cacheCommitted)
            continue; // region decommitted (already reclaimed by the trace)
        // A stale/half-initialized referent (null or corrupt MethodTable) must
        // not be dereferenced by the object scan; LXRObjectSize returns 0 for
        // such granules. Skip it - its region was reclaimed or it is not yet a
        // real object, so there is nothing to recurse into.
        size_t sz = LXRObjectSize(dead);
        if (sz == 0)
            continue;
        uint8_t* blk = (uint8_t*)((uintptr_t)dead & ~(lxr::kBlockSize - 1));
        lxr::BlockMeta* meta = MetaForBlock(blk);
        if (meta != nullptr && meta->liveObjects > 0)
            meta->liveObjects--;

        // Recursive decrement: dropping 'dead' releases one reference from each
        // object it points at. Any referent that hits zero cascades.
        GCScanObjectRefs(dead, sz, [this](Object** ref)
        {
            Object* child = *ref;
            if (child != nullptr && RCDecrement(child))
                EnqueueZeroCount(child);
        });
    }
}

// --- Backup-trace mark primitives -----------------------------------------

bool LXRCollector::IsMarked(Object* obj) const
{
    if (!InHeap(obj))
        return true; // treat off-heap as permanently live (never reclaimed)
    size_t granule = ((uint8_t*)obj - m_heapBase) / lxr::kObjectGranule;
    size_t byteIdx = granule >> 3;
    if (byteIdx >= m_markCommittedBytes)
        return false; // beyond the committed/zeroed range -> unmarked (dead)
    uint8_t* byte = &m_markTable[byteIdx];
    return (*byte & (uint8_t)(1u << (granule & 7))) != 0;
}

bool LXRCollector::MarkObject(Object* obj)
{
    if (!InHeap(obj))
        return false;
    size_t granule = ((uint8_t*)obj - m_heapBase) / lxr::kObjectGranule;
    size_t byteIdx = granule >> 3;
    if (byteIdx >= m_markCommittedBytes)
    {
        // Live, in-heap object above the mark table's committed high-water; it
        // cannot be marked. EnsureMarkCommitted covers the whole used heap
        // before each trace, so this is not expected to fire.
        return false;
    }
    char bit = (char)(1u << (granule & 7));
    // Atomic set so parallel mark workers (P5) never lose a bit racing on the
    // same byte; the returned previous value tells us if WE were the marker.
    char prev = _InterlockedOr8((volatile char*)&m_markTable[byteIdx], bit);
    return (prev & bit) == 0;
}

// Line-marks-populated-this-cycle flag. Only when true (a full mark that ran the
// line-marking scan sites) AND on a mark-authoritative cycle may the sweep carve
// free line runs; otherwise unmarked-but-live (RC-kept) objects could sit in an
// apparently-free line and be reused. Set by ResetMarks (arming) + drain sites.
static volatile LONG g_lineMarksValid = 0;
// Set whenever ConservativelyKeepAliveInterior fires during a cycle. Such a
// keep-alive marks only the interior's single granule (the object bounds are
// unknown), so the object's body lines are neither line-marked nor mark-bit-set
// and cannot be excluded from a free-line run. Disable line carving for the whole
// cycle when this happens — conservative keep-alive is a rare unresolved-interior
// fallback, so the footprint cost of skipping carve is negligible and correctness
// is guaranteed. Reset at ResetMarks.
static volatile LONG g_conservativeKeepAliveThisCycle = 0;

void LXRCollector::MarkLines(Object* obj, size_t size)
{
    if (m_lineMarkTable == nullptr || !InHeap(obj) || size == 0)
        return;
    uint8_t* p = (uint8_t*)obj;
    uint8_t* end = p + size;
    if (end > m_heapBase + m_heapBytes)
        end = m_heapBase + m_heapBytes;
    size_t firstLine = (size_t)(p - m_heapBase) / lxr::kLineSize;
    size_t lastLine  = (size_t)(end - 1 - m_heapBase) / lxr::kLineSize;
    for (size_t line = firstLine; line <= lastLine; line++)
    {
        size_t byteIdx = line >> 3;
        if (byteIdx >= m_lineMarkCommittedBytes)
            break; // beyond the committed line-table prefix (shouldn't happen)
        _InterlockedOr8((volatile char*)&m_lineMarkTable[byteIdx], (char)(1u << (line & 7)));
    }
}

uint8_t* LXRCollector::FirstMarkedAtOrAfter(uint8_t* from, uint8_t* end) const
{
    if (from < m_heapBase) from = m_heapBase;
    if (end <= from) return end;
    size_t g = (size_t)(from - m_heapBase) / lxr::kObjectGranule;
    size_t gEnd = (size_t)(end - m_heapBase) / lxr::kObjectGranule;
    size_t maxG = m_markCommittedBytes * 8;
    if (gEnd > maxG) gEnd = maxG;
    for (; g < gEnd; g++)
    {
        if (m_markTable[g >> 3] & (uint8_t)(1u << (g & 7)))
            return m_heapBase + g * lxr::kObjectGranule;
    }
    return end;
}

void LXRCollector::CarveFreeRuns(size_t i)
{
    // Caller holds g_chunkLock and has verified g_chunks[i] is a committed,
    // retired (Owner==null), non-FreeRun region that DID contain live objects.
    uint8_t* Start = g_chunks[i].Start;
    uint8_t* End   = g_chunks[i].UsedEnd;
    if (m_lineMarkTable == nullptr || End <= Start)
        return;
    size_t minRun = g_lineReuseMinBytes ? g_lineReuseMinBytes : (4 * lxr::kLineSize);

    // Enumerate maximal free-line runs fully inside [Start, End) using ONLY the
    // line-mark bitmap, then snap each run's end forward to the first live object
    // start (mark bit) so the following live segment begins on a real object
    // boundary and stays linearly parseable. Collect the resulting (plugStart,
    // plugEnd) free segments in order; they never overlap (a run's snapped end is
    // < the next run's line-aligned start, which is unmarked).
    uint8_t* firstLineAddr = (uint8_t*)(((uintptr_t)Start + lxr::kLineSize - 1) & ~((uintptr_t)lxr::kLineSize - 1));
    struct Seg { uint8_t* a; uint8_t* b; };
    Seg segs[512];
    int nseg = 0;
    uint8_t* runStart = nullptr;
    for (uint8_t* la = firstLineAddr; la + lxr::kLineSize <= End; la += lxr::kLineSize)
    {
        size_t line = (size_t)(la - m_heapBase) / lxr::kLineSize;
        size_t byteIdx = line >> 3;
        bool freeLine = (byteIdx >= m_lineMarkCommittedBytes) ||
                        ((m_lineMarkTable[byteIdx] & (uint8_t)(1u << (line & 7))) == 0);
        if (freeLine)
        {
            if (runStart == nullptr) runStart = la;
        }
        else if (runStart != nullptr)
        {
            uint8_t* plugEnd = FirstMarkedAtOrAfter(la, End);
            // Mark table is authoritative: only carve if NO mark bit falls in the
            // run. Line marks can miss an object that was mark-bit-set without a
            // MarkLines (e.g. a conservatively kept unresolved-interior target);
            // such an object is live and must never be reclaimed. This parse-free
            // bitmap check makes the carve sound regardless of line-mark coverage.
            if ((size_t)(plugEnd - runStart) >= minRun && !AnyMarkedInRange(runStart, plugEnd) &&
                nseg < (int)(sizeof(segs)/sizeof(segs[0])))
                segs[nseg++] = { runStart, plugEnd };
            runStart = nullptr;
        }
    }
    if (runStart != nullptr)
    {
        // Trailing free run extends to End (nothing marked after it).
        if ((size_t)(End - runStart) >= minRun && !AnyMarkedInRange(runStart, End) &&
            nseg < (int)(sizeof(segs)/sizeof(segs[0])))
            segs[nseg++] = { runStart, End };
    }
    if (nseg == 0)
        return;

    // Re-tile [Start, End) into ordered sub-regions: live segments interleaved
    // with the carved free segments. Region i is rewritten to the first
    // sub-region; the rest are appended. Plug each free segment as one free
    // object so any incidental linear parse of it stays valid, and list it for
    // reuse.
    bool firstWritten = false;
    uint8_t* cursor = Start;
    for (int s = 0; s < nseg; s++)
    {
        uint8_t* fa = segs[s].a;
        uint8_t* fb = segs[s].b;
        if (cursor < fa) // live segment before this free run
        {
            if (!firstWritten)
            {
                g_chunks[i].Start = cursor; g_chunks[i].UsedEnd = fa;
                g_chunks[i].Size = (size_t)(fa - cursor); g_chunks[i].Owner = nullptr;
                g_chunks[i].Committed = true; g_chunks[i].FreeRun = false;
                firstWritten = true;
            }
            else AppendRegionLocked(cursor, fa, (size_t)(fa - cursor), /*freeRun*/ false);
        }
        // free segment
        PlugFreeRange(fa, (size_t)(fb - fa));
        int fidx;
        if (!firstWritten)
        {
            g_chunks[i].Start = fa; g_chunks[i].UsedEnd = fb;
            g_chunks[i].Size = (size_t)(fb - fa); g_chunks[i].Owner = nullptr;
            g_chunks[i].Committed = true; g_chunks[i].FreeRun = true;
            firstWritten = true;
            fidx = (int)i;
        }
        else fidx = AppendRegionLocked(fa, fb, (size_t)(fb - fa), /*freeRun*/ true);
        if (fidx >= 0) PushFreeRunLocked((size_t)fidx);
        InterlockedIncrement64(&g_carveRunsTotal);
        InterlockedExchangeAdd64(&g_carveBytesTotal, (int64_t)(fb - fa));
        cursor = fb;
    }
    if (cursor < End) // trailing live segment
    {
        if (!firstWritten)
        {
            g_chunks[i].Start = cursor; g_chunks[i].UsedEnd = End;
            g_chunks[i].Size = (size_t)(End - cursor); g_chunks[i].FreeRun = false;
            firstWritten = true;
        }
        else AppendRegionLocked(cursor, End, (size_t)(End - cursor), /*freeRun*/ false);
    }
}

bool LXRCollector::AnyMarkedInRange(uint8_t* start, uint8_t* end) const
{
    if (start < m_heapBase) start = m_heapBase;
    if (end <= start) return false;
    size_t gStart = (size_t)(start - m_heapBase) / lxr::kObjectGranule;
    size_t gEnd   = (size_t)(end - m_heapBase + lxr::kObjectGranule - 1) / lxr::kObjectGranule;
    size_t maxGranule = m_markCommittedBytes * 8;
    if (gEnd > maxGranule) gEnd = maxGranule;
    for (size_t byteIdx = gStart >> 3; byteIdx < ((gEnd + 7) >> 3); byteIdx++)
    {
        uint8_t bits = m_markTable[byteIdx];
        if (bits == 0) continue;
        for (int b = 0; b < 8; b++)
        {
            size_t g = (byteIdx << 3) + (size_t)b;
            if (g < gStart || g >= gEnd) continue;
            if (bits & (1u << b)) return true;
        }
    }
    return false;
}

// RC-authoritative counterpart of AnyMarkedInRange. The RC table is one
// saturating byte per 8-byte granule, reserved-only and committed lazily on
// first RC set. An uncommitted page therefore means "no granule in that page
// ever had a non-zero RC" -> treat as all-zero (skip it, don't fault). Within a
// committed page any non-zero byte is a live (RC>=1) object start.
bool LXRCollector::AnyRCNonZeroInRange(uint8_t* start, uint8_t* end) const
{
    if (start < m_heapBase) start = m_heapBase;
    if (end <= start) return false;
    if (end > m_heapBase + m_heapBytes) end = m_heapBase + m_heapBytes;
    size_t gStart = (size_t)(start - m_heapBase) / lxr::kObjectGranule;
    size_t gEnd   = (size_t)(end - m_heapBase + lxr::kObjectGranule - 1) / lxr::kObjectGranule;
    uint8_t* rcStart = m_rcTable + gStart;                 // one byte per granule
    uint8_t* rcEnd   = m_rcTable + gEnd;
    uintptr_t pageMask = (uintptr_t)g_pageSize - 1;
    uint8_t* p = (uint8_t*)((uintptr_t)rcStart & ~pageMask);
    while (p < rcEnd)
    {
        uint8_t* pageEnd = p + g_pageSize;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        {
            p = pageEnd; // reserved/uncommitted -> all RC zero here
            continue;
        }
        uint8_t* scanFrom = (p < rcStart) ? rcStart : p;
        uint8_t* scanTo   = (pageEnd < rcEnd) ? pageEnd : rcEnd;
        for (uint8_t* q = scanFrom; q < scanTo; q++)
            if (*q != 0)
                return true;
        p = pageEnd;
    }
    return false;
}

// Zero the RC side-table bytes for [start,end) when the region is reclaimed.
// Symmetric to AnyRCNonZeroInRange: walk the RC-table pages covering the range,
// skip uncommitted pages (already all-zero), and memset the committed portion so
// no stale reference count survives into a decommitted region.
void LXRCollector::ClearRCRange(uint8_t* start, uint8_t* end)
{
    if (start < m_heapBase) start = m_heapBase;
    if (end <= start) return;
    if (end > m_heapBase + m_heapBytes) end = m_heapBase + m_heapBytes;
    size_t gStart = (size_t)(start - m_heapBase) / lxr::kObjectGranule;
    size_t gEnd   = (size_t)(end - m_heapBase + lxr::kObjectGranule - 1) / lxr::kObjectGranule;
    uint8_t* rcStart = m_rcTable + gStart;                 // one byte per granule
    uint8_t* rcEnd   = m_rcTable + gEnd;
    uintptr_t pageMask = (uintptr_t)g_pageSize - 1;
    uint8_t* p = (uint8_t*)((uintptr_t)rcStart & ~pageMask);
    while (p < rcEnd)
    {
        uint8_t* pageEnd = p + g_pageSize;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        {
            p = pageEnd; // reserved/uncommitted -> RC already all-zero here
            continue;
        }
        uint8_t* scanFrom = (p < rcStart) ? rcStart : p;
        uint8_t* scanTo   = (pageEnd < rcEnd) ? pageEnd : rcEnd;
        if (scanTo > scanFrom)
            memset(scanFrom, 0, (size_t)(scanTo - scanFrom));
        p = pageEnd;
    }
}

// every MARKED object, check that each of its in-heap referents is ALSO marked.
// A marked object pointing at an unmarked in-heap object is a trace-completeness
// bug: that referent will be swept/reused while still reachable, dangling the
// pointer. Prints the first offenders (parent + child metadata) so we can
// identify which object shape the field enumeration is missing.
void LXRCollector::VerifyTraceComplete()
{
    EnterCriticalSection(&g_chunkLock);
    int reported = 0;
    int64_t offenders = 0;
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed) continue;
        uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        uint8_t* p = c.Start;
        while (p < end)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) break;
            if (IsMarked(o))
            {
                MethodTable* pmt = o->GetGCSafeMethodTable();
                GCScanObjectRefs(o, sz, [this, o, pmt, &reported, &offenders](Object** ref)
                {
                    Object* child = *ref;
                    if (child != nullptr && InHeap(child) && !IsMarked(child))
                    {
                        offenders++;
                        if (reported < 12)
                        {
                            reported++;
                            bool childNew  = (uint8_t*)child >= g_concWatermark;
                            bool parentNew = (uint8_t*)o     >= g_concWatermark;
                            fprintf(stderr,
                                "LXRGC: [verify] MARKED parent %p mt=%p base=%u comp=%d parentNew=%d -> UNMARKED child %p (fieldoff=%lld childNew=%d wm=%p)\n",
                                (void*)o, (void*)pmt, (unsigned)pmt->GetBaseSize(),
                                pmt->HasComponentSize() ? 1 : 0, parentNew ? 1 : 0, (void*)child,
                                (long long)((uint8_t*)ref - (uint8_t*)o), childNew ? 1 : 0, (void*)g_concWatermark);
                        }
                    }
                });
            }
            p += sz;
        }
    }
    LeaveCriticalSection(&g_chunkLock);
    fprintf(stderr, "LXRGC: [verify] trace-completeness offenders=%lld markStackDrops=%lld\n",
            (long long)offenders, (long long)g_lxrCounters.MarkStackDrops);
    fflush(stderr);
}

// Close the transitive closure over every currently-marked object. This is a
// STW-only soundness backstop for the concurrent trace: it walks the whole used
// heap and, for each MARKED (retained/live) object, pushes any in-heap child
// that is not yet marked, then drains. Marking a child of a marked object is
// definitionally correct - the child is reachable from a live object, so it is
// live - and it never retains genuinely-dead objects (an object no marked object
// points to is never visited). This makes the trace complete regardless of which
// deletion-barrier path (JIT-elided null store, native VM ref store, IntPtr-typed
// CoreLib ref-array clear, drain/mark-commit ordering) failed to grey a slot.
// Returns the number of objects it had to newly mark (the closure gap = 0 when
// the concurrent trace was already complete), which is the root-cause signal.
// Classify a heap address relative to the concurrent snapshot: returns true if it
// was born DURING the trace window (at/above its region's recorded snapshot
// high-water, or in a region that did not exist at snapshot time). A false result
// means the object was already live at snapshot time - so if such an object is a
// closure gap, some snapshot-era incoming edge was deleted without a barrier.
static bool LXRBornInWindow(uint8_t* addr)
{
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed) continue;
        uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        if (addr < c.Start || addr >= end) continue;
        if (i >= g_snapChunkCount || g_snapUsedEnd == nullptr || i >= g_snapUsedEnd->size())
            return true; // region born mid-trace
        return addr >= (*g_snapUsedEnd)[i];
    }
    return false;
}

int64_t LXRCollector::CompleteClosureOverMarked()
{
    int64_t newlyMarked = 0;
    int64_t gapBornInWindow = 0, gapSnapshotEra = 0;
    int reported = 0;
    bool progress = true;
    // Iterate to a fixpoint: draining pushed children marks transitively, but a
    // marked object located earlier in the linear walk than the parent that
    // revealed it is only re-examined on a subsequent pass. A pass that adds
    // nothing proves closure completeness.
    while (progress)
    {
        progress = false;
        EnterCriticalSection(&g_chunkLock);
        for (size_t i = 0; i < g_chunkCount; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed) continue;
            uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
            // Skip provably-dead regions without parsing them. A region with no
            // mark bit set contains no reachable object, so it can hold no
            // marked->unmarked edge; the linear object parse (the expensive part,
            // proportional to total heap) is only needed where marks exist. This
            // keeps closure-completion proportional to the LIVE heap plus a cheap
            // bitmap scan, not to the whole used heap.
            if (!AnyMarkedInRange(c.Start, end)) continue;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p;
                size_t sz = LXRObjectSize(o);
                if (sz == 0) break;
                if (IsMarked(o))
                {
                    if (g_lineMarksValid)
                        MarkLines(o, sz);
                    MethodTable* pmt = o->GetGCSafeMethodTable();
                    GCScanObjectRefs(o, sz, [this, o, pmt, &newlyMarked, &gapBornInWindow, &gapSnapshotEra, &reported, &progress](Object** ref)
                    {
                        Object* child = *ref;
                        if (child != nullptr && InHeap(child) && !IsMarked(child))
                        {
                            bool childBorn  = LXRBornInWindow((uint8_t*)child);
                            bool parentBorn = LXRBornInWindow((uint8_t*)o);
                            if (childBorn) gapBornInWindow++; else gapSnapshotEra++;
                            if (!childBorn)
                                LXRWriteGapMiniDump(); // one-shot: capture for offline SOS type resolution
                            if (reported < 12)
                            {
                                reported++;
                                fprintf(stderr,
                                    "LXRGC: [closure] gap: parent %p mt=%p comp=%d born=%d -> child %p (fieldoff=%lld childBorn=%d)\n",
                                    (void*)o, (void*)pmt, pmt->HasComponentSize() ? 1 : 0, parentBorn ? 1 : 0,
                                    (void*)child, (long long)((uint8_t*)ref - (uint8_t*)o), childBorn ? 1 : 0);
                            }
                            newlyMarked++;
                            progress = true;
                            PushMark(child);   // mark + enqueue; children scanned by drain below
                        }
                    });
                }
                p += sz;
            }
        }
        LeaveCriticalSection(&g_chunkLock);
        // Scan everything just pushed (transitively). Newly marked objects reached
        // here need one more linear pass in case they precede a parent that would
        // reveal further children, hence the outer fixpoint loop.
        DrainClosure();
    }
    if (newlyMarked != 0 && getenv("LXR_VERIFY_TRACE") != nullptr)
        fprintf(stderr, "LXRGC: [closure] gap classification: bornInWindow=%lld snapshotEra=%lld total=%lld\n",
                (long long)gapBornInWindow, (long long)gapSnapshotEra, (long long)newlyMarked);
    return newlyMarked;
}

void LXRCollector::ResetMarks()
{
    // Commit and zero exactly the mark-table prefix that covers the heap used so
    // far (heap high-water), so every live AND dead object below the water mark
    // has a readable, zeroed mark bit. Committing the covering prefix (rather
    // than lazily per-mark) is what lets the sweep safely read mark bits for
    // never-marked dead chunks without faulting on reserved memory.
    uint8_t* highWater = (g_lxrGCHeap != nullptr) ? g_lxrGCHeap->HeapHighWater()
                                                  : (m_heapBase + m_heapBytes);
    size_t usedBytes = (size_t)(highWater - m_heapBase);
    size_t neededBytes = (usedBytes / lxr::kObjectGranule + 7) / 8;
    // Round up to a page so the whole covering range is committed.
    neededBytes = (neededBytes + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
    size_t cap = (m_heapBytes / lxr::kObjectGranule + 7) / 8;
    if (neededBytes > cap)
        neededBytes = cap;
    if (neededBytes > 0)
    {
        VirtualAlloc(m_markTable, neededBytes, MEM_COMMIT, PAGE_READWRITE);
        memset(m_markTable, 0, neededBytes);
    }
    m_markCommittedBytes = neededBytes;

    // Immix line reuse: commit+zero the covering line-table prefix so line marks
    // accumulate from a clean slate this cycle. Arm g_lineMarksValid only when
    // line reuse is enabled; the drain/closure sites then populate line marks and
    // the sweep may carve free line runs (mark-authoritative cycles only).
    if (g_lineReuse > 0 && m_lineMarkTable != nullptr)
    {
        size_t lineNeeded = (usedBytes / lxr::kLineSize + 7) / 8;
        lineNeeded = (lineNeeded + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
        size_t lineCap = (m_heapBytes / lxr::kLineSize + 7) / 8;
        if (lineNeeded > lineCap) lineNeeded = lineCap;
        if (lineNeeded > 0)
        {
            VirtualAlloc(m_lineMarkTable, lineNeeded, MEM_COMMIT, PAGE_READWRITE);
            memset(m_lineMarkTable, 0, lineNeeded);
        }
        m_lineMarkCommittedBytes = lineNeeded;
        InterlockedExchange(&g_lineMarksValid, 1);
        InterlockedExchange(&g_conservativeKeepAliveThisCycle, 0);
    }
    else
    {
        InterlockedExchange(&g_lineMarksValid, 0);
    }
}

void LXRCollector::PushMark(Object* obj)
{
    if (obj == nullptr || !MarkObject(obj))
        return;
    if (g_markTop == g_markCap)
    {
        size_t newCap = g_markCap ? g_markCap * 2 : 4096;
        Object** grown = (Object**)realloc(g_markStack, newCap * sizeof(Object*));
        if (grown == nullptr)
        {
            InterlockedIncrement64(&g_lxrCounters.MarkStackDrops);
            return; // best-effort; a dropped push only risks over-retention via re-scan
        }
        g_markStack = grown;
        g_markCap = newCap;
    }
    g_markStack[g_markTop++] = obj;
}

void LXRCollector::DrainMarkStack()
{
    int verify = (getenv("LXR_VERIFY_TRACE") != nullptr) ? 1 : 0;
    while (g_markTop > 0)
    {
        Object* o = g_markStack[--g_markTop];
        if (verify)
        {
            uintptr_t pm = (*(uintptr_t*)o) & ~(uintptr_t)7;
            if (pm == 0 || (pm & 7) || pm < 0x10000ull || pm > 0x00007FFFFFFFFFFFull)
            {
                fprintf(stderr, "LXRGC: [verify] SCAN bad object %p mt=%p (skipped)\n",
                        (void*)o, (void*)pm);
                fflush(stderr);
                continue; // don't dereference garbage MT
            }
        }
        size_t osz = LXRObjectSize(o);
        if (osz == 0)
        {
            // Not a valid object start. This is almost always an interior/byref
            // value pushed as-is (runtime-async continuation captures, ref locals
            // stored on the heap): GCScanObjectRefs cannot distinguish byref from
            // objref slots, so *ref may point into an object's interior. Resolve
            // it to the containing object's base so the target is scanned (its
            // out-edges followed) - otherwise objects reachable only via a byref
            // are never traced and get swept -> AV in e.g. DispatchContinuations.
            if (InHeap(o))
            {
                Object* base = ResolveInterior((uint8_t*)o);
                if (base != nullptr)
                {
                    o = base;
                    osz = LXRObjectSize(o);
                    MarkObject(o); // record the true base granule as marked
                }
            }
            if (osz == 0)
                continue; // genuinely not scannable
        }
        if (g_lineMarksValid)
            MarkLines(o, osz);
        GCScanObjectRefs(o, osz, [this, o, verify](Object** ref)
        {
            Object* child = *ref;
            if (verify && child != nullptr && InHeap(child))
            {
                uintptr_t cm = (*(uintptr_t*)child) & ~(uintptr_t)7;
                if (cm == 0 || (cm & 7) || cm < 0x10000ull || cm > 0x00007FFFFFFFFFFFull)
                {
                    MethodTable* pmt = o->GetGCSafeMethodTable();
                    size_t ncomp = pmt->HasComponentSize() ? (size_t)((ArrayBase*)o)->GetNumComponents() : 0;
                    fprintf(stderr,
                        "LXRGC: [verify] PARENT %p mt=%p base=%u compsz=%u ncomp=%llu size=%llu fieldoff=%lld -> BAD child %p mt=%p marked=%d\n",
                        (void*)o, (void*)pmt, (unsigned)pmt->GetBaseSize(),
                        (unsigned)pmt->RawGetComponentSize(), (unsigned long long)ncomp,
                        (unsigned long long)LXRObjectSize(o),
                        (long long)((uint8_t*)ref - (uint8_t*)o),
                        (void*)child, (void*)cm, IsMarked(child) ? 1 : 0);
                    fflush(stderr);
                    return; // don't push garbage
                }
            }
            PushMark(*ref);
        });
    }
}

// Drain one worker's local grey set to completion. The atomic mark bit
// (MarkObject) claims each object for exactly one worker, so workers never scan
// the same object and need no shared stack or termination protocol.
void LXRCollector::DrainSliceLocal(std::vector<Object*>& local)
{
    while (!local.empty())
    {
        Object* o = local.back();
        local.pop_back();
        size_t osz = LXRObjectSize(o);
        if (osz == 0)
        {
            // Interior/byref value (see DrainMarkStack): resolve to base so the
            // target's out-edges are scanned. Claim the base atomically so two
            // workers resolving different interiors into the same object don't
            // both scan it.
            if (InHeap(o))
            {
                Object* base = ResolveInterior((uint8_t*)o);
                if (base != nullptr && MarkObject(base))
                {
                    o = base;
                    osz = LXRObjectSize(o);
                }
                else
                {
                    continue; // unresolvable, or base already claimed/scanned
                }
            }
            if (osz == 0)
                continue;
        }
        if (g_lineMarksValid)
            MarkLines(o, osz);
        GCScanObjectRefs(o, osz, [this, &local](Object** ref)
        {
            Object* c = *ref;
            if (c != nullptr && MarkObject(c)) // atomic claim
                local.push_back(c);
        });
    }
}

// Persistent worker-pool thread body. Blocks until signalled for a drain, then
// processes its dedicated slice (index = poolIndex+1; slice 0 is the main GC
// thread) and signals completion. Runs only inside the STW mark pause.
static void LXRMarkWorkerProc(void* idx)
{
    int w = (int)(intptr_t)idx; // pool index [0, g_poolWorkers)
    for (;;)
    {
        WaitForSingleObject(g_poolStart[w], INFINITE);
        std::vector<std::vector<Object*>>* slices = g_poolSlices;
        if (slices != nullptr && (size_t)(w + 1) < slices->size() && g_poolCollector != nullptr)
            g_poolCollector->DrainSliceLocal((*slices)[(size_t)(w + 1)]);
        SetEvent(g_poolDone[w]);
    }
}

// Create the persistent parallel-mark worker pool once, at Initialize time (never
// during a STW pause). No-op unless LXR_GC_THREADS>1. On any failure we leave
// g_poolWorkers at its partial count; ParallelDrainMarkStack falls back to serial.
static void EnsureMarkWorkerPool()
{
    if (g_poolWorkers != 0 || g_gcThreads <= 1 || g_theGCToCLR == nullptr)
        return;
    int want = g_gcThreads - 1;
    g_poolStart = new (std::nothrow) HANDLE[want];
    g_poolDone  = new (std::nothrow) HANDLE[want];
    if (g_poolStart == nullptr || g_poolDone == nullptr)
        return;
    g_poolCollector = &g_lxrCollector;
    for (int w = 0; w < want; w++)
    {
        g_poolStart[w] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_poolDone[w]  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_poolStart[w] == nullptr || g_poolDone[w] == nullptr)
            return; // partial pool; ParallelDrainMarkStack clamps to g_poolWorkers
        if (!g_theGCToCLR->CreateThread(&LXRMarkWorkerProc, (void*)(intptr_t)w,
                                        /*is_suspendable*/ false, ".NET LXR mark worker"))
            return;
        g_poolWorkers = w + 1; // publish only fully-created workers
    }
}

// Parallel transitive closure (P5). The seed set already sits in g_markStack.
// Partition it round-robin across the available lanes (main thread = lane 0, each
// pooled worker = lane w+1); each lane drains its own local stack to completion.
// Correctness rests on the atomic mark bit: an object is claimed by exactly one
// lane. Runs inside the STW trace pause. Uses the persistent worker pool; if the
// pool is unavailable it falls back to the serial drain (never per-drain threads).
void LXRCollector::ParallelDrainMarkStack(int workers)
{
    if (workers < 2 || g_markTop == 0)
    {
        DrainMarkStack();
        return;
    }
    if (g_poolWorkers < 1)
    {
        // Pool not available (creation failed / gcThreads changed): stay correct.
        DrainMarkStack();
        return;
    }

    int lanes = workers;
    if (lanes > g_poolWorkers + 1)
        lanes = g_poolWorkers + 1; // clamp to what the pool can serve

    static std::vector<std::vector<Object*>> slices; // reused; single drain at a time
    slices.assign((size_t)lanes, std::vector<Object*>());
    size_t n = g_markTop;
    for (size_t i = 0; i < n; i++)
        slices[i % (size_t)lanes].push_back(g_markStack[i]);
    g_markTop = 0; // consumed into the per-lane slices

    g_poolSlices = &slices;
    for (int w = 1; w < lanes; w++)   // wake pooled workers 0..lanes-2 -> slices 1..lanes-1
        SetEvent(g_poolStart[w - 1]);

    DrainSliceLocal(slices[0]);       // main thread drains lane 0

    for (int w = 1; w < lanes; w++)
        WaitForSingleObject(g_poolDone[w - 1], INFINITE);
    g_poolSlices = nullptr;
}

// Drain the current grey set (already in g_markStack) using the parallel closure
// when LXR_GC_THREADS>1, else the serial one. Shared by the STW backup trace and
// the concurrent trace's drain/finish so all three compose with parallel marking.
void LXRCollector::DrainClosure()
{
    if (g_gcThreads > 1)
        ParallelDrainMarkStack(g_gcThreads);
    else
        DrainMarkStack();
}

// Diagnostic filter: on a fault during the linear parse, dump the region bounds,
// the walk pointer, and the committed-state of the faulting page vs. UsedEnd so
// we can tell a stale-`Committed` decommit (UsedEnd beyond committed) apart from
// an in-committed parse desync. Enabled via LXR_FAULT_DIAG. Returns
// EXCEPTION_EXECUTE_HANDLER so the caller's __except still runs.
static LONG LXRParseFaultFilter(EXCEPTION_POINTERS* ep, uint8_t* start, uint8_t* usedEnd,
                                uint8_t* p, uint8_t* interior)
{
    static int diag = -1;
    if (diag < 0) diag = (getenv("LXR_FAULT_DIAG") != nullptr) ? 1 : 0;
    if (diag && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        void* accessed = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        auto qstate = [](void* a) -> const char* {
            MEMORY_BASIC_INFORMATION mbi = {};
            VirtualQuery(a, &mbi, sizeof(mbi));
            return (mbi.State == MEM_COMMIT) ? "COMMIT" : (mbi.State == MEM_RESERVE) ? "RESERVE" : "FREE";
        };
        fprintf(stderr,
            "LXRGC: [ResolveInterior fault] interior=%p region=[%p,%p) walk_p=%p accessed=%p "
            "accessed-state=%s usedEnd-1-state=%s p-state=%s p_off_from_start=%lld region_len=%lld\n",
            interior, start, usedEnd, p, accessed,
            qstate(accessed), qstate(usedEnd - 1), qstate(p),
            (long long)(p - start), (long long)(usedEnd - start));
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Linear-parse [start,usedEnd) for the object containing `interior`. The walk
// can desync (a suspended mid-allocation slot, or a wrong size from a
// momentarily-inconsistent MethodTable) and step `p` onto an unmapped page; the
// `*p` dereference then faults BEFORE LXRObjectSize's sentinel can reject it.
// Guard the walk with SEH so such a fault is treated as "not resolved here"
// instead of crashing the GC. `p` is volatile so the fault filter observes the
// walk pointer's value at the faulting iteration. No C++ objects with destructors
// may live in this frame (SEH rule) — only POD locals are used.
static Object* ParseContainingObjectGuarded(uint8_t* start, uint8_t* usedEnd, uint8_t* interior)
{
    volatile uint8_t* vp = start;
    __try
    {
        uint8_t* p = start;
        uint8_t* lastObj = nullptr;
        while (p < usedEnd)
        {
            vp = p;
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0)
                break;
            if (interior >= p && interior < p + sz)
                return o;
            lastObj = p;
            p += sz;
        }
        // One-past-the-end interior pointer (e.g. `ref array[array.Length]`, a
        // span/loop terminator that runtime-async spills into heap continuation
        // state). C# permits a byref one element past the end; it never resolves
        // by the strict `interior < p+sz` test above. When `interior` lands
        // exactly at the end of the parsed run, it is the one-past-end of the
        // last object, so resolve to it - otherwise that object and its out-edges
        // are never scanned and get swept -> AV (DispatchContinuations).
        if (interior == p && lastObj != nullptr && interior == usedEnd)
            return (Object*)lastObj;
    }
    __except (LXRParseFaultFilter(GetExceptionInformation(), start, usedEnd, (uint8_t*)vp, interior))
    {
        // Parse walked into unmapped memory: the region layout is inconsistent
        // for this interior pointer; resolve it as "not found" rather than crash.
    }
    return nullptr;
}

// Resolve an interior pointer to the object that contains it by parsing the
// enclosing allocation region. Returns nullptr if it cannot be resolved (the
// caller then keeps the region conservatively live).
Object* LXRCollector::ResolveInterior(uint8_t* interior)
{
    if (interior < m_heapBase || interior >= m_heapBase + m_heapBytes)
        return nullptr;

    EnterCriticalSection(&g_chunkLock);
    ChunkRegion region = {};
    bool found = false;
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed)
            continue;
        uint8_t* usedEnd = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        // Allow interior == usedEnd (one-past-the-end byref of the last object in
        // this region); ParseContainingObjectGuarded resolves it to that object.
        if (interior >= c.Start && interior <= usedEnd)
        {
            region = c;
            region.UsedEnd = usedEnd;
            found = true;
            // Prefer a region that strictly contains the pointer over one where
            // it only touches the boundary, so an object-boundary byref between
            // two adjacent regions binds to the region it is the end of.
            if (interior < usedEnd)
                break;
        }
    }
    LeaveCriticalSection(&g_chunkLock);
    if (!found)
        return nullptr;

    return ParseContainingObjectGuarded(region.Start, region.UsedEnd, interior);
}

// Count of interior roots that could not be resolved and were handled by the
// conservative keep-alive fallback (0 under all validated sweep-only runs).
static volatile int64_t g_unresolvedInteriorRoots = 0;

// Conservative fallback: an in-heap interior/byref root did not resolve to a base
// object (e.g. a transient chunk-registry inconsistency). Rather than DROP the
// root — which would let the sweep reclaim a region a live byref still points
// into (a use-after-free that "explodes later") — set a mark bit at the
// interior's own granule. The sweep's AnyMarkedInRange(Start,UsedEnd) test then
// retains the whole containing chunk for this cycle. This over-approximates
// liveness (never under-approximates), so it is always sound; it is never a
// wrong-object resolution. The stray bit is not at an object start, so
// object-start mark walks (evac, verify) never mis-read it as a live object.
bool LXRCollector::ConservativelyKeepAliveInterior(uint8_t* interior)
{
    if (interior < m_heapBase || interior >= m_heapBase + m_heapBytes)
        return false;
    size_t granule = (size_t)(interior - m_heapBase) / lxr::kObjectGranule;
    size_t byteIdx = granule >> 3;
    if (byteIdx >= m_markCommittedBytes)
        return false;
    _InterlockedOr8((volatile char*)&m_markTable[byteIdx], (char)(1u << (granule & 7)));
    InterlockedExchange(&g_conservativeKeepAliveThisCycle, 1);
    int64_t n = InterlockedIncrement64(&g_unresolvedInteriorRoots);
    static int diag = -1;
    if (diag < 0) diag = (getenv("LXR_FAULT_DIAG") != nullptr) ? 1 : 0;
    if (diag && n <= 16)
        fprintf(stderr, "LXRGC: [interior] unresolved in-heap interior root %p -> region kept alive conservatively (total=%lld)\n",
                (void*)interior, (long long)n);
    return true;
}

void LXRCollector::ProcessModifiedBuffers()
{
    // Coalescing reference counting (Levanoni-Petrank, paper A/§3.2.1). For the
    // period t_n -> t_{n+1} it is sufficient to apply, per MODIFIED FIELD, exactly
    // ONE decrement of the referent at t_n (the FIRST logged old value) and ONE
    // increment of the referent at t_{n+1} (the field's final value), ignoring all
    // intermediate referents. Our field-logging barrier records every store (no
    // per-field unlogged bit yet - see A(ii)), so a field written N times appears
    // N times here; we coalesce at processing time into one (oldValue,*slot) pair
    // per slot. Without this a repeatedly-written field over-increments its final
    // referent and spuriously decrements transients, making RC imprecise. The map
    // keeps the FIRST old value per slot (append order == store order within a
    // thread), which is the t_n value; *slot (read under STW) is the t_{n+1} value.
    EnterCriticalSection(&m_collectLock);
    EnterCriticalSection(&g_buffersLock);
    std::unordered_map<Object**, Object*> coalesced;
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        for (size_t i = 0; i < buf->Count; i++)
            coalesced.emplace(buf->Entries[i].Slot, buf->Entries[i].OldValue);
        buf->Count = 0; // epoch consumed
    }
    LeaveCriticalSection(&g_buffersLock);
    // Deferred-RC root capture (paper §3.2.1): scan the roots at this STW pause so
    // root-reachable mature objects are incremented now and decremented at the
    // next pause (m_rootDeferredPrev). This is what makes RC self-standing rather
    // than reliant on the mark trace to protect roots. Safe here: ProcessModified-
    // Buffers is only ever called under SuspendEE (STW / concurrent-finish pause).
    std::vector<Object*> rootsNow;
    CaptureRoots(rootsNow);
    // Increments before decrements (paper B/§3.2.1): apply ALL increments of the
    // final referents first, so an object that gains a new reference this epoch is
    // never transiently driven to zero (and freed) by an earlier field's decrement.
    for (const auto& kv : coalesced)
    {
        Object* newValue = *(kv.first);
        if (newValue != nullptr)
            RCIncrement(newValue);
    }
    for (Object* r : rootsNow)          // root increments (this epoch's root set)
        RCIncrement(r);
    for (const auto& kv : coalesced)
    {
        Object* oldValue = kv.second;
        if (oldValue != nullptr && RCDecrement(oldValue))
            EnqueueZeroCount(oldValue);
    }
    for (Object* r : m_rootDeferredPrev) // deferred root decrements (prior epoch)
        if (RCDecrement(r))
            EnqueueZeroCount(r);
    m_rootDeferredPrev.swap(rootsNow);   // this epoch's roots -> next epoch's decs
    DrainZeroCountWorkList();
    // Restore the unlogged-bit invariant for the next epoch: every set logged bit
    // must correspond to a currently-buffered field. On a modified-buffer overflow
    // some first-logs set a bit without leaving a buffer entry, so wholesale-clear;
    // otherwise clear exactly the coalesced (now-consumed) fields - O(modified
    // fields), NOT O(heap). Under STW here, so the non-atomic clears are safe.
    if (g_modifiedOverflow)
        ResetLoggedTable();
    else
        for (const auto& kv : coalesced)
            ClearLoggedBit(kv.first);
    LeaveCriticalSection(&m_collectLock);
    InterlockedExchange(&g_modifiedOverflow, 0);
}

// #1 concurrent/lazy decrements - STW half. Detach every mutator's coalescing-RC
// modified buffer into g_rcSnap*, capturing (oldValue, newValue=*slot) while the
// mutators are stopped so both reads are stable, then reset each buffer so
// logging resumes into fresh space. This is the only part that costs pause time
// (a bounded copy proportional to the epoch's mutations); the RC arithmetic and
// the recursive free run off-pause in ProcessSnapshotDecrements. Must be called
// under STW (the concurrent snapshot pause).
void LXRCollector::SnapshotModifiedBuffers()
{
    EnterCriticalSection(&g_buffersLock);
    // Coalesce per field (paper A/§3.2.1) exactly as ProcessModifiedBuffers: keep
    // the FIRST logged old value per slot (the t_n referent) and pair it with the
    // final *slot (the t_{n+1} referent, read here under STW so it is stable), one
    // (old,new) pair per modified field this epoch. ProcessSnapshotDecrements then
    // replays these off-pause with all-increments-before-decrements ordering.
    std::unordered_map<Object**, Object*> coalesced;
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        for (size_t i = 0; i < buf->Count; i++)
            coalesced.emplace(buf->Entries[i].Slot, buf->Entries[i].OldValue);
        buf->Count = 0; // epoch consumed (snapshotted)
    }
    for (const auto& kv : coalesced)
    {
        if (g_rcSnapCount == g_rcSnapCap)
        {
            size_t newCap = g_rcSnapCap ? g_rcSnapCap * 2 : 4096;
            RCSnapshotEntry* grown = (RCSnapshotEntry*)realloc(
                g_rcSnapEntries, newCap * sizeof(RCSnapshotEntry));
            if (grown == nullptr) { break; }
            g_rcSnapEntries = grown;
            g_rcSnapCap = newCap;
        }
        g_rcSnapEntries[g_rcSnapCount].OldValue = kv.second;
        g_rcSnapEntries[g_rcSnapCount].NewValue = *(kv.first);
        g_rcSnapCount++;
    }
    // Deferred-RC root capture at this STW snapshot pause (paper §3.2.1): stash the
    // current root set for the off-pause replay in ProcessSnapshotDecrements, which
    // increments it and rotates the deferred-decrement chain.
    CaptureRoots(m_rootDeferredSnap);
    // Restore the unlogged-bit invariant (see ProcessModifiedBuffers): STW here
    // (the snapshot pause), so clear the consumed fields' bits (or wholesale on a
    // buffer overflow) before mutators resume logging into the fresh epoch.
    if (g_modifiedOverflow)
        ResetLoggedTable();
    else
        for (const auto& kv : coalesced)
            ClearLoggedBit(kv.first);
    LeaveCriticalSection(&g_buffersLock);
    InterlockedExchange(&g_modifiedOverflow, 0);
}

// #1 concurrent/lazy decrements - off-pause half. Replay the snapshotted epoch
// with strict coalescing-RC ordering (ALL increments before ANY decrement, so a
// referent incremented by a later store is never transiently freed by an earlier
// store's decrement), then run the recursive zero-count cascade. Runs while
// mutators execute; sound because (a) only the collector ever mutates the RC
// side table (the barrier merely logs), so no RC race with mutators, and (b)
// reclamation stays gated on the mark-authoritative sweep, so a rare resurrection
// racing a decrement cannot free a reachable object. Aligned pointer loads of a
// possibly-resurrected dead object's fields are atomic on amd64 (no torn read).
void LXRCollector::ProcessSnapshotDecrements()
{
    EnterCriticalSection(&m_collectLock);
    for (size_t i = 0; i < g_rcSnapCount; i++)
    {
        Object* newValue = g_rcSnapEntries[i].NewValue;
        if (newValue != nullptr)
            RCIncrement(newValue);
    }
    for (Object* r : m_rootDeferredSnap)  // root increments captured at the pause
        RCIncrement(r);
    for (size_t i = 0; i < g_rcSnapCount; i++)
    {
        Object* oldValue = g_rcSnapEntries[i].OldValue;
        if (oldValue != nullptr && RCDecrement(oldValue))
            EnqueueZeroCount(oldValue);
    }
    for (Object* r : m_rootDeferredPrev)  // deferred root decrements (prior pause)
        if (RCDecrement(r))
            EnqueueZeroCount(r);
    m_rootDeferredPrev.swap(m_rootDeferredSnap); // rotate the deferral chain
    m_rootDeferredSnap.clear();
    g_rcSnapCount = 0;
    DrainZeroCountWorkList();
    LeaveCriticalSection(&m_collectLock);
}

// into it; under a Yuasa/SATB *deletion* barrier the newly installed referent is
// not otherwise greyed, so it can be transiently missed (empirically ~1 object
// per cycle). Rather than reconcile with an O(live-heap) closure over every
// marked object, mark the CURRENT value of every slot written during the window:
// the coalescing-RC modified buffer already records each written slot, so this is
// proportional to the mutation working set, not the live heap. Mutators are
// stopped (STW finish), so *Slot is stable. Marking a currently-referenced object
// is definitionally sound (it is live) and at worst floats a little garbage for
// one cycle. Returns the number of objects newly marked.
int64_t LXRCollector::MarkModifiedNewValues()
{
    int64_t marked = 0;
    EnterCriticalSection(&g_buffersLock);
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        for (size_t i = 0; i < buf->Count; i++)
        {
            Object* newValue = *(buf->Entries[i].Slot);
            if (newValue != nullptr && InHeap(newValue) && !IsMarked(newValue))
            {
                PushMark(newValue);
                marked++;
            }
        }
    }
    LeaveCriticalSection(&g_buffersLock);
    return marked;
}

// Root callback for handle scanning: mark the referent of a live handle.
static void LXRMarkHandleRef(Object** ref, void* /*ctx*/)
{
    g_lxrCollector.PushMark(*ref);
}

// promote_func for GcScanRoots: mark a stack/static/finalizer root. Interior
// pointers are resolved to their containing object so nothing reachable is
// missed. An in-heap interior root that cannot be resolved is NOT dropped: its
// containing region is kept alive conservatively so a live byref never dangles.
static void LXRPromoteRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        uint8_t* interior = (uint8_t*)o;
        o = g_lxrCollector.ResolveInterior(interior);
        if (o == nullptr)
        {
            g_lxrCollector.ConservativelyKeepAliveInterior(interior);
            return;
        }
    }
    g_lxrCollector.PushMark(o);
}

// --- Deferred-RC root capture (paper §2.1/§3.2.1) --------------------------
// Collect (not mark) the current unique in-heap root+handle referents so the RC
// pause can increment them and defer a matching decrement to the next pause. The
// collection target is a thread-local-free file static because GcScanRoots only
// accepts a bare function pointer; CaptureRoots sets it around the scan and all
// capture callbacks run on that single collector thread, under STW.
static std::unordered_set<Object*>* g_rootDeferralCollect = nullptr;

static void LXRCollectHandleRoot(Object** ref, void* /*ctx*/)
{
    Object* o = (ref != nullptr) ? *ref : nullptr;
    if (o != nullptr && g_rootDeferralCollect != nullptr)
        g_rootDeferralCollect->insert(o);
}

static void LXRCollectRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        // Resolve to the containing object. If it cannot be resolved, skip it for
        // RC deferral: the mark trace already keeps such a region alive
        // conservatively (ConservativelyKeepAliveInterior), so RC need not.
        o = g_lxrCollector.ResolveInterior((uint8_t*)o);
        if (o == nullptr)
            return;
    }
    if (g_rootDeferralCollect != nullptr)
        g_rootDeferralCollect->insert(o);
}

// Scan handles + stack/static/finalizer roots into `out` as a de-duplicated set
// of referents. MUST run under STW (GcScanRoots requires a suspended EE). Young
// and off-heap referents are collected too but are no-ops under RCIncrement/
// RCDecrement, so callers need not filter them.
void LXRCollector::CaptureRoots(std::vector<Object*>& out)
{
    out.clear();
    if (g_theGCToCLR == nullptr)
        return;
    std::unordered_set<Object*> seen;
    g_rootDeferralCollect = &seen;
    LXRGCHandleStore::ForEachLiveHandle(&LXRCollectHandleRoot, nullptr);
    ScanContext sc;
    sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRCollectRoot, 2, 2, &sc);
    g_rootDeferralCollect = nullptr;
    out.assign(seen.begin(), seen.end());
}

void LXRCollector::BackupTrace()
{
    // LXR's periodic backup trace: a stop-the-world mark from all roots that
    // (a) collects the dead cycles pure reference counting can never reclaim
    // and (b) is the safety backstop guaranteeing we never free a reachable
    // object. Marking uses the generic runtime object-scanning facility for the
    // transitive closure. Reclamation itself happens in SweepAndSelectDefrag.
    InterlockedIncrement64(&g_lxrCounters.BackupTraces);
    if (g_theGCToCLR == nullptr)
        return;

    g_markTop = 0;
    ResetMarks(); // clear all mark bits before this pass

    // 1. Handle roots (strong and - conservatively - weak: over-retention is
    //    always safe, under-retention is not).
    LXRGCHandleStore::ForEachLiveHandle(&LXRMarkHandleRef, nullptr);

    // 2. Stack, static and finalizer roots via the EE's root enumeration.
    ScanContext sc;
    sc.promotion = true;
    const int maxgen = 2;
    size_t rootsBefore = g_markTop;
    g_theGCToCLR->GcScanRoots(&LXRPromoteRoot, maxgen, maxgen, &sc);
    size_t rootsPushed = g_markTop - rootsBefore;

    // 2b. SATB deletion set: while a trace window is open, referents unlinked by
    //     mutators since the snapshot must be kept live for this trace (Yuasa).
    //     Under STW this is a no-op unless LXR_SATB exercises it; P4 drives it
    //     from a concurrent marker.
    if (g_satbActive)
        DrainSatbBuffers();

    // 3. Transitive closure over reachable objects.
    DrainClosure();

    if (getenv("LXR_VERIFY_TRACE") != nullptr)
    {
        fprintf(stderr, "LXRGC: [trace] rootsPushed=%llu heapNextFree=+%lldMB\n",
                (unsigned long long)rootsPushed,
                (long long)((g_lxrGCHeap ? (g_lxrGCHeap->HeapHighWater() - g_lxrGCHeap->HeapBase()) : 0) >> 20));
        VerifyTraceComplete();
    }
}

// --- Concurrent SATB backup trace (P4) -------------------------------------
// Called under the STW *snapshot* pause. Resets marks, opens the SATB window,
// seeds the mark stack from the roots/handles (without draining), and records a
// per-region allocation high-water so objects born during the concurrent window
// can be retained (allocate-black) at the finish pause.
void LXRCollector::ConcurrentTraceSnapshot()
{
    if (g_theGCToCLR == nullptr)
        return;
    InterlockedIncrement64(&g_lxrCounters.BackupTraces);
    InterlockedIncrement64(&g_lxrCounters.ConcurrentTraces);

    g_markTop = 0;
    ResetMarks();

    // Open the SATB window BEFORE seeding roots so any mutator deletion that
    // races the snapshot is captured (the mutators are suspended here, so this
    // simply arms the barrier for after RestartEE).
    ResetSatbBuffers();
    SetSatbActive(true);

    // Record the allocation high-water per region + overall, for allocate-black.
    g_concWatermark = (g_lxrGCHeap != nullptr) ? g_lxrGCHeap->HeapHighWater()
                                               : (m_heapBase + m_heapBytes);
    if (g_snapUsedEnd == nullptr)
        g_snapUsedEnd = new (std::nothrow) std::vector<uint8_t*>();
    EnterCriticalSection(&g_chunkLock);
    g_snapChunkCount = g_chunkCount;
    if (g_snapUsedEnd != nullptr)
    {
        g_snapUsedEnd->clear();
        g_snapUsedEnd->reserve(g_chunkCount);
        for (size_t i = 0; i < g_chunkCount; i++)
        {
            ChunkRegion& c = g_chunks[i];
            uint8_t* used = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
            g_snapUsedEnd->push_back(used);
        }
    }
    LeaveCriticalSection(&g_chunkLock);

    // Suppress block reuse for the duration of the window (see ReuseChunk).
    InterlockedExchange(&g_traceWindowOpen, 1);

    // Seed the grey set from the roots/handles; do NOT drain here (that is the
    // concurrent phase's job).
    LXRGCHandleStore::ForEachLiveHandle(&LXRMarkHandleRef, nullptr);
    ScanContext sc;
    sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRPromoteRoot, 2, 2, &sc);
}

// Runs OUTSIDE any pause: marks the transitive closure while the mutators run and
// log deletions through the SATB barrier. Bounded, best-effort: the STW finish
// pass is the correctness guarantee, so this only needs to offload the bulk.
void LXRCollector::ConcurrentTraceDrain()
{
    // Interleave marking the grey set with consuming freshly-logged SATB
    // deletions, until a full pass adds no new work or the iteration budget is
    // hit. Only this (single) collector thread touches the mark stack.
    const int kMaxRounds = 64;
    for (int round = 0; round < kMaxRounds; round++)
    {
        size_t before = g_lxrCounters.SatbMarks;
        DrainClosure();         // scan everything currently grey (parallel if enabled)
        DrainSatbBuffers();     // pull in deletions logged since last pass
        DrainClosure();         // scan those too
        bool moreSatb = (size_t)g_lxrCounters.SatbMarks != before;
        if (g_markTop == 0 && !moreSatb)
            break;              // quiescent (mutators may still trickle; finish mops up)
        Sleep(0);               // yield so mutators make progress / accrue SATB work
    }
}

// promote_func for the FINAL root rescan at the concurrent finish pause. Marks
// like LXRPromoteRoot but counts objects that were still WHITE (unmarked) when a
// root reached them - i.e. live objects the concurrent trace missed and which the
// following mark-authoritative sweep would otherwise reclaim (a use-after-free).
static volatile LONG64 g_finalRescanMarked = 0;
static int             g_finalRescanReported = 0;
static void LXRPromoteRootFinal(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        uint8_t* interior = (uint8_t*)o;
        o = g_lxrCollector.ResolveInterior(interior);
        if (o == nullptr)
        {
            g_lxrCollector.ConservativelyKeepAliveInterior(interior);
            return;
        }
    }
    if (!g_lxrCollector.IsMarked(o))
    {
        InterlockedIncrement64(&g_finalRescanMarked);
        static int s_verify = (getenv("LXR_VERIFY_TRACE") != nullptr) ? 1 : 0;
        if (s_verify && g_finalRescanReported < 16)
        {
            g_finalRescanReported++;
            MethodTable* mt = o->GetGCSafeMethodTable();
            fprintf(stderr, "LXRGC: [final-rescan] MISSED live root object %p mt=%p comp=%d bornInWindow=%d\n",
                    (void*)o, (void*)mt, mt && mt->HasComponentSize() ? 1 : 0,
                    LXRBornInWindow((uint8_t*)o) ? 1 : 0);
        }
    }
    g_lxrCollector.PushMark(o);
}

// Called under the STW *finish* pause. Consumes residual SATB, finishes the
// closure, then applies allocate-black: every object allocated since the
// snapshot (at/above its region's snapshot high-water) is retained this cycle so
// the following sweep cannot free a live, never-marked new object. Closes the
// SATB window.
void LXRCollector::ConcurrentTraceFinish()
{
    // Residual deletions logged between the last concurrent pass and the pause.
    DrainSatbBuffers();
    DrainClosure();

    // SATB overflow fallback: if any mutator dropped a snapshot entry during the
    // window (its pre-sized buffer filled), the off-pause closure may be
    // incomplete. Recover soundness the unconditional way - re-seed the grey set
    // from ALL current roots + handles and re-run the closure. Mutators are
    // stopped here, so this is an atomic, hazard-free full trace (exactly the STW
    // BackupTrace backstop). Genuinely-dead objects stay unmarked and are still
    // reclaimed; nothing reachable can be missed.
    if (g_satbOverflow)
    {
        InterlockedIncrement64(&g_lxrCounters.SatbOverflowRetraces);
        LXRGCHandleStore::ForEachLiveHandle(&LXRMarkHandleRef, nullptr);
        ScanContext sc;
        sc.promotion = true;
        g_theGCToCLR->GcScanRoots(&LXRPromoteRoot, 2, 2, &sc);
        DrainClosure();
    }

    // Allocate-black: mark objects born during the window. Extend the mark table
    // to cover new allocations, then for each committed region mark every object
    // at/above the region's snapshot high-water (or all objects for regions that
    // did not exist at snapshot time).
    uint8_t* highWater = (g_lxrGCHeap != nullptr) ? g_lxrGCHeap->HeapHighWater()
                                                  : (m_heapBase + m_heapBytes);
    EnsureMarkCommitted(highWater);

    EnterCriticalSection(&g_chunkLock);
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed)
            continue;
        uint8_t* floor = c.Start; // regions born mid-trace: everything is new
        if (i < g_snapChunkCount && g_snapUsedEnd != nullptr && i < g_snapUsedEnd->size())
            floor = (*g_snapUsedEnd)[i];
        uint8_t* usedEnd = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        uint8_t* p = c.Start;
        while (p < usedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0)
                break;
            if (p >= floor)
            {
                // Allocate-black: retain objects born during the window. They must
                // be SCANNED, not merely mark-bit-set: a window-born object's ref
                // fields are filled by the mutator with arbitrary referents, and
                // under a Yuasa/SATB *deletion* barrier (which never greys the NEW
                // referent of a store) those referents are only otherwise reached
                // if snapshot-reachable. Pushing black objects onto the mark stack
                // traces their fields so any referent they solely keep alive is
                // marked. (With per-thread alloc contexts a black object can even
                // sit below the global high-water, so parentNew classification is
                // not a reliable proxy for "already scanned".)
                if (!IsMarked(o))
                {
                    PushMark(o);
                    InterlockedIncrement64(&g_lxrCounters.ConcAllocBlack);
                }
            }
            p += sz;
        }
    }
    LeaveCriticalSection(&g_chunkLock);

    // Trace the transitive closure of every allocate-black object just pushed, so
    // their referents (incl. snapshot-era objects they solely reference) are marked.
    DrainClosure();

    // FINAL ROOT RESCAN (the concurrent-mark completion pause). A Yuasa SATB
    // deletion barrier keeps every object reachable *through the heap* at snapshot
    // markable, but it does NOT cover a live object whose sole surviving reference
    // migrated into a mutator ROOT (a register/stack slot) during the window: the
    // snapshot scanned that thread's roots at their OLD values, and a plain root
    // overwrite fires no write barrier, so such an object is never greyed. .NET 11
    // runtime-async makes this concrete - DispatchContinuations walks a heap
    // continuation chain while advancing a root (asyncDispatcherInfo.NextContinuation),
    // and a resumed frame can hold the only reference to a snapshot-era object in a
    // register. Re-scanning ALL roots + handles here (mutators are STW-stopped)
    // re-greys anything a live root still points at, then we close over it. This is
    // the standard concurrent-collector final mark pause and is bounded by root
    // count + the tiny missed subgraph, NOT the live heap - so it REPLACES the
    // O(live-heap) CompleteClosureOverMarked backstop as the soundness guarantee.
    {
        LXRGCHandleStore::ForEachLiveHandle(&LXRMarkHandleRef, nullptr);
        ScanContext sc;
        sc.promotion = true;
        int64_t rescanBefore = g_finalRescanMarked;
        g_theGCToCLR->GcScanRoots(&LXRPromoteRootFinal, 2, 2, &sc);
        DrainClosure();
        int64_t rescanNew = g_finalRescanMarked - rescanBefore;
        if (rescanNew > 0)
        {
            InterlockedExchangeAdd64(&g_lxrCounters.FinalRescanMarked, rescanNew);
            static int s_verify = (getenv("LXR_VERIFY_TRACE") != nullptr) ? 1 : 0;
            if (s_verify)
                fprintf(stderr, "LXRGC: [final-rescan] rescued %lld live root-reachable object(s) the concurrent trace missed\n",
                        (long long)rescanNew);
        }
    }

    // Concurrent-marking-race reconciliation (LXR difference #3). The off-pause
    // drain can scan an object before a mutator installs a new reference into it;
    // under a Yuasa/SATB deletion barrier that new referent is not otherwise
    // greyed, so it can be transiently missed (empirically ~1 object/cycle).
    // Rather than reconcile with an O(live-heap) closure over every marked object
    // (the user's perf objection), mark the CURRENT value of every slot written
    // during the window - the coalescing-RC modified buffer already records each
    // written slot, so this is proportional to the mutation working set, not the
    // live heap. This makes the trace complete EVERY finish, so the sweep is
    // mark-authoritative every cycle (dead cycles collected promptly).
    //
    // The full O(live-heap) closure is retained ONLY as (a) an overflow fallback
    // when a mutator dropped a written slot (modified/SATB buffer full), and (b) a
    // verifier under LXR_VERIFY_TRACE that asserts the cheap reconciliation left
    // the trace complete (closureGap must be 0). In production with no overflow it
    // never runs.
    bool needFullClosure = g_satbOverflow || g_modifiedOverflow;
    int64_t raceMarked = 0;
    if (!needFullClosure)
    {
        raceMarked = MarkModifiedNewValues();
        DrainClosure();
    }
    InterlockedExchange(&g_traceCompleteThisCycle, 1);

    static int s_verify = -1;
    if (s_verify < 0) s_verify = (getenv("LXR_VERIFY_TRACE") != nullptr) ? 1 : 0;

    if (needFullClosure || s_verify)
    {
        int64_t closureGap = CompleteClosureOverMarked();
        if (closureGap != 0)
        {
            InterlockedExchangeAdd64(&g_lxrCounters.ClosureGapMarked, closureGap);
            if (s_verify)
                fprintf(stderr, "LXRGC: [conc-finish] closure marked %lld object(s) AFTER race-reconciliation (raceMarked=%lld fullFallback=%d) - reconciliation INCOMPLETE\n",
                        (long long)closureGap, (long long)raceMarked, needFullClosure ? 1 : 0);
        }
        if (s_verify)
        {
            fprintf(stderr, "LXRGC: [conc-finish] verifying after race-reconciliation (raceMarked=%lld gap=%lld)\n",
                    (long long)raceMarked, (long long)closureGap);
            VerifyTraceComplete();
        }
    }
    // the SATB buffers for the next cycle (safe: still STW here).
    SetSatbActive(false);
    InterlockedExchange(&g_traceWindowOpen, 0);
    ResetSatbBuffers();
}

void LXRCollector::SweepAndSelectDefrag()
{
    // Immix-style reclamation: any retired allocation region containing no
    // marked (reachable) object is fully dead; decommit its pages so committed
    // memory actually drops, and recycle it for future allocation. Regions with
    // some live objects are kept (LXR would evacuate/defragment them - future
    // work). Runs inside the same stop-the-world pause as BackupTrace so the
    // mark bits and region high-water marks are stable.
    EnterCriticalSection(&g_chunkLock);
    // Line reuse carves dead runs out of RETAINED regions on mark-authoritative
    // cycles only, when the line marks were populated this cycle and evacuation
    // (the alternative defragmentation strategy) is not running: on those cycles
    // every kept object is marked and so has its lines marked, making unmarked
    // lines provably dead. Snapshot the count so appended sub-regions (from a
    // split) are not re-scanned this pass.
    bool carveLines = (g_lineReuse > 0) && g_traceCompleteThisCycle &&
                      (g_lineMarksValid != 0) && (g_evacActive == 0) &&
                      (g_conservativeKeepAliveThisCycle == 0);
    size_t sweepCount = g_chunkCount;
    for (size_t i = 0; i < sweepCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr || c.FreeRun)
            continue; // uncommitted, active, or an already-carved free run

        // Region liveness via the mark bits directly (parse-independent). The
        // old linear object walk depended on LXRObjectSize correctly parsing
        // EVERY object from the region start; one misparse desynced the cursor
        // and could step over a marked (live) object, wrongly reclaiming and
        // reusing a live region - producing a dangling reference and a later
        // access violation in the next trace (seen on multi-GB continuously-
        // mutating graphs, e.g. the growing-cache workload). Mark bits are set
        // only at granule-aligned live-object starts, so "any mark bit set in
        // [Start,UsedEnd)" is an exact liveness test with no parsing.
        // RC-authoritative liveness (LXR difference #2). A region is reclaimable
        // only if it holds NO kept object. On a COMPLETE-trace cycle
        // (g_traceCompleteThisCycle: STW backup trace, or a concurrent finish
        // that ran CompleteClosureOverMarked) "kept" = marked, so unmarked dead
        // cycles - RC>=1 but unreachable - are reclaimed (mark-authoritative).
        // On a fast concurrent finish that skipped the closure, "kept" also
        // includes RC>=1: an object the concurrent SATB trace missed is still
        // referenced (RC>=1, e.g. a Kestrel MemoryPoolBlock held in a
        // ConcurrentQueue slot), so its region is kept without relying on the
        // (possibly incomplete) trace. Both tests are parse-free side-table
        // scans, so a misparse can never wrongly reclaim a live region.
        bool anyLive = AnyMarkedInRange(c.Start, c.UsedEnd) ||
                       (!g_traceCompleteThisCycle &&
                        (AnyRCNonZeroInRange(c.Start, c.UsedEnd) ||
                         IsYoung((Object*)c.Start)));
        if (anyLive)
        {
            // Retained region: recover its dead line runs for reuse (Immix line
            // recycling). CarveFreeRuns may realloc g_chunks, so do not touch 'c'
            // afterwards - continue to the next index.
            if (carveLines)
                CarveFreeRuns(i);
            continue;
        }

        // Red-handed check (LXR_VERIFY_TRACE): AnyMarkedInRange says this chunk
        // is dead. Linearly parse it and confirm no object carries a mark bit.
        // If one does, the sweep is about to reclaim a live region (addressing
        // or boundary bug); log it. If none does, the objects here are genuinely
        // unmarked (a missed-root / trace-completeness gap upstream).
        if (getenv("LXR_VERIFY_TRACE") != nullptr)
        {
            uint8_t* p = c.Start;
            uint8_t* end = c.UsedEnd;
            int parsedMarked = 0, parsedTotal = 0;
            while (p < end && parsedTotal < 1000000)
            {
                Object* o = (Object*)p;
                size_t sz = LXRObjectSize(o);
                if (sz == 0) break;
                parsedTotal++;
                if (IsMarked(o))
                {
                    parsedMarked++;
                    if (parsedMarked <= 4)
                        fprintf(stderr, "LXRGC: [sweep] RECLAIM chunk %p-%p size=%llu BUT marked obj at %p sz=%llu mt=%p\n",
                                (void*)c.Start, (void*)c.UsedEnd, (unsigned long long)c.Size,
                                (void*)o, (unsigned long long)sz, (void*)o->GetGCSafeMethodTable());
                }
                p += sz;
            }
            if (parsedMarked > 0)
                fprintf(stderr, "LXRGC: [sweep] *** reclaiming chunk with %d/%d MARKED objects (AnyMarkedInRange=false) ***\n",
                        parsedMarked, parsedTotal);
        }

        // Decommit the page-aligned interior of the dead region (the <=1 page
        // fringe at each end may hold a neighbor's object header, so leave it).
        uint8_t* dbeg = (uint8_t*)(((uintptr_t)c.Start + g_pageSize - 1) & ~((uintptr_t)g_pageSize - 1));
        uint8_t* dend = (uint8_t*)(((uintptr_t)(c.Start + c.Size)) & ~((uintptr_t)g_pageSize - 1));
        if (dend > dbeg)
        {
            VirtualFree(dbeg, dend - dbeg, MEM_DECOMMIT);
            InterlockedExchangeAdd64(&m_reclaimedBytes, (int64_t)(dend - dbeg));
            InterlockedExchangeAdd64(&g_committedInUse, -(int64_t)(dend - dbeg));
        }
        c.Committed = false;
        // Reclaimed => RC 0. Clear this region's RC bytes so no stale count
        // survives into the decommitted range (a later decrement of a
        // pre-sweep-logged old value would otherwise resurrect a dangling
        // pointer and fault in DrainZeroCountWorkList). Only [Start,UsedEnd)
        // ever held objects/RC; beyond UsedEnd the RC table is already zero.
        ClearRCRange(c.Start, c.UsedEnd);
        if (g_freeChunkTop == g_freeChunkCap)
        {
            size_t nc = g_freeChunkCap ? g_freeChunkCap * 2 : 256;
            size_t* grown = (size_t*)realloc(g_freeChunks, nc * sizeof(size_t));
            if (grown != nullptr) { g_freeChunks = grown; g_freeChunkCap = nc; }
        }
        if (g_freeChunkTop < g_freeChunkCap)
            g_freeChunks[g_freeChunkTop++] = i;
    }
    LeaveCriticalSection(&g_chunkLock);
    if (carveLines && getenv("LXR_VERIFY_TRACE") != nullptr)
        fprintf(stderr, "LXRGC: [sweep] line reuse: carved %lld run(s) / %lld MiB cumulative; freeRunStack=%llu\n",
                (long long)g_carveRunsTotal, (long long)(g_carveBytesTotal >> 20),
                (unsigned long long)g_freeRunTop);
}

void LXRCollector::SetEvacActive(bool active) { InterlockedExchange(&g_evacActive, active ? 1 : 0); }
bool LXRCollector::IsEvacActive() const { return g_evacActive != 0; }

void LXRCollector::EnsureMarkCommitted(uint8_t* addrEnd)
{
    if (addrEnd <= m_heapBase)
        return;
    size_t usedBytes = (size_t)(addrEnd - m_heapBase);
    size_t neededBytes = (usedBytes / lxr::kObjectGranule + 7) / 8;
    neededBytes = (neededBytes + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
    size_t cap = (m_heapBytes / lxr::kObjectGranule + 7) / 8;
    if (neededBytes > cap)
        neededBytes = cap;
    if (neededBytes > m_markCommittedBytes)
    {
        uint8_t* from = m_markTable + m_markCommittedBytes;
        size_t delta = neededBytes - m_markCommittedBytes;
        VirtualAlloc(from, delta, MEM_COMMIT, PAGE_READWRITE);
        memset(from, 0, delta);
        m_markCommittedBytes = neededBytes;
    }
}

// Pre-pass callbacks: pin every root/handle referent (never move it) so
// evacuation only ever has to forward heap references, not roots or handles.
static std::unordered_set<Object*>* g_evacPinned = nullptr;
// Set if an interior root could not be resolved during the evac pin pass: its
// target cannot be pinned and roots are not fixed up, so evacuation is skipped
// this cycle (sweep still runs). Rare (0 under all validated runs).
static volatile LONG g_evacUnresolvedInterior = 0;
static void LXRPinRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        o = g_lxrCollector.ResolveInterior((uint8_t*)o);
        if (o == nullptr)
        {
            InterlockedExchange(&g_evacUnresolvedInterior, 1);
            return;
        }
    }
    if (g_evacPinned != nullptr)
        g_evacPinned->insert(o);
}
static void LXRPinHandle(Object** ref, void* ctx)
{
    Object* o = *ref;
    if (o != nullptr)
        ((std::unordered_set<Object*>*)ctx)->insert(o);
}

// STW incremental evacuation (P3). Runs inside the trace pause, after
// BackupTrace has marked every live object. Relocates the live objects out of
// the most fragmented regions into fresh space and frees those regions.
void LXRCollector::Evacuate()
{
    if (g_lxrGCHeap == nullptr || g_theGCToCLR == nullptr)
        return;
    InterlockedIncrement64(&g_lxrCounters.EvacPasses);

    bool verbose = getenv("LXR_VERBOSE") != nullptr;

    // Policy knobs.
    static int64_t s_fragPct = -1, s_budgetBytes = -1;
    if (s_fragPct < 0)
    {
        const char* f = getenv("LXR_EVAC_FRAG_PCT");
        s_fragPct = f ? _atoi64(f) : 50;              // evacuate regions >= this % dead
        const char* b = getenv("LXR_EVAC_BUDGET_MB");
        s_budgetBytes = (b ? _atoi64(b) : 32) * (int64_t)1024 * 1024; // copy at most this per pause
    }

    // 1. Pin all root/handle referents (interior roots resolve to their base).
    std::unordered_set<Object*> pinned;
    InterlockedExchange(&g_evacUnresolvedInterior, 0);
    g_evacPinned = &pinned;
    ScanContext sc; sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRPinRoot, 2, 2, &sc);
    g_evacPinned = nullptr;
    LXRGCHandleStore::ForEachLiveHandle(&LXRPinHandle, &pinned);

    // If any interior root could not be resolved, its target is unpinned and
    // roots are not fixed up; moving anything risks dangling that root byref.
    // Skip evacuation this cycle (the sweep still reclaims dead regions).
    if (g_evacUnresolvedInterior != 0)
    {
        if (verbose) { fprintf(stderr, "LXRGC: [evac] skipped: unresolved interior root this cycle\n"); fflush(stderr); }
        return;
    }

    // 2. Select fragmented regions within the copy budget. Snapshot first so
    //    that registering destination chunks (which may realloc g_chunks) cannot
    //    invalidate the source list. Indices stay valid across realloc.
    struct EvacRegion { size_t index; uint8_t* start; uint8_t* usedEnd; };
    std::vector<EvacRegion> evac;
    int64_t liveBudget = s_budgetBytes;
    EnterCriticalSection(&g_chunkLock);
    for (size_t i = 0; i < g_chunkCount && liveBudget > 0; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr || c.UsedEnd <= c.Start)
            continue;
        size_t total = 0, live = 0;
        uint8_t* p = c.Start;
        bool parseOk = true;
        while (p < c.UsedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) { parseOk = false; break; }
            total += sz;
            if (IsMarked(o)) live += sz;
            p += sz;
        }
        if (!parseOk || total == 0 || live == 0 || live == total)
            continue; // unparseable, empty, fully dead (sweep handles), or fully live
        int64_t deadPct = (int64_t)((total - live) * 100 / total);
        if (deadPct < s_fragPct)
            continue;
        evac.push_back({ i, c.Start, c.UsedEnd });
        liveBudget -= (int64_t)live;
    }
    LeaveCriticalSection(&g_chunkLock);

    if (evac.empty())
    {
        if (verbose) { fprintf(stderr, "LXRGC: [evac] no fragmented regions selected\n"); fflush(stderr); }
        return;
    }

    // 3. Copy live, non-pinned objects into fresh destination chunks; record
    //    forwarding old->new. Pinned live objects are left in place.
    std::unordered_map<Object*, Object*> forwarding;
    // Interior/byref support (defect 3): also record each moved object's old
    // address range and new base, so a heap byref/interior pointer that lands
    // *inside* a moved object (e.g. a runtime-async continuation's captured `ref`
    // field) can be rebased preserving its offset, not just object-start refs.
    struct MovedRange { uint8_t* oldStart; uint8_t* oldEnd; uint8_t* newStart; };
    std::vector<MovedRange> movedRanges;
    uint8_t* destPtr = nullptr;
    uint8_t* destEnd = nullptr;
    int      curDestIndex = -1;
    auto evacAlloc = [&](size_t sz) -> uint8_t*
    {
        if (destPtr == nullptr || destPtr + sz > destEnd)
        {
            if (curDestIndex >= 0)
                g_chunks[curDestIndex].UsedEnd = destPtr; // finalize previous dest run
            size_t claim = (sz > CONTEXT_ALLOC_QUANTUM) ? sz : CONTEXT_ALLOC_QUANTUM;
            claim = (claim + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
            uint8_t* base = g_lxrGCHeap->ClaimBlocks(claim);
            if (base == nullptr)
                return nullptr;
            size_t committed = CommitRange(base, claim);
            InterlockedExchangeAdd64(&g_committedInUse, (int64_t)committed);
            curDestIndex = RegisterChunk(base, claim, nullptr);
            destPtr = base;
            destEnd = base + claim;
            EnsureMarkCommitted(destEnd);
        }
        uint8_t* r = destPtr;
        destPtr += sz;
        return r;
    };

    std::vector<size_t> freeableEvacIndices;
    for (const EvacRegion& er : evac)
    {
        size_t skipped = 0, moved = 0;
        uint8_t* p = er.start;
        while (p < er.usedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0)
                break;
            p += sz;
            if (!IsMarked(o))
                continue; // dead: freed with the region
            if (pinned.count(o) != 0)
            {
                skipped++; // live but pinned: must stay in place
                continue;
            }
            uint8_t* d = evacAlloc(sz);
            if (d == nullptr) { skipped++; continue; } // out of space: leave in place
            memcpy(d, o, sz);
            forwarding.emplace(o, (Object*)d);
            movedRanges.push_back({ (uint8_t*)o, (uint8_t*)o + sz, d });
            MarkObject((Object*)d);
            CommitPageFor(RCSlot((Object*)d));
            CommitPageFor(RCSlot(o));
            *RCSlot((Object*)d) = *RCSlot(o);
            *RCSlot(o) = 0; // source granule retired
            moved++;
            InterlockedIncrement64(&g_lxrCounters.EvacObjects);
            InterlockedExchangeAdd64(&g_lxrCounters.EvacBytesCopied, (int64_t)sz);
        }
        InterlockedExchangeAdd64(&g_lxrCounters.EvacPinnedSkipped, (int64_t)skipped);
        if (moved > 0)
            InterlockedIncrement64(&g_lxrCounters.EvacRegions);
        if (skipped == 0 && moved > 0)
            freeableEvacIndices.push_back(er.index);
    }
    if (curDestIndex >= 0)
        g_chunks[curDestIndex].UsedEnd = destPtr; // finalize last dest run

    // 4. Fix up every heap reference to a moved object. Roots/handles need no
    //    fix-up (their referents were pinned). Walk all live objects (including
    //    the freshly copied destinations) and forward their fields. Forwarded
    //    source objects are dead and skipped.
    //
    //    Both object-start references and *interior/byref* pointers are handled:
    //    GCScanObjectRefs visits objref and byref slots identically, so a field
    //    value may point at a moved object's start (objref) or into its interior
    //    (a heap byref, e.g. a runtime-async continuation's captured `ref`).
    //    Sort the moved ranges by old address so an interior value can be located
    //    by binary search and rebased preserving its offset (defect 3). Without
    //    this, interior byrefs into moved objects dangle -> NRE in runtime-async.
    std::sort(movedRanges.begin(), movedRanges.end(),
              [](const MovedRange& a, const MovedRange& b) { return a.oldStart < b.oldStart; });
    auto rebaseField = [&forwarding, &movedRanges](Object** f)
    {
        uint8_t* v = (uint8_t*)*f;
        if (v == nullptr)
            return;
        // Fast path: exact object-start reference to a moved object.
        auto it = forwarding.find((Object*)v);
        if (it != forwarding.end())
        {
            *f = it->second;
            InterlockedIncrement64(&g_lxrCounters.EvacFieldsForwarded);
            return;
        }
        // Interior/byref: find the moved object whose [oldStart,oldEnd) strictly
        // contains v (v==oldStart is the exact case handled above). upper_bound
        // gives the first range with oldStart > v; its predecessor is the only
        // candidate whose oldStart <= v.
        if (movedRanges.empty())
            return;
        size_t lo = 0, hi = movedRanges.size();
        while (lo < hi) // first index with oldStart > v
        {
            size_t mid = (lo + hi) >> 1;
            if (movedRanges[mid].oldStart <= v) lo = mid + 1; else hi = mid;
        }
        if (lo == 0)
            return; // no range starts at/below v
        const MovedRange& r = movedRanges[lo - 1];
        if (v > r.oldStart && v < r.oldEnd)
        {
            *f = (Object*)(r.newStart + (v - r.oldStart));
            InterlockedIncrement64(&g_lxrCounters.EvacFieldsForwarded);
        }
    };
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed)
            continue;
        uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        uint8_t* p = c.Start;
        while (p < end)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0)
                break;
            p += sz;
            if (forwarding.find(o) != forwarding.end())
                continue; // dead source
            if (!IsMarked(o))
                continue; // unreachable garbage: sweep will handle
            GCScanObjectRefs(o, sz, rebaseField);
        }
    }

    // 4b. VERIFY (LXR_VERIFY_TRACE): before freeing any source region, confirm
    //     step 4 forwarded EVERY heap reference to a moved object. Any marked,
    //     non-source object whose field still points at a forwarding source is a
    //     miss that would dangle once the source region is decommitted. Log the
    //     referrer (region/owner/offset/MT) so the structural gap is pinpointed.
    if (getenv("LXR_VERIFY_TRACE") != nullptr)
    {
        int64_t misses = 0;
        for (size_t i = 0; i < g_chunkCount; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed)
                continue;
            uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p;
                size_t sz = LXRObjectSize(o);
                if (sz == 0)
                    break;
                uint8_t* op = p;
                p += sz;
                if (forwarding.find(o) != forwarding.end() || !IsMarked(o))
                    continue;
                bool owner = (c.Owner != nullptr);
                GCScanObjectRefs(o, sz, [&](Object** f)
                {
                    if (forwarding.find(*f) != forwarding.end())
                    {
                        if (misses < 20)
                            fprintf(stderr, "LXRGC: [verify-evac] UNFORWARDED ref: referrer=%p mt=%p region=%zu owner=%d off=%lld -> stale %p (fwd->%p)\n",
                                    (void*)o, (void*)o->GetGCSafeMethodTable(), i, (int)owner,
                                    (long long)((uint8_t*)f - op), (void*)*f, (void*)forwarding[*f]);
                        misses++;
                    }
                });
            }
        }
        if (misses > 0)
        {
            fprintf(stderr, "LXRGC: [verify-evac] TOTAL unforwarded refs = %lld (freed %zu source regions)\n",
                    (long long)misses, freeableEvacIndices.size());
            fflush(stderr);
        }
    }

    // 5. Free fully-evacuated regions (no pinned/left-behind live object).
    EnterCriticalSection(&g_chunkLock);
    for (size_t idx : freeableEvacIndices)
    {
        ChunkRegion& c = g_chunks[idx];
        if (!c.Committed)
            continue;
        uint8_t* dbeg = (uint8_t*)(((uintptr_t)c.Start + g_pageSize - 1) & ~((uintptr_t)g_pageSize - 1));
        uint8_t* dend = (uint8_t*)(((uintptr_t)(c.Start + c.Size)) & ~((uintptr_t)g_pageSize - 1));
        if (dend > dbeg)
        {
            VirtualFree(dbeg, dend - dbeg, MEM_DECOMMIT);
            InterlockedExchangeAdd64(&m_reclaimedBytes, (int64_t)(dend - dbeg));
            InterlockedExchangeAdd64(&g_committedInUse, -(int64_t)(dend - dbeg));
        }
        c.Committed = false;
        // Reclaimed => RC 0 (see the sweep decommit site). Bounded to the used
        // extent that actually held objects/RC.
        ClearRCRange(c.Start, c.UsedEnd);
        if (g_freeChunkTop == g_freeChunkCap)
        {
            size_t nc = g_freeChunkCap ? g_freeChunkCap * 2 : 256;
            size_t* grown = (size_t*)realloc(g_freeChunks, nc * sizeof(size_t));
            if (grown != nullptr) { g_freeChunks = grown; g_freeChunkCap = nc; }
        }
        if (g_freeChunkTop < g_freeChunkCap)
            g_freeChunks[g_freeChunkTop++] = idx;
    }
    LeaveCriticalSection(&g_chunkLock);

    if (verbose)
    {
        fprintf(stderr, "LXRGC: [evac] regions=%zu moved=%lld bytes=%lld pinnedSkipped=%lld fieldsForwarded=%lld freed=%zu\n",
                evac.size(), (long long)g_lxrCounters.EvacObjects, (long long)g_lxrCounters.EvacBytesCopied,
                (long long)g_lxrCounters.EvacPinnedSkipped, (long long)g_lxrCounters.EvacFieldsForwarded,
                freeableEvacIndices.size());
        fflush(stderr);
    }
}

// --- Item D: young/nursery collection at RC pauses -------------------------
// Bounded closure state, published to the file-scope root/handle callbacks the
// same way Evacuate publishes g_evacPinned (GcScanRoots takes only a fn-ptr).
struct NurseryClosure
{
    std::unordered_set<Object*> live;   // young object starts proven reachable
    std::vector<Object*>        work;   // worklist for the transitive young->young scan
    void AddYoung(Object* base)
    {
        if (base != nullptr && live.insert(base).second)
            work.push_back(base);
    }
};
static NurseryClosure* g_nurseryClosure = nullptr;

// Seed one candidate pointer (a root, handle, or remembered-set value). Only
// pointers into a young block matter; resolve interior/byref to the containing
// young object's start so the transitive scan parses it correctly.
static void LXRNurserySeed(uint8_t* v)
{
    if (v == nullptr || g_nurseryClosure == nullptr)
        return;
    // O(1) reject: not in a young (this-epoch) block -> irrelevant to the nursery.
    if (!g_lxrCollector.IsYoung((Object*)v))
        return;
    // Common case: exact object-start reference we already know is live.
    if (g_nurseryClosure->live.count((Object*)v) != 0)
        return;
    Object* base = g_lxrCollector.ResolveInterior(v);
    if (base != nullptr && g_lxrCollector.IsYoung(base))
        g_nurseryClosure->AddYoung(base);
}

static void LXRNurseryRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    // Interior roots are handled uniformly by LXRNurserySeed -> ResolveInterior,
    // so GC_CALL_INTERIOR needs no special case here.
    (void)flags;
    LXRNurserySeed((uint8_t*)o);
}
static void LXRNurseryHandle(Object** ref, void* /*ctx*/)
{
    LXRNurserySeed((uint8_t*)*ref);
}
static void LXRNurseryRemsetSlot(Object** slot, void* /*ctx*/)
{
    // Re-read the slot: a remembered-set entry only means this slot *once* held an
    // inter-block pointer; its current value is what keeps a young object live.
    LXRNurserySeed((uint8_t*)*slot);
}

// Young/nursery collection, run under the RC-pause STW. Reclaims young regions
// that hold no reachable young object. Reclaim-only (no copying): survivors stay
// young and are compacted/promoted by the trace-cycle Evacuate. Sound because the
// closure is a *complete* over-approximation of young liveness (roots + handles +
// the complete mature->young remembered set + young->young), so a reclaimed
// region provably has no live referrer; guarded off whenever completeness cannot
// be guaranteed.
void LXRCollector::CollectNursery()
{
    if (!g_youngRC || !g_nurseryActive || !g_remsetActive || g_theGCToCLR == nullptr)
        return;
    // Completeness guards. A dropped mature->young edge (remset overflow) or an
    // in-flight concurrent trace (whose marks/allocate-black also keep young
    // alive) would make freeing young unsafe -> fall back to the authoritative
    // trace, which reclaims young at the next trace cycle as today.
    if (g_remsetOverflow || g_traceWindowOpen)
    {
        InterlockedIncrement64(&g_lxrCounters.NurserySkipped);
        return;
    }

    bool verbose = getenv("LXR_VERBOSE") != nullptr;

    // 1. Build the young-live closure. Seeds: roots, handles, and every current
    //    mature->young (and young->young) remembered-set value.
    NurseryClosure closure;
    g_nurseryClosure = &closure;
    ScanContext sc; sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRNurseryRoot, 2, 2, &sc);
    LXRGCHandleStore::ForEachLiveHandle(&LXRNurseryHandle, nullptr);
    EnumerateRemsetSlots(&LXRNurseryRemsetSlot, nullptr);

    // DIAGNOSTIC (LXR_NURSERY_FULLSCAN): seed young-liveness from EVERY object's
    // fields via a full-heap walk instead of trusting the remembered set. This is
    // O(heap) and only for isolating a completeness gap: if the AV disappears with
    // this on, the remset is missing a mature->young edge (a write-barrier gap);
    // if it persists, the bug is in the closure/reclaim itself.
    static int s_fullScan = (getenv("LXR_NURSERY_FULLSCAN") != nullptr) ? 1 : 0;
    if (s_fullScan)
    {
        EnterCriticalSection(&g_chunkLock);
        size_t n = g_chunkCount;
        for (size_t i = 0; i < n; i++)
        {
            ChunkRegion c = g_chunks[i];
            if (!c.Committed)
                continue;
            uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p;
                size_t sz = LXRObjectSize(o);
                if (sz == 0) break;
                p += sz;
                GCScanObjectRefs(o, sz, [](Object** ref) { LXRNurserySeed((uint8_t*)*ref); });
            }
        }
        LeaveCriticalSection(&g_chunkLock);
    }

    // 2. Transitive young->young closure. Parsing a young object and following its
    //    fields is safe: nothing is reclaimed yet, so every young region is still
    //    committed. Over-retention (e.g. a field of a still-committed dead young
    //    object) is harmless.
    while (!closure.work.empty())
    {
        Object* o = closure.work.back();
        closure.work.pop_back();
        size_t sz = LXRObjectSize(o);
        if (sz == 0)
            continue;
        GCScanObjectRefs(o, sz, [](Object** ref) { LXRNurserySeed((uint8_t*)*ref); });
    }
    g_nurseryClosure = nullptr;
    InterlockedExchange64(&g_lxrCounters.NurseryLiveYoung, (int64_t)closure.live.size());

    // 3. Reclaim young regions with no reachable young object (region-granular,
    //    Immix-style). Mirrors the sweep decommit path.
    int64_t regionsReclaimed = 0, bytesReclaimed = 0;
    EnterCriticalSection(&g_chunkLock);
    size_t sweepCount = g_chunkCount;
    for (size_t i = 0; i < sweepCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr || c.FreeRun)
            continue;                       // uncommitted, active, or a carved free run
        if (c.UsedEnd <= c.Start)
            continue;
        if (!IsYoung((Object*)c.Start))
            continue;                       // mature region: handled by the trace, not here

        // Any reachable young object in [Start,UsedEnd) keeps the whole region.
        bool anyLive = false;
        uint8_t* p = c.Start;
        while (p < c.UsedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) { anyLive = true; break; } // unparseable: keep, never free
            if (closure.live.count(o) != 0) { anyLive = true; break; }
            p += sz;
        }
        if (anyLive)
            continue;

        // Dead young region: decommit its page-aligned interior and recycle it.
        uint8_t* dbeg = (uint8_t*)(((uintptr_t)c.Start + g_pageSize - 1) & ~((uintptr_t)g_pageSize - 1));
        uint8_t* dend = (uint8_t*)(((uintptr_t)(c.Start + c.Size)) & ~((uintptr_t)g_pageSize - 1));
        if (dend > dbeg)
        {
            VirtualFree(dbeg, dend - dbeg, MEM_DECOMMIT);
            InterlockedExchangeAdd64(&m_reclaimedBytes, (int64_t)(dend - dbeg));
            InterlockedExchangeAdd64(&g_committedInUse, -(int64_t)(dend - dbeg));
            bytesReclaimed += (int64_t)(dend - dbeg);
        }
        c.Committed = false;
        // Reclaimed => RC 0 (see the sweep decommit site): young objects are RC-
        // exempt so their RC bytes should already be 0, but clear defensively so
        // no stale count survives into the decommitted range.
        ClearRCRange(c.Start, c.UsedEnd);
        if (g_freeChunkTop == g_freeChunkCap)
        {
            size_t nc = g_freeChunkCap ? g_freeChunkCap * 2 : 256;
            size_t* grown = (size_t*)realloc(g_freeChunks, nc * sizeof(size_t));
            if (grown != nullptr) { g_freeChunks = grown; g_freeChunkCap = nc; }
        }
        if (g_freeChunkTop < g_freeChunkCap)
            g_freeChunks[g_freeChunkTop++] = i;
        regionsReclaimed++;
    }
    LeaveCriticalSection(&g_chunkLock);

    InterlockedIncrement64(&g_lxrCounters.NurseryPasses);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryRegionsReclaimed, regionsReclaimed);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryBytesReclaimed, bytesReclaimed);
    if (verbose)
    {
        fprintf(stderr, "LXRGC: [nursery] liveYoung=%zu regionsReclaimed=%lld bytesReclaimed=%lld\n",
                closure.live.size(), (long long)regionsReclaimed, (long long)bytesReclaimed);
        fflush(stderr);
    }
}

// ===========================================================================
//                              LXRGCHeap
// ===========================================================================
LXRGCHeap* LXRGCHeap::CreateAndInitialize()
{
    return new (nothrow) LXRGCHeap();
}

// Generic pluggable write-barrier callback (WriteBarrierKind::Callback).
//
// The runtime invokes this on every in-heap reference-field store AFTER it has
// performed the store, handing us the slot, the new value, and the OLD value it
// just overwrote. Capturing that old value is precisely the ingredient LXR's
// coalescing reference-counting barrier needs, and precisely what the standalone
// GC ABI could not previously surface (see FEASIBILITY.md). Runs in cooperative
// mode from the barrier, so it stays leaf-like: it only appends to the calling
// thread's modified buffer.
static void LXRWriteBarrierCallback(Object** slot, Object* newValue, Object* oldValue)
{
    g_lxrCollector.LogModifiedField(slot, oldValue, newValue);
}

// Generic pluggable *bulk* write-barrier callback (WriteBarrierKind::Callback).
//
// Bulk GC-reference moves (Array.Copy, span copies, struct block-copies lowered
// to CORINFO_HELP_BULK_WRITEBARRIER) bypass the JIT single-slot write barrier, so
// without this hook LXR would never see the old referents they overwrite nor the
// new referents they install - the concrete unsoundness that let a concurrent
// SATB trace free still-reachable objects. The runtime invokes this BEFORE the
// copy overwrites the destination, so both dest[i] (old) and src[i] (new) are
// still readable. We replay each slot through the same coalescing-RC + SATB +
// remembered-set logging as the single-slot barrier. Leaf-like (no allocation
// beyond the once-per-thread buffer first-touch, matching the single-slot path).
static void LXRBulkWriteBarrierCallback(Object** dest, Object** src, size_t byteCount)
{
    size_t slots = byteCount / sizeof(Object*);
    for (size_t i = 0; i < slots; i++)
    {
        // old = current dest[i] (about to be overwritten); new = src[i].
        g_lxrCollector.LogModifiedField(&dest[i], dest[i], src[i]);
    }
}

HRESULT LXRGCHeap::Initialize()
{
    InitializeCriticalSection(&g_frozenSegmentsLock);
    QueryPerformanceFrequency(&m_qpcFrequency);
    QueryPerformanceCounter(&m_startTime);

    m_heapBase = (uint8_t*)VirtualAlloc(nullptr, HEAP_RESERVE_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (m_heapBase == nullptr)
        return E_OUTOFMEMORY;
    m_heapReservedEnd = m_heapBase + HEAP_RESERVE_SIZE;
    m_heapNextFree = m_heapBase;

    if (!g_lxrCollector.Initialize(m_heapBase, HEAP_RESERVE_SIZE))
        return E_OUTOFMEMORY;

    // Wire up the card table exactly like ZeroGC so the JIT-emitted write
    // barrier never faults. NOTE: for LXR this card table is a poor
    // substitute for the field-logging barrier - it records WHICH cards were
    // dirtied but NOT the OLD field values LXR needs for coalescing RC. See
    // FEASIBILITY.md.
    const int card_byte_shift = 11; // 2048 bytes/card (matches JIT_WriteBarrier: shr rcx,0Bh)
    uintptr_t lowCardIndex = (uintptr_t)m_heapBase >> card_byte_shift;
    uintptr_t highCardIndex = (uintptr_t)m_heapReservedEnd >> card_byte_shift;
    size_t cardTableSize = (highCardIndex - lowCardIndex) + 1;
    uint8_t* cardTableRaw = (uint8_t*)calloc(cardTableSize, 1);
    if (cardTableRaw == nullptr)
        return E_OUTOFMEMORY;
    uint8_t* cardTableBiased = cardTableRaw - lowCardIndex;

    const int card_bundle_byte_shift = 21;
    uint8_t* cardBundleBiased = nullptr;
#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    {
        uintptr_t lowBundleIndex = (uintptr_t)m_heapBase >> card_bundle_byte_shift;
        uintptr_t highBundleIndex = (uintptr_t)m_heapReservedEnd >> card_bundle_byte_shift;
        size_t cardBundleSize = (highBundleIndex - lowBundleIndex) + 1;
        uint8_t* cardBundleRaw = (uint8_t*)calloc(cardBundleSize, 1);
        if (cardBundleRaw == nullptr)
            return E_OUTOFMEMORY;
        cardBundleBiased = cardBundleRaw - lowBundleIndex;
    }
#endif

    WriteBarrierParameters wbParams = {};
    wbParams.operation = WriteBarrierOp::Initialize;
    wbParams.is_runtime_suspended = true;
    wbParams.requires_upper_bounds_check = false;
    wbParams.card_table = (uint32_t*)cardTableBiased;
    wbParams.card_bundle_table = (uint32_t*)cardBundleBiased;
    wbParams.lowest_address = m_heapBase;
    wbParams.highest_address = m_heapReservedEnd;
    wbParams.ephemeral_low = (uint8_t*)1;
    wbParams.ephemeral_high = (uint8_t*)~(uintptr_t)0;
    wbParams.region_to_generation_table = nullptr;
    wbParams.region_shr = 0;
    wbParams.region_use_bitwise_write_barrier = false;

    if (g_theGCToCLR != nullptr)
        g_theGCToCLR->StompWriteBarrier(&wbParams);

    // Cache the runtime's free-object MethodTable so we can write parseable
    // "plugs" over dead byte ranges (see PlugFreeRange). This is what keeps the
    // linear heap walk in sync after Immix line reuse overwrites part of a dead
    // run with fresh allocations. GetFreeObjectMethodTable is part of the
    // standalone GC-to-EE interface (no runtime change needed).
    if (g_theGCToCLR != nullptr && g_freeObjectMT == nullptr)
    {
        g_freeObjectMT = (MethodTable*)g_theGCToCLR->GetFreeObjectMethodTable();
        if (g_freeObjectMT != nullptr)
            g_freeObjectBaseSize = g_freeObjectMT->GetBaseSize();
    }
    if (g_lineReuse < 0)
        g_lineReuse = (getenv("LXR_LINE_REUSE") != nullptr) ? 1 : 0;
    if (g_lineReuseMinBytes == 0)
    {
        const char* e = getenv("LXR_LINE_MIN");
        int64_t mb = e ? _atoi64(e) : 0;
        g_lineReuseMinBytes = (mb > 0) ? (size_t)mb : (32 * lxr::kLineSize); // default 8 KiB
    }

    // Upgrade from the card-marking barrier to the generic, GC-agnostic Callback
    // barrier now exposed by the runtime. From here on the runtime hands us the
    // overwritten (old) value on every reference-field store, which drives LXR's
    // coalescing reference-counting engine. This is the pluggable-write-barrier
    // runtime change that resolves the wall documented in FEASIBILITY.md.
    if (g_theGCToCLR != nullptr && getenv("LXR_NO_CALLBACK_BARRIER") == nullptr)
    {
        WriteBarrierParameters cb = {};
        cb.operation = WriteBarrierOp::SwitchToCustomBarrier;
        cb.is_runtime_suspended = true;
        cb.write_barrier_kind = WriteBarrierKind::Callback;
        cb.write_barrier_callback = &LXRWriteBarrierCallback;
        // Also observe bulk GC-ref moves (arrays/spans/struct copies) that bypass
        // the single-slot JIT barrier - required for a complete SATB snapshot and
        // correct coalescing RC under real workloads. Gated off by LXR_NO_BULK_BARRIER
        // for A/B measurement of its cost/soundness contribution.
        if (getenv("LXR_NO_BULK_BARRIER") == nullptr)
            cb.write_barrier_bulk_callback = &LXRBulkWriteBarrierCallback;
        // Ask the JIT to barrier EVERY in-heap ref-field store, including
        // `obj.field = null` and frozen-constant stores that the card barrier
        // elides. These are SATB referent deletions the concurrent trace must
        // observe (see FEASIBILITY.md). Gate off for A/B via LXR_NO_FULL_REF_BARRIERS.
        if (getenv("LXR_NO_FULL_REF_BARRIERS") == nullptr)
            cb.write_barrier_requires_all_ref_stores = true;
        cb.lowest_address = m_heapBase;
        cb.highest_address = m_heapReservedEnd;
        cb.card_table = (uint32_t*)cardTableBiased;
        cb.card_bundle_table = (uint32_t*)cardBundleBiased;
        g_theGCToCLR->StompWriteBarrier(&cb);
    }

    for (int i = 0; i < MAX_FROZEN_SEGMENTS; i++)
        g_frozenSegments[i].InUse = false;

    // Bring up the dedicated collector thread so every stop-the-world cycle runs
    // on a single non-suspendable GC thread (see RequestLXRCollection). Created
    // via the runtime's GC-thread facility with is_suspendable=false, exactly as
    // the built-in Server/Background GC threads are.
    if (g_theGCToCLR != nullptr && g_collectRequestEvent == nullptr)
    {
        g_collectRequestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_collectDoneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_theGCToCLR->CreateThread(&LXRCollectorThreadProc, nullptr, /*is_suspendable*/ false, ".NET LXR GC"))
        {
            // If the runtime refused to create the thread, fall back to inline
            // collection (correctness over the concurrency optimization).
            CloseHandle(g_collectRequestEvent); g_collectRequestEvent = nullptr;
            CloseHandle(g_collectDoneEvent);    g_collectDoneEvent = nullptr;
        }
        if (getenv("LXR_WATCHDOG") != nullptr)
        {
            QueryPerformanceFrequency(&g_lxrQpcFreq);
            InterlockedExchange(&g_lxrWatchdog, 1);
            g_theGCToCLR->CreateThread(&LXRWatchdogThreadProc, nullptr, /*is_suspendable*/ false, ".NET LXR watchdog");
        }
        (void)&LXRWatchdogThreadProc;
        // Diagnostic (LXR_AV_STACKS=1): install a first-chance handler that dumps
        // the faulting thread's native stack on an AV inside the LXR heap (a UAF).
        if (getenv("LXR_AV_STACKS") != nullptr)
            AddVectoredExceptionHandler(1, &LXRAvVectoredHandler);

        // Bring up the persistent parallel-mark worker pool now (init time, not
        // during a pause) so parallel drains never create threads under STW.
        EnsureMarkWorkerPool();
    }

    return S_OK;
}

uint8_t* LXRGCHeap::ClaimBlocks(size_t bytes)
{
    uint8_t* oldNext = (uint8_t*)InterlockedExchangeAdd64((volatile LONG64*)&m_heapNextFree, (LONG64)bytes);
    if (oldNext + bytes > m_heapReservedEnd)
        return nullptr;
    return oldNext;
}

Object* LXRGCHeap::AllocateSlow(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    // Provision this thread's write-barrier buffers here (safe frame), so the
    // cooperative-mode leaf barrier never has to allocate (see EnsureThreadBuffers
    // / LogModifiedField): a mutator frozen inside malloc in the barrier would
    // hang SuspendEE. Every heap-ref store is preceded by an allocation, so the
    // buffers are always ready by the time the barrier logs.
    EnsureThreadBuffers();

    size_t alignedSize = (size + 7) & ~(size_t)7;
    const size_t headerPad = sizeof(void*);

    bool isLarge = (flags & GC_ALLOC_LARGE_OBJECT_HEAP) != 0 || (flags & GC_ALLOC_PINNED_OBJECT_HEAP) != 0
        || alignedSize >= LARGE_OBJECT_SIZE;
    size_t chunkSize = isLarge ? alignedSize : max(alignedSize, CONTEXT_ALLOC_QUANTUM);
    size_t reserveSize = chunkSize + headerPad;

    ThreadHeapState& th = t_threadHeap;

    // Allocation-triggered collection policy: run a full STW LXR collection once
    // committed-in-use has grown by an adaptive budget since the last cycle. The
    // budget is max(LXR_GC_TRIGGER_MB floor, LXR_GC_GROWTH_PCT % of the live heap
    // retained after the previous cycle). Making the budget scale with the live
    // heap is what a real generational/region GC does (allocation budget is a
    // fraction of survivors); a fixed delta would collect every N MiB even for a
    // multi-GB live heap, making each O(heap) trace+sweep fire constantly and
    // driving overall cost quadratic. This keeps collection overhead amortized.
    if (g_gcTriggerBytes < 0)
    {
        const char* e = getenv("LXR_GC_TRIGGER_MB");
        int64_t mb = e ? _atoi64(e) : 32; // floor: collect at least every +32 MiB of growth
        g_gcTriggerBytes = mb * (int64_t)1024 * 1024;
        const char* pctEnv = getenv("LXR_GC_GROWTH_PCT");
        g_gcGrowthPct = pctEnv ? _atoi64(pctEnv) : 50; // grow the heap by 50% before collecting
    }
    if (g_theGCToCLR != nullptr && g_gcTriggerBytes > 0 && g_inCollection == 0)
    {
        int64_t live = g_lxrCounters.LastCollectCommitted;
        int64_t budget = g_gcTriggerBytes;
        int64_t adaptive = (live * g_gcGrowthPct) / 100;
        if (adaptive > budget) budget = adaptive;
        int64_t grown = g_committedInUse - live;
        if (grown >= budget)
            RequestLXRCollection(/*wait*/ false, /*forceTrace*/ false);
    }

    // Retire the context chunk this thread was filling: its high-water mark is
    // the exhausted context's alloc_ptr. This makes [Start, UsedEnd) a
    // parseable run of complete objects for the trace/sweep.
    if (th.CurrentChunkIndex >= 0)
    {
        FinalizeChunk(th.CurrentChunkIndex, acontext->alloc_ptr);
        th.CurrentChunkIndex = -1;
    }

    uint8_t* chunkStart = nullptr;
    int newIndex = -1;

    // Block reuse: prefer a previously reclaimed standard-size region.
    if (!isLarge && chunkSize == CONTEXT_ALLOC_QUANTUM)
        chunkStart = ReuseChunk(acontext, &newIndex);

    // Immix line reuse: else hand out a carved dead line run before extending the
    // committed heap (its pages are already committed, so this offsets heap
    // growth). Small-object allocation nominally wants a 128 KiB quantum chunk,
    // but a run only needs to fit the CURRENT object; the whole run is then
    // adopted as this chunk and bump-filled by subsequent fast-path allocations.
    // Requiring only object-fit (not the full quantum) is what lets the many
    // small carved runs actually be consumed.
    if (chunkStart == nullptr && !isLarge && g_lineReuse > 0)
    {
        size_t runObjSize = 0;
        uint8_t* r = ReuseFreeRun(acontext, alignedSize + headerPad, headerPad, &newIndex, &runObjSize);
        if (r != nullptr)
        {
            chunkStart = r;
            chunkSize = runObjSize;
        }
    }

    if (chunkStart == nullptr)
    {
        bool needsNewRun = (th.RunBase == nullptr) || ((size_t)(th.RunEnd - th.NextFree) < reserveSize);
        if (needsNewRun)
        {
            size_t claimSize = max(reserveSize, THREAD_BLOCK_RUN);
            claimSize = (claimSize + lxr::kBlockSize - 1) & ~(lxr::kBlockSize - 1); // whole Immix blocks
            uint8_t* claimBase = ClaimBlocks(claimSize);
            if (claimBase == nullptr)
                return nullptr;
            th.RunBase = claimBase;
            th.RunEnd = claimBase + claimSize;
            th.NextFree = claimBase;
            th.CommitEnd = claimBase;
            InterlockedExchangeAdd64(&g_lxrCounters.BlocksAllocated, (int64_t)(claimSize / lxr::kBlockSize));
        }

        if ((size_t)(th.CommitEnd - th.NextFree) < reserveSize)
        {
            size_t needed = reserveSize - (th.CommitEnd - th.NextFree);
            size_t commitSize = max(needed, COMMIT_CHUNK);
            commitSize = (commitSize + 0xFFFF) & ~(size_t)0xFFFF;
            if (th.CommitEnd + commitSize > th.RunEnd)
                commitSize = th.RunEnd - th.CommitEnd;
            if (VirtualAlloc(th.CommitEnd, commitSize, MEM_COMMIT, PAGE_READWRITE) == nullptr)
                return nullptr;
            th.CommitEnd += commitSize;
            InterlockedExchangeAdd64(&g_committedInUse, (int64_t)commitSize);
            // Commit the covering unlogged-bit (logged-table) pages for the newly
            // committed heap here, off the barrier, so the cooperative-mode barrier
            // never faults on or has to commit a reserved bitmap page.
            g_lxrCollector.EnsureLoggedUpTo(th.CommitEnd);
        }

        uint8_t* rawStart = th.NextFree;
        chunkStart = rawStart + headerPad;
        th.NextFree = rawStart + reserveSize;
        newIndex = RegisterChunk(chunkStart, chunkSize, acontext);
    }

    th.CurrentChunkIndex = newIndex;

    if (!isLarge)
    {
        acontext->alloc_ptr = chunkStart + alignedSize;
        acontext->alloc_limit = chunkStart + chunkSize;
    }
    else
    {
        acontext->alloc_ptr = chunkStart + alignedSize;
        acontext->alloc_limit = chunkStart + alignedSize;
    }

    acontext->alloc_bytes += (int64_t)alignedSize;
    acontext->alloc_count++;

    InterlockedExchangeAdd64(&g_lxrCounters.TotalAllocatedBytes, (int64_t)alignedSize);

    // In a working LXR the new object is born with RC=0 and stuck-if-young;
    // it gains references only through the write barrier / root scan. Because
    // that barrier never fires here, we deliberately do NOT touch the RC table
    // on the allocation fast path.
    return (Object*)chunkStart;
}

Object* LXRGCHeap::Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    return AllocateSlow(acontext, size, flags);
}

void LXRGCHeap::PublishObject(uint8_t* obj) { }
void LXRGCHeap::SetWaitForGCEvent() { }
void LXRGCHeap::ResetWaitForGCEvent() { }

bool LXRGCHeap::IsValidSegmentSize(size_t size) { return (size & (size - 1)) == 0; }
bool LXRGCHeap::IsValidGen0MaxSize(size_t size) { return true; }
size_t LXRGCHeap::GetValidSegmentSize(bool large_seg) { return COMMIT_CHUNK; }
void LXRGCHeap::SetReservedVMLimit(size_t vmlimit) { }

void LXRGCHeap::WaitUntilConcurrentGCComplete() { }
bool LXRGCHeap::IsConcurrentGCInProgress() { return false; }
void LXRGCHeap::TemporaryEnableConcurrentGC() { }
void LXRGCHeap::TemporaryDisableConcurrentGC() { }
bool LXRGCHeap::IsConcurrentGCEnabled() { return false; }
HRESULT LXRGCHeap::WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout) { return S_OK; }

size_t LXRGCHeap::GetNumberOfFinalizable() { return 0; }
Object* LXRGCHeap::GetNextFinalizable() { return nullptr; }

void LXRGCHeap::GetMemoryInfo(uint64_t* highMemLoadThresholdBytes,
                              uint64_t* totalAvailableMemoryBytes,
                              uint64_t* lastRecordedMemLoadBytes,
                              uint64_t* lastRecordedHeapSizeBytes,
                              uint64_t* lastRecordedFragmentationBytes,
                              uint64_t* totalCommittedBytes,
                              uint64_t* promotedBytes,
                              uint64_t* pinnedObjectCount,
                              uint64_t* finalizationPendingCount,
                              uint64_t* index,
                              uint32_t* generation,
                              uint32_t* pauseTimePct,
                              bool* isCompaction,
                              bool* isConcurrent,
                              uint64_t* genInfoRaw,
                              uint64_t* pauseInfoRaw,
                              int kind)
{
    MEMORYSTATUSEX memStatus = {};
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    size_t committed = GetTotalBytesInUse();

    if (highMemLoadThresholdBytes) *highMemLoadThresholdBytes = (uint64_t)((memStatus.ullTotalPhys * 90) / 100);
    if (totalAvailableMemoryBytes) *totalAvailableMemoryBytes = memStatus.ullTotalPhys;
    if (lastRecordedMemLoadBytes) *lastRecordedMemLoadBytes = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
    if (lastRecordedHeapSizeBytes) *lastRecordedHeapSizeBytes = committed;
    if (lastRecordedFragmentationBytes) *lastRecordedFragmentationBytes = 0;
    if (totalCommittedBytes) *totalCommittedBytes = committed;
    if (promotedBytes) *promotedBytes = 0;
    if (pinnedObjectCount) *pinnedObjectCount = 0;
    if (finalizationPendingCount) *finalizationPendingCount = 0;
    if (index) *index = 0;
    if (generation) *generation = 0;
    if (pauseTimePct) *pauseTimePct = 0;
    if (isCompaction) *isCompaction = false;
    if (isConcurrent) *isConcurrent = false;
    if (genInfoRaw) memset(genInfoRaw, 0, sizeof(uint64_t) * 8);
    if (pauseInfoRaw) memset(pauseInfoRaw, 0, sizeof(uint64_t) * 2);
}

uint32_t LXRGCHeap::GetMemoryLoad()
{
    MEMORYSTATUSEX memStatus = {};
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);
    return memStatus.dwMemoryLoad;
}

int LXRGCHeap::GetGcLatencyMode() { return 2; }
int LXRGCHeap::SetGcLatencyMode(int newLatencyMode) { return 0; }
int LXRGCHeap::GetLOHCompactionMode() { return 0; }
void LXRGCHeap::SetLOHCompactionMode(int newLOHCompactionMode) { }
bool LXRGCHeap::RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage) { return false; }
bool LXRGCHeap::CancelFullGCNotification() { return false; }
int LXRGCHeap::WaitForFullGCApproach(int millisecondsTimeout) { return wait_full_gc_na; }
int LXRGCHeap::WaitForFullGCComplete(int millisecondsTimeout) { return wait_full_gc_na; }

unsigned LXRGCHeap::WhichGeneration(Object* obj) { return 0; }
int LXRGCHeap::CollectionCount(int generation, int get_bgc_fgc_coutn)
{
    // LXR runs unified full-heap STW cycles; report the same count for every
    // requested generation so runtime GC counters reflect real collections.
    return (int)g_lxrCounters.Collections;
}
int LXRGCHeap::StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC) { return start_no_gc_success; }
int LXRGCHeap::EndNoGCRegion() { return end_no_gc_success; }

size_t LXRGCHeap::GetTotalBytesInUse()
{
    int64_t v = g_committedInUse;
    return v > 0 ? (size_t)v : 0;
}
uint64_t LXRGCHeap::GetTotalAllocatedBytes() { return (uint64_t)g_lxrCounters.TotalAllocatedBytes; }

// Resolve the phase-policy env knobs once. LXR_TRACE_BUDGET_MB caps committed
// growth between full traces; LXR_TRACE_EVERY_EPOCHS caps RC epochs between full
// traces (0 disables the epoch cap and paces traces purely by growth).
static void EnsureTracePolicy()
{
    if (g_traceBudgetBytes < 0)
    {
        const char* e = getenv("LXR_TRACE_BUDGET_MB");
        int64_t mb = e ? _atoi64(e) : 128;
        g_traceBudgetBytes = mb * (int64_t)1024 * 1024;
    }
    if (g_traceEveryEpochs < 0)
    {
        const char* e = getenv("LXR_TRACE_EVERY_EPOCHS");
        g_traceEveryEpochs = e ? _atoi64(e) : 8;
    }
}

// Decide whether this epoch is a light RC pause or a full backup-trace pause.
// A trace is forced on induced/gen2 collection; otherwise it escalates when the
// RC-epoch cap is reached or committed growth since the last trace exceeds the
// trace budget. The epoch cap is scaled by the survival-rate EWMA: when a high
// fraction of the heap survives each trace, floating garbage accrues slowly, so
// traces can be rarer; when survival is low (churny short-lived cycles) trace
// sooner.
static LXRPhase DecidePhase(bool forceTrace)
{
    EnsureTracePolicy();
    if (forceTrace)
        return LXRPhase::TracePause;

    int64_t epochCap = g_traceEveryEpochs;
    if (epochCap > 0 && g_lxrCounters.SurvivalPctEwma >= 0)
    {
        // survival 0% -> 0.5x cap, 100% -> 1.5x cap.
        epochCap = (epochCap * (50 + g_lxrCounters.SurvivalPctEwma)) / 100;
        if (epochCap < 1) epochCap = 1;
    }
    if (g_traceEveryEpochs > 0 && g_epochsSinceTrace >= epochCap)
        return LXRPhase::TracePause;

    int64_t growth = g_committedInUse - g_lastTraceCommitted;
    if (g_traceBudgetBytes > 0 && growth >= g_traceBudgetBytes)
        return LXRPhase::TracePause;

    return LXRPhase::RCPause;
}

// --- GC-suspension helpers: drive g_gcCompleteEvent/g_gcInProgress around every
//     SuspendEE..RestartEE window so threads trapped in RareDisablePreemptiveGC
//     block (in our WaitUntilGCComplete) instead of busy-looping. See
//     g_gcCompleteEvent for why (SuspendAllThreads livelock otherwise).
static void LXREnsureGcCompleteEvent()
{
    if (g_gcCompleteEvent == nullptr)
    {
        // Manual-reset, initially signaled (no GC in progress at startup).
        HANDLE e = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (InterlockedCompareExchangePointer(&g_gcCompleteEvent, e, nullptr) != nullptr)
            CloseHandle(e); // lost the race; keep the winner
    }
}
static void LXRSuspendEE()
{
    LXREnsureGcCompleteEvent();
    InterlockedExchange(&g_gcInProgress, 1);
    ResetEvent(g_gcCompleteEvent); // trapped threads will now block in WaitUntilGCComplete
    g_theGCToCLR->SuspendEE(SUSPEND_FOR_GC);
}
static void LXRRestartEE()
{
    g_theGCToCLR->RestartEE(true); // clears the runtime suspend trap first
    InterlockedExchange(&g_gcInProgress, 0);
    if (g_gcCompleteEvent != nullptr)
        SetEvent(g_gcCompleteEvent); // release threads parked in WaitUntilGCComplete
}

// Runs one LXR epoch. Every epoch replays the coalescing-RC modified buffers (a
// cheap RC pause). Occasionally - as decided by DecidePhase - the epoch is a full
// TracePause that additionally runs a stop-the-world backup trace + Immix sweep,
// the only phase that reclaims dead cycles and actually returns committed memory.
// Reentrancy-guarded so an allocation-triggered epoch can never re-enter.
// Returns bytes reclaimed this epoch.
static int64_t RunLXRCollection(int generation, bool forceTrace)
{
    if (InterlockedCompareExchange(&g_inCollection, 1, 0) != 0)
        return 0; // a collection is already in progress on another/this thread

    LXRPhase phase = DecidePhase(forceTrace);
    int64_t reclaimedBefore = g_lxrCollector.ReclaimedBytes();
    int64_t committedBefore = g_committedInUse;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    bool verbose = getenv("LXR_VERBOSE") != nullptr;

    bool doBuffers = getenv("LXR_NO_BUFFERS") == nullptr;
    bool doTrace   = getenv("LXR_NO_TRACE")   == nullptr;
    bool doSweep   = getenv("LXR_NO_SWEEP")   == nullptr;

    // Concurrent SATB backup trace (P4): only the two brief STW pauses count as
    // pause time; the transitive mark runs while the mutators execute. Falls back
    // to the single-pause STW trace when concurrency is disabled or unavailable.
    //
    // Copying/evacuation, however, must run under a COMPLETE, precise STW trace.
    // The LXR paper copies ONLY during stop-the-world pauses, and moving an
    // object requires every live referrer to be marked so Evacuate's fix-up pass
    // can forward it to the new location. A concurrent SATB trace + allocate-black
    // is conservative for liveness but is NOT a safe basis for moving objects: a
    // referrer the SATB deletion barrier failed to re-mark would be left pointing
    // at the freed source copy -> dangling pointer -> access violation (observed
    // as an NRE deep in socket IO on the webapi workload under CONCURRENT+EVAC).
    // So per trace cycle we do EITHER a concurrent, non-moving SATB trace OR a
    // fully-STW trace+evacuate - never evacuate on concurrent-only marks. When
    // both features are enabled we alternate: most cycles trace concurrently
    // (cheap pauses, collects cyclic garbage), and every kEvacEveryN-th cycle is
    // a STW trace+evac that actually defragments. Every feature stays active.
    static LONG s_traceCycleCounter = 0;
    bool evacCycle = false;
    if (phase == LXRPhase::TracePause && g_evacActive)
    {
        const LONG kEvacEveryN = 4;
        LONG cyc = InterlockedIncrement(&s_traceCycleCounter);
        evacCycle = (!g_concurrentEnabled) || (cyc % kEvacEveryN == 0);
    }
    bool useConcurrent = (phase == LXRPhase::TracePause) && doTrace &&
                         g_concurrentEnabled && g_theGCToCLR != nullptr && !evacCycle;

    int64_t pauseMicros = 0;

    if (useConcurrent)
    {
        LARGE_INTEGER a0, a1;
        // --- Snapshot pause (STW): mod buffers, reset marks, seed roots ---
        QueryPerformanceCounter(&a0);
        LXRSetPhase("conc:suspend-snapshot");
        LXRSuspendEE();
        LXRSetPhase("conc:snapshot-buffers");
        if (doBuffers)
        {
            // #1: with concurrent decrements, only DETACH the modified buffers
            // here (bounded pause); the RC replay + recursive free run off-pause
            // in the drain window below. Otherwise process them STW as before.
            if (g_concDecrements)
                g_lxrCollector.SnapshotModifiedBuffers();
            else
                g_lxrCollector.ProcessModifiedBuffers();
        }
        LXRSetPhase("conc:snapshot");
        g_lxrCollector.ConcurrentTraceSnapshot();
        LXRSetPhase("conc:restart-snapshot");
        LXRRestartEE();
        QueryPerformanceCounter(&a1);
        int64_t snapMicros = (int64_t)((a1.QuadPart - a0.QuadPart) * 1000000 / freq.QuadPart);
        if (verbose) { fprintf(stderr, "LXRGC: [stage] concurrent snapshot done (pause=%lldus)\n", (long long)snapMicros); fflush(stderr); }

        // --- Concurrent drain (mutators running) ---
        QueryPerformanceCounter(&a0);
        LXRSetPhase("conc:drain");
        // Diagnostic: LXR_CONC_NO_DRAIN skips the off-pause drain so the ENTIRE
        // closure is computed at the STW finish from snapshot-greyed roots + SATB
        // + allocate-black. Discriminates a barrier/SATB-completeness bug (offenders
        // persist) from an off-pause concurrent-marking race (offenders vanish).
        static int s_noDrain = (getenv("LXR_CONC_NO_DRAIN") != nullptr) ? 1 : 0;
        if (!s_noDrain)
            g_lxrCollector.ConcurrentTraceDrain();
        // #1: replay the coalescing-RC increments/decrements + recursive free of
        // the buffers snapshotted at the pause, off-pause alongside the trace drain.
        if (g_concDecrements && doBuffers)
            g_lxrCollector.ProcessSnapshotDecrements();
        QueryPerformanceCounter(&a1);
        int64_t drainMicros = (int64_t)((a1.QuadPart - a0.QuadPart) * 1000000 / freq.QuadPart);
        if (verbose) { fprintf(stderr, "LXRGC: [stage] concurrent drain done (%lldus off-pause)\n", (long long)drainMicros); fflush(stderr); }

        // --- Finish pause (STW): residual SATB, allocate-black, evac, sweep ---
        QueryPerformanceCounter(&a0);
        LXRSetPhase("conc:suspend-finish");
        LXRSuspendEE();
        LXRSetPhase("conc:finish");
        // Diagnostic/soundness switch: when set, the finish pause discards the
        // concurrent (SATB) marks and re-marks the whole live graph from roots
        // under STW (identical to the proven-clean BackupTrace). This is sound
        // regardless of write-barrier/SATB completeness - used to confirm whether
        // the concurrent-path AVs stem from an incomplete SATB (byref/bulk stores
        // that bypass the pluggable callback) rather than from the sweep/evac.
        static int s_concFinishFullTrace = (getenv("LXR_CONC_FINISH_FULLTRACE") != nullptr) ? 1 : 0;
        if (s_concFinishFullTrace)
        {
            g_lxrCollector.BackupTrace();
            // A full STW re-trace is complete by construction -> mark-authoritative
            // sweep (reclaims dead cycles). (ConcurrentTraceFinish sets this flag
            // itself, based on whether it ran the closure this cycle.)
            InterlockedExchange(&g_traceCompleteThisCycle, 1);
            // Close the SATB window that ConcurrentTraceSnapshot opened.
            g_lxrCollector.SetSatbActive(false);
            InterlockedExchange(&g_traceWindowOpen, 0);
            g_lxrCollector.ResetSatbBuffers();
        }
        else
        {
            g_lxrCollector.ConcurrentTraceFinish();
        }
        LXRSetPhase("conc:finish-buffers");
        if (doBuffers)
            g_lxrCollector.ProcessModifiedBuffers();
        LXRSetPhase("conc:finish-sweep");
        if (doSweep)
            g_lxrCollector.SweepAndSelectDefrag();
        // Item F: a complete trace re-establishes remset completeness (all live
        // mature->young edges are re-derivable from the marked-object graph) and
        // the epoch bump below ages all current young to mature, so pre-trace
        // remset entries are irrelevant to the next nursery window. Discard them
        // under the pause (the barrier appends lock-free; resetting post-restart
        // would race). Clears the overflow flag, restoring completeness.
        if (g_remsetActive)
            g_lxrCollector.ResetRemsets();
        LXRSetPhase("conc:restart-finish");
        LXRRestartEE();
        LXRSetPhase("idle");
        QueryPerformanceCounter(&a1);
        int64_t finMicros = (int64_t)((a1.QuadPart - a0.QuadPart) * 1000000 / freq.QuadPart);

        pauseMicros = snapMicros + finMicros;
        InterlockedExchangeAdd64(&g_lxrCounters.ConcSnapshotMicros, snapMicros);
        InterlockedExchangeAdd64(&g_lxrCounters.ConcFinishMicros, finMicros);
        InterlockedExchangeAdd64(&g_lxrCounters.ConcDrainMicros, drainMicros);
        if (verbose) { fprintf(stderr, "LXRGC: [stage] concurrent finish done (pause=%lldus allocBlack=%lld)\n",
                               (long long)finMicros, (long long)g_lxrCounters.ConcAllocBlack); fflush(stderr); }
    }
    else
    {
        QueryPerformanceCounter(&t0);
        bool suspended = false;
        LXRSetPhase(phase == LXRPhase::TracePause ? "stw:suspend-trace" : "stw:suspend-rc");
        if (g_theGCToCLR != nullptr)
        {
            LXRSuspendEE();
            suspended = true;
        }
        if (verbose) { fprintf(stderr, "LXRGC: [stage] phase=%s suspended=%d\n",
                               phase == LXRPhase::TracePause ? "trace" : "rc", (int)suspended); fflush(stderr); }

        LXRSetPhase("stw:buffers");
        if (doBuffers)
        {
            g_lxrCollector.ProcessModifiedBuffers();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] ProcessModifiedBuffers done\n"); fflush(stderr); }
        }
        if (phase == LXRPhase::TracePause)
        {
            // STW backup trace is complete by construction (no mutator window) ->
            // mark-authoritative sweep, which reclaims dead cycles. If tracing is
            // disabled (LXR_NO_TRACE diagnostic) fall back to the RC-authoritative
            // sweep so a stale/empty mark table can never free a live region.
            InterlockedExchange(&g_traceCompleteThisCycle, doTrace ? 1 : 0);
            if (doTrace)
            {
                LXRSetPhase("stw:backuptrace");
                g_lxrCollector.BackupTrace();
                if (verbose) { fprintf(stderr, "LXRGC: [stage] BackupTrace done\n"); fflush(stderr); }
            }
            if (evacCycle)
            {
                LXRSetPhase("stw:evacuate");
                g_lxrCollector.Evacuate();
                if (verbose) { fprintf(stderr, "LXRGC: [stage] Evacuate done\n"); fflush(stderr); }
            }
            if (doSweep)
            {
                LXRSetPhase("stw:sweep");
                g_lxrCollector.SweepAndSelectDefrag();
                if (verbose) { fprintf(stderr, "LXRGC: [stage] SweepAndSelectDefrag done\n"); fflush(stderr); }
            }
            // Clear SATB buffers accumulated by a STW SATB exercise (LXR_SATB),
            // safe here under the pause.
            if (g_lxrCollector.IsSatbActive())
                g_lxrCollector.ResetSatbBuffers();
            // Item F: discard the pre-trace remembered set under the pause (see the
            // concurrent-finish path for the rationale: complete trace + young
            // aging make pre-trace mature->young edges irrelevant, and resetting
            // post-restart would race the lock-free barrier append).
            if (g_remsetActive)
                g_lxrCollector.ResetRemsets();
        }
        else if (g_youngRC && g_nurseryActive)
        {
            // Item D: young/nursery collection at the RC pause. Reclaims young
            // (this-epoch) regions proven dead by a bounded closure over roots +
            // handles + the complete mature->young remembered set (item F) +
            // young->young edges. OFF by default (LXR_NURSERY) pending a complete
            // write barrier; self-guards on remset completeness besides.
            LXRSetPhase("stw:nursery");
            g_lxrCollector.CollectNursery();
        }

        LXRSetPhase("stw:restart");
        if (suspended)
            LXRRestartEE();
        LXRSetPhase("idle");
        QueryPerformanceCounter(&t1);
        pauseMicros = (int64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    }
    InterlockedExchangeAdd64(&g_lxrCounters.TotalPauseMicros, pauseMicros);
    InterlockedIncrement64(&g_lxrCounters.Epochs);
    g_lxrCounters.LastCollectCommitted = g_committedInUse;

    if (phase == LXRPhase::TracePause)
    {
        InterlockedIncrement64(&g_lxrCounters.TracePauses);
        InterlockedExchangeAdd64(&g_lxrCounters.TracePausePauseMicros, pauseMicros);
        // Survival-rate prediction: fraction of committed memory that survived
        // this trace, folded into an EWMA (7:1) to pace future trace cadence.
        if (committedBefore > 0)
        {
            int64_t survivalPct = (g_committedInUse * 100) / committedBefore;
            if (survivalPct > 100) survivalPct = 100;
            int64_t prev = g_lxrCounters.SurvivalPctEwma;
            g_lxrCounters.SurvivalPctEwma = (prev < 0) ? survivalPct : (prev * 7 + survivalPct) / 8;
        }
        g_lastTraceCommitted = g_committedInUse;
        g_epochsSinceTrace = 0;
        // #6: age the nursery. Regions born in the window just ended (their blocks
        // stamped with the pre-bump g_traceEpoch) are no longer young after this
        // trace has had the chance to mark/reclaim them, so RC resumes for them.
        InterlockedIncrement64(&g_traceEpoch);
    }
    else
    {
        InterlockedIncrement64(&g_lxrCounters.RCPauses);
        InterlockedExchangeAdd64(&g_lxrCounters.RCPausePauseMicros, pauseMicros);
        InterlockedIncrement64(&g_epochsSinceTrace);
    }
    // Legacy "Collections" counter continues to count full reclaiming cycles so
    // existing runtime GC counters / reports keep reporting real collections.
    if (phase == LXRPhase::TracePause)
        InterlockedIncrement64(&g_lxrCounters.Collections);

    if (verbose) { fprintf(stderr, "LXRGC: [stage] restarted (phase=%s pause=%lldus survivalEwma=%lld%% satbEntries=%lld satbMarks=%lld remsetEntries=%lld)\n",
                           phase == LXRPhase::TracePause ? "trace" : "rc",
                           (long long)pauseMicros, (long long)g_lxrCounters.SurvivalPctEwma,
                           (long long)g_lxrCounters.SatbEntries, (long long)g_lxrCounters.SatbMarks,
                           (long long)g_lxrCounters.RemsetEntries); fflush(stderr); }

    int64_t reclaimedNow = g_lxrCollector.ReclaimedBytes();
    InterlockedExchange(&g_inCollection, 0);
    return reclaimedNow - reclaimedBefore;
}

// Monitor thread (LXR_WATCHDOG): if a collection phase stalls, print which one.
// --- In-process native stack dumper (diagnostic; LXR_STACKS=1) --------------
// When the watchdog detects a stuck SuspendEE, walk and symbolize EVERY other
// thread's native+managed stack from inside the process using dbghelp
// StackWalk64. External post-mortem tools (dotnet-dump/SOS) cannot unwind native
// frames of a custom-GC dump, so this is the only way to see WHICH thread the
// suspension is waiting on and where it is wedged. Runs at most once.
static volatile LONG g_lxrStacksDumped = 0;
static void LXRDumpAllThreadStacks()
{
    if (getenv("LXR_STACKS") == nullptr)
        return;
    if (InterlockedCompareExchange(&g_lxrStacksDumped, 1, 0) != 0)
        return;

    HANDLE proc = GetCurrentProcess();
    DWORD selfPid = GetCurrentProcessId();
    DWORD selfTid = GetCurrentThreadId();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // Point the symbol path at the app dir (coreclr.pdb / LXRGC.pdb sit next to
    // the binaries) plus the runtime PDB dir; fInvadeProcess=TRUE loads modules.
    const char* symPath =
        "C:\\github\\runtimelab\\src\\LXRGC\\samples\\WebApi\\publish;"
        "C:\\github\\runtime\\artifacts\\bin\\coreclr\\windows.x64.Release\\PDB;"
        "C:\\github\\runtime\\artifacts\\bin\\coreclr\\windows.x64.Release";
    SymInitialize(proc, symPath, TRUE);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "LXRGC: [stacks] snapshot failed err=%lu\n", GetLastError());
        return;
    }
    THREADENTRY32 te; te.dwSize = sizeof(te);
    fprintf(stderr, "LXRGC: [stacks] ===== all-thread native stack dump (stuck suspend) =====\n");
    fflush(stderr);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != selfPid) continue;
            if (te.th32ThreadID == selfTid) continue;

            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (th == nullptr) continue;

            DWORD susp = SuspendThread(th);
            (void)susp;

            CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(th, &ctx)) { ResumeThread(th); CloseHandle(th); continue; }

            STACKFRAME64 sf; memset(&sf, 0, sizeof(sf));
            sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;

            fprintf(stderr, "LXRGC: [stacks] --- thread %lu (rip=%p) ---\n",
                    te.th32ThreadID, (void*)ctx.Rip);
            for (int frame = 0; frame < 40; frame++)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (sf.AddrPC.Offset == 0) break;

                DWORD64 disp = 0;
                char buf[sizeof(SYMBOL_INFO) + 512];
                SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 512;
                char modname[128] = "?";
                DWORD64 modbase = SymGetModuleBase64(proc, sf.AddrPC.Offset);
                IMAGEHLP_MODULE64 mi; mi.SizeOfStruct = sizeof(mi);
                if (modbase && SymGetModuleInfo64(proc, modbase, &mi))
                    strncpy(modname, mi.ModuleName, sizeof(modname) - 1);
                if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym))
                    fprintf(stderr, "LXRGC: [stacks]     %-16s %s+0x%llx\n",
                            modname, sym->Name, (unsigned long long)disp);
                else
                    fprintf(stderr, "LXRGC: [stacks]     %-16s 0x%llx\n",
                            modname, (unsigned long long)sf.AddrPC.Offset);
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    fprintf(stderr, "LXRGC: [stacks] ===== end stack dump =====\n");
    fflush(stderr);
}

// --- On-fault native stack dumper (diagnostic; LXR_AV_STACKS=1) -------------
// A Vectored Exception Handler that fires on an access violation whose faulting
// address lies inside the LXR heap reservation - i.e. a use-after-free of a
// decommitted/reclaimed chunk (our bug), as opposed to a managed
// NullReferenceException (which faults near address 0 and must pass through
// untouched). It walks the FAULTING thread's native+managed stack from the
// exception CONTEXT and classifies the faulting chunk's state, then lets the
// normal crash path proceed (EXCEPTION_CONTINUE_SEARCH). One-shot.
static volatile LONG g_lxrAvDumped = 0;

static void LXREnsureSymForFault(HANDLE proc)
{
    static volatile LONG s_symInited = 0;
    if (InterlockedCompareExchange(&s_symInited, 1, 0) != 0)
        return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const char* symPath =
        "C:\\github\\runtimelab\\src\\LXRGC\\samples\\WebApi\\publish;"
        "C:\\github\\runtime\\artifacts\\bin\\coreclr\\windows.x64.Release\\PDB;"
        "C:\\github\\runtime\\artifacts\\bin\\coreclr\\windows.x64.Release";
    SymInitialize(proc, symPath, TRUE);
}

static LONG CALLBACK LXRAvVectoredHandler(EXCEPTION_POINTERS* ep)
{
    static int s_enabled = -1;
    if (s_enabled < 0) s_enabled = (getenv("LXR_AV_STACKS") != nullptr) ? 1 : 0;
    if (!s_enabled)
        return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er == nullptr || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* faultAddr = (er->NumberParameters >= 2) ? (uint8_t*)er->ExceptionInformation[1] : nullptr;
    uint8_t* base  = g_lxrCollector.HeapBase();
    size_t   bytes = g_lxrCollector.HeapBytes();
    bool inHeap = (base != nullptr && faultAddr >= base && faultAddr < base + bytes);
    // Default: only OUR classic bug - a fault INSIDE the LXR heap reservation (a
    // UAF of a decommitted chunk). Managed null derefs fault near 0 and are
    // handled by the runtime's own VEH - pass them through. With LXR_AV_ANY=1 we
    // also capture *wild-pointer* faults above the null-guard page (a corrupted
    // ref-field read - e.g. the concurrent-SATB DispatchContinuations AV, whose
    // fault lands OUTSIDE the heap), to get a native stack for diagnosis.
    static int s_any = -1;
    if (s_any < 0) s_any = (getenv("LXR_AV_ANY") != nullptr) ? 1 : 0;
    bool wild = (s_any && faultAddr >= (uint8_t*)0x10000);
    if (!inHeap && !wild)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_lxrAvDumped, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    HANDLE proc = GetCurrentProcess();
    LXREnsureSymForFault(proc);
    DWORD rw = (er->NumberParameters >= 1) ? (DWORD)er->ExceptionInformation[0] : 0;
    fprintf(stderr, "LXRGC: [AV] ===== access violation (%s) =====\n",
            inHeap ? "in LXR heap" : "wild pointer / outside heap");
    fprintf(stderr, "LXRGC: [AV] fault %s addr=%p (heap [%p,%p) %s) rip=%p tid=%lu\n",
            rw == 1 ? "WRITE" : (rw == 8 ? "EXEC" : "READ"), (void*)faultAddr,
            (void*)base, (void*)(base + bytes),
            inHeap ? "IN-HEAP" : "outside", (void*)ep->ContextRecord->Rip, GetCurrentThreadId());

    // Classify the containing chunk state at the moment of the fault.
    __try
    {
        for (size_t i = 0; i < g_chunkCount; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (faultAddr >= c.Start && faultAddr < c.Start + c.Size)
            {
                fprintf(stderr, "LXRGC: [AV] chunk[%zu] [%p,%p) committed=%d freeRun=%d owner=%p usedEnd=%p\n",
                        i, (void*)c.Start, (void*)(c.Start + c.Size), c.Committed ? 1 : 0,
                        c.FreeRun ? 1 : 0, (void*)c.Owner, (void*)c.UsedEnd);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }

    // Write a full-memory minidump *with the faulting exception context* so the
    // managed stack and the corrupted object can be inspected offline with
    // dotnet-dump / SOS. One-shot (g_lxrAvDumped already claimed above).
    {
        const wchar_t* path = L"C:\\temp\\lxr-av.dmp";
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            BOOL ok = MiniDumpWriteDump(proc, GetCurrentProcessId(), h,
                                        MiniDumpWithFullMemory, &mei, nullptr, nullptr);
            CloseHandle(h);
            fprintf(stderr, "LXRGC: [AV] wrote %ls ok=%d\n", path, ok);
        }
    }

    // Walk the faulting thread's stack from the exception CONTEXT (copy it -
    // StackWalk64 mutates the CONTEXT it is given).
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf; memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    HANDLE th = GetCurrentThread();
    for (int frame = 0; frame < 50; frame++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        DWORD64 disp = 0;
        char symbuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 512;
        char modname[128] = "?";
        DWORD64 modbase = SymGetModuleBase64(proc, sf.AddrPC.Offset);
        IMAGEHLP_MODULE64 mi; mi.SizeOfStruct = sizeof(mi);
        if (modbase && SymGetModuleInfo64(proc, modbase, &mi))
            strncpy(modname, mi.ModuleName, sizeof(modname) - 1);
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym))
            fprintf(stderr, "LXRGC: [AV]     %-16s %s+0x%llx\n", modname, sym->Name, (unsigned long long)disp);
        else
            fprintf(stderr, "LXRGC: [AV]     %-16s 0x%llx\n", modname, (unsigned long long)sf.AddrPC.Offset);
    }
    fprintf(stderr, "LXRGC: [AV] ===== end AV dump =====\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void LXRWatchdogThreadProc(void*)
{
    const int64_t stallMicros = 20 * 1000000; // 20s without a phase change = stuck
    int64_t lastReportedSeq = -1;
    for (;;)
    {
        Sleep(2000);
        if (g_collectorShutdown) break;
        const char* ph = g_lxrPhase;
        if (ph == nullptr || strcmp(ph, "idle") == 0)
            continue;
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        int64_t stampAge = (int64_t)((n.QuadPart - g_lxrPhaseStamp) * 1000000 /
                                     (g_lxrQpcFreq.QuadPart ? g_lxrQpcFreq.QuadPart : 1));
        int64_t seq = g_lxrPhaseSeq;
        if (stampAge > stallMicros && seq != lastReportedSeq)
        {
            fprintf(stderr, "LXRGC: [WATCHDOG] STUCK in phase '%s' for %llds (seq=%lld)\n",
                    ph, (long long)(stampAge / 1000000), (long long)seq);
            fflush(stderr);
            lastReportedSeq = seq;
            // If we're stuck inside a SuspendEE, dump every thread's native stack
            // so we can see which mutator is failing to reach a safepoint.
            if (strstr(ph, "suspend") != nullptr)
                LXRDumpAllThreadStacks();
        }
    }
}

// Body of the dedicated, non-suspendable GC thread: wait for a request, run one
// full collection, then publish completion so synchronous waiters (GC.Collect)
// can observe it.
static void LXRCollectorThreadProc(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_collectRequestEvent, INFINITE);
        if (g_collectorShutdown)
            break;
        InterlockedExchange(&g_collectPending, 0);
        bool forceTrace = InterlockedExchange(&g_requestTrace, 0) != 0;
        RunLXRCollection(-1, forceTrace);
        InterlockedIncrement64(&g_collectCompletedSeq);
        SetEvent(g_collectDoneEvent);
    }
}

// Post a collection request to the dedicated collector thread. When wait==true
// (explicit GC.Collect) block until a cycle that started after this request has
// finished; otherwise (allocation trigger) fire-and-forget, coalescing repeated
// requests so the allocator never blocks or drives SuspendEE itself.
static void RequestLXRCollection(bool wait, bool forceTrace)
{
    if (forceTrace)
        InterlockedExchange(&g_requestTrace, 1);

    if (g_collectRequestEvent == nullptr)
    {
        // Collector thread not up yet (very early startup): fall back to a direct
        // synchronous collection on the calling thread (single-threaded at this
        // point, so the concurrency hazard does not apply).
        InterlockedExchange(&g_requestTrace, 0);
        RunLXRCollection(-1, forceTrace);
        return;
    }

    if (wait)
    {
        int64_t before = g_collectCompletedSeq;
        SetEvent(g_collectRequestEvent);
        while (g_collectCompletedSeq <= before)
            WaitForSingleObject(g_collectDoneEvent, 50);
        return;
    }

    // Fire-and-forget: only arm the collector once until it consumes the request.
    if (InterlockedCompareExchange(&g_collectPending, 1, 0) == 0)
        SetEvent(g_collectRequestEvent);
}

HRESULT LXRGCHeap::GarbageCollect(int generation, bool low_memory_p, int mode)
{
    InterlockedIncrement64(&g_lxrCounters.InducedCollectRequests);
    int64_t before = g_lxrCollector.ReclaimedBytes();
    RequestLXRCollection(/*wait*/ true, /*forceTrace*/ true);
    int64_t reclaimed = g_lxrCollector.ReclaimedBytes() - before;
    fprintf(stderr,
            "LXRGC: GC(gen=%d) -> RC inc=%lld dec=%lld, backupTraces=%lld, collections=%lld, reclaimed this GC=%lld bytes (total=%lld)\n",
            generation,
            (long long)g_lxrCounters.RCIncrements,
            (long long)g_lxrCounters.RCDecrements,
            (long long)g_lxrCounters.BackupTraces,
            (long long)g_lxrCounters.Collections,
            (long long)reclaimed,
            (long long)g_lxrCollector.ReclaimedBytes());
    fflush(stderr);
    return S_OK;
}

unsigned LXRGCHeap::GetMaxGeneration() { return 2; }
void LXRGCHeap::SetFinalizationRun(Object* obj) { }
bool LXRGCHeap::RegisterForFinalization(int gen, Object* obj) { return true; }
int LXRGCHeap::GetLastGCPercentTimeInGC() { return 0; }
size_t LXRGCHeap::GetLastGCGenerationSize(int gen) { return 0; }

bool LXRGCHeap::IsPromoted(Object* object) { return true; }

bool LXRGCHeap::IsHeapPointer(void* object, bool small_heap_only)
{
    uint8_t* p = (uint8_t*)object;
    return p >= m_heapBase && p < HeapHighWater();
}

unsigned LXRGCHeap::GetCondemnedGeneration() { return 0; }
bool LXRGCHeap::IsGCInProgressHelper(bool bConsiderGCStart) { return g_gcInProgress != 0; }
unsigned LXRGCHeap::GetGcCount() { return 0; }
bool LXRGCHeap::IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number) { return true; }
bool LXRGCHeap::IsEphemeral(Object* object) { return true; }
// Block a thread trapped in Thread::RareDisablePreemptiveGC until the current
// SuspendEE..RestartEE window closes (event SET by LXRRestartEE). Returning
// immediately - as the old stub did - busy-loops trapped threads and livelocks
// SuspendAllThreads under a thread storm (see g_gcCompleteEvent).
uint32_t LXRGCHeap::WaitUntilGCComplete(bool bConsiderGCStart)
{
    HANDLE e = g_gcCompleteEvent;
    if (e != nullptr && g_gcInProgress != 0)
        WaitForSingleObject(e, INFINITE);
    return 0;
}
void LXRGCHeap::FixAllocContext(gc_alloc_context* acontext, void* arg, void* heap) { }
size_t LXRGCHeap::GetCurrentObjSize() { return (size_t)g_lxrCounters.TotalAllocatedBytes; }
void LXRGCHeap::SetGCInProgress(bool fInProgress) { }
bool LXRGCHeap::RuntimeStructuresValid() { return true; }
void LXRGCHeap::SetSuspensionPending(bool fSuspensionPending) { }
void LXRGCHeap::SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor) { }
void LXRGCHeap::Shutdown() { }

size_t LXRGCHeap::GetLastGCStartTime(int generation) { return 0; }
size_t LXRGCHeap::GetLastGCDuration(int generation) { return 0; }

size_t LXRGCHeap::GetNow()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (size_t)((now.QuadPart - m_startTime.QuadPart) * 1000 / m_qpcFrequency.QuadPart);
}

bool LXRGCHeap::IsLargeObject(Object* pObj)
{
    MethodTable* mt = pObj->GetGCSafeMethodTable();
    return mt->GetBaseSize() >= LARGE_OBJECT_SIZE;
}

void LXRGCHeap::ValidateObjectMember(Object* obj) { }
Object* LXRGCHeap::NextObj(Object* object) { return nullptr; }
Object* LXRGCHeap::GetContainingObject(void* pInteriorPtr, bool fCollectedGenOnly) { return nullptr; }

void LXRGCHeap::DiagWalkObject(Object* obj, walk_fn fn, void* context) { }
void LXRGCHeap::DiagWalkObject2(Object* obj, walk_fn2 fn, void* context) { }
void LXRGCHeap::DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) { }
void LXRGCHeap::DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number) { }
void LXRGCHeap::DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn) { }
void LXRGCHeap::DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context) { }
void LXRGCHeap::DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context) { }
void LXRGCHeap::DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context) { }
void LXRGCHeap::DiagDescrGenerations(gen_walk_fn fn, void* context)
{
    if (fn != nullptr)
        fn(context, 0, m_heapBase, HeapHighWater(), m_heapReservedEnd);
}
void LXRGCHeap::DiagTraceGCSegments() { }
void LXRGCHeap::DiagGetGCSettings(EtwGCSettingsInfo* settings)
{
    if (settings != nullptr)
        memset(settings, 0, sizeof(*settings));
}

bool LXRGCHeap::StressHeap(gc_alloc_context* acontext) { return false; }

segment_handle LXRGCHeap::RegisterFrozenSegment(segment_info* pseginfo)
{
    EnterCriticalSection(&g_frozenSegmentsLock);
    for (int i = 0; i < MAX_FROZEN_SEGMENTS; i++)
    {
        if (!g_frozenSegments[i].InUse)
        {
            g_frozenSegments[i].InUse = true;
            g_frozenSegments[i].Base = (uint8_t*)pseginfo->pvMem;
            g_frozenSegments[i].Allocated = (uint8_t*)pseginfo->pvMem + pseginfo->ibAllocated;
            g_frozenSegments[i].Committed = (uint8_t*)pseginfo->pvMem + pseginfo->ibCommit;
            g_frozenSegments[i].Reserved = (uint8_t*)pseginfo->pvMem + pseginfo->ibReserved;
            LeaveCriticalSection(&g_frozenSegmentsLock);
            return (segment_handle)(intptr_t)(i + 1);
        }
    }
    LeaveCriticalSection(&g_frozenSegmentsLock);
    return (segment_handle)nullptr;
}

void LXRGCHeap::UnregisterFrozenSegment(segment_handle seg)
{
    intptr_t idx = (intptr_t)seg - 1;
    if (idx < 0 || idx >= MAX_FROZEN_SEGMENTS)
        return;
    EnterCriticalSection(&g_frozenSegmentsLock);
    g_frozenSegments[idx].InUse = false;
    LeaveCriticalSection(&g_frozenSegmentsLock);
}

bool LXRGCHeap::IsInFrozenSegment(Object* object)
{
    uint8_t* p = (uint8_t*)object;
    bool found = false;
    EnterCriticalSection(&g_frozenSegmentsLock);
    for (int i = 0; i < MAX_FROZEN_SEGMENTS; i++)
    {
        if (g_frozenSegments[i].InUse && p >= g_frozenSegments[i].Base && p < g_frozenSegments[i].Committed)
        {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_frozenSegmentsLock);
    return found;
}

void LXRGCHeap::ControlEvents(GCEventKeyword keyword, GCEventLevel level) { }
void LXRGCHeap::ControlPrivateEvents(GCEventKeyword keyword, GCEventLevel level) { }

unsigned int LXRGCHeap::GetGenerationWithRange(Object* object, uint8_t** ppStart, uint8_t** ppAllocated, uint8_t** ppReserved)
{
    if (ppStart) *ppStart = m_heapBase;
    if (ppAllocated) *ppAllocated = HeapHighWater();
    if (ppReserved) *ppReserved = m_heapReservedEnd;
    return 0;
}

int64_t LXRGCHeap::GetTotalPauseDuration() { return 0; }

void LXRGCHeap::EnumerateConfigurationValues(void* context, ConfigurationValueFunc configurationValueFunc)
{
    if (configurationValueFunc != nullptr)
        configurationValueFunc(context, "LXRGC", "System.GC.Name", GCConfigurationType::StringUtf8, (int64_t)(intptr_t)"LXRGC");
}

void LXRGCHeap::UpdateFrozenSegment(segment_handle seg, uint8_t* allocated, uint8_t* committed)
{
    intptr_t idx = (intptr_t)seg - 1;
    if (idx < 0 || idx >= MAX_FROZEN_SEGMENTS)
        return;
    EnterCriticalSection(&g_frozenSegmentsLock);
    g_frozenSegments[idx].Allocated = allocated;
    g_frozenSegments[idx].Committed = committed;
    LeaveCriticalSection(&g_frozenSegmentsLock);
}

int LXRGCHeap::RefreshMemoryLimit() { return refresh_success; }

enable_no_gc_region_callback_status LXRGCHeap::EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem* callback, uint64_t callback_threshold) { return not_started; }
FinalizerWorkItem* LXRGCHeap::GetExtraWorkForFinalization() { return nullptr; }
uint64_t LXRGCHeap::GetGenerationBudget(int generation) { return COMMIT_CHUNK; }
size_t LXRGCHeap::GetLOHThreshold() { return LARGE_OBJECT_SIZE; }
void LXRGCHeap::DiagWalkHeapWithACHandling(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) { }
void LXRGCHeap::NullBridgeObjectsWeakRefs(size_t length, void* unreachableObjectHandles) { }
