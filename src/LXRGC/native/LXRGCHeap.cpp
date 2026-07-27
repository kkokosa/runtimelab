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
#include <intrin.h>
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
// Per-thread ahead-of-use commit granularity. Kept small (2 MiB, not 16 MiB) so
// the committed footprint tracks live+in-flight allocation instead of a large
// per-thread over-commit "overhang": every allocating thread eagerly commits up
// to COMMIT_CHUNK past its bump pointer, so with N allocating threads the wasted
// committed tail is ~N*COMMIT_CHUNK. At 16 MiB * ~12 webapi threads that was
// ~190 MiB of committed-but-never-carved memory; 2 MiB cuts it ~8x for one extra
// VirtualAlloc per ~2 MiB allocated (negligible on the slow path).
static const size_t COMMIT_CHUNK = 2 * 1024 * 1024;         // 2 MiB
static const size_t CONTEXT_ALLOC_QUANTUM = 128 * 1024;
static const size_t THREAD_BLOCK_RUN = 64 * 1024 * 1024;    // 64 MiB reserved (not committed) per thread at a time

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
    // Shared-queue overflow handling (paper §3.2.1: a full buffer is handed to a
    // shared queue and the mutator continues into a fresh one, so NO first-log is
    // ever dropped - a dropped first-log permanently loses an RC increment, which
    // for young objects is fatal: the referent is stuck at RC 0 and the nursery
    // frees it while live). NextFree links this buffer on the global free-list;
    // InUse marks it as a thread's CURRENT buffer (must not be recycled).
    ModifiedBuffer* NextFree = nullptr;
    volatile LONG InUse = 0;
};

static thread_local ModifiedBuffer* t_modifiedBuffer = nullptr;
static ModifiedBuffer* volatile g_registeredBuffers = nullptr;
// Free-list of spare, pre-registered modified buffers the barrier can swap to
// when its current buffer fills (lock-free Treiber stack via NextFree). Populated
// OFF the barrier (EnsureThreadBuffers, a proper allocating frame) so the barrier
// never allocates. Replenished at drain (recycled buffers pushed back).
static ModifiedBuffer* volatile g_freeModifiedBuffers = nullptr;
static volatile LONG g_freeModifiedCount = 0;
static CRITICAL_SECTION g_buffersLock;

// Lock-free push/pop of a spare buffer on the free-list. ABA-safe here: pop only
// ever runs in the barrier (cooperative) and push only at STW drain or off-barrier
// top-up; a popped buffer is never concurrently pushed (single logical owner).
static void PushFreeModifiedBuffer(ModifiedBuffer* nb)
{
    for (;;)
    {
        ModifiedBuffer* head = g_freeModifiedBuffers;
        nb->NextFree = head;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_freeModifiedBuffers,
                                              nb, head) == head)
        {
            InterlockedIncrement(&g_freeModifiedCount);
            return;
        }
    }
}
static ModifiedBuffer* PopFreeModifiedBuffer()
{
    for (;;)
    {
        ModifiedBuffer* head = g_freeModifiedBuffers;
        if (head == nullptr)
            return nullptr;
        ModifiedBuffer* next = head->NextFree;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_freeModifiedBuffers,
                                              next, head) == head)
        {
            InterlockedDecrement(&g_freeModifiedCount);
            head->NextFree = nullptr;
            return head;
        }
    }
}

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
// Set when a first-log was dropped (shared free-list exhausted): the young RC is
// now known-incomplete (a live young referent may be stuck below its true count),
// so CollectNursery must NOT reclaim RC=0 young until the next COMPLETE trace ages
// all current young to mature (mark-authoritative) and starts a fresh young epoch.
// Cleared at each complete trace (BackupTrace / ConcurrentTraceFinish).
static volatile LONG g_youngRCIncomplete = 0;

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
    uint32_t Ver[kCapacity];   // item F (§3.3.4): source line reuse version at insert
    size_t Count = 0;
    LONG InUse = 0;            // 1 = a thread's current append target / collector-owned
    RemsetBuffer* NextRegistered = nullptr;
    RemsetBuffer* NextFree = nullptr;   // Treiber free-list link (spare when non-null owner)
};
static thread_local RemsetBuffer* t_remsetBuffer = nullptr;
static RemsetBuffer* volatile g_registeredRemsetBuffers = nullptr;
// Shared free-list of spare remset buffers (paper §3.2.1 shared-queue swap-in),
// mirroring g_freeModifiedBuffers: when a thread's per-thread remset buffer fills
// mid-window the barrier swaps in a pre-registered spare instead of DROPPING the
// inter-block edge (which forced the O(live-heap) full-walk evac-fixup fallback).
// Topped up OFF the barrier in EnsureThreadBuffers; replenished at CompactRemsets.
static RemsetBuffer* volatile g_freeRemsetBuffers = nullptr;
static volatile LONG g_freeRemsetCount = 0;
// Item F: a collector-owned remset buffer for edges created by GC-internal object
// relocation (evac copies / young-survivor promotions). Those are memcpy stores
// that never fire the write barrier, so their inter-block out-edges must be
// registered explicitly (RecordRemsetEdge). Appended only by the single collector
// thread under STW; linked into g_registeredRemsetBuffers so all consumers see it.
static RemsetBuffer* g_collectorRemset = nullptr;
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

// --- Item F (paper §3.3): candidate-scoped evacuation remembered set ---------
// The paper's per-block remembered sets record only inter-block edges whose
// TARGET is an evacuation CANDIDATE selected at the start of the trace, and the
// trace rebuilds them each cycle. Recording all inter-block edges heap-wide (as
// the persistent barrier remset does) is O(all edges); scoping to candidates
// bounds both the remset size and the finish-pause replay cost.
//
// We approximate the per-block candidate set with a committed bytemap keyed by
// 128 KiB region-slot (== CONTEXT_ALLOC_QUANTUM, the minimum chunk granule). A
// set byte means "this region-slot overlaps an evac candidate selected for this
// trace"; the hot barrier, the mark closures, and evac-copy edge recording all
// gate their inter-block edge recording on it. Selection uses each region's
// persisted DeadPctEstimate (stamped by the previous Evacuate's occupancy scan),
// so it is a cheap side-table read at the snapshot pause (no live-heap walk).
static uint8_t* g_evacCandidate      = nullptr;  // committed bytemap, 1 byte/region-slot
static uint8_t* g_evacCandidateBase  = nullptr;  // == heap base
static size_t   g_evacCandidateSlots = 0;
static volatile LONG g_evacCandidateScope = 0;   // 1 => candidate-scoped recording active this cycle
static int      g_evacCandidateEnabled = -1;     // env LXR_EVAC_CANDIDATE_SCOPE (default ON)

static inline bool CandidateScopeEnabled()
{
    if (g_evacCandidateEnabled < 0)
        g_evacCandidateEnabled = (getenv("LXR_EVAC_CANDIDATE_SCOPE") != nullptr &&
                                  getenv("LXR_EVAC_CANDIDATE_SCOPE")[0] == '0') ? 0 : 1;
    return g_evacCandidateEnabled != 0;
}

// Is 'p' inside a region-slot currently flagged as an evacuation candidate?
static inline bool IsEvacCandidateAddr(void* p)
{
    uint8_t* a = (uint8_t*)p;
    if (g_evacCandidate == nullptr || a < g_evacCandidateBase) return false;
    size_t slot = (size_t)(a - g_evacCandidateBase) / CONTEXT_ALLOC_QUANTUM;
    if (slot >= g_evacCandidateSlots) return false;
    return g_evacCandidate[slot] != 0;
}

// Mark every 128 KiB region-slot overlapping [start,end) as a candidate. Chunks
// are not region-slot aligned, so over-approximating to the covered slots is
// sound (a non-candidate address never becomes a candidate spuriously in a way
// that drops an edge; extra candidate slots only over-record, never under).
static inline void SetEvacCandidateRange(uint8_t* start, uint8_t* end)
{
    if (g_evacCandidate == nullptr || end <= start) return;
    size_t s0 = (start <= g_evacCandidateBase) ? 0
              : (size_t)(start - g_evacCandidateBase) / CONTEXT_ALLOC_QUANTUM;
    size_t s1 = (size_t)((end - 1) - g_evacCandidateBase) / CONTEXT_ALLOC_QUANTUM;
    for (size_t s = s0; s <= s1 && s < g_evacCandidateSlots; s++)
        g_evacCandidate[s] = 1;
}

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

// Lock-free push/pop of a spare remset buffer (mirrors PushFreeModifiedBuffer /
// PopFreeModifiedBuffer). A buffer stays permanently linked in the append-only
// registry (NextRegistered) and, when free, additionally sits on this Treiber
// stack (NextFree) with Count==0. Pop runs only in the cooperative-mode barrier;
// push only off-barrier / at STW compaction, so a popped buffer is never
// concurrently pushed (single logical owner).
static void PushFreeRemsetBuffer(RemsetBuffer* nb)
{
    for (;;)
    {
        RemsetBuffer* head = g_freeRemsetBuffers;
        nb->NextFree = head;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_freeRemsetBuffers,
                                              nb, head) == head)
        {
            InterlockedIncrement(&g_freeRemsetCount);
            return;
        }
    }
}
static RemsetBuffer* PopFreeRemsetBuffer()
{
    for (;;)
    {
        RemsetBuffer* head = g_freeRemsetBuffers;
        if (head == nullptr)
            return nullptr;
        RemsetBuffer* next = head->NextFree;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_freeRemsetBuffers,
                                              next, head) == head)
        {
            InterlockedDecrement(&g_freeRemsetCount);
            head->NextFree = nullptr;
            return head;
        }
    }
}

// --- Item F (F3): trace-bootstrapped, evac-scoped reference fix-up ---------
// The pre-F3 Evacuate() fixed up references to moved objects by walking the
// ENTIRE live heap (parse every region, scan every marked object's fields) -- an
// O(live-heap) STW pass. The paper instead scopes fix-up to a remembered set of
// inter-block edges, bootstrapped by the trace. We reproduce that faithfully:
// during an evac cycle's STW backup-trace mark (which already scans every live
// object's fields exactly once), each mark lane logs the SLOTS holding an
// INTER-block reference into its own thread-local vector (no locks: distinct
// threads write distinct vectors; the shared registry is touched once per thread
// at first use). The mark runs in the SAME pause immediately before Evacuate()
// with no mutator in between, so the union of the lane logs is a COMPLETE
// remembered set of every live inter-block edge at evac time. Intra-block edges
// never need external fix-up (source and target share a region and co-move -- the
// destination-copy scan handles them), so they are deliberately excluded.
// Evacuate() then fixes up only (a) the moved objects' destination copies and
// (b) these recorded slots -- eliminating the whole-heap walk. Recording is
// active only on evac cycles (g_recordEvacEdges). Safety net: a lane hitting its
// cap, or a conservative-keep-alive cycle (objects marked without a field scan),
// sets fall-back conditions so Evacuate() reverts to the sound full-heap walk;
// LXR_VERIFY_TRACE's [verify-evac] pass still asserts 0 unforwarded refs.
static volatile LONG g_recordEvacEdges = 0;       // set only during an evac-cycle mark
static volatile LONG g_evacEdgeOverflow = 0;      // a lane hit its cap -> full-walk fallback
static thread_local std::vector<Object**>* t_evacEdgeLog = nullptr; // this lane's log
static std::vector<std::vector<Object**>*> g_evacEdgeLogs;          // registry of all lane logs
static CRITICAL_SECTION g_evacEdgeLock;           // guards the registry (not the hot append)
static const size_t kEvacEdgeLaneCap = 16u * 1024u * 1024u; // 16M slots/lane (~128MB) -> overflow

// --- Item G (§3.5): globals for parallel scan of a single very large ref array --
// (Definitions/rationale at the implementation block near ParallelDrainMarkStack.)
static std::vector<Object*> g_bigRefArrays;              // deferred huge ref arrays
static CRITICAL_SECTION     g_bigArrayLock;              // guards g_bigRefArrays
static volatile LONG        g_bigArrayParallel = 1;      // env LXR_PARALLEL_BIGARRAY (opt-out)
static const size_t         kBigRefArraySlots  = 64u * 1024u;  // >=512KB ref array => partition
static const size_t         kBigArrayChunkSlots = 16u * 1024u; // 128KB element chunk / lane task
static volatile LONG64      g_bigArrayScans        = 0;  // stats: arrays partitioned
static volatile LONG64      g_bigArraySlotsScanned = 0;  // stats: element slots scanned in parallel

static void DeferBigRefArray(Object* o)
{
    EnterCriticalSection(&g_bigArrayLock);
    g_bigRefArrays.push_back(o);
    LeaveCriticalSection(&g_bigArrayLock);
}

// Append an inter-block slot to this lane's log. Lazily allocates + registers the
// lane's vector on first use. Lock-free on the hot path (each thread owns its
// vector); the one-time registration is under g_evacEdgeLock.
static inline void RecordEvacEdge(Object** slot)
{
    std::vector<Object**>* log = t_evacEdgeLog;
    if (log == nullptr)
    {
        log = new (std::nothrow) std::vector<Object**>();
        if (log == nullptr) { InterlockedExchange(&g_evacEdgeOverflow, 1); return; }
        log->reserve(1u << 16);
        EnterCriticalSection(&g_evacEdgeLock);
        g_evacEdgeLogs.push_back(log);
        LeaveCriticalSection(&g_evacEdgeLock);
        t_evacEdgeLog = log;
    }
    if (log->size() >= kEvacEdgeLaneCap) { InterlockedExchange(&g_evacEdgeOverflow, 1); return; }
    log->push_back(slot);
}

// Clear all lane logs (retain capacity) and the overflow flag. Called under STW
// at the start of an evac-cycle mark so each cycle's remembered set is fresh.
static void ResetEvacEdges()
{
    EnterCriticalSection(&g_evacEdgeLock);
    for (std::vector<Object**>* log : g_evacEdgeLogs)
        log->clear();
    LeaveCriticalSection(&g_evacEdgeLock);
    InterlockedExchange(&g_evacEdgeOverflow, 0);
}

// --- Dormant parity-fallback reporter --------------------------------------
// The LXR parity paths carry a few SOUND graceful-degrade fallbacks that fire
// only on pathological resource exhaustion (a bounded buffer overflowing under a
// burst). On a normal run their fire count is 0 in every validated config, and
// the paper itself degrades gracefully on overflow rather than aborting -- so we
// keep them. But a fallback must never fire SILENTLY, or a real regression that
// merely pushes the collector onto the slow-but-sound path would masquerade as
// "still correct". Each therefore reports LOUDLY the first time it fires and,
// under LXR_STRICT=1, aborts so it screams in CI/tests.
static volatile LONG64 g_parityFallbacksFired = 0;
static bool LXRStrictFallbacks()
{
    static int s = -1;
    if (s < 0) s = (getenv("LXR_STRICT") != nullptr && getenv("LXR_STRICT")[0] != '0') ? 1 : 0;
    return s != 0;
}
static void LXRReportFallback(volatile LONG* oneShot, const char* which, const char* detail)
{
    InterlockedIncrement64(&g_parityFallbacksFired);
    if (InterlockedCompareExchange(oneShot, 1, 0) != 0)
        return; // this kind already announced (one-shot to avoid log spam)
    fprintf(stderr,
            "LXRGC: [PARITY-FALLBACK] *** '%s' graceful-degrade path FIRED *** (%s). "
            "Sound but OFF the paper's fast path; it must not fire on a normal run -- "
            "investigate the resource pressure. (set LXR_STRICT=1 to abort here)\n",
            which, detail ? detail : "resource exhaustion");
    fflush(stderr);
    if (LXRStrictFallbacks())
    {
        fprintf(stderr, "LXRGC: [PARITY-FALLBACK] LXR_STRICT=1 -> aborting.\n");
        fflush(stderr);
        abort();
    }
}
static volatile LONG g_reportedEvacFullWalk   = 0;
static volatile LONG g_reportedDCopyFullWalk  = 0;
static volatile LONG g_reportedNurseryDefer   = 0;
static volatile LONG g_reportedBigArrayShape  = 0;
static volatile LONG g_reportedStraddle       = 0;

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

// --- Multi-epoch concurrent trace (LXR §3.2.3 parity, env LXR_MULTIEPOCH) ---
// The paper's SATB trace SPANS MULTIPLE RC EPOCHS and has NO dedicated trace
// pauses: the snapshot piggybacks on an ordinary RC pause, a background marker
// thread marks the transitive closure concurrently while subsequent RC pauses
// run normally, and a later RC pause finalizes the trace once the marker is
// quiescent. This differs from the legacy concurrent path, which confines a
// trace to a single collection with two dedicated (snapshot+finish) pauses,
// monopolizing the collection lock so no RC pause can interleave. Moving the
// drain onto a persistent thread frees the collection lock between the snapshot
// and finish, so RC pauses (the spanned epochs) interleave with marking.
//
// Soundness: during MARKING, g_traceWindowOpen==1 suppresses ALL decommit/reuse
// (CollectNursery, ReuseChunk, ReuseFreeRun) and the sweep runs only at the
// finish pause when the marker has already returned (sole owner of the mark
// stack), so the concurrent marker never races a decommit and two threads never
// touch the mark stack at once. Reclamation deferral of RC-zero objects until
// the trace completes is already parity-correct (see g_traceWindowOpen gating).
enum { TRACE_IDLE = 0, TRACE_MARKING = 1 };
static volatile LONG g_concDecrements = 0;  // env LXR_CONC_DECREMENTS (declared here so the marker can gate)
static volatile LONG g_multiEpoch      = 0;  // env LXR_MULTIEPOCH: span trace over RC epochs
static volatile LONG g_traceState      = TRACE_IDLE;
static volatile LONG g_markerQuiescent = 0;  // marker finished its drain-to-quiescence
static volatile LONG g_snapshotConsumed = 0; // marker has replayed the meStart snapshot decrements (safe for spanned epochs to drain)
static volatile LONG g_markerStop      = 0;  // shutdown request for the marker thread
static HANDLE  g_markerThread    = nullptr;
static HANDLE  g_markerStartEvent = nullptr; // auto-reset: wakes the marker to drain
static volatile LONG64 g_multiEpochSpans = 0; // diagnostic: RC epochs spanned across traces

static void RequestLXRCollection(bool wait, bool forceTrace); // fwd (defined below)

// Persistent background marker: waits for a snapshot to open a trace, marks the
// transitive closure to quiescence (ConcurrentTraceDrain, which itself yields so
// mutators + RC pauses make progress), then signals quiescent and parks. It runs
// exactly ONE drain per trace; the finish pause mops up residual SATB. Because it
// has returned (parked) before the finish pause runs, the driver is the sole
// owner of the mark stack at finish - no two-thread mark race.
static DWORD WINAPI LXRMarkerThreadProc(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_markerStartEvent, INFINITE);
        if (g_markerStop)
            break;
        // #1 off-pause decrement replay FIRST (before the long closure drain):
        // consume the buffers detached at the meStart snapshot (SnapshotModified-
        // Buffers) so the coalescing-RC root-deferral rotation (m_rootDeferredPrev)
        // returns to a clean state. Publishing g_snapshotConsumed then lets spanned
        // RC epochs safely run a full ProcessModifiedBuffers to DRAIN the mutation
        // accumulating during this (possibly long) marking window - bounding the
        // finish pause instead of batching the whole window's RC work + free
        // cascade into one giant STW ProcessModifiedBuffers at the finish.
        if (g_concDecrements)
            g_lxrCollector.ProcessSnapshotDecrements();
        InterlockedExchange(&g_snapshotConsumed, 1);
        g_lxrCollector.ConcurrentTraceDrain();
        InterlockedExchange(&g_markerQuiescent, 1);
        // Finalize PROMPTLY: request an RC pause now so meFinish runs right after
        // the drain instead of waiting for the next allocation trigger. Without
        // this the meStart->meFinish window stretches to the next growth/increment
        // trigger, letting the marking window's modified buffers (hence the finish
        // detach + the off-pause RC replay) grow unbounded. A prompt finalize
        // bounds the finish epoch to the drain window's mutations.
        RequestLXRCollection(/*wait*/ false, /*forceTrace*/ false);
    }
    return 0;
}

static void LXREnsureMarkerThread()
{
    if (g_markerThread != nullptr)
        return;
    if (g_markerStartEvent == nullptr)
        g_markerStartEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    g_markerThread = CreateThread(nullptr, 0, LXRMarkerThreadProc, nullptr, 0, nullptr);
}

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
// and only ever taken on the concurrent path. (Declared above near the trace
// state so the multi-epoch marker thread can gate its off-pause decrement replay.)
struct RCSnapshotEntry { Object** Slot; Object* OldValue; Object* NewValue; };
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

// Item G (§3.5): the same persistent pool also runs a generic parallel-for so
// phases beyond marking (parallel reference-count apply) scale across the GC
// worker threads. g_poolWorkKind selects which body a woken worker runs: 0 = the
// mark drain (g_poolSlices), 1 = the generic parallel-for (g_poolForFn over lane
// index). Only one pool activity runs at a time (collection is serialized), so
// this shared state needs no lock. g_poolActiveLanes is the lane count for the
// current activity (main thread = lane 0, workers = lanes 1..).
static volatile LONG g_poolWorkKind = 0;                       // 0=mark, 1=parallel-for
static void (*g_poolForFn)(int lane, int lanes, void* ctx) = nullptr;
static void*   g_poolForCtx = nullptr;
static int     g_poolActiveLanes = 1;
// Mutual exclusion for the shared worker pool. Item C's background marker drains
// the mark closure on the pool OFF-PAUSE while, on the driver thread, a spanned
// RC epoch now also drives the pool for parallel RC apply (item G). Both must not
// wake the shared workers at once, so every pool-driving site (ParallelDrainMark-
// Stack and RunOnPool) holds this lock for one complete wake->join cycle. No
// nesting (pool bodies never re-enter the pool), so no deadlock; a driver STW
// pause may briefly wait for an in-flight marker drain to finish, which is a
// latency cost, not a hang.
static CRITICAL_SECTION g_poolLock;
static volatile LONG64 g_parRCApplies = 0; // item G diagnostic: epochs applied in parallel
static volatile LONG64 g_serRCApplies = 0; // item G diagnostic: epochs applied serially

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

// Item E — LXR collection-trigger predictors (paper §3.2.2/§3.2.5).
// (a) increment-count RC trigger: fire an RC pause once N reference-count
//     increments have been logged since the last pause (reuses the existing
//     ModifiedBufferEntries counter, so no new hot-path cost). (b) wastage-based
//     trace trigger: escalate to a trace once the PROJECTED floating garbage
//     (WastagePctEwma applied to allocation since the last trace) reaches a % of
//     the heap. Both predictors use the paper's asymmetric ("biased") exponential
//     decay: react fast to a rising signal (¾ new), decay slowly on a falling one
//     (¼ new), which is conservative (never under-provisions the collector).
static volatile int64_t g_incrementBaseline = 0;  // ModifiedBufferEntries snapshot at last RC pause
static volatile int64_t g_incrementTrigger  = -1; // env LXR_INCREMENT_TRIGGER: RC pause after N increments (0=off)
static volatile int64_t g_wastageTriggerPct = -1; // env LXR_WASTAGE_PCT: trace when projected floating garbage >= % of heap (0=off)
// Item E survival-rate RC-pause trigger (paper §3.2.2 "Heuristic: RC Triggers").
// The paper's third RC-pause trigger (besides heap-full and the increment
// threshold): a young-survival predictor that modulates WHEN the pause fires so
// as to bound the EXPECTED per-pause cost (recursive increments + young-survivor
// copying), favouring throughput over worst-case allocation triggering. We
// realise it by scaling the allocation-growth budget by the young-survival EWMA:
// high predicted survival (expensive pause) -> smaller budget (pause sooner);
// low survival (cheap pause, most young die) -> larger budget (pause later, more
// young reclaimed per pass). g_survivalTrigger != 0 enables it (default ON).
static volatile int64_t g_survivalTrigger   = -1; // env LXR_SURVIVAL_TRIGGER (0=off, default on)
static volatile int64_t g_youngSurvCopiedBaseline    = 0; // NurseryCopyBytes snapshot at last RC pause
static volatile int64_t g_youngSurvReclaimedBaseline = 0; // NurseryBytesReclaimed snapshot at last RC pause

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
// Item D-copy: young-survivor copy at RC pauses (env LXR_NURSERY_COPY). When set,
// ProcessModifiedBuffers accumulates every modified slot whose CURRENT value is a
// young object into g_dcopyModifiedSlots -- the paper's "remembered set of
// references to young objects" (§3.3). This is maintained PERSISTENTLY across RC
// pauses (a young object survives many RC pauses; it only ages to mature at a
// TRACE), and reset only when a trace bumps g_traceEpoch (ResetDCopyRemset).
// CopyYoungSurvivors replays it to fix up incoming edges to moved survivors.
// Persistence is required: the coalescing modified buffer is cleared every RC
// pause, so an A->B edge created several pauses ago (B pinned/budget-skipped and
// still young) would otherwise be lost. On overflow the remset is abandoned and
// D-copy falls back to a full committed-heap walk.
static volatile LONG        g_dcopyCaptureModified = 0;
static volatile LONG        g_dcopyRemsetOverflow = 0;
static const size_t         kDCopyRemsetCap = 4u * 1024u * 1024u; // entries

// Per-evacuation-region D-copy remembered set (paper §3.3: a per-evacuation-BLOCK
// remembered set, initialized at the trace and kept up to date by the barrier;
// at evacuation only the evac candidates' remsets are processed). D-copy is
// budget-limited to a SUBSET of young regions per RC pause, so a single flat log
// of every mature->young edge would replay edges into regions not touched this
// pass. Instead we key the remset by the TARGET young object's 128KB region-slot
// (index = (target - heapBase) / CONTEXT_ALLOC_QUANTUM): each RC pass replays ONLY
// the buckets of the young regions it actually evacuated, leaving edges into
// regions deferred to a later pass in their buckets untouched. This bounds 4b work
// to O(incoming edges of the evacuated regions), matching the paper's scoped
// per-block remset rather than an O(all-young-edges) flat replay. g_dcopyRemsetCount
// tracks the live total for the overflow cap; buckets are reset at a trace epoch
// bump (the window's young ages to mature) or on overflow.
static std::vector<std::vector<Object**>> g_dcopyRemsetBuckets; // [regionSlot] -> incoming field slots
static size_t                             g_dcopyRemsetCount = 0;

// Drop every entry (trace epoch bump / overflow). Keeps the outer vector sized so
// the next window does not re-grow it.
static inline void DCopyRemsetClearAll()
{
    for (auto& b : g_dcopyRemsetBuckets)
    {
        b.clear();
        b.shrink_to_fit();
    }
    g_dcopyRemsetCount = 0;
}

// Append field `slot` (whose current value `target` is a young object) to the
// per-region remembered set, keyed by target's 128KB region. Returns false and
// sets/clears-on overflow when the live total exceeds the cap.
static inline bool DCopyRemsetAppend(Object** slot, Object* target, uint8_t* heapBase, size_t heapBytes)
{
    if (g_dcopyRemsetBuckets.empty())
        g_dcopyRemsetBuckets.resize(heapBytes / CONTEXT_ALLOC_QUANTUM + 1);
    if (g_dcopyRemsetCount >= kDCopyRemsetCap)
    {
        g_dcopyRemsetOverflow = 1;
        DCopyRemsetClearAll();
        return false;
    }
    size_t rs = (size_t)((uint8_t*)target - heapBase) / CONTEXT_ALLOC_QUANTUM;
    if (rs >= g_dcopyRemsetBuckets.size())
        return false;
    g_dcopyRemsetBuckets[rs].push_back(slot);
    g_dcopyRemsetCount++;
    return true;
}
static volatile int64_t g_traceEpoch = 0;

// Env-gated diagnostic (LXR_NURSERY_GUARD): a ring of recently-decommitted young
// ranges. If a marker later reaches an object inside one, we log the exact
// parent->child edge that kept it reachable (proving which store the RC missed)
// instead of faulting on the decommitted page. Declared here (above the markers)
// so DrainMarkStack/DrainSliceLocal can reference it.
struct FreedYoungRange { uint8_t* s; uint8_t* e; int64_t pass; };
static FreedYoungRange g_freedYoung[1024];
static volatile LONG    g_freedYoungCount = 0;
static int g_nurseryGuard = -1;
static void RecordFreedYoung(uint8_t* s, uint8_t* e, int64_t pass)
{
    if (g_nurseryGuard <= 0) return;
    LONG idx = InterlockedIncrement(&g_freedYoungCount) - 1;
    FreedYoungRange& r = g_freedYoung[idx & 1023];
    r.s = s; r.e = e; r.pass = pass;
}
static bool InFreedYoung(uint8_t* p)
{
    if (g_nurseryGuard <= 0) return false;
    LONG n = g_freedYoungCount; if (n > 1024) n = 1024;
    for (LONG k = 0; k < n; k++)
        if (p >= g_freedYoung[k].s && p < g_freedYoung[k].e)
            return true;
    return false;
}

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
static volatile LONG g_collectorReady = 0;     // SET by the collector thread once it
                                               // is alive and parked on its request
                                               // event. Until then NO collection may
                                               // be triggered: the only alternative
                                               // is to drive SuspendEE from a random
                                               // cooperative mutator (the documented
                                               // early-startup deadlock, seen as an
                                               // intermittent hang when EventPipe/
                                               // dotnet-counters attaches mid-init).
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
    uint8_t           DeadPctEstimate; // Item F: last Evacuate's occupancy scan result
                                  // (dead bytes / total, 0-100); predicts evac candidacy
                                  // at the next trace's snapshot (0 => not a candidate).
};
static ChunkRegion* g_chunks = nullptr;
static size_t g_chunkCount = 0;
static size_t g_chunkCap = 0;
static size_t* g_freeChunks = nullptr;   // stack of reclaimed (decommitted) chunk indices
static size_t g_freeChunkTop = 0;
static size_t g_freeChunkCap = 0;
static CRITICAL_SECTION g_chunkLock;

// Deferred (off-pause) decommit (LXR_DEFER_DECOMMIT, default ON). The sweep runs
// under STW; the dominant sweep cost is VirtualFree(MEM_DECOMMIT) (~94% of the
// decommit phase, O(freed pages), and the source of the rare multi-hundred-ms
// sweep spike when a large dead area is reclaimed at once). Instead of freeing
// pages inside the pause, the sweep records dead regions here (Committed=false,
// RC/logged already cleared, but NOT yet on g_freeChunks so no allocator can
// reuse them, and NOT yet physically decommitted). DrainPendingDecommit() runs
// AFTER RestartEE (mutators live) to VirtualFree each range and THEN push the
// index onto g_freeChunks -- preserving the invariant that every g_freeChunks
// entry is decommitted. Serialized by the single-driver collection loop, so the
// drain always completes before the next sweep.
static int g_deferDecommit = 1;
static std::vector<std::pair<uint8_t*, uint8_t*>> g_pendingDecommit;
static std::vector<size_t> g_pendingFreeChunks;

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
        // Account the bytes actually consumed from this chunk. The runtime
        // bump-allocates inline within [Start, alloc_limit] without re-entering
        // the GC, so the only faithful place to tally allocation volume is here,
        // when a filled chunk is retired: its true used extent is [Start, usedEnd)
        // (usedEnd == the exhausted context's alloc_ptr). Counting alignedSize on
        // the slow path instead would only ever see the first object per chunk and
        // undercount ~by the object-count-per-chunk factor.
        uint8_t* start = g_chunks[index].Start;
        if (usedEnd > start)
            InterlockedExchangeAdd64(&g_lxrCounters.TotalAllocatedBytes, (int64_t)(usedEnd - start));
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
    g_chunks[idx].DeadPctEstimate = 0;
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
    // Per-page committed bitmap for the RC table so the hot serial RC apply
    // (RCIncrement/RCDecrement) skips the VirtualAlloc(MEM_COMMIT) syscall once a
    // page is committed. Was ~1us/RC-op (~3ms/RC pause on the serial path).
    m_rcPageCount = (rcTableBytes + g_pageSize - 1) / g_pageSize;
    m_rcPageCommitted = (uint8_t*)calloc((m_rcPageCount + 7) / 8, 1);

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
    // Per-page committed bitmap for the block-meta table so MetaForBlock (a very
    // hot per-field call in the RC pause) can skip the VirtualAlloc(MEM_COMMIT)
    // syscall once a page is committed. Was costing ~1us/field (~4ms/RC pause).
    m_metaPageCount = (metaBytes + g_pageSize - 1) / g_pageSize;
    m_metaPageCommitted = (uint8_t*)calloc((m_metaPageCount + 7) / 8, 1);

    // Immix line-mark side table: 1 bit per 256 B line. Reserved only; committed
    // and zeroed per trace over the used-heap prefix (see ResetMarks). Enables
    // O(lines) free-line-run discovery for line reuse (LXR_LINE_REUSE).
    size_t lineTableBytes = (heapReservedBytes / lxr::kLineSize + 7) / 8;
    m_lineMarkTable = (uint8_t*)VirtualAlloc(nullptr, lineTableBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_lineMarkTable == nullptr)
        return false;

    // Item F (paper §3.3.4): per-line reuse-version table, 1 uint16 per 256 B
    // line. Reserved only; committed to the used-heap prefix alongside the mark
    // table (EnsureLineReuseCommitted). Backs the persistent evac remembered set's
    // stale-entry filtering.
    size_t lineVerBytes = (heapReservedBytes / lxr::kLineSize) * sizeof(uint16_t);
    m_lineReuseVer = (uint16_t*)VirtualAlloc(nullptr, lineVerBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_lineReuseVer == nullptr)
        return false;

    // Item F (paper §3.3): evacuation-candidate bytemap, 1 byte per 128 KiB
    // region-slot. Small enough (heap/128KB bytes; 128 KiB for a 16 GiB heap) to
    // commit up front. Read by the hot barrier + mark closures, so keep it fully
    // resident rather than lazy-committed.
    g_evacCandidateBase  = heapBase;
    g_evacCandidateSlots = heapReservedBytes / CONTEXT_ALLOC_QUANTUM + 1;
    g_evacCandidate = (uint8_t*)VirtualAlloc(nullptr, g_evacCandidateSlots, MEM_COMMIT, PAGE_READWRITE);
    if (g_evacCandidate == nullptr)
        return false;

    InitializeCriticalSection(&m_collectLock);
    InitializeCriticalSection(&g_buffersLock);
    InitializeCriticalSection(&g_satbLock);
    InitializeCriticalSection(&g_remsetLock);
    InitializeCriticalSection(&g_evacEdgeLock);
    InitializeCriticalSection(&g_poolLock);
    InitializeCriticalSection(&g_chunkLock);
    InitializeCriticalSection(&g_bigArrayLock);

    // Full-LXR parity is the DEFAULT (1:1 with the paper). Each knob below is now
    // default-ON and only an explicit "=0" opts OUT (for A/B testing) - mirroring
    // LXR_RC_RECLAIM / LXR_PARALLEL_RC. LXR_SATB stays a pure STW-exercise
    // diagnostic (SATB is driven by the concurrent trace), so it is opt-IN.
    auto envOn = [](const char* name) -> int {
        const char* e = getenv(name);
        return (e != nullptr && e[0] == '0') ? 0 : 1; // default ON unless "=0"
    };
    if (getenv("LXR_SATB") != nullptr)   g_satbActive = 1;
    g_remsetActive     = envOn("LXR_REMSET");
    g_evacActive       = envOn("LXR_EVAC");
    if (getenv("LXR_FAULT_DIAG") != nullptr) AddVectoredExceptionHandler(1, &LXRFaultDiag);
    // P4: run the backup trace concurrently with the mutators (two brief STW
    // pauses + off-pause marking). The SATB window is opened per-trace, so
    // g_satbActive is NOT forced on here.
    g_concurrentEnabled = envOn("LXR_CONCURRENT");
    // Item C (paper §3.2.3): span the SATB trace over multiple RC epochs with no
    // dedicated trace pauses (snapshot + finish piggyback on RC pauses; a
    // background thread marks concurrently across the spanned epochs). Requires
    // LXR_CONCURRENT. See g_multiEpoch.
    g_multiEpoch = envOn("LXR_MULTIEPOCH");
    // #1: replay coalescing-RC decrements + the recursive free OFF the STW
    // pause (concurrent path only). Only the bounded buffer snapshot is paused.
    if (getenv("LXR_CONC_DECREMENTS") != nullptr) g_concDecrements = 1;
    // Item D (paper §2.1/§3.3): reference-count young objects from birth. A young
    // object is born RC 0; references to it accrue RC via the coalescing barrier /
    // deferred root capture exactly like a mature object, and a young object still
    // at RC 0 at an RC pause is implicitly dead (reclaimed by CollectNursery).
    g_youngRC = envOn("LXR_YOUNG_RC");
    // Item D: implicitly-dead-young reclamation at RC pauses (CollectNursery).
    // Young liveness is decided by the precise coalescing RC itself: after a pause
    // applies all increments (modbuf new-values + deferred root captures) and
    // decrements, young regions in which every young object is RC 0 are reclaimed.
    // No remembered set and no O(heap) closure - the RC side table is the oracle.
    // Guards on g_traceWindowOpen so it never frees under an in-flight trace.
    g_nurseryActive = envOn("LXR_NURSERY");
    // Item D-copy: young-survivor copy at the RC pause. When enabled, tell
    // ProcessModifiedBuffers to snapshot the epoch's modified slots so D-copy can
    // replay them as its (complete, bounded) remembered set for reference fix-up.
    g_dcopyCaptureModified = envOn("LXR_NURSERY_COPY");
    // P5: parallel mark worker count. Clamped to [1, 64]; 1 keeps the verified
    // single-threaded closure.
    // Parallelism in every phase is part of paper parity (§3.5): default the GC
    // worker count to the machine's logical processors (clamped) rather than 1.
    // LXR_GC_THREADS overrides (and =1 forces the serial closure for A/B testing).
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int cores = (int)si.dwNumberOfProcessors;
        if (cores < 1) cores = 1;
        if (cores > 64) cores = 64;
        g_gcThreads = cores;
    }
    if (const char* t = getenv("LXR_GC_THREADS"))
    {
        int n = atoi(t);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        g_gcThreads = n;
    }
    // Item G (§3.5): partition a single very large reference array's mark scan
    // across the worker pool. Default-ON (parity); LXR_PARALLEL_BIGARRAY=0 opts out
    // (each lane then scans a claimed array end-to-end, as before).
    g_bigArrayParallel = envOn("LXR_PARALLEL_BIGARRAY");
    return true;
}

lxr::BlockMeta* LXRCollector::MetaForBlock(uint8_t* blockAddr)
{
    size_t idx = (size_t)(blockAddr - m_heapBase) / lxr::kBlockSize;
    if (idx >= m_blockCount)
        return nullptr;
    // Fast path: commit the block-meta-table page lazily, but only once per page.
    // A per-page committed bit avoids a VirtualAlloc(MEM_COMMIT) syscall on every
    // call (this is the hottest per-field lookup in the RC pause).
    uint8_t* addr = (uint8_t*)&m_blockMeta[idx];
    if (m_metaPageCommitted != nullptr)
    {
        size_t pg = (size_t)((uintptr_t)addr - (uintptr_t)m_blockMeta) / g_pageSize;
        if (!(m_metaPageCommitted[pg >> 3] & (uint8_t)(1u << (pg & 7))))
        {
            uintptr_t page = (uintptr_t)addr & ~((uintptr_t)g_pageSize - 1);
            VirtualAlloc((void*)page, g_pageSize, MEM_COMMIT, PAGE_READWRITE);
            m_metaPageCommitted[pg >> 3] |= (uint8_t)(1u << (pg & 7));
        }
    }
    else
    {
        CommitPageFor(addr);
    }
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

// Stamp the blocks spanned by [start, start+size) as MATURE (aged out of the
// current inter-trace window) so IsYoung() reports false for objects placed
// there. Used by CopyYoungSurvivors for the promotion destinations: survivors
// copied to fresh chunks must land in mature space, otherwise RegisterChunk's
// StampBornEpoch would leave them young and they would be re-evacuated at every
// subsequent RC pause (infinite copying). g_traceEpoch-1 is behind every real
// (>=0) epoch, so a promoted survivor is never young again until, and unless, a
// real trace legitimately re-ages the block.
void LXRCollector::StampMatureEpoch(uint8_t* start, size_t size)
{
    if (!g_youngRC || start < m_heapBase)
        return;
    int64_t mature = g_traceEpoch - 1;
    uint8_t* blk = (uint8_t*)((uintptr_t)start & ~(lxr::kBlockSize - 1));
    uint8_t* end = start + size;
    for (; blk < end; blk += lxr::kBlockSize)
    {
        lxr::BlockMeta* meta = MetaForBlock(blk);
        if (meta != nullptr)
            meta->bornTraceEpoch = mature;
    }
}

uint8_t* LXRCollector::RCSlot(Object* obj) const
{
    size_t idx = ((uint8_t*)obj - m_heapBase) / lxr::kObjectGranule;
    return &m_rcTable[idx];
}

// Commit the RC-table page backing `slot` lazily, but only once per page (a
// per-page committed bit avoids a VirtualAlloc syscall on every RC op). Safe
// under the parallel free cascade: the bit is set only AFTER the commit, so a
// set bit always implies a committed page; a race merely re-commits (idempotent).
void LXRCollector::EnsureRCPage(uint8_t* slot)
{
    if (m_rcPageCommitted == nullptr) { CommitPageFor(slot); return; }
    size_t pg = (size_t)((uintptr_t)slot - (uintptr_t)m_rcTable) / g_pageSize;
    if (pg >= m_rcPageCount) { CommitPageFor(slot); return; }
    if (!(m_rcPageCommitted[pg >> 3] & (uint8_t)(1u << (pg & 7))))
    {
        uintptr_t page = (uintptr_t)slot & ~((uintptr_t)g_pageSize - 1);
        VirtualAlloc((void*)page, g_pageSize, MEM_COMMIT, PAGE_READWRITE);
        m_rcPageCommitted[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    }
}

void LXRCollector::RCIncrement(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return; // not our heap (frozen segment, boot object, etc.)
    // Young objects ARE reference-counted (paper §2.1/§3.3): a reference to a young
    // object is an increment via the coalescing barrier / root deferral, and young
    // objects still at RC 0 at the RC pause are implicitly dead (CollectNursery).
    uint8_t* slot = RCSlot(obj);
    EnsureRCPage(slot);
    if (*slot != 0xFF) // 0xFF is the "stuck / overflowed" sentinel
        (*slot)++;
    InterlockedIncrement64(&g_lxrCounters.RCIncrements);
}

bool LXRCollector::RCDecrement(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return false;
    uint8_t* slot = RCSlot(obj);
    EnsureRCPage(slot);
    InterlockedIncrement64(&g_lxrCounters.RCDecrements);
    if (*slot == 0 || *slot == 0xFF)
        return false; // already zero, or stuck-high (resolved by backup trace)
    (*slot)--;
    return (*slot == 0);
}

// Item G (§3.5): thread-safe RC apply. The RC slot is a single byte; a CAS loop
// (_InterlockedCompareExchange8) makes the saturating increment / floored
// decrement atomic so multiple parallel-RC workers can touch the same object's
// count without a lost update. The page is pre-committed by the serial flatten
// pass, so no VirtualAlloc happens here (that would not be safe to contend and
// would defeat the parallelism).
void LXRCollector::RCIncrementAtomic(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return;
    char volatile* p = (char volatile*)RCSlot(obj);
    for (;;)
    {
        char cur = *p;
        if ((uint8_t)cur == 0xFF)               // stuck-high sentinel: never wraps
            break;
        char nxt = (char)((uint8_t)cur + 1);
        if (_InterlockedCompareExchange8(p, nxt, cur) == cur)
            break;
    }
    InterlockedIncrement64(&g_lxrCounters.RCIncrements);
}

bool LXRCollector::RCDecrementAtomic(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return false;
    InterlockedIncrement64(&g_lxrCounters.RCDecrements);
    char volatile* p = (char volatile*)RCSlot(obj);
    for (;;)
    {
        char cur = *p;
        uint8_t u = (uint8_t)cur;
        if (u == 0 || u == 0xFF)                // already zero / stuck-high
            return false;
        char nxt = (char)(u - 1);
        if (_InterlockedCompareExchange8(p, nxt, cur) == cur)
            return (u - 1) == 0;                // exactly one worker sees the 1->0 edge
    }
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
        if (mb != nullptr) { mb->InUse = 1; t_modifiedBuffer = mb; RegisterModifiedBuffer(mb); }
    }
    // Keep the shared free-list of spare modified buffers topped up OFF the
    // barrier (a proper allocating frame). The barrier swaps to one of these when
    // its current buffer fills, so a full buffer is never dropped (paper's shared
    // queue). A spare per hardware thread comfortably absorbs bursts between two
    // allocation-path visits; each spare holds 4096 first-logs.
    const LONG kSpareTarget = 64;
    while (g_freeModifiedCount < kSpareTarget)
    {
        ModifiedBuffer* mb = new (std::nothrow) ModifiedBuffer();
        if (mb == nullptr) break;
        RegisterModifiedBuffer(mb);   // registered so the STW drain walks it
        PushFreeModifiedBuffer(mb);   // available for the barrier to swap in
    }
    if (t_satbBuffer == nullptr)
    {
        SatbBuffer* sb = new (std::nothrow) SatbBuffer();
        if (sb != nullptr) { t_satbBuffer = sb; RegisterSatbBuffer(sb); }
    }
    if (t_remsetBuffer == nullptr)
    {
        RemsetBuffer* rb = new (std::nothrow) RemsetBuffer();
        if (rb != nullptr) { rb->InUse = 1; t_remsetBuffer = rb; RegisterRemsetBuffer(rb); }
    }
    // Keep the shared free-list of spare remset buffers topped up OFF the barrier
    // (item F, §3.2.1). The barrier swaps to one of these when a thread's remset
    // buffer fills mid-window, so an inter-block edge is never dropped (which would
    // force the O(live-heap) full-walk evac fixup). Each spare holds 4096 edges.
    if (g_remsetActive)
    {
        const LONG kRemsetSpareTarget = 64;
        while (g_freeRemsetCount < kRemsetSpareTarget)
        {
            RemsetBuffer* rb = new (std::nothrow) RemsetBuffer();
            if (rb == nullptr) break;
            RegisterRemsetBuffer(rb);   // registered so consumers/compaction walk it
            PushFreeRemsetBuffer(rb);   // available for the barrier to swap in
        }
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
    // Item F: grow the per-line reuse-version table on the same alloc-path cadence
    // so a young region reclaimed before the next trace already has committed
    // version slots to bump (else its reclamation would go unversioned and a stale
    // remset entry into it could escape filtering).
    EnsureLineReuseCommitted(usedBytes);
}

// Unlogged-bit test-and-set (paper §3.4). Returns true the FIRST time a field is
// stored this epoch (so the barrier logs it exactly once), false thereafter.
// Off-heap slots always log (no coalescing metadata). If the covering bitmap page
// is not yet committed (a store racing ahead of the allocation-path commit -
// vanishingly rare), we log without coalescing rather than fault or commit here.
// Hot path: one monotonic-watermark compare + one word read + at most one atomic.
bool LXRCollector::TryFirstLogField(Object** slot)
{
    static int s_noCoalesce = -1;
    if (s_noCoalesce < 0)
        s_noCoalesce = (getenv("LXR_NO_COALESCE") != nullptr) ? 1 : 0;
    if (s_noCoalesce)
        return true; // diagnostic: log every store, never coalesce (stale-bit test)
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

// Clear every logged bit covering [start, end). CRITICAL for young RC soundness:
// when a region is reclaimed and its address range is later REUSED for a fresh
// object, that object's field slots must start UNlogged so the coalescing barrier
// (TryFirstLogField) records their first init store. The logged table is a
// separate side table that is never decommitted with the heap, so a stale set bit
// left by a prior occupant would make TryFirstLogField return false and DROP the
// new object's first-store buffer entry -> its RC increment is lost -> the young
// object is undercounted (RC 0) and the nursery frees it while still live. Called
// at every reclaim/reuse site (decommit + line-run carve), mirroring ClearRCRange.
// STW only (non-atomic byte writes). Bounded to the committed logged prefix.
void LXRCollector::ClearLoggedRange(uint8_t* start, uint8_t* end)
{
    // Item F (§3.3.4): this is the canonical "range reclaimed/reused" hook, so it
    // is also where the persistent evac remembered set's per-line reuse version is
    // bumped. Any remset entry recorded against a line in [start,end) becomes
    // STALE here (its source object is gone / the line may be reused), and evac
    // fix-up detects that by the advanced version. Bump BEFORE the logged-bit
    // clamp mutates start/end.
    BumpLineReuseRange(start, end);
    if (m_loggedTable == nullptr) return;
    if (start < m_heapBase) start = m_heapBase;
    if (end > m_heapBase + m_heapBytes) end = m_heapBase + m_heapBytes;
    if (end <= start) return;
    size_t gStart = (size_t)(start - m_heapBase) / lxr::kObjectGranule;
    size_t gEnd   = (size_t)(end - m_heapBase + lxr::kObjectGranule - 1) / lxr::kObjectGranule;
    // Clear whole bytes strictly inside [gStart, gEnd); handle the partial head/
    // tail bytes bit-by-bit so we never touch a neighboring region's granules.
    size_t g = gStart;
    while (g < gEnd)
    {
        size_t byteOff = g >> 3;
        if (byteOff >= m_loggedCommittedBytes) break; // beyond committed prefix: already 0
        if ((g & 7) == 0 && g + 8 <= gEnd)
        {
            m_loggedTable[byteOff] = 0; // full byte (8 granules)
            g += 8;
        }
        else
        {
            m_loggedTable[byteOff] &= ~(uint8_t)(1u << (g & 7));
            g++;
        }
    }
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

// --- Item F (§3.3.4): per-line reuse versioning ---------------------------
// Commit the version-table prefix covering the used heap. Unlike the mark/line
// tables (re-zeroed each cycle), this table PERSISTS for the whole run: only the
// committed prefix grows, and freshly-committed pages are zero (version 0). Never
// memset the existing prefix (it would wipe live versions). STW only.
void LXRCollector::EnsureLineReuseCommitted(size_t usedBytes)
{
    if (m_lineReuseVer == nullptr) return;
    size_t needLines = usedBytes / lxr::kLineSize + 1;
    size_t needBytes = needLines * sizeof(uint16_t);
    needBytes = (needBytes + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
    size_t cap = (m_heapBytes / lxr::kLineSize) * sizeof(uint16_t);
    if (needBytes > cap) needBytes = cap;
    if (needBytes > m_lineReuseVerCommittedBytes)
    {
        // MEM_COMMIT is idempotent and does NOT re-zero already-committed pages,
        // so committing the whole prefix preserves existing versions; only the
        // new tail pages come in zeroed.
        VirtualAlloc(m_lineReuseVer, needBytes, MEM_COMMIT, PAGE_READWRITE);
        m_lineReuseVerCommittedBytes = needBytes;
    }
}

// Read a line's current reuse version. Off-prefix / off-heap => 0 (a never-
// reclaimed line). Safe to call from the barrier (monotonic committed prefix).
uint32_t LXRCollector::LineReuseVerOf(void* addr) const
{
    if (m_lineReuseVer == nullptr) return 0;
    uint8_t* p = (uint8_t*)addr;
    if (p < m_heapBase || p >= m_heapBase + m_heapBytes) return 0;
    size_t line = (size_t)(p - m_heapBase) / lxr::kLineSize;
    if (line * sizeof(uint16_t) >= m_lineReuseVerCommittedBytes) return 0;
    return m_lineReuseVer[line];
}

// Increment the reuse version of every line overlapping [start,end). STW only
// (non-atomic 16-bit increments): every caller (carve, sweep decommit, evac free)
// runs inside a collection pause.
void LXRCollector::BumpLineReuseRange(uint8_t* start, uint8_t* end)
{
    if (m_lineReuseVer == nullptr || end <= start) return;
    if (start < m_heapBase) start = m_heapBase;
    if (end > m_heapBase + m_heapBytes) end = m_heapBase + m_heapBytes;
    if (end <= start) return;
    size_t firstLine = (size_t)(start - m_heapBase) / lxr::kLineSize;
    size_t lastLine  = (size_t)(end - 1 - m_heapBase) / lxr::kLineSize;
    size_t maxLine = m_lineReuseVerCommittedBytes / sizeof(uint16_t);
    if (firstLine >= maxLine) return;
    if (lastLine >= maxLine) lastLine = maxLine - 1;
    for (size_t line = firstLine; line <= lastLine; line++)
        m_lineReuseVer[line]++;
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
        if (buf == nullptr || buf->Count >= ModifiedBuffer::kCapacity)
        {
            // Current buffer full (or absent): hand it to the shared queue by
            // swapping in a fresh pre-registered spare (paper §3.2.1) so NO
            // first-log is ever dropped. The old buffer stays registered and is
            // drained + recycled at the next RC pause. Both writes below are by
            // this buffer's sole owner (this thread), so they need no atomicity.
            ModifiedBuffer* fresh = PopFreeModifiedBuffer();
            if (fresh != nullptr)
            {
                if (buf != nullptr) buf->InUse = 0; // swapped away: recyclable at drain
                fresh->Count = 0;
                fresh->InUse = 1;
                t_modifiedBuffer = fresh;
                buf = fresh;
            }
        }
        if (buf != nullptr && buf->Count < ModifiedBuffer::kCapacity)
        {
            buf->Entries[buf->Count].Slot = slot;
            buf->Entries[buf->Count].OldValue = oldValue;
            buf->Count++;
            InterlockedIncrement64(&g_lxrCounters.ModifiedBufferEntries);
        }
        else
        {
            // Shared free-list momentarily exhausted (vanishingly rare: 64 spares,
            // each 4096 first-logs, replenished on every allocation) or buffer
            // absent (a store before this thread's first allocation). A logged bit
            // is now set with no buffer entry, so the RC increment for this store
            // is lost -> mark the young RC as incomplete until the next complete
            // trace re-establishes liveness (CollectNursery skips reclaiming while
            // set), and force the sound full trace closure this cycle.
            InterlockedExchange(&g_modifiedOverflow, 1);
            InterlockedExchange(&g_youngRCIncomplete, 1);
        }
    }
    // Buffers are handed to the shared free-list on fill (above), so first-logs are
    // never dropped except on the rare free-list exhaustion handled above.

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
        // Item F (paper §3.3): when candidate-scoping is active this trace, record
        // ONLY edges whose TARGET is an evacuation candidate (the paper's per-block
        // remset). This bounds the persistent remset (and its finish-pause replay)
        // to candidate incoming edges. Pre-existing candidate edges not mutated
        // this window are supplied by the concurrent mark (g_evacEdgeLogs), so the
        // union stays complete. When scoping is off, record every inter-block edge
        // (the prior heap-wide behaviour).
        if (sblk != tblk && (!g_evacCandidateScope || IsEvacCandidateAddr(newValue)))
        {
            RemsetBuffer* rb = t_remsetBuffer;
            if (rb == nullptr || rb->Count >= RemsetBuffer::kCapacity)
            {
                // Item F (§3.2.1): current buffer full (or absent) -> swap in a
                // pre-registered spare from the shared free-list so NO inter-block
                // edge is dropped (a drop would force the O(live-heap) full-walk
                // evac fixup). The old buffer stays registered (consumers read the
                // whole chain) and is recycled at CompactRemsets. Sole-owner writes.
                RemsetBuffer* fresh = PopFreeRemsetBuffer();
                if (fresh != nullptr)
                {
                    if (rb != nullptr) rb->InUse = 0; // swapped away: recyclable at compact
                    fresh->Count = 0;
                    fresh->InUse = 1;
                    t_remsetBuffer = fresh;
                    rb = fresh;
                }
            }
            if (rb != nullptr && rb->Count < RemsetBuffer::kCapacity)
            {
                // Item F (§3.3.4): tag with the source line's current reuse
                // version so evac can drop this entry as stale if the line is
                // reclaimed before the next evacuation.
                rb->Entries[rb->Count] = slot;
                rb->Ver[rb->Count] = LineReuseVerOf(slot);
                rb->Count++;
                InterlockedIncrement64(&g_lxrCounters.RemsetEntries);
            }
            else
            {
                // Shared free-list momentarily exhausted (rare: spares topped up on
                // every allocation) -> dropped an inter-block edge. Mark the remset
                // incomplete so evac uses the sound full-walk fixup and the nursery
                // falls back to the authoritative trace this cycle.
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

// Item F (§3.3.4): register an inter-block edge created by GC-internal relocation
// (evac copy / young-survivor promotion). memcpy stores skip the barrier, so the
// copy's outgoing inter-block references would otherwise be absent from the
// persistent remembered set and dangle at a later evacuation. Appends (slot,
// current line reuse version) to the collector-owned buffer. STW / single-thread.
void LXRCollector::RecordRemsetEdge(Object** slot)
{
    if (!g_remsetActive)
        return;
    if (g_collectorRemset == nullptr || g_collectorRemset->Count >= RemsetBuffer::kCapacity)
    {
        RemsetBuffer* nb = new (std::nothrow) RemsetBuffer();
        if (nb == nullptr) { InterlockedExchange(&g_remsetOverflow, 1); return; }
        nb->InUse = 1;                 // collector-owned: never recycled as a spare
        RegisterRemsetBuffer(nb);      // link into the shared chain
        g_collectorRemset = nb;
    }
    RemsetBuffer* rb = g_collectorRemset;
    rb->Entries[rb->Count] = slot;
    rb->Ver[rb->Count] = LineReuseVerOf(slot);
    rb->Count++;
    InterlockedIncrement64(&g_lxrCounters.RemsetEntries);
}

// Item F (§3.3.4): the persistent alternative to ResetRemsets. Called at trace
// finish AFTER Evacuate() has consumed the set. Instead of clearing (which would
// drop still-valid edges that the barrier will never re-log - notably memcpy'd
// relocation edges), it prunes:
//   * STALE entries  - the source line's reuse version advanced since insert, so
//                      the referrer object was reclaimed / the line reused;
//   * DUPLICATE entries - the same slot logged multiple times.
// keeping the set bounded to the live inter-block edge working set. If the barrier
// dropped entries at any point (g_remsetOverflow), completeness is lost and cannot
// be recovered by pruning, so it REBUILDS the set from the just-completed mark
// (every live inter-block edge is re-derivable by scanning marked objects) - an
// O(live) pass that runs only on the rare overflow. STW only.
void LXRCollector::CompactRemsets()
{
    if (g_remsetOverflow)
    {
        EnterCriticalSection(&g_remsetLock);
        // Discard the (incomplete) set: clear every buffer and recycle swapped-away
        // per-thread buffers (InUse==0) that held data back onto the spare list.
        // A thread's current buffer (InUse==1) and collector buffers stay put.
        for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
        {
            bool hadData = rb->Count > 0;
            rb->Count = 0;
            if (hadData && rb->InUse == 0)
                PushFreeRemsetBuffer(rb);
        }
        LeaveCriticalSection(&g_remsetLock);
        EnterCriticalSection(&g_chunkLock);
        size_t n = g_chunkCount;
        for (size_t i = 0; i < n; i++)
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
                if (!IsMarked(o))
                    continue;
                GCScanObjectRefs(o, sz, [this](Object** f)
                {
                    Object* nv = *f;
                    if (nv == nullptr || !InHeap(nv))
                        return;
                    uintptr_t sb = (uintptr_t)f  & ~(lxr::kBlockSize - 1);
                    uintptr_t tb = (uintptr_t)nv & ~(lxr::kBlockSize - 1);
                    if (sb != tb)
                        RecordRemsetEdge(f);
                });
            }
        }
        LeaveCriticalSection(&g_chunkLock);
        InterlockedExchange(&g_remsetOverflow, 0);
        InterlockedIncrement64(&g_lxrCounters.RemsetRebuilds);
        return;
    }

    EnterCriticalSection(&g_remsetLock);
    std::unordered_set<Object**> seen;
    for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
    {
        bool hadData = rb->Count > 0;
        size_t w = 0;
        for (size_t i = 0; i < rb->Count; i++)
        {
            Object** slot = rb->Entries[i];
            if (LineReuseVerOf(slot) != rb->Ver[i])
                continue;                       // stale: source line reclaimed
            if (!seen.insert(slot).second)
                continue;                       // duplicate
            rb->Entries[w] = slot;
            rb->Ver[w] = rb->Ver[i];
            w++;
        }
        rb->Count = w;
        // A swapped-away per-thread buffer (InUse==0) whose entries all pruned to
        // empty is returned to the spare free-list for reuse (paper §3.2.1). Guard
        // on hadData so an already-free spare (Count was 0) is never double-pushed;
        // current/collector buffers (InUse==1) are never recycled.
        if (hadData && w == 0 && rb->InUse == 0)
            PushFreeRemsetBuffer(rb);
    }
    LeaveCriticalSection(&g_remsetLock);
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
        ClearLoggedRange(fa, fb); // carved dead run is reused -> must start unlogged
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

// Item ★ Stage 2 — RC-authoritative dead-object-run carving (see header). Mirrors
// CarveFreeRuns' re-tiling, but enumerates dead runs by walking real object
// boundaries and consulting the RC table (+ roots) instead of the stale line
// marks, so it is sound OUTSIDE a trace window. Returns bytes carved for reuse.
int64_t LXRCollector::CarveDeadRunsByRC(size_t i, const std::vector<uint8_t*>& rootSorted)
{
    uint8_t* Start = g_chunks[i].Start;
    uint8_t* End   = g_chunks[i].UsedEnd;
    if (End <= Start)
        return 0;
    size_t minRun = g_lineReuseMinBytes ? g_lineReuseMinBytes : (4 * lxr::kLineSize);

    auto rootInRange = [&](uint8_t* s, uint8_t* e) -> bool {
        auto it = std::lower_bound(rootSorted.begin(), rootSorted.end(), s);
        return it != rootSorted.end() && *it < e;
    };
    // Page-cached RC read: an object never incremented has a reserved-but-
    // uncommitted RC page (definitionally RC 0); guard the read so a dormant page
    // never faults the walk.
    uint8_t* rcCacheBase = nullptr; size_t rcCacheLen = 0; bool rcCacheCommitted = false;
    auto rcValue = [&](Object* o) -> uint8_t {
        uint8_t* slot = RCSlot(o);
        if (slot < rcCacheBase || slot >= rcCacheBase + rcCacheLen)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(slot, &mbi, sizeof(mbi)) == 0) return 0;
            rcCacheBase = (uint8_t*)mbi.BaseAddress;
            rcCacheLen = mbi.RegionSize;
            rcCacheCommitted = (mbi.State == MEM_COMMIT);
        }
        return rcCacheCommitted ? *slot : (uint8_t)0;
    };

    // Linear-walk object boundaries, coalescing consecutive DEAD objects into free
    // segments. Snapped to real object starts on both ends => parse stays valid.
    struct Seg { uint8_t* a; uint8_t* b; };
    Seg segs[512];
    int nseg = 0;
    uint8_t* runStart = nullptr;
    uint8_t* p = Start;
    while (p < End)
    {
        Object* o = (Object*)p;
        size_t sz = LXRObjectSize(o);
        if (sz == 0)
            return 0;                       // unparseable: never carve this region
        // Live if RC>0 (incl. stuck 0xFF), young, or covered by a raw root.
        bool live = (rcValue(o) != 0) || IsYoung(o) || rootInRange(p, p + sz);
        if (!live)
        {
            if (runStart == nullptr) runStart = p;
        }
        else if (runStart != nullptr)
        {
            if ((size_t)(p - runStart) >= minRun && nseg < (int)(sizeof(segs)/sizeof(segs[0])))
                segs[nseg++] = { runStart, p };
            runStart = nullptr;
        }
        p += sz;
    }
    if (runStart != nullptr && (size_t)(End - runStart) >= minRun &&
        nseg < (int)(sizeof(segs)/sizeof(segs[0])))
        segs[nseg++] = { runStart, End };
    if (nseg == 0)
        return 0;

    // Item b.5 (paper §3.1 straddling-object soundness): the paper marks the RC
    // table for a large object's TRAILING LINES so its per-line RC free-scan never
    // reuses a line a straddling live object still occupies. Our carve is instead
    // object-boundary precise (each dead run spans only whole DEAD objects), which
    // subsumes that guarantee. LXR_VERIFY_STRADDLE turns "sound by construction"
    // into a checked invariant: no carved dead run may overlap any LIVE object's
    // extent (which would mean a straddling live tail was about to be reused).
    if (getenv("LXR_VERIFY_STRADDLE") != nullptr)
    {
        for (uint8_t* q = Start; q < End; )
        {
            Object* o = (Object*)q;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) break;
            bool live = (rcValue(o) != 0) || IsYoung(o) || rootInRange(q, q + sz);
            if (live)
            {
                for (int s = 0; s < nseg; s++)
                    if (q < segs[s].b && (q + sz) > segs[s].a) // live object overlaps a carved dead run
                    {
                        LXRReportFallback(&g_reportedStraddle, "carve-straddle-overlap",
                            "LIVE object overlaps an RC-carved dead run (straddling-tail reuse)");
                        break;
                    }
            }
            q += sz;
        }
    }

    // Re-tile [Start, End) into ordered sub-regions: live segments interleaved with
    // the carved dead segments (mirrors CarveFreeRuns exactly). Plug each dead run
    // as one free object so an incidental linear parse stays valid, list it for
    // reuse. May realloc g_chunks -> caller must not touch a prior g_chunks ref.
    int64_t carved = 0;
    bool firstWritten = false;
    uint8_t* cursor = Start;
    for (int s = 0; s < nseg; s++)
    {
        uint8_t* fa = segs[s].a;
        uint8_t* fb = segs[s].b;
        if (cursor < fa) // live segment before this dead run
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
        PlugFreeRange(fa, (size_t)(fb - fa));
        ClearLoggedRange(fa, fb);           // reused dead run must start unlogged
        ClearRCRange(fa, fb);               // reused range starts at RC 0
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
        carved += (int64_t)(fb - fa);
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
    return carved;
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
    fprintf(stderr, "LXRGC: [verify] trace-completeness offenders=%lld markStackDrops=%lld rcApply[par=%lld ser=%lld] bigArrays[scans=%lld slots=%lld par=%d]\n",
            (long long)offenders, (long long)g_lxrCounters.MarkStackDrops,
            (long long)g_parRCApplies, (long long)g_serRCApplies,
            (long long)g_bigArrayScans, (long long)g_bigArraySlotsScanned, (int)g_bigArrayParallel);
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
    // Item F: grow the persistent per-line reuse-version table to cover the used
    // heap (persists across cycles; never re-zeroed).
    EnsureLineReuseCommitted(usedBytes);
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
            // Item F (F3): on an evac-cycle mark, log inter-block reference slots
            // so Evacuate() can scope its fix-up to the remembered set instead of
            // walking the whole heap. Records the SLOT; intra-block edges (source
            // and target co-move) are excluded.
            if (g_recordEvacEdges && child != nullptr && InHeap(child))
            {
                uintptr_t sblk = (uintptr_t)ref   & ~(lxr::kBlockSize - 1);
                uintptr_t tblk = (uintptr_t)child & ~(lxr::kBlockSize - 1);
                if (sblk != tblk && (!g_evacCandidateScope || IsEvacCandidateAddr(child))) RecordEvacEdge(ref);
            }
            if (g_nurseryGuard > 0 && child != nullptr && InFreedYoung((uint8_t*)child))
            {
                MethodTable* pmt = *(MethodTable**)o;
                fprintf(stderr, "LXRGC: [nursery-guard] REACHED FREED YOUNG child=%p from parent=%p parentMT=%p "
                        "parentSize=%llu fieldOff=%lld childYoung=%d MARKED=%d\n",
                        (void*)child, (void*)o, (void*)pmt, (unsigned long long)LXRObjectSize(o),
                        (long long)((uint8_t*)ref - (uint8_t*)o), IsYoung(child) ? 1 : 0, IsMarked(child) ? 1 : 0);
                fflush(stderr);
                return; // diagnostic only: don't push the freed child
            }
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
        size_t bigSlots = 0;
        if (g_bigArrayParallel && IsBigRefArray(o, osz, &bigSlots))
        {
            // Item G: this lane has already claimed (marked) the array; defer its
            // element scan so DrainDeferredBigArrays can partition it across the
            // pool instead of one lane walking all bigSlots serially.
            DeferBigRefArray(o);
            continue;
        }
        GCScanObjectRefs(o, osz, [this, &local, o](Object** ref)
        {
            Object* c = *ref;
            if (c == nullptr)
                return;
            // Item F (F3): log inter-block reference slots on an evac-cycle mark
            // (see DrainMarkStack). Per-lane thread-local log -> no lock on the
            // hot path; distinct workers write distinct logs.
            if (g_recordEvacEdges && InHeap(c))
            {
                uintptr_t sblk = (uintptr_t)ref & ~(lxr::kBlockSize - 1);
                uintptr_t tblk = (uintptr_t)c   & ~(lxr::kBlockSize - 1);
                if (sblk != tblk && (!g_evacCandidateScope || IsEvacCandidateAddr(c))) RecordEvacEdge(ref);
            }
            if (g_nurseryGuard > 0 && InFreedYoung((uint8_t*)c))
            {
                MethodTable* pmt = *(MethodTable**)o;
                fprintf(stderr, "LXRGC: [nursery-guard] REACHED FREED YOUNG child=%p from parent=%p parentMT=%p "
                        "parentSize=%llu fieldOff=%lld childYoung=%d childRC-page? MARKED=%d\n",
                        (void*)c, (void*)o, (void*)pmt, (unsigned long long)LXRObjectSize(o),
                        (long long)((uint8_t*)ref - (uint8_t*)o), IsYoung(c) ? 1 : 0, IsMarked(c) ? 1 : 0);
                fflush(stderr);
                return; // don't scan the freed child (avoid the AV) - diagnostic only
            }
            if (MarkObject(c)) // atomic claim
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
        if (g_poolWorkKind == 0)
        {
            std::vector<std::vector<Object*>>* slices = g_poolSlices;
            if (slices != nullptr && (size_t)(w + 1) < slices->size() && g_poolCollector != nullptr)
                g_poolCollector->DrainSliceLocal((*slices)[(size_t)(w + 1)]);
        }
        else // g_poolWorkKind == 1: generic parallel-for, this worker is lane w+1
        {
            if (g_poolForFn != nullptr)
                g_poolForFn(w + 1, g_poolActiveLanes, g_poolForCtx);
        }
        SetEvent(g_poolDone[w]);
    }
}

// Item G (§3.5): run `fn` across the persistent worker pool as a parallel-for.
// Lane 0 runs on the calling (collector) thread; lanes 1..(lanes-1) on pooled
// workers. `fn(lane, lanes, ctx)` computes its own [lane, lanes) index stripe.
// Falls back to a single serial lane when the pool is unavailable / lanes<2.
// Must NOT be called during a STW mark (that uses the pool via g_poolWorkKind=0);
// the RC apply that uses this runs at its own pause / off-pause, never nested.
static void RunOnPool(int lanes, void (*fn)(int lane, int lanes, void* ctx), void* ctx)
{
    if (lanes < 2 || g_poolWorkers < 1)
    {
        fn(0, 1, ctx);
        return;
    }
    if (lanes > g_poolWorkers + 1)
        lanes = g_poolWorkers + 1;
    EnterCriticalSection(&g_poolLock); // serialize with the marker's mark-drain
    g_poolForFn = fn;
    g_poolForCtx = ctx;
    g_poolActiveLanes = lanes;
    InterlockedExchange(&g_poolWorkKind, 1);
    for (int w = 1; w < lanes; w++)   // wake pooled workers 0..lanes-2 -> lanes 1..lanes-1
        SetEvent(g_poolStart[w - 1]);
    fn(0, lanes, ctx);                // main thread is lane 0
    for (int w = 1; w < lanes; w++)
        WaitForSingleObject(g_poolDone[w - 1], INFINITE);
    InterlockedExchange(&g_poolWorkKind, 0);
    g_poolForFn = nullptr;
    g_poolForCtx = nullptr;
    g_poolActiveLanes = 1;
    LeaveCriticalSection(&g_poolLock);
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

// --- Item G (§3.5): parallel scan of a single very large reference array -------
// The base parallel closure claims each object atomically for exactly one lane and
// that lane scans the whole object. A single huge object[] (or covariant ref array)
// is therefore scanned end-to-end by one lane -- the scalability cliff the paper
// calls out (§3.5). To distribute it, a lane that meets such an array during the
// closure DEFERS it: it has already atomically claimed (marked) the array, so it
// pushes the array here instead of scanning the elements inline. After the per-lane
// mark closure joins (and releases g_poolLock), DrainDeferredBigArrays partitions
// every deferred array's element range into fixed-size chunks and scans the chunks
// across the pool; each lane drains the greys it discovers to full local closure
// (which may defer further nested big arrays), and the whole thing iterates to a
// fixpoint. Sound by construction: the array is atomically claimed once, its
// elements are visited exactly once, discovered greys are transitively closed
// before return, and marking is monotone (a re-seen object is a no-op).

// Is `o` a single-series reference array big enough to be worth partitioning?
// Only the object[]/covariant-ref-array shape (one positive GC series covering a
// contiguous run of pointer-sized element slots) qualifies; value-type arrays use
// the repeating (negative GetNumSeries) encoding and are excluded. outSlots gets
// the element-slot count.
bool LXRCollector::IsBigRefArray(Object* o, size_t osz, size_t* outSlots) const
{
    MethodTable* mt = o->GetGCSafeMethodTable();
    if (mt == nullptr || !mt->HasComponentSize())
        return false;
    if (mt->RawGetComponentSize() != sizeof(void*)) // ref arrays store pointer-sized slots
        return false;
    if (!mt->ContainsGCPointers())
        return false;
    CGCDesc* map = CGCDesc::GetCGCDescFromMT(mt);
    if (map->GetNumSeries() != 1) // exclude the repeating value-type-array encoding
        return false;
    CGCDescSeries* series = map->GetHighestSeries();
    // Mirror gcobjscan.h's single-series element region: [op + off, op + off + (SeriesSize + size)).
    uint8_t** parm  = (uint8_t**)((uint8_t*)o + series->GetSeriesOffset());
    uint8_t** ppstop = (uint8_t**)((uint8_t*)parm + series->GetSeriesSize() + osz);
    if (ppstop <= parm)
        return false;
    size_t slots = (size_t)(ppstop - parm);
    if (slots < kBigRefArraySlots)
        return false;
    if (outSlots != nullptr)
        *outSlots = slots;
    return true;
}

// Scan element slots [slotStart, slotEnd) of a single-series ref array, claiming
// live children into `local`. Replicates the F3 evac-edge logging + nursery guard
// of the DrainSliceLocal scan callback so item-F soundness/diagnostics still hold
// when a big array's elements are the inter-block source slots.
void LXRCollector::ScanBigRefArrayChunk(Object* o, size_t slotStart, size_t slotEnd, std::vector<Object*>& local)
{
    MethodTable* mt = o->GetGCSafeMethodTable();
    CGCDesc* map = CGCDesc::GetCGCDescFromMT(mt);
    CGCDescSeries* series = map->GetHighestSeries();
    uint8_t** parm = (uint8_t**)((uint8_t*)o + series->GetSeriesOffset());
    for (size_t i = slotStart; i < slotEnd; i++)
    {
        Object** ref = (Object**)(parm + i);
        Object* c = *ref;
        if (c == nullptr)
            continue;
        if (g_recordEvacEdges && InHeap(c))
        {
            uintptr_t sblk = (uintptr_t)ref & ~(lxr::kBlockSize - 1);
            uintptr_t tblk = (uintptr_t)c   & ~(lxr::kBlockSize - 1);
            if (sblk != tblk && (!g_evacCandidateScope || IsEvacCandidateAddr(c))) RecordEvacEdge(ref);
        }
        if (g_nurseryGuard > 0 && InFreedYoung((uint8_t*)c))
        {
            fprintf(stderr, "LXRGC: [nursery-guard] REACHED FREED YOUNG child=%p from bigarray=%p slot=%llu\n",
                    (void*)c, (void*)o, (unsigned long long)i);
            fflush(stderr);
            continue;
        }
        if (MarkObject(c)) // atomic claim
            local.push_back(c);
    }
}

// Chunk descriptor + parallel-for context for the deferred big-array scan.
namespace {
struct BigArrayChunk { Object* array; size_t start; size_t end; };
struct BigArrayForCtx { std::vector<BigArrayChunk>* chunks; LXRCollector* self; };
}

// RunOnPool body: each lane scans its stripe of chunks, then drains the greys it
// discovered to full local closure (which may defer further nested big arrays into
// g_bigRefArrays for the next fixpoint round).
static void LXRBigArrayScanFn(int lane, int lanes, void* ctxp)
{
    BigArrayForCtx* ctx = (BigArrayForCtx*)ctxp;
    const std::vector<BigArrayChunk>& chunks = *ctx->chunks;
    std::vector<Object*> local;
    for (size_t i = (size_t)lane; i < chunks.size(); i += (size_t)lanes)
    {
        const BigArrayChunk& ch = chunks[i];
        ctx->self->ScanBigRefArrayChunk(ch.array, ch.start, ch.end, local);
    }
    ctx->self->DrainSliceLocal(local);
}

// Phase B of item G. Called after the per-lane mark closure has joined and released
// g_poolLock. Repeatedly drains the deferred-big-array list: snapshot+clear it,
// slice each array's element range into kBigArrayChunkSlots chunks, and RunOnPool a
// parallel-for over the chunks. A round may itself defer newly discovered nested
// big arrays, so it loops until the list stays empty.
void LXRCollector::DrainDeferredBigArrays(int workers)
{
    if (!g_bigArrayParallel)
        return;
    for (;;)
    {
        std::vector<Object*> arrays;
        EnterCriticalSection(&g_bigArrayLock);
        arrays.swap(g_bigRefArrays);
        LeaveCriticalSection(&g_bigArrayLock);
        if (arrays.empty())
            return;

        std::vector<BigArrayChunk> chunks;
        for (Object* a : arrays)
        {
            size_t slots = 0;
            if (!IsBigRefArray(a, LXRObjectSize(a), &slots))
            {
                // Invariant: only big ref arrays are ever deferred, and an object's
                // shape/size is immutable across the trace window, so this cannot
                // happen. If it ever did, skipping would leave the array's elements
                // unscanned (unsound: its children stay unmarked) -- report loudly.
                LXRReportFallback(&g_reportedBigArrayShape,
                    "deferred big-array no longer a big ref array", "shape/size changed mid-trace");
                continue;
            }
            for (size_t s = 0; s < slots; s += kBigArrayChunkSlots)
            {
                size_t e = s + kBigArrayChunkSlots;
                if (e > slots) e = slots;
                chunks.push_back({ a, s, e });
            }
            InterlockedIncrement64(&g_bigArrayScans);
            InterlockedExchangeAdd64(&g_bigArraySlotsScanned, (LONG64)slots);
        }
        if (chunks.empty())
            continue;

        BigArrayForCtx ctx{ &chunks, this };
        RunOnPool(workers, &LXRBigArrayScanFn, &ctx);
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
    EnterCriticalSection(&g_poolLock); // serialize with parallel-RC pool use (item G)
    InterlockedExchange(&g_poolWorkKind, 0); // 0 = mark-drain body in the worker proc
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
    LeaveCriticalSection(&g_poolLock);

    // Item G: scan any huge reference arrays the lanes deferred during the closure,
    // partitioned across the pool. Runs OUTSIDE g_poolLock (RunOnPool re-takes it)
    // and iterates to a fixpoint over nested big arrays.
    DrainDeferredBigArrays(lanes);
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

// Item G (§3.5): parallel RC apply. Context + parallel-for bodies for RunOnPool.
// Each lane strides its index range so a huge coalesced epoch (e.g. a large
// reference array being filled -> one entry per element) distributes across the
// GC worker threads. RC slot updates use the atomic CAS variants so concurrent
// lanes touching the same object's count never lose an update.
struct LXRParRC
{
    LXRCollector* self;
    Object** arr;
    size_t   n;
    std::vector<Object*>* laneZeros; // [lanes]: decrement victims per lane (dec phase)
};
static void LXRParRCIncFn(int lane, int lanes, void* ctx)
{
    LXRParRC* c = (LXRParRC*)ctx;
    for (size_t i = (size_t)lane; i < c->n; i += (size_t)lanes)
    {
        Object* o = c->arr[i];
        if (o != nullptr)
            c->self->RCIncrementAtomic(o);
    }
}
static void LXRParRCDecFn(int lane, int lanes, void* ctx)
{
    LXRParRC* c = (LXRParRC*)ctx;
    std::vector<Object*>& zeros = c->laneZeros[lane];
    for (size_t i = (size_t)lane; i < c->n; i += (size_t)lanes)
    {
        Object* o = c->arr[i];
        if (o != nullptr && c->self->RCDecrementAtomic(o))
            zeros.push_back(o);
    }
}

// Apply one coalesced RC epoch: ALL increments before ANY decrement (paper's
// ordering, so an object gaining a reference this epoch is never transiently
// freed by an earlier decrement), collecting zero-count victims for the (serial)
// free cascade the caller drains. Parallelises across the worker pool when the
// epoch is large enough to amortise the wake cost; otherwise stays serial. The
// two RunOnPool calls form the required increments->decrements barrier (RunOnPool
// joins all lanes before returning).
void LXRCollector::ApplyRCEpoch(std::vector<Object*>& incs, std::vector<Object*>& decs)
{
    static int s_parRC = -1;
    if (s_parRC < 0)
        s_parRC = (getenv("LXR_PARALLEL_RC") != nullptr && getenv("LXR_PARALLEL_RC")[0] == '0') ? 0 : 1;
    // Below this many entries the pool wake/join overhead outweighs the win.
    const size_t kParThreshold = 8192;
    int lanes = g_gcThreads;
    bool parallel = s_parRC && lanes > 1 && g_poolWorkers > 0 &&
                    (incs.size() + decs.size()) >= kParThreshold;

    if (!parallel)
    {
        InterlockedIncrement64(&g_serRCApplies);
        for (Object* o : incs)
            if (o != nullptr) RCIncrement(o);
        for (Object* o : decs)
            if (o != nullptr && RCDecrement(o)) EnqueueZeroCount(o);
        return;
    }
    InterlockedIncrement64(&g_parRCApplies);

    // Pre-commit the RC-table pages for every touched object SERIALLY (deduped by
    // page) so the atomic apply never calls VirtualAlloc under contention. The RC
    // table is 1 byte/granule, so a 4 KB page spans many objects -> few unique
    // pages. m_collectLock is already held by the caller.
    {
        std::unordered_set<uintptr_t> pages;
        pages.reserve((incs.size() + decs.size()) / 8 + 16);
        uintptr_t pmask = ~((uintptr_t)g_pageSize - 1);
        auto commit = [&](Object* o) {
            if (o == nullptr) return;
            if ((uint8_t*)o < m_heapBase || (uint8_t*)o >= m_heapBase + m_heapBytes) return;
            uintptr_t pg = (uintptr_t)RCSlot(o) & pmask;
            if (pages.insert(pg).second)
                EnsureRCPage(RCSlot(o));
        };
        for (Object* o : incs) commit(o);
        for (Object* o : decs) commit(o);
    }

    if (lanes > g_poolWorkers + 1) lanes = g_poolWorkers + 1;
    std::vector<std::vector<Object*>> laneZeros((size_t)lanes);

    LXRParRC inc{ this, incs.data(), incs.size(), nullptr };
    RunOnPool(lanes, &LXRParRCIncFn, &inc);

    LXRParRC dec{ this, decs.data(), decs.size(), laneZeros.data() };
    RunOnPool(lanes, &LXRParRCDecFn, &dec);

    for (std::vector<Object*>& z : laneZeros)
        for (Object* o : z)
            EnqueueZeroCount(o);
}

// Item D-copy incoming-edge source: the COMPLETE set of field slots modified this
// epoch (the coalesced modified-buffer keys), captured by ProcessModifiedBuffers
// for CopyYoungSurvivors to replay as its remembered set. Storage declared near
// g_nurseryActive (needed earlier, in Initialize). This is sound and complete
// precisely when D-copy is allowed to run: a this-epoch young survivor can only
// be referenced by a field written this epoch, and coalescing RC logs every
// first-modified field before the pause (a dropped first-log sets
// g_youngRCIncomplete and makes D-copy self-skip). Bounded by the epoch's
// modified-field count (= the paper's field-logging barrier cost), NOT the heap.

void LXRCollector::ProcessModifiedBuffers()
{    // Coalescing reference counting (Levanoni-Petrank, paper A/§3.2.1). For the
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
    static int s_pmbProf = (getenv("LXR_PMB_PROFILE") != nullptr) ? 1 : 0;
    LARGE_INTEGER pf; QueryPerformanceCounter(&pf); LARGE_INTEGER pq; QueryPerformanceFrequency(&pq);
    LARGE_INTEGER pt0 = pf, pt1, pt15, pt2, pt3, pt4, pt5;
    EnterCriticalSection(&m_collectLock);
    EnterCriticalSection(&g_buffersLock);
    std::unordered_map<Object**, Object*> coalesced;
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        bool hadData = buf->Count > 0;
        for (size_t i = 0; i < buf->Count; i++)
            coalesced.emplace(buf->Entries[i].Slot, buf->Entries[i].OldValue);
        buf->Count = 0; // epoch consumed
        // Recycle a swapped-away overflow buffer (had data, not a thread's current
        // buffer) back onto the shared free-list for reuse. Idle spares (Count==0)
        // and current buffers (InUse) are skipped, so nothing is double-pushed.
        if (hadData && buf->InUse == 0)
            PushFreeModifiedBuffer(buf);
    }
    LeaveCriticalSection(&g_buffersLock);
    QueryPerformanceCounter(&pt1); // coalesce done
    // Item D-copy: maintain the persistent "references to young" remembered set.
    // For every modified slot whose CURRENT value (read under STW) is a young
    // object, append the slot to g_dcopyModifiedSlots. This set is NOT cleared per
    // pause -- a young object lives across many RC pauses (aged only by a trace),
    // so an A->B edge created in an earlier pause must be retained until B is
    // promoted or the epoch turns over. Reset happens at the trace epoch bump
    // (ResetDCopyRemset). On overflow we abandon the set and D-copy full-walks.
    //
    // Skip capture entirely WHILE A TRACE WINDOW IS OPEN. A concurrent trace spans
    // many RC pauses; throughout it CopyYoungSurvivors self-skips (it never moves
    // young under an in-flight trace), so nothing prunes this set, yet every RC
    // pause here would keep appending. At trace completion the window's young ages
    // to mature and the whole set is cleared (see the g_traceEpoch bump), so any
    // slot captured during the window is discarded unused. Capturing it anyway is
    // pure waste that let the set balloon to ~1M entries across a long trace -- a
    // single ~330 ms D-copy pass dominated by the O(n log n) re-sort of that dead
    // accumulation. Gating on !g_traceWindowOpen keeps the set bounded to the
    // inter-trace window that D-copy actually consumes.
    if (g_dcopyCaptureModified && !g_dcopyRemsetOverflow && !g_traceWindowOpen)
    {
        for (const auto& kv : coalesced)
        {
            Object* cur = *(kv.first);
            if (cur == nullptr) continue;
            if ((uint8_t*)cur < m_heapBase || (uint8_t*)cur >= m_heapBase + m_heapBytes) continue;
            if (!IsYoung(cur)) continue;
            if (!DCopyRemsetAppend(kv.first, cur, m_heapBase, m_heapBytes))
                break; // overflow: set abandoned, D-copy full-walks until next trace
        }
    }
    QueryPerformanceCounter(&pt15); // dcopy-capture done
    // Deferred-RC root capture (paper §3.2.1): scan the roots at this STW pause so
    // root-reachable mature objects are incremented now and decremented at the
    // next pause (m_rootDeferredPrev). This is what makes RC self-standing rather
    // than reliant on the mark trace to protect roots. Safe here: ProcessModified-
    // Buffers is only ever called under SuspendEE (STW / concurrent-finish pause).
    std::vector<Object*> rootsNow;
    CaptureRoots(rootsNow);
    QueryPerformanceCounter(&pt2); // dcopy-capture + roots done
    // Increments before decrements (paper B/§3.2.1): apply ALL increments of the
    // final referents first, so an object that gains a new reference this epoch is
    // never transiently driven to zero (and freed) by an earlier field's decrement.
    // Item G (§3.5): build flat increment/decrement lists and apply them across the
    // worker pool (ApplyRCEpoch), which distributes a large epoch (e.g. a big
    // reference array's per-element entries) instead of serialising on one thread.
    std::vector<Object*> incs; incs.reserve(coalesced.size() + rootsNow.size());
    std::vector<Object*> decs; decs.reserve(coalesced.size() + m_rootDeferredPrev.size());
    for (const auto& kv : coalesced)
    {
        Object* newValue = *(kv.first); // t_{n+1}, read under STW here
        if (newValue != nullptr) incs.push_back(newValue);
    }
    for (Object* r : rootsNow) incs.push_back(r);            // this epoch's root incs
    for (const auto& kv : coalesced)
        if (kv.second != nullptr) decs.push_back(kv.second); // first old value (t_n)
    for (Object* r : m_rootDeferredPrev) decs.push_back(r);  // deferred root decs
    ApplyRCEpoch(incs, decs);
    QueryPerformanceCounter(&pt3); // apply-rc done
    m_rootDeferredPrev.swap(rootsNow);   // this epoch's roots -> next epoch's decs
    DrainZeroCountWorkList();
    QueryPerformanceCounter(&pt4); // drain-zero done
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
    QueryPerformanceCounter(&pt5); // clear-logged done
    if (s_pmbProf)
    {
        auto us = [&](LARGE_INTEGER a, LARGE_INTEGER b){ return (long long)((b.QuadPart - a.QuadPart) * 1000000 / pq.QuadPart); };
        fprintf(stderr, "LXRGC:   [pmb-prof] fields=%zu coalesce=%lldus dcopy=%lldus roots=%lldus applyrc=%lldus drainzero=%lldus clearlog=%lldus\n",
                coalesced.size(), us(pt0,pt1), us(pt1,pt15), us(pt15,pt2), us(pt2,pt3), us(pt3,pt4), us(pt4,pt5));
        fflush(stderr);
    }
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
    // FLAT detach (paper §3.2.5 SSB swap): copy the raw (slot, oldValue, *slot)
    // triples out of every mutator buffer WITHOUT coalescing. The final referent
    // (*slot, the t_{n+1} value) MUST be read here under STW so it is stable, but
    // the O(#modified fields) hash-map coalescing is deferred to the off-pause
    // ProcessSnapshotDecrements. This keeps the snapshot/finish PAUSE proportional
    // to a flat array copy (no per-field node allocation), so a large epoch -
    // e.g. a survival-paced multi-epoch trace window that accumulated millions of
    // mutations - no longer produces a multi-second STW coalescing cliff. Repeated
    // writes to one slot appear as multiple triples all carrying the SAME *slot
    // (read once here), so off-pause coalescing (first old + this new) is exact.
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        bool hadData = buf->Count > 0;
        for (size_t i = 0; i < buf->Count; i++)
        {
            Object** slot = buf->Entries[i].Slot;
            if (g_rcSnapCount == g_rcSnapCap)
            {
                size_t newCap = g_rcSnapCap ? g_rcSnapCap * 2 : 4096;
                RCSnapshotEntry* grown = (RCSnapshotEntry*)realloc(
                    g_rcSnapEntries, newCap * sizeof(RCSnapshotEntry));
                if (grown == nullptr) { break; }
                g_rcSnapEntries = grown;
                g_rcSnapCap = newCap;
            }
            g_rcSnapEntries[g_rcSnapCount].Slot = slot;
            g_rcSnapEntries[g_rcSnapCount].OldValue = buf->Entries[i].OldValue;
            g_rcSnapEntries[g_rcSnapCount].NewValue = *slot; // t_{n+1}, stable under STW
            g_rcSnapCount++;
            // Restore the unlogged-bit invariant before mutators resume logging
            // into the fresh epoch (redundant per-slot clears are harmless).
            if (!g_modifiedOverflow)
                ClearLoggedBit(slot);
        }
        buf->Count = 0; // epoch consumed (snapshotted)
        if (hadData && buf->InUse == 0)
            PushFreeModifiedBuffer(buf);
    }
    // Deferred-RC root capture at this STW snapshot pause (paper §3.2.1): stash the
    // current root set for the off-pause replay in ProcessSnapshotDecrements, which
    // increments it and rotates the deferred-decrement chain.
    CaptureRoots(m_rootDeferredSnap);
    // On a buffer overflow some first-logs set a bit without leaving an entry, so
    // wholesale-clear rather than per-slot (see ProcessModifiedBuffers).
    if (g_modifiedOverflow)
        ResetLoggedTable();
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
    // Off-pause coalescing (moved here from the STW pause): keep the FIRST logged
    // old value per slot (t_n) paired with the *slot read at the pause (t_{n+1}).
    // emplace keeps the first insertion per key, and the flat snapshot preserves
    // per-buffer store order, so this reproduces the paper's per-field coalescing
    // exactly - but the hash-map cost is now borne while mutators run, not at the
    // pause. All triples for one slot carry the same NewValue (read once under
    // STW), so any is correct.
    std::unordered_map<Object**, std::pair<Object*, Object*>> coalesced;
    coalesced.reserve(g_rcSnapCount);
    for (size_t i = 0; i < g_rcSnapCount; i++)
        coalesced.emplace(g_rcSnapEntries[i].Slot,
                          std::make_pair(g_rcSnapEntries[i].OldValue, g_rcSnapEntries[i].NewValue));
    // Increments before decrements: apply ALL final-referent increments first so
    // an object that gained a reference this epoch is never transiently freed by
    // an earlier field's decrement. Item G (§3.5): distribute the apply across the
    // worker pool via ApplyRCEpoch when the epoch is large. This runs OFF-pause;
    // the atomic RC ops touch only the collector-private RC side table (never
    // mutator-visible object memory), so parallelising it off-pause is sound.
    std::vector<Object*> incs; incs.reserve(coalesced.size() + m_rootDeferredSnap.size());
    std::vector<Object*> decs; decs.reserve(coalesced.size() + m_rootDeferredPrev.size());
    for (const auto& kv : coalesced)
        if (kv.second.second != nullptr) incs.push_back(kv.second.second); // t_{n+1}
    for (Object* r : m_rootDeferredSnap) incs.push_back(r);  // root incs (this pause)
    for (const auto& kv : coalesced)
        if (kv.second.first != nullptr) decs.push_back(kv.second.first);   // t_n
    for (Object* r : m_rootDeferredPrev) decs.push_back(r);  // deferred root decs
    ApplyRCEpoch(incs, decs);
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

// Item F (paper §3.3): select the evacuation candidates for the trace that is
// about to start, using each region's persisted DeadPctEstimate (from the last
// Evacuate's occupancy scan). Runs at the snapshot/backup STW pause BEFORE the
// mark, so the barrier + concurrent mark can scope inter-block edge recording to
// candidate targets for the whole trace window. Cheap: a side-table read per
// region, no live-heap walk. Regions with no estimate yet (never evac-scanned)
// are not candidates, so evacuation warms up over the first few traces.
static void SelectEvacCandidates()
{
    // Clear last cycle's candidate bytemap. On a 16 GiB heap this is a 128 KiB
    // memset; only the used prefix is ever non-zero but clearing all is trivial.
    if (g_evacCandidate == nullptr) { InterlockedExchange(&g_evacCandidateScope, 0); return; }
    // The occupancy predictor (DeadPctEstimate) is stamped from the line-mark
    // table by the sweep, so candidate-scoping requires line reuse to be enabled.
    // When scoping is disabled (A/B), evac off, or the predictor is unavailable,
    // record ALL inter-block edges (the pre-existing heap-wide behaviour) and do
    // NOT candidate-restrict evacuation.
    if (!CandidateScopeEnabled() || g_evacActive == 0 || g_lineReuse == 0)
    {
        memset(g_evacCandidate, 0, g_evacCandidateSlots);
        InterlockedExchange(&g_evacCandidateScope, 0);
        return;
    }
    static int64_t s_fragPct = -1;
    if (s_fragPct < 0)
    {
        const char* f = getenv("LXR_EVAC_FRAG_PCT");
        s_fragPct = f ? _atoi64(f) : 50;
    }
    memset(g_evacCandidate, 0, g_evacCandidateSlots);
    size_t nCand = 0;
    EnterCriticalSection(&g_chunkLock);
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.FreeRun || c.UsedEnd <= c.Start)
            continue;
        if ((int64_t)c.DeadPctEstimate < s_fragPct)
            continue;
        SetEvacCandidateRange(c.Start, c.UsedEnd);
        nCand++;
    }
    LeaveCriticalSection(&g_chunkLock);
    // Scope is active for the WHOLE trace whenever enabled (even with 0 candidates
    // this cycle): the barrier + mark then record nothing and Evacuate moves
    // nothing, which keeps the invariant "evacuate ONLY candidates whose incoming
    // edges are (re)recorded this cycle" - so the persistent remset's history is
    // never trusted for a non-candidate region (avoids a scope-transition dangle).
    InterlockedExchange(&g_evacCandidateScope, 1);
    if (getenv("LXR_VERBOSE") != nullptr)
    {
        fprintf(stderr, "LXRGC: [evac-cand] selected %zu candidate regions (>=%lld%% dead est)\n",
                nCand, (long long)s_fragPct);
        fflush(stderr);
    }
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

    // Item F: pick this trace's evacuation candidates from the previous cycle's
    // occupancy estimates (must precede any edge recording below).
    SelectEvacCandidates();

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
        fprintf(stderr, "LXRGC: [trace] rootsPushed=%llu heapNextFree=+%lldMB rcApply[par=%lld ser=%lld] bigArrays[scans=%lld slots=%lld par=%d]\n",
                (unsigned long long)rootsPushed,
                (long long)((g_lxrGCHeap ? (g_lxrGCHeap->HeapHighWater() - g_lxrGCHeap->HeapBase()) : 0) >> 20),
                (long long)g_parRCApplies, (long long)g_serRCApplies,
                (long long)g_bigArrayScans, (long long)g_bigArraySlotsScanned, (int)g_bigArrayParallel);
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

    // Item F: select this trace's evacuation candidates BEFORE opening the SATB
    // window, so the barrier (armed just below) and the concurrent mark scope
    // their inter-block edge recording to candidate targets for the whole window.
    SelectEvacCandidates();

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
    LARGE_INTEGER swPreLock, swPostLock; QueryPerformanceCounter(&swPreLock);
    EnterCriticalSection(&g_chunkLock);
    QueryPerformanceCounter(&swPostLock);
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
    bool sweepVerbose = getenv("LXR_VERBOSE") != nullptr;
    LARGE_INTEGER swFreq, swClk0; QueryPerformanceFrequency(&swFreq); QueryPerformanceCounter(&swClk0);
    int64_t swLivenessTicks = 0, swCarveTicks = 0, swDecommitTicks = 0;
    int64_t swLiveRegions = 0, swCarvedRegions = 0, swFreedRegions = 0;
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
        LARGE_INTEGER swL0; QueryPerformanceCounter(&swL0);
        bool anyLive = AnyMarkedInRange(c.Start, c.UsedEnd) ||
                       (!g_traceCompleteThisCycle &&
                        (AnyRCNonZeroInRange(c.Start, c.UsedEnd) ||
                         IsYoung((Object*)c.Start)));
        { LARGE_INTEGER swL1; QueryPerformanceCounter(&swL1); swLivenessTicks += swL1.QuadPart - swL0.QuadPart; }
        if (anyLive)
        {
            // Retained region: recover its dead line runs for reuse (Immix line
            // recycling). CarveFreeRuns may realloc g_chunks, so do not touch 'c'
            // afterwards - continue to the next index.
            swLiveRegions++;
            // Item F: stamp the occupancy predictor for next trace's candidate
            // selection from the line marks (cheap: O(lines/8), a side-table read).
            // Must precede CarveFreeRuns (it may realloc g_chunks, invalidating c).
            if (CandidateScopeEnabled() && g_lineMarksValid && m_lineMarkTable != nullptr)
            {
                size_t firstLine = (size_t)(c.Start - m_heapBase) / lxr::kLineSize;
                size_t lastLine  = (size_t)((c.UsedEnd - 1) - m_heapBase) / lxr::kLineSize;
                size_t totalLines = lastLine - firstLine + 1, liveLines = 0;
                for (size_t line = firstLine; line <= lastLine; line++)
                {
                    size_t byteIdx = line >> 3;
                    if (byteIdx >= m_lineMarkCommittedBytes) break;
                    if (m_lineMarkTable[byteIdx] & (uint8_t)(1u << (line & 7))) liveLines++;
                }
                int64_t deadPct = totalLines ? (int64_t)((totalLines - liveLines) * 100 / totalLines) : 0;
                c.DeadPctEstimate = (uint8_t)(deadPct > 100 ? 100 : deadPct);
            }
            if (carveLines)
            {
                LARGE_INTEGER swC0; QueryPerformanceCounter(&swC0);
                CarveFreeRuns(i);
                LARGE_INTEGER swC1; QueryPerformanceCounter(&swC1);
                swCarveTicks += swC1.QuadPart - swC0.QuadPart; swCarvedRegions++;
            }
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

        // Decommit the page-aligned interior of the dead region (deferred off-pause
        // by ReclaimRegionMemory; the <=1 page fringe at each end may hold a
        // neighbor's object header, so it leaves that in place).
        LARGE_INTEGER swD0; QueryPerformanceCounter(&swD0);
        swFreedRegions++;
        // Reclaimed => RC 0. Clear this region's RC bytes so no stale count
        // survives into the decommitted range (a later decrement of a
        // pre-sweep-logged old value would otherwise resurrect a dangling
        // pointer and fault in DrainZeroCountWorkList). Only [Start,UsedEnd)
        // ever held objects/RC; beyond UsedEnd the RC table is already zero.
        ClearRCRange(c.Start, c.UsedEnd);
        ClearLoggedRange(c.Start, c.UsedEnd); // reused range must start unlogged
        ReclaimRegionMemory(i);
        { LARGE_INTEGER swD1; QueryPerformanceCounter(&swD1); swDecommitTicks += swD1.QuadPart - swD0.QuadPart; }
    }
    LeaveCriticalSection(&g_chunkLock);
    if (sweepVerbose)
    {
        LARGE_INTEGER swClk1; QueryPerformanceCounter(&swClk1);
        auto us = [&](int64_t t){ return (long long)(t * 1000000 / swFreq.QuadPart); };
        fprintf(stderr, "LXRGC: [sweep-breakdown] total=%lldus lockwait=%lldus regions=%zu live=%lld carved=%lld freed=%lld | liveness=%lldus carve=%lldus decommit=%lldus deferred=%d pending=%zu chunkCountNow=%zu\n",
                us(swClk1.QuadPart - swClk0.QuadPart), us(swPostLock.QuadPart - swPreLock.QuadPart), sweepCount,
                (long long)swLiveRegions, (long long)swCarvedRegions, (long long)swFreedRegions,
                us(swLivenessTicks), us(swCarveTicks), us(swDecommitTicks),
                (int)g_deferDecommit, g_pendingDecommit.size(), g_chunkCount);
        fflush(stderr);
    }
    if (carveLines && getenv("LXR_VERIFY_TRACE") != nullptr)
        fprintf(stderr, "LXRGC: [sweep] line reuse: carved %lld run(s) / %lld MiB cumulative; freeRunStack=%llu\n",
                (long long)g_carveRunsTotal, (long long)(g_carveBytesTotal >> 20),
                (unsigned long long)g_freeRunTop);
}

// Off-pause drain of deferred region decommits (see g_deferDecommit). Called by
// the collection driver after LXRRestartEE, while mutators run. Regions here are
// in limbo (Committed=false, RC/logged cleared, NOT yet on g_freeChunks so the
// allocator cannot hand them out, and their pages are still committed). We
// VirtualFree each range, then publish the indices to g_freeChunks so they become
// reusable -- preserving the invariant that every g_freeChunks entry is
// decommitted. The single-driver collection loop serializes this against the next
// sweep, so the pending vectors are drained before they can be re-populated.
void LXRCollector::DrainPendingDecommit()
{
    if (g_pendingDecommit.empty() && g_pendingFreeChunks.empty())
        return;
    int64_t freedBytes = 0;
    for (auto& r : g_pendingDecommit)
    {
        uint8_t* dbeg = r.first;
        uint8_t* dend = r.second;
        if (dend > dbeg)
        {
            VirtualFree(dbeg, dend - dbeg, MEM_DECOMMIT);
            freedBytes += (int64_t)(dend - dbeg);
        }
    }
    if (freedBytes != 0)
    {
        InterlockedExchangeAdd64(&m_reclaimedBytes, freedBytes);
        InterlockedExchangeAdd64(&g_committedInUse, -freedBytes);
    }
    g_pendingDecommit.clear();

    EnterCriticalSection(&g_chunkLock);
    for (size_t idx : g_pendingFreeChunks)
    {
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
    g_pendingFreeChunks.clear();
}

int64_t LXRCollector::ReclaimRegionMemory(size_t chunkIndex)
{
    ChunkRegion& c = g_chunks[chunkIndex];
    uint8_t* dbeg = (uint8_t*)(((uintptr_t)c.Start + g_pageSize - 1) & ~((uintptr_t)g_pageSize - 1));
    uint8_t* dend = (uint8_t*)(((uintptr_t)(c.Start + c.Size)) & ~((uintptr_t)g_pageSize - 1));
    int64_t bytes = (dend > dbeg) ? (int64_t)(dend - dbeg) : 0;
    c.Committed = false;
    if (g_deferDecommit)
    {
        // Off-pause: record the range + chunk index; DrainPendingDecommit (after
        // RestartEE) frees the pages and only THEN publishes the chunk to the
        // reusable free list. The region is in limbo until then (pages committed,
        // RC/log to be cleared by the caller, unreachable by the allocator), so a
        // deferred RC decrement into it finds RC 0 and its page never faults.
        if (dend > dbeg) g_pendingDecommit.push_back({ dbeg, dend });
        g_pendingFreeChunks.push_back(chunkIndex);
        return bytes;
    }
    if (dend > dbeg)
    {
        VirtualFree(dbeg, dend - dbeg, MEM_DECOMMIT);
        InterlockedExchangeAdd64(&m_reclaimedBytes, bytes);
        InterlockedExchangeAdd64(&g_committedInUse, -bytes);
    }
    if (g_freeChunkTop == g_freeChunkCap)
    {
        size_t nc = g_freeChunkCap ? g_freeChunkCap * 2 : 256;
        size_t* grown = (size_t*)realloc(g_freeChunks, nc * sizeof(size_t));
        if (grown != nullptr) { g_freeChunks = grown; g_freeChunkCap = nc; }
    }
    if (g_freeChunkTop < g_freeChunkCap)
        g_freeChunks[g_freeChunkTop++] = chunkIndex;
    return bytes;
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
// Count of interior roots that could not be resolved to an object base during the
// evac pin pass. Instead of aborting ALL evacuation (which starves defrag on
// workloads that always carry an unresolvable interior root), each such interior
// address is recorded in g_evacUnresolvedInteriorList so that ONLY the region it
// points into is excluded from the evacuation set; the rest of the fragmented
// heap is still compacted. (Region kept in place => a live byref never dangles.)
static volatile LONG g_evacUnresolvedInterior = 0;
static std::vector<uint8_t*>* g_evacUnresolvedInteriorList = nullptr;
static void LXRPinRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
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
            InterlockedIncrement(&g_evacUnresolvedInterior);
            if (g_evacUnresolvedInteriorList != nullptr)
                g_evacUnresolvedInteriorList->push_back(interior);
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
    static int64_t s_fragPct = -1, s_budgetBytes = -1, s_budgetMs = -1;
    if (s_fragPct < 0)
    {
        const char* f = getenv("LXR_EVAC_FRAG_PCT");
        s_fragPct = f ? _atoi64(f) : 50;              // evacuate regions >= this % dead
        const char* b = getenv("LXR_EVAC_BUDGET_MB");
        s_budgetBytes = (b ? _atoi64(b) : 32) * (int64_t)1024 * 1024; // copy at most this per pause
        const char* ms = getenv("LXR_EVAC_BUDGET_MS");
        s_budgetMs = ms ? _atoi64(ms) : 20;           // and stop after this wall-clock ms
    }

    // 1. Pin all root/handle referents (interior roots resolve to their base).
    LARGE_INTEGER evPinFreq, evPin0, evPin1; QueryPerformanceFrequency(&evPinFreq); QueryPerformanceCounter(&evPin0);
    std::unordered_set<Object*> pinned;
    std::vector<uint8_t*> unresolvedInteriors;
    InterlockedExchange(&g_evacUnresolvedInterior, 0);
    g_evacPinned = &pinned;
    g_evacUnresolvedInteriorList = &unresolvedInteriors;
    ScanContext sc; sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRPinRoot, 2, 2, &sc);
    g_evacPinned = nullptr;
    g_evacUnresolvedInteriorList = nullptr;
    LXRGCHandleStore::ForEachLiveHandle(&LXRPinHandle, &pinned);
    QueryPerformanceCounter(&evPin1);
    if (verbose) { fprintf(stderr, "LXRGC: [evac-pin] roots+handles=%lldus pinned=%zu unresolvedInterior=%ld\n", (long long)((evPin1.QuadPart-evPin0.QuadPart)*1000000/evPinFreq.QuadPart), pinned.size(), (long)g_evacUnresolvedInterior); fflush(stderr); }

    // An unresolvable interior/byref root no longer aborts the whole pass: below,
    // candidate selection excludes ONLY the region each such interior points into
    // (that region is kept in place so the live byref never dangles), while the
    // rest of the fragmented heap is still compacted. Previously a single
    // unresolvable interior root skipped all evacuation, which -- on workloads
    // that carry one nearly every cycle -- starved defrag and let committed memory
    // ratchet up unboundedly (fragmented regions were never reclaimed).

    // 2. Select fragmented regions. Paper §3.3.4: the evacuation set is the N
    //    LOWEST-occupancy blocks (most fragmented first) up to the copy budget,
    //    NOT simply the first regions over the threshold - evacuating the least-
    //    occupied blocks first maximises compaction (freed blocks) per live byte
    //    copied. Gather every region >= s_fragPct dead, sort by occupancy ratio
    //    (live/total) ascending, then take under the byte budget. Snapshot first
    //    so registering destination chunks (which may realloc g_chunks) cannot
    //    invalidate the source list; indices stay valid across realloc.
    //    LOWEST-occupancy blocks (most fragmented first) up to the copy budget,
    //    NOT simply the first regions over the threshold - evacuating the least-
    //    occupied blocks first maximises compaction (freed blocks) per live byte
    //    copied. Gather every region >= s_fragPct dead, sort by occupancy ratio
    //    (live/total) ascending, then take under the byte budget. Snapshot first
    //    so registering destination chunks (which may realloc g_chunks) cannot
    //    invalidate the source list; indices stay valid across realloc.
    struct EvacRegion { size_t index; uint8_t* start; uint8_t* usedEnd; };
    struct EvacCand { size_t index; uint8_t* start; uint8_t* usedEnd; int64_t live; int64_t total; };
    std::vector<EvacCand> cands;
    LARGE_INTEGER evSelFreq, evSel0, evSel1; QueryPerformanceFrequency(&evSelFreq); QueryPerformanceCounter(&evSel0);
    EnterCriticalSection(&g_chunkLock);
    for (size_t i = 0; i < g_chunkCount; i++)
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
        {
            c.DeadPctEstimate = 0; // not an evac candidate: fully dead (sweep),
                                   // fully live (0 dead), empty, or unparseable
            continue;
        }
        int64_t deadPct = (int64_t)((total - live) * 100 / total);
        c.DeadPctEstimate = (uint8_t)(deadPct > 100 ? 100 : deadPct); // Item F predictor
        if (deadPct < s_fragPct)
            continue;
        // Item F candidate-scoping: only evacuate regions selected as candidates
        // at the trace's snapshot. Their incoming inter-block edges are the ONLY
        // ones the (candidate-scoped) barrier + mark recorded this cycle, so a non-
        // candidate region (e.g. one that fragmented only after the snapshot) has
        // no complete remembered set and MUST NOT move (it stays put, and will be a
        // candidate next cycle). Every 128 KiB slot the region overlaps must be
        // flagged (edge recording is slot-granular).
        if (g_evacCandidateScope)
        {
            bool allCand = true;
            for (uint8_t* q = c.Start; q < c.UsedEnd; q += CONTEXT_ALLOC_QUANTUM)
                if (!IsEvacCandidateAddr(q)) { allCand = false; break; }
            if (allCand && !IsEvacCandidateAddr(c.UsedEnd - 1)) allCand = false;
            if (!allCand)
                continue;
        }
        // Exclude a region that a live but unresolvable interior/byref root points
        // into: moving it could dangle that byref. Keep it in place (still swept /
        // RC-reclaimed when it becomes wholly dead). Interiors are typically 0-1
        // per cycle, so this scan is negligible.
        if (!unresolvedInteriors.empty())
        {
            bool pinnedByInterior = false;
            for (uint8_t* it : unresolvedInteriors)
                if (it >= c.Start && it <= c.UsedEnd) { pinnedByInterior = true; break; }
            if (pinnedByInterior)
                continue;
        }
        cands.push_back({ i, c.Start, c.UsedEnd, (int64_t)live, (int64_t)total });
    }
    LeaveCriticalSection(&g_chunkLock);
    QueryPerformanceCounter(&evSel1);
    if (verbose) { fprintf(stderr, "LXRGC: [evac-select] scan=%lldus cands=%zu (of %zu regions)\n", (long long)((evSel1.QuadPart-evSel0.QuadPart)*1000000/evSelFreq.QuadPart), cands.size(), g_chunkCount); fflush(stderr); }
    // Lowest occupancy (live/total) first: a.live/a.total < b.live/b.total, via
    // cross-multiplication (all terms positive) to avoid floating point.
    std::sort(cands.begin(), cands.end(), [](const EvacCand& a, const EvacCand& b) {
        return a.live * b.total < b.live * a.total;
    });

    std::vector<EvacRegion> evac;
    int64_t liveBudget = s_budgetBytes;
    for (const EvacCand& c : cands)
    {
        if (liveBudget <= 0)
            break;
        evac.push_back({ c.index, c.start, c.usedEnd });
        liveBudget -= c.live;
    }

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
    // Time-budgeted incremental copy (paper §3.3.4): bound the STW copy by wall
    // clock, checked only at region BOUNDARIES so a region is never left half-
    // moved (a partially-copied region would have live, un-forwarded objects yet
    // could be freed -> dangling). Regions not reached this pass stay put and are
    // re-selected next evac cycle. s_budgetMs<=0 disables the time cap.
    LARGE_INTEGER evClkFreq, evClk0;
    QueryPerformanceFrequency(&evClkFreq);
    QueryPerformanceCounter(&evClk0);
    int64_t evBudgetTicks = (s_budgetMs > 0) ? (s_budgetMs * evClkFreq.QuadPart / 1000) : 0;
    for (const EvacRegion& er : evac)
    {
        if (evBudgetTicks > 0)
        {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (now.QuadPart - evClk0.QuadPart >= evBudgetTicks)
            {
                InterlockedIncrement64(&g_lxrCounters.EvacTimeBudgetHits);
                break; // out of time this pause; remaining regions wait for next evac
            }
        }
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
    LARGE_INTEGER evPhFreq, evCopyEnd; QueryPerformanceFrequency(&evPhFreq); QueryPerformanceCounter(&evCopyEnd);
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
    // 4c. Apply the fix-up. Prefer the trace-bootstrapped, evac-scoped path
    //     (F3): fix up (a) the moved objects' destination copies (their outgoing
    //     edges) and (b) the inter-block slots recorded during this cycle's mark
    //     (incoming edges from non-moved referrers). This replaces the O(live-
    //     heap) full walk. Fall back to the sound full walk when the remembered
    //     set may be incomplete: an overflowed lane log, or a conservative-keep-
    //     alive cycle (objects kept live by an interior probe are marked WITHOUT a
    //     field scan, so their out-edges were never recorded).
    static int s_scopedFixup = -1;
    if (s_scopedFixup < 0)
        s_scopedFixup = (getenv("LXR_EVAC_SCOPED_FIXUP") != nullptr && getenv("LXR_EVAC_SCOPED_FIXUP")[0] == '0') ? 0 : 1;
    // Item F (§3.3.4): by default the incoming-edge source is the PERSISTENT,
    // barrier-maintained, line-reuse-tagged remembered set - which is complete
    // without a dedicated STW mark, so evacuation can run in a concurrent-trace
    // finish pause. LXR_EVAC_PERSIST=0 selects the legacy per-evac-cycle STW-mark-
    // derived edge logs (g_evacEdgeLogs) for A/B bisection.
    static int s_persist = -1;
    if (s_persist < 0)
        s_persist = (getenv("LXR_EVAC_PERSIST") != nullptr && getenv("LXR_EVAC_PERSIST")[0] == '0') ? 0 : 1;
    // Fall back to the sound full-heap walk when the chosen remembered set may be
    // incomplete: persistent set -> a barrier drop this interval (repaired only at
    // the *next* CompactRemsets); legacy set -> a lane overflow or a conservative-
    // keep-alive cycle (objects marked without a field scan, so no out-edges logged).
    // Item F candidate-scoping (scopeUnion): the persistent remset is now scoped to
    // candidate-target MUTATIONS and the concurrent mark supplies the pre-existing
    // candidate live edges (g_evacEdgeLogs) - so BOTH must be complete (neither the
    // remset nor an evac-edge lane overflowed, and no conservative-keep-alive object
    // was marked without a field scan).
    bool scopeUnion = s_persist && (g_evacCandidateScope != 0);
    bool useScoped = s_scopedFixup &&
        (scopeUnion ? (g_remsetActive && g_remsetOverflow == 0 &&
                       g_evacEdgeOverflow == 0 && g_conservativeKeepAliveThisCycle == 0)
         : s_persist ? (g_remsetActive && g_remsetOverflow == 0)
                     : (g_evacEdgeOverflow == 0 && g_conservativeKeepAliveThisCycle == 0));

    // A dormant parity fallback firing: the scoped (remembered-set) fix-up was
    // requested but a real remset incompleteness forced the sound full-heap walk.
    // (Distinguish from the intentional A/B disable, LXR_EVAC_SCOPED_FIXUP=0.)
    if (s_scopedFixup && !useScoped)
    {
        const char* why = scopeUnion
            ? (!g_remsetActive ? "candidate-scoped remset inactive (g_remsetActive=0)"
               : g_remsetOverflow ? "candidate-scoped persistent remset overflowed"
               : g_evacEdgeOverflow ? "candidate-scoped mark edge-log overflowed"
                                    : "conservative-keep-alive cycle (object marked without a field scan)")
            : s_persist
            ? (g_remsetActive ? "persistent remset barrier-dropped an inter-block edge this interval"
                              : "persistent remset inactive (g_remsetActive=0)")
            : (g_evacEdgeOverflow ? "legacy evac-edge lane overflowed its cap"
                                  : "conservative-keep-alive cycle (object marked without a field scan)");
        LXRReportFallback(&g_reportedEvacFullWalk, "evac remembered-set -> full-heap walk", why);
    }

    if (useScoped)
    {
        InterlockedIncrement64(&g_lxrCounters.EvacRemsetFixups);
        // (a) Outgoing edges of moved objects: scan each destination copy, forward
        //     its fields, AND (persistent mode) register its inter-block out-edges
        //     into the remembered set. The copy was produced by memcpy, so those
        //     edges never fired the write barrier; unless remembered here they
        //     would be absent from the set and dangle at a FUTURE evacuation.
        for (const MovedRange& r : movedRanges)
            GCScanObjectRefs((Object*)r.newStart, (size_t)(r.oldEnd - r.oldStart),
                [&](Object** f)
                {
                    rebaseField(f);
                    if (s_persist)
                    {
                        Object* nv = *f;
                        if (nv != nullptr && InHeap(nv))
                        {
                            uintptr_t sb = (uintptr_t)f  & ~(lxr::kBlockSize - 1);
                            uintptr_t tb = (uintptr_t)nv & ~(lxr::kBlockSize - 1);
                            if (sb != tb) RecordRemsetEdge(f);
                        }
                    }
                });
        // (b) Incoming edges from non-moved referrers.
        LARGE_INTEGER evB0; QueryPerformanceCounter(&evB0);
        if (s_persist)
        {
            // Persistent remset entries can point into a region freed since insert.
            // The line-reuse version catches the reclaim; a cached-VirtualQuery
            // committed guard catches any residual (e.g. a store that raced ahead
            // of the version-table commit).
            uint8_t* cqBase = nullptr; size_t cqLen = 0; bool cqComm = false;
            auto slotCommitted = [&](void* s) -> bool
            {
                if ((uint8_t*)s < cqBase || (uint8_t*)s >= cqBase + cqLen)
                {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery(s, &mbi, sizeof(mbi)) == 0) { cqBase = nullptr; cqLen = 0; cqComm = false; return false; }
                    cqBase = (uint8_t*)mbi.BaseAddress; cqLen = mbi.RegionSize; cqComm = (mbi.State == MEM_COMMIT);
                }
                return cqComm;
            };
            // Gather the surviving (non-stale, not-inside-a-moved-source) remset
            // slots, then SORT them by address before the committed-VirtualQuery
            // replay. This is the same fix as D-copy 4b (commit 4a4b854): the
            // slotCommitted guard caches a single MEMORY_BASIC_INFORMATION region,
            // so scattered insertion-order slots thrash it to ONE VirtualQuery
            // syscall each - the dominant trace-finish evac cost (measured ~145ms,
            // pathologically up to tens of seconds on a large remset). Sorting
            // clusters same-region slots so the cache hits: ~one syscall per
            // distinct region. std::unique drops duplicate logs.
            std::vector<Object**> liveSlots;
            EnterCriticalSection(&g_remsetLock);
            for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
            {
                for (size_t i = 0; i < rb->Count; i++)
                {
                    Object** slot = rb->Entries[i];
                    if (LineReuseVerOf(slot) != rb->Ver[i])
                    { InterlockedIncrement64(&g_lxrCounters.RemsetStaleSkipped); continue; } // stale: source line reclaimed
                    uint8_t* sa = (uint8_t*)slot;
                    size_t lo = 0, hi = movedRanges.size();
                    while (lo < hi) { size_t mid = (lo + hi) >> 1; if (movedRanges[mid].oldStart <= sa) lo = mid + 1; else hi = mid; }
                    if (lo != 0)
                    {
                        const MovedRange& mr = movedRanges[lo - 1];
                        if (sa >= mr.oldStart && sa < mr.oldEnd)
                            continue; // slot inside a moved source: handled by (a)
                    }
                    liveSlots.push_back(slot);
                }
            }
            LeaveCriticalSection(&g_remsetLock);
            // STW finish pause: mutators suspended, marker parked -> the remset is
            // quiescent and the heap fields rebaseField touches are stable, so the
            // committed-check + rebase can run outside g_remsetLock.
            std::sort(liveSlots.begin(), liveSlots.end());
            liveSlots.erase(std::unique(liveSlots.begin(), liveSlots.end()), liveSlots.end());
            for (Object** slot : liveSlots)
            {
                if (!slotCommitted(slot))
                    continue; // referrer region decommitted since insert
                rebaseField(slot);
            }
            // Item F candidate-scoping: the barrier-maintained persistent remset
            // above holds only candidate-target MUTATIONS this window. The
            // pre-existing candidate incoming edges (from live objects the mark
            // scanned) are in g_evacEdgeLogs, repopulated by this cycle's concurrent
            // mark. Replay them too so the union covers every incoming candidate
            // edge. Same moved-source skip + committed guard as above.
            if (scopeUnion)
            {
                std::vector<Object**> markSlots;
                EnterCriticalSection(&g_evacEdgeLock);
                for (std::vector<Object**>* log : g_evacEdgeLogs)
                    for (Object** slot : *log)
                    {
                        uint8_t* sa = (uint8_t*)slot;
                        size_t lo = 0, hi = movedRanges.size();
                        while (lo < hi) { size_t mid = (lo + hi) >> 1; if (movedRanges[mid].oldStart <= sa) lo = mid + 1; else hi = mid; }
                        if (lo != 0)
                        {
                            const MovedRange& mr = movedRanges[lo - 1];
                            if (sa >= mr.oldStart && sa < mr.oldEnd)
                                continue; // slot inside a moved source: handled by (a)
                        }
                        markSlots.push_back(slot);
                    }
                LeaveCriticalSection(&g_evacEdgeLock);
                std::sort(markSlots.begin(), markSlots.end());
                markSlots.erase(std::unique(markSlots.begin(), markSlots.end()), markSlots.end());
                for (Object** slot : markSlots)
                {
                    if (!slotCommitted(slot))
                        continue;
                    rebaseField(slot);
                }
            }
        }
        else
        {
            EnterCriticalSection(&g_evacEdgeLock);
            for (std::vector<Object**>* log : g_evacEdgeLogs)
            {
                for (Object** slot : *log)
                {
                    uint8_t* sa = (uint8_t*)slot;
                    size_t lo = 0, hi = movedRanges.size();
                    while (lo < hi) { size_t mid = (lo + hi) >> 1; if (movedRanges[mid].oldStart <= sa) lo = mid + 1; else hi = mid; }
                    if (lo != 0)
                    {
                        const MovedRange& mr = movedRanges[lo - 1];
                        if (sa >= mr.oldStart && sa < mr.oldEnd)
                            continue; // slot inside a moved source: handled by (a)
                    }
                    rebaseField(slot);
                }
            }
            LeaveCriticalSection(&g_evacEdgeLock);
        }
        // (c) In-place survivors that share a 32 KiB Immix BLOCK with an evacuated
        //     object may hold INTRA-block references to objects that DID move.
        LARGE_INTEGER evB1; QueryPerformanceCounter(&evB1);
        static bool s_evacPhaseVerbose = getenv("LXR_VERBOSE") != nullptr;
        if (s_evacPhaseVerbose) { fprintf(stderr, "LXRGC: [evac-fixup] b_remset=%lldus\n", (long long)((evB1.QuadPart-evB0.QuadPart)*1000000/evPhFreq.QuadPart)); fflush(stderr); }
        //     Evacuation is per-OBJECT and our regions are sub-block (the sweep
        //     carves a block into several regions), so a referrer can sit in a
        //     DIFFERENT region of the SAME block as a moved object - including an
        //     active alloc-context region. Such intra-block edges are deliberately
        //     excluded from the (inter-block) remembered set, so scan every live
        //     object in every block touched by the evac set (not just the evac
        //     regions). Bounded by the evac-set's block span (the copy budget), NOT
        //     the whole heap.
        std::unordered_set<uintptr_t> touchedBlocks;
        for (const EvacRegion& er : evac)
            for (uint8_t* b = (uint8_t*)((uintptr_t)er.start & ~(lxr::kBlockSize - 1));
                 b < er.usedEnd; b += lxr::kBlockSize)
                touchedBlocks.insert((uintptr_t)b);
        for (size_t i = 0; i < g_chunkCount; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed)
                continue;
            uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
            if (end <= c.Start)
                continue;
            bool overlaps = false;
            for (uint8_t* b = (uint8_t*)((uintptr_t)c.Start & ~(lxr::kBlockSize - 1));
                 b < end; b += lxr::kBlockSize)
                if (touchedBlocks.count((uintptr_t)b)) { overlaps = true; break; }
            if (!overlaps)
                continue;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p;
                size_t sz = LXRObjectSize(o);
                if (sz == 0)
                    break;
                p += sz;
                if (forwarding.find(o) != forwarding.end())
                    continue; // moved source (its copy is scanned in (a))
                if (!IsMarked(o))
                    continue; // dead: freed with the region
                GCScanObjectRefs(o, sz, rebaseField);
            }
        }
    }
    else
    {
        InterlockedIncrement64(&g_lxrCounters.EvacFullWalkFallbacks);
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
    }
    // Item D-copy: an evac copy M' is produced by memcpy, so M's mature->young
    // edges are duplicated into M' at NEW slot addresses while the D-copy old->
    // young remembered set (g_dcopyModifiedSlots) still holds the DEAD source M's
    // slots. If this finish pause does NOT age the nursery (a multi-epoch meFinish
    // runs with phase==RCPause, so the epoch bump that would mature the targets and
    // clear the remset is skipped), M'->young stays live-and-untracked and would
    // dangle when a later nursery-copy relocates the young target. Re-register each
    // evac copy's young out-edges at the copy's stable (mature) slot. At a
    // TracePause finish the subsequent epoch bump clears these again, harmlessly.
    LARGE_INTEGER evFixupEnd; QueryPerformanceCounter(&evFixupEnd);
    if (g_dcopyCaptureModified && !g_dcopyRemsetOverflow)
    {
        for (const MovedRange& r : movedRanges)
            GCScanObjectRefs((Object*)r.newStart, (size_t)(r.oldEnd - r.oldStart),
                [&](Object** f)
                {
                    Object* t = *f;
                    if (t == nullptr) return;
                    if ((uint8_t*)t < m_heapBase || (uint8_t*)t >= m_heapBase + m_heapBytes) return;
                    if (!IsYoung(t)) return;
                    DCopyRemsetAppend(f, t, m_heapBase, m_heapBytes);
                });
    }
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
        // Reclaimed => RC 0 (see the sweep decommit site). Bounded to the used
        // extent that actually held objects/RC.
        ClearRCRange(c.Start, c.UsedEnd);
        ClearLoggedRange(c.Start, c.UsedEnd); // reused range must start unlogged
        ReclaimRegionMemory(idx); // decommit deferred off-pause
    }
    LeaveCriticalSection(&g_chunkLock);

    if (verbose)
    {
        LARGE_INTEGER evFreeEnd; QueryPerformanceCounter(&evFreeEnd);
        auto usp = [&](LARGE_INTEGER a, LARGE_INTEGER b){ return (long long)((b.QuadPart-a.QuadPart)*1000000/evPhFreq.QuadPart); };
        fprintf(stderr, "LXRGC: [evac-phases] copy=%lldus fixup=%lldus free+dcopy=%lldus\n",
                usp(evSel1, evCopyEnd), usp(evCopyEnd, evFixupEnd), usp(evFixupEnd, evFreeEnd));
        fprintf(stderr, "LXRGC: [evac] regions=%zu moved=%lld bytes=%lld pinnedSkipped=%lld fieldsForwarded=%lld freed=%zu fixup=%s(scoped=%lld fullwalk=%lld)\n",
                evac.size(), (long long)g_lxrCounters.EvacObjects, (long long)g_lxrCounters.EvacBytesCopied,
                (long long)g_lxrCounters.EvacPinnedSkipped, (long long)g_lxrCounters.EvacFieldsForwarded,
                freeableEvacIndices.size(),
                useScoped ? "scoped" : "fullwalk",
                (long long)g_lxrCounters.EvacRemsetFixups, (long long)g_lxrCounters.EvacFullWalkFallbacks);
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

// Diagnostic (LXR_NURSERY_DIAG): definitively classify any young liveness the
// remembered-set closure misses versus a full-heap closure. CRASH-SAFE: it only
// computes both closures and reports; it never decommits. For every young object
// the full scan keeps but the remset closure misses, it locates a referrer and
// reports whether that referrer is MATURE (=> a genuine mature->young remembered-
// set / write-barrier gap) or YOUNG (=> a broken young->young chain), plus the
// referrer's MethodTable and whether the edge is inter-block (barrier-visible).
// From-roots full (any-path) reachability probe: seeds roots + handles and
// follows ALL references (mature and young). Used by the nursery diagnostic to
// decide whether a "missed" young object is genuinely LIVE (reachable from a
// root => a real remembered-set/closure bug) or DEAD (only reachable from dead
// unswept objects that the full-heap field scan conservatively retained).
static std::unordered_set<Object*>* g_diagRootReach = nullptr;
static std::vector<Object*>*        g_diagRootWork  = nullptr;
static void DiagRootSeed(uint8_t* v)
{
    if (v == nullptr || g_diagRootReach == nullptr)
        return;
    Object* base = g_lxrCollector.ResolveInterior(v);
    if (base != nullptr && g_diagRootReach->insert(base).second)
        g_diagRootWork->push_back(base);
}
static void DiagRootRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t /*flags*/)
{
    if (*ppObj != nullptr) DiagRootSeed((uint8_t*)*ppObj);
}
static void DiagRootHandle(Object** ref, void* /*ctx*/) { DiagRootSeed((uint8_t*)*ref); }

static void RunNurseryDiag()
{
    // (1) Remembered-set closure: roots + handles + remset + young->young.
    NurseryClosure remsetClo;
    g_nurseryClosure = &remsetClo;
    ScanContext sc; sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRNurseryRoot, 2, 2, &sc);
    LXRGCHandleStore::ForEachLiveHandle(&LXRNurseryHandle, nullptr);
    g_lxrCollector.EnumerateRemsetSlots(&LXRNurseryRemsetSlot, nullptr);
    while (!remsetClo.work.empty())
    {
        Object* o = remsetClo.work.back(); remsetClo.work.pop_back();
        size_t sz = LXRObjectSize(o); if (sz == 0) continue;
        GCScanObjectRefs(o, sz, [](Object** ref) { LXRNurserySeed((uint8_t*)*ref); });
    }

    // (2) Full-heap closure: seed from every object's fields + young->young.
    NurseryClosure fullClo;
    g_nurseryClosure = &fullClo;
    EnterCriticalSection(&g_chunkLock);
    size_t n = g_chunkCount;
    for (size_t i = 0; i < n; i++)
    {
        ChunkRegion c = g_chunks[i];
        if (!c.Committed) continue;
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
    while (!fullClo.work.empty())
    {
        Object* o = fullClo.work.back(); fullClo.work.pop_back();
        size_t sz = LXRObjectSize(o); if (sz == 0) continue;
        GCScanObjectRefs(o, sz, [](Object** ref) { LXRNurserySeed((uint8_t*)*ref); });
    }
    g_nurseryClosure = nullptr;

    // (3) Diff: young objects the full closure keeps but the remset closure misses.
    std::unordered_set<Object*> missed;
    for (Object* y : fullClo.live)
        if (remsetClo.live.count(y) == 0)
            missed.insert(y);
    fprintf(stderr, "LXRGC: [nursery-diag] remsetLive=%zu fullLive=%zu missed=%zu\n",
            remsetClo.live.size(), fullClo.live.size(), missed.size());
    if (missed.empty()) { fflush(stderr); return; }

    // (3b) From-roots full (any-path) reachability: the authoritative live test.
    //      If missed young objects are reachable from a root, the remset closure
    //      has a real gap (=> unsafe to reclaim). If NONE are, they are dead and
    //      the remset closure is correct (the AV lies elsewhere).
    std::unordered_set<Object*> rootReach;
    std::vector<Object*> rootWork;
    g_diagRootReach = &rootReach; g_diagRootWork = &rootWork;
    ScanContext sc2; sc2.promotion = true;
    g_theGCToCLR->GcScanRoots(&DiagRootRoot, 2, 2, &sc2);
    LXRGCHandleStore::ForEachLiveHandle(&DiagRootHandle, nullptr);
    while (!rootWork.empty())
    {
        Object* o = rootWork.back(); rootWork.pop_back();
        size_t sz = LXRObjectSize(o); if (sz == 0) continue;
        GCScanObjectRefs(o, sz, [](Object** ref) { DiagRootSeed((uint8_t*)*ref); });
    }
    g_diagRootReach = nullptr; g_diagRootWork = nullptr;
    size_t missedLive = 0;
    for (Object* y : missed)
        if (rootReach.count(y) != 0)
            missedLive++;
    fprintf(stderr, "LXRGC: [nursery-diag] rootReach=%zu  missedReachableFromRoots=%zu / %zu  => %s\n",
            rootReach.size(), missedLive, missed.size(),
            missedLive == 0 ? "ALL-MISSED-ARE-DEAD (remset closure correct)"
                            : "SOME-MISSED-ARE-LIVE (remset closure gap!)");

    // (3c) Smoking-gun for the nursery AV: young objects the remset closure deems
    //      DEAD (not live => reclaimable) but that a trace has MARKED. Freeing a
    //      marked object makes a subsequent trace drain / sweep walk a decommitted
    //      page. If this is > 0, the AV is a nursery-vs-trace (SATB) coordination
    //      gap, NOT a remembered-set/barrier gap.
    size_t youngTotal = 0, youngMarked = 0, youngDeadMarked = 0;
    EnterCriticalSection(&g_chunkLock);
    size_t n3 = g_chunkCount;
    for (size_t i = 0; i < n3; i++)
    {
        ChunkRegion c = g_chunks[i];
        if (!c.Committed) continue;
        uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        uint8_t* p = c.Start;
        while (p < end)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) break;
            p += sz;
            if (!g_lxrCollector.IsYoung(o)) continue;
            youngTotal++;
            bool marked = g_lxrCollector.IsMarked(o);
            if (marked) youngMarked++;
            if (marked && remsetClo.live.count(o) == 0) youngDeadMarked++;
        }
    }
    LeaveCriticalSection(&g_chunkLock);
    fprintf(stderr, "LXRGC: [nursery-diag] youngTotal=%zu youngMarked=%zu youngDeadMarked=%zu  => %s\n",
            youngTotal, youngMarked, youngDeadMarked,
            youngDeadMarked == 0 ? "no dead-but-marked young"
                                 : "DEAD-BUT-MARKED YOUNG (nursery-vs-trace SATB gap => AV source)");

    // (4) Locate + classify EXTERNAL referrers of the missed young objects
    //     (referrers not themselves in the missed set) — the true entry points.
    int64_t matureRef = 0, youngInRemset = 0, youngNotRemset = 0;
    int matched = 0;
    EnterCriticalSection(&g_chunkLock);
    size_t n2 = g_chunkCount;
    for (size_t i = 0; i < n2 && matched < 64; i++)
    {
        ChunkRegion c = g_chunks[i];
        if (!c.Committed) continue;
        uint8_t* end = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
        uint8_t* p = c.Start;
        while (p < end && matched < 64)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) break;
            p += sz;
            if (missed.count(o) != 0) continue; // internal edge: not an entry point
            GCScanObjectRefs(o, sz, [&](Object** ref) {
                if (matched >= 64) return;
                uint8_t* v = (uint8_t*)*ref;
                if (v == nullptr) return;
                Object* base = (missed.count((Object*)v) != 0) ? (Object*)v : nullptr;
                if (base == nullptr)
                {
                    if (!g_lxrCollector.IsYoung((Object*)v)) return; // O(1) reject
                    base = g_lxrCollector.ResolveInterior(v);
                    if (base == nullptr || missed.count(base) == 0) return;
                }
                bool oYoung = g_lxrCollector.IsYoung(o);
                bool oInRemset = (remsetClo.live.count(o) != 0);
                if (!oYoung) matureRef++;
                else if (oInRemset) youngInRemset++;
                else youngNotRemset++;
                uintptr_t sblk = (uintptr_t)ref  & ~(lxr::kBlockSize - 1);
                uintptr_t tblk = (uintptr_t)base  & ~(lxr::kBlockSize - 1);
                fprintf(stderr, "LXRGC: [nursery-diag]  ENTRY O=%p MT=%p young=%d inRemsetClo=%d interBlock=%d slot=%p -> Y=%p\n",
                        (void*)o, (void*)o->GetGCSafeMethodTable(), (int)oYoung, (int)oInRemset,
                        (int)(sblk != tblk), (void*)ref, (void*)base);
                matched++;
            });
        }
    }
    LeaveCriticalSection(&g_chunkLock);
    fprintf(stderr, "LXRGC: [nursery-diag] EXTERNAL entries: mature=%lld youngInRemsetClo=%lld youngNotInRemsetClo=%lld (matched<=64)\n",
            (long long)matureRef, (long long)youngInRemset, (long long)youngNotRemset);
    fflush(stderr);
}

// --- Nursery root keep-set --------------------------------------------------
// A pure RC pause has no backup-trace backstop, so the nursery cannot rely on
// marks for root reachability. Direct root referents ARE increment-protected by
// CaptureRoots' deferred RC, EXCEPT interior/byref roots that ResolveInterior
// cannot map to a base object (LXRCollectRoot skips those for RC). Such a root
// still pins the young object it points into, so the nursery must keep any region
// that contains a raw root address. We collect ALL raw root referent addresses
// (interior included, no resolution) under STW and keep any region covering one.
// O(roots) collect + O(regions log roots) test; no O(heap) scan.
static std::vector<uint8_t*>* g_nurseryRootAddrs = nullptr;

// Env-gated diagnostic (LXR_NURSERY_GUARD): a ring of recently-decommitted young
// ranges. If a marker later reaches an object inside one, we log the exact
// parent->child edge that kept it reachable (proving which store the RC missed)
// instead of faulting on the decommitted page.
static void LXRNurseryCollectHandleAddr(Object** ref, void* /*ctx*/)
{
    Object* o = (ref != nullptr) ? *ref : nullptr;
    if (o != nullptr && g_nurseryRootAddrs != nullptr)
        g_nurseryRootAddrs->push_back((uint8_t*)o);
}

static void LXRNurseryCollectRootAddr(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t /*flags*/)
{
    Object* o = (ppObj != nullptr) ? *ppObj : nullptr;
    if (o != nullptr && g_nurseryRootAddrs != nullptr)
        g_nurseryRootAddrs->push_back((uint8_t*)o); // raw address, interior kept unresolved
}

// Young/nursery collection, run under the RC-pause STW. Implements the paper's
// implicitly-dead-young reclamation (§3.3): young objects are born RC 0 and are
// reference-counted from birth like any other object (RCIncrement/RCDecrement no
// longer skip them). By the time this runs in the RC pause, ProcessModifiedBuffers
// has already applied ALL increments for this epoch - both the coalesced modified-
// buffer new-values AND the deferred root increments (CaptureRoots) - and drained
// decrements. A young object still at RC 0 therefore has no counted reference from
// any root or heap object: it is *implicitly dead* and reclaimed here, before the
// engine restarts. Reclaim-only (no copying): survivors (RC>=1) stay young and are
// compacted/promoted by the trace-cycle Evacuate, then aged to mature at the next
// trace. This reuses the precise coalescing RC as the sole liveness oracle - no
// remembered set, no closure walk, no O(heap) scan of live young.
void LXRCollector::CollectNursery()
{
    if (!g_youngRC || !g_nurseryActive || g_theGCToCLR == nullptr)
        return;
    if (g_nurseryGuard < 0)
        g_nurseryGuard = (getenv("LXR_NURSERY_GUARD") != nullptr) ? 1 : 0;

    // Crash-safe classification diagnostic (kept env-gated for future debugging):
    // reports what a remembered-set closure would miss vs a full-heap closure,
    // then returns WITHOUT reclaiming. Zero AV risk.
    static int s_diag = (getenv("LXR_NURSERY_DIAG") != nullptr) ? 1 : 0;
    if (s_diag)
    {
        RunNurseryDiag();
        return;
    }

    // Coordinate with the concurrent backup trace: while a trace window is open the
    // marker may be walking young regions and its marks / allocate-black keep young
    // objects alive independently of RC, so freeing young underneath it is unsafe.
    // Defer to the trace, which reclaims dead young at its next cycle.
    if (g_traceWindowOpen)
    {
        InterlockedIncrement64(&g_lxrCounters.NurserySkipped);
        return;
    }

    // A first-log RC increment was dropped since the last complete trace (rare
    // shared free-list exhaustion), so a live young referent may be stuck below its
    // true RC. Reclaiming RC=0 young now could free a live object -> defer nursery
    // reclamation until the next complete trace re-establishes liveness mark-
    // authoritatively (which clears this flag and ages current young to mature).
    if (g_youngRCIncomplete)
    {
        InterlockedIncrement64(&g_lxrCounters.NurserySkipped);
        LXRReportFallback(&g_reportedNurseryDefer, "nursery reclaim deferred (young RC incomplete)",
                          "a modified-buffer first-log was dropped -> young RC may be undercounted; "
                          "deferring reclaim to the next complete trace");
        return;
    }

    bool verbose = getenv("LXR_VERBOSE") != nullptr;

    // Collect all raw root referent addresses (interior/byref included, UNresolved)
    // under the RC-pause STW. Any young region covering a root address is pinned and
    // must not be reclaimed even if RC==0 and unmarked (closes the interior-root
    // hole where LXRCollectRoot skips unresolvable interior roots for RC deferral).
    std::vector<uint8_t*> rootAddrs;
    if (g_theGCToCLR != nullptr)
    {
        g_nurseryRootAddrs = &rootAddrs;
        LXRGCHandleStore::ForEachLiveHandle(&LXRNurseryCollectHandleAddr, nullptr);
        ScanContext rsc;
        rsc.promotion = true;
        g_theGCToCLR->GcScanRoots(&LXRNurseryCollectRootAddr, 2, 2, &rsc);
        g_nurseryRootAddrs = nullptr;
        std::sort(rootAddrs.begin(), rootAddrs.end());
    }
    auto rootInRange = [&](uint8_t* start, uint8_t* end) -> bool {
        auto it = std::lower_bound(rootAddrs.begin(), rootAddrs.end(), start);
        return it != rootAddrs.end() && *it < end;
    };

    // Reclaim young regions in which EVERY young object is implicitly dead (RC 0),
    // region-granular Immix-style. Mirrors the sweep decommit path.
    int64_t regionsReclaimed = 0, bytesReclaimed = 0, liveYoung = 0;

    // A never-incremented young object's RC-table page may be reserved-but-
    // uncommitted; that is definitionally RC 0. Guard the read with a one-page
    // VirtualQuery cache (successive granules share a page) so a still-dormant page
    // never faults the scan.
    uint8_t* rcCacheBase = nullptr; size_t rcCacheLen = 0; bool rcCacheCommitted = false;
    auto rcValue = [&](Object* o) -> uint8_t {
        uint8_t* slot = RCSlot(o);
        if (slot < rcCacheBase || slot >= rcCacheBase + rcCacheLen)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(slot, &mbi, sizeof(mbi)) == 0) return 0;
            rcCacheBase = (uint8_t*)mbi.BaseAddress;
            rcCacheLen = mbi.RegionSize;
            rcCacheCommitted = (mbi.State == MEM_COMMIT);
        }
        return rcCacheCommitted ? *slot : (uint8_t)0;
    };

    // Diagnostic (LXR_NURSERY_ROOTPROBE): the AUTHORITATIVE live test for the RC=0
    // victim set. Build the set of would-be-freed young (RC==0, unmarked), then run
    // a from-roots (interior-aware via ResolveInterior) reachability closure and
    // count how many victims are actually reachable from a root. If >0 the RC=0
    // criterion would free LIVE objects (root capture / RC gap => fix the barrier /
    // root capture); if ==0 all victims are genuinely dead and the AV is a
    // nursery-vs-trace decommit/recycle race (fix the reclamation coordination).
    // Never reclaims -> zero AV risk.
    static int s_rootprobe = (getenv("LXR_NURSERY_ROOTPROBE") != nullptr) ? 1 : 0;
    if (s_rootprobe)
    {
        EnterCriticalSection(&g_chunkLock);
        std::unordered_set<Object*> victims;
        size_t nchunks = g_chunkCount;
        for (size_t i = 0; i < nchunks; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed || c.FreeRun || c.Owner != nullptr) continue;
            uint8_t* end = c.UsedEnd;
            if (end <= c.Start || !IsYoung((Object*)c.Start)) continue;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p; size_t sz = LXRObjectSize(o);
                if (sz == 0) break;
                if (rcValue(o) == 0 && !IsMarked(o)) victims.insert(o);
                p += sz;
            }
        }
        LeaveCriticalSection(&g_chunkLock);
        // From-roots reachability (authoritative), interior-aware.
        std::unordered_set<Object*> rootReach;
        std::vector<Object*> rootWork;
        g_diagRootReach = &rootReach; g_diagRootWork = &rootWork;
        ScanContext sc2; sc2.promotion = true;
        g_theGCToCLR->GcScanRoots(&DiagRootRoot, 2, 2, &sc2);
        LXRGCHandleStore::ForEachLiveHandle(&DiagRootHandle, nullptr);
        while (!rootWork.empty())
        {
            Object* o = rootWork.back(); rootWork.pop_back();
            size_t sz = LXRObjectSize(o); if (sz == 0) continue;
            GCScanObjectRefs(o, sz, [](Object** ref) { DiagRootSeed((uint8_t*)*ref); });
        }
        g_diagRootReach = nullptr; g_diagRootWork = nullptr;
        // Collect the LIVE victims (RC0 unmarked young that ARE root-reachable).
        std::unordered_set<Object*> liveVictims;
        for (Object* v : victims)
            if (rootReach.count(v) != 0)
                liveVictims.insert(v);
        // For each live victim, find a root-reachable REFERRER P and report the
        // edge P.field -> O: the exact uncounted store. P's young/mature + RC tells
        // us which store form bypassed the RC barrier.
        size_t loggedLive = 0;
        if (!liveVictims.empty())
        {
            for (Object* P : rootReach)
            {
                if (loggedLive >= 20) break;
                size_t sz = LXRObjectSize(P); if (sz == 0) continue;
                GCScanObjectRefs(P, sz, [&](Object** ref){
                    Object* O = *ref;
                    if (O != nullptr && liveVictims.count(O) != 0 && loggedLive < 20)
                    {
                        MethodTable* Pmt = *(MethodTable**)P;
                        MethodTable* Omt = *(MethodTable**)O;
                        fprintf(stderr, "LXRGC: [rootprobe] LIVE EDGE P=%p Pmt=%p Pyoung=%d Prc=%u "
                                "fieldOff=%lld -> O=%p Omt=%p Oyoung=%d Obase=%u Ocomp=%u\n",
                                (void*)P, (void*)Pmt, IsYoung(P) ? 1 : 0, (unsigned)rcValue(P),
                                (long long)((uint8_t*)ref - (uint8_t*)P),
                                (void*)O, (void*)Omt, IsYoung(O) ? 1 : 0,
                                (unsigned)Omt->GetBaseSize(), (unsigned)Omt->RawGetComponentSize());
                        loggedLive++;
                    }
                });
            }
        }
        fprintf(stderr, "LXRGC: [rootprobe] victims=%zu rootReach=%zu victimsReachableFromRoots=%zu => %s\n",
                victims.size(), rootReach.size(), liveVictims.size(),
                liveVictims.empty() ? "ALL-VICTIMS-DEAD (RC=0 criterion sound; AV is decommit-vs-trace race)"
                                : "SOME-VICTIMS-LIVE (RC=0 frees live young; root/RC gap)");
        fflush(stderr);
        return; // diagnostic: never reclaim
    }

    // Diagnostic (LXR_NURSERY_REFSCAN): prove/locate the uncounted edge. Build the
    // set of would-be-freed young objects (RC==0), then linear-walk the whole
    // committed heap looking for any object P whose field points at one of them.
    // Such an edge is a live reference the RC missed -> logs P/O MethodTables +
    // offset + young/mature, then returns WITHOUT reclaiming (no AV). STW-safe.
    static int s_refscan = (getenv("LXR_NURSERY_REFSCAN") != nullptr) ? 1 : 0;
    if (s_refscan)
    {
        EnterCriticalSection(&g_chunkLock);
        std::unordered_set<uint8_t*> victims;
        size_t nchunks = g_chunkCount;
        for (size_t i = 0; i < nchunks; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed || c.FreeRun || c.Owner != nullptr) continue; // skip active (mid-alloc) chunks
            uint8_t* end = c.UsedEnd;
            if (end <= c.Start || !IsYoung((Object*)c.Start)) continue;
            uint8_t* p = c.Start;
            while (p < end)
            {
                Object* o = (Object*)p; size_t sz = LXRObjectSize(o);
                if (sz == 0) break;
                if (rcValue(o) == 0 && !IsMarked(o)) victims.insert(p);
                p += sz;
            }
        }
        int logged = 0;
        for (size_t i = 0; i < nchunks && logged < 40; i++)
        {
            ChunkRegion& c = g_chunks[i];
            if (!c.Committed || c.FreeRun || c.Owner != nullptr) continue; // skip active (mid-alloc) chunks
            uint8_t* end = c.UsedEnd;
            if (end <= c.Start) continue;
            uint8_t* p = c.Start;
            while (p < end && logged < 40)
            {
                Object* P = (Object*)p; size_t sz = LXRObjectSize(P);
                if (sz == 0) break;
                MethodTable* Pmt = *(MethodTable**)P;
                bool Pyoung = IsYoung(P);
                bool Plive = (rcValue(P) != 0) || IsMarked(P) || rootInRange(p, p + sz);
                if (!Plive) { p += sz; continue; } // dead P -> its edges don't keep O alive
                GCScanObjectRefs(P, sz, [&](Object** ref){
                    uint8_t* c2 = (uint8_t*)*ref;
                    if (c2 != nullptr && victims.find(c2) != victims.end() && logged < 40)
                    {
                        fprintf(stderr, "LXRGC: [refscan] UNCOUNTED EDGE P=%p Pmt=%p Pyoung=%d Pbase=%u Pcomp=%u "
                                "fieldOff=%lld -> O=%p Omt=%p Obase=%u Ocomp=%u (O is RC0 young, would be freed)\n",
                                (void*)P, (void*)Pmt, Pyoung ? 1 : 0,
                                (unsigned)Pmt->GetBaseSize(), (unsigned)Pmt->RawGetComponentSize(),
                                (long long)((uint8_t*)ref - (uint8_t*)P),
                                (void*)c2, (void*)*(MethodTable**)c2,
                                (unsigned)(*(MethodTable**)c2)->GetBaseSize(),
                                (unsigned)(*(MethodTable**)c2)->RawGetComponentSize());
                        fflush(stderr);
                        logged++;
                    }
                });
                p += sz;
            }
        }
        fprintf(stderr, "LXRGC: [refscan] pass done victims=%llu uncountedEdgesLogged=%d\n",
                (unsigned long long)victims.size(), logged);
        fflush(stderr);
        LeaveCriticalSection(&g_chunkLock);
        return; // diagnostic: never reclaim in refscan mode
    }

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

        // Mark-authoritative safety net (mirrors SweepAndSelectDefrag): the backup
        // trace is LXR's liveness backstop, so NEVER free a region the trace marked
        // - RC may transiently undercount an object the trace has proven reachable
        // (e.g. a young object still on a concurrent marker's work list, or one kept
        // by the last trace whose decrement/increment straddled an STW/concurrent
        // boundary). A marked object here means a marker holds / recently held a
        // reference into this region; decommitting it would strand that reference.
        if (AnyMarkedInRange(c.Start, c.UsedEnd))
            continue;

        // Root-authoritative safety net: a raw root (including an unresolvable
        // interior/byref root that RC deferral skipped) pointing anywhere into this
        // region pins it. Reclaiming underneath a live root strands that reference.
        if (rootInRange(c.Start, c.UsedEnd))
            continue;

        // Any live (RC>=1) young object in [Start,UsedEnd) keeps the whole region.
        bool anyLive = false;
        uint8_t* p = c.Start;
        while (p < c.UsedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) { anyLive = true; break; }      // unparseable: keep, never free
            if (rcValue(o) != 0) { anyLive = true; liveYoung++; break; } // implicitly LIVE
            p += sz;
        }
        if (anyLive)
            continue;

        // Dead young region: reclaim its memory (decommit deferred off-pause by
        // ReclaimRegionMemory) and recycle it. RC/log cleared in-pause.
        RecordFreedYoung(c.Start, c.Start + c.Size, g_lxrCounters.NurseryPasses);
        // Reclaimed => RC 0. Young objects are now reference-counted, so clear
        // their RC bytes as the region is decommitted (mirrors the sweep decommit
        // site) so no stale count survives into the decommitted range and a later
        // wild decrement can never index a live-looking slot here.
        ClearRCRange(c.Start, c.UsedEnd);
        ClearLoggedRange(c.Start, c.UsedEnd); // reused range must start unlogged
        bytesReclaimed += ReclaimRegionMemory(i);
        regionsReclaimed++;
    }
    LeaveCriticalSection(&g_chunkLock);

    InterlockedExchange64(&g_lxrCounters.NurseryLiveYoung, liveYoung);
    InterlockedIncrement64(&g_lxrCounters.NurseryPasses);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryRegionsReclaimed, regionsReclaimed);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryBytesReclaimed, bytesReclaimed);
    if (verbose)
    {
        fprintf(stderr, "LXRGC: [nursery] liveYoungRegions=%lld regionsReclaimed=%lld bytesReclaimed=%lld\n",
                (long long)liveYoung, (long long)regionsReclaimed, (long long)bytesReclaimed);
        fflush(stderr);
    }
}

// ===========================================================================
//  Item D-copy — young-survivor copy at the RC pause (paper §3.3.1-3, RCImmix
//  "judicious copying of young survivors to defragment blocks").
//
//  CollectNursery reclaims the IMPLICITLY-DEAD young (RC 0). This is its
//  defragmenting/promoting complement: the young SURVIVORS (RC>0) are copied out
//  of the young regions into fresh MATURE space so the emptied young regions can
//  be recycled here, at the RC pause, instead of leaving survivors in place to be
//  compacted only by the far less frequent trace-cycle Evacuate. Copying young
//  survivors both defragments (a survivor amid dead young no longer pins its
//  region) and promotes (a survivor that lived through an RC pause graduates out
//  of the nursery), matching the paper's premise that most reclamation - and here
//  most compaction - happens at the light RC pause, not the occasional trace.
//
//  Liveness authority is RC, NOT marks: outside a trace window marks are stale
//  (set at the last trace, which predates every current-epoch young object), so
//  a survivor is any young object with RC>0. This mirrors ReclaimMatureByRC.
//
//  Soundness of the reference fix-up (the hard part of any moving collector):
//   * Roots/handles PIN their referents (a young object any raw root/handle
//     address falls within is never moved), so no root or handle needs fixing
//     up - exactly as Evacuate. Raw addresses (not resolved-then-bail like
//     Evacuate) are used because the RC pause is frequent and one unresolvable
//     interior byref must not disable copying for the whole pause.
//   * Incoming heap edges into a moved survivor come only from (i) other young
//     objects or (ii) mature objects (roots are pinned). A this-epoch young
//     survivor can only be referenced by a field written THIS epoch, and the
//     coalescing write barrier logs every first-modified field before the pause,
//     so ProcessModifiedBuffers' coalesced slot set (captured into
//     g_dcopyModifiedSlots) is the COMPLETE incoming-edge remembered set. D-copy
//     replays it (step 4b) - re-reading each slot's current value, skipping slots
//     inside moved sources (handled by the dest-copy scan 4a), guarding against
//     slots in a region freed by the zero-count cascade - and additionally scans
//     the moved copies' own out-edges (4a). This is bounded by the epoch's
//     modified-field count (the paper's field-logging barrier cost), NOT the heap.
//     Completeness holds precisely when D-copy runs: a dropped first-log sets
//     g_youngRCIncomplete, which makes this pass self-skip.
//   * If the modified-slot capture is unavailable the pass falls back to a sound
//     O(heap) full walk (like Evacuate). LXR_VERIFY_TRACE cross-checks that no
//     live field is left pointing at a moved source.
//
//  Promotion destinations are stamped MATURE (StampMatureEpoch) so a promoted
//  survivor is not seen as young again and re-copied every pause (infinite copy).
//
//  Deferred entirely while g_traceWindowOpen (marker mutating marks / walking
//  young) or g_youngRCIncomplete (a dropped first-log => possible RC undercount),
//  the same guards CollectNursery / ReclaimMatureByRC use. Runs BEFORE
//  CollectNursery so a region emptied of survivors here is freed here, and a
//  region of purely dead young is mopped up there. Opt-in via LXR_NURSERY_COPY.
// ===========================================================================
struct DCopyMovedRange { uint8_t* oldStart; uint8_t* oldEnd; uint8_t* newStart; };
struct DCopyFixupCtx
{
    std::unordered_map<Object*, Object*>* forwarding = nullptr;
    std::vector<DCopyMovedRange>*         movedRanges = nullptr; // sorted by oldStart
    int64_t                               forwarded = 0;
    // One-page VirtualQuery cache: a remembered-set slot may lie in a region
    // decommitted by an earlier RC pause (the remset is only reset at traces), so
    // guard every slot read against a non-committed page instead of faulting.
    uint8_t* cacheBase = nullptr; size_t cacheLen = 0; bool cacheCommitted = false;
    bool SlotCommitted(void* slot)
    {
        if ((uint8_t*)slot < cacheBase || (uint8_t*)slot >= cacheBase + cacheLen)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(slot, &mbi, sizeof(mbi)) == 0)
            {
                cacheBase = nullptr; cacheLen = 0; cacheCommitted = false; return false;
            }
            cacheBase = (uint8_t*)mbi.BaseAddress; cacheLen = mbi.RegionSize;
            cacheCommitted = (mbi.State == MEM_COMMIT);
        }
        return cacheCommitted;
    }
    void Rebase(Object** f)
    {
        uint8_t* v = (uint8_t*)*f;
        if (v == nullptr)
            return;
        // Hot path: a single binary search over the (small, sorted) moved ranges
        // resolves both exact-start and interior refs. This is called for every
        // field of every young object in 4a-young (~millions/pass), so the former
        // per-ref unordered_map::find is elided -- movedRanges' oldStart keys are
        // exactly forwarding's keys, and the >= lower bound catches exact starts.
        if (movedRanges->empty())
            return;
        size_t lo = 0, hi = movedRanges->size();
        while (lo < hi) { size_t mid = (lo + hi) >> 1; if ((*movedRanges)[mid].oldStart <= v) lo = mid + 1; else hi = mid; }
        if (lo == 0)
            return;
        const DCopyMovedRange& r = (*movedRanges)[lo - 1];
        if (v >= r.oldStart && v < r.oldEnd)
        {
            *f = (Object*)(r.newStart + (v - r.oldStart)); forwarded++;
            InterlockedIncrement64(&g_lxrCounters.NurseryCopyFieldsForwarded);
        }
    }
    // O(log n) "is this object a moved source?" (replaces forwarding->find in the
    // 4a-young per-object skip check; equivalent since moved starts == forwarding keys).
    bool IsMovedSource(Object* o) const
    {
        uint8_t* v = (uint8_t*)o;
        if (movedRanges->empty())
            return false;
        size_t lo = 0, hi = movedRanges->size();
        while (lo < hi) { size_t mid = (lo + hi) >> 1; if ((*movedRanges)[mid].oldStart <= v) lo = mid + 1; else hi = mid; }
        return lo != 0 && (*movedRanges)[lo - 1].oldStart == v;
    }
};
static DCopyFixupCtx* g_dcopyFixup = nullptr;

// Parallel 4a-young fix-up: the young-space rescan (rebase the out-edges of every
// live young object so young->young edges to moved survivors are corrected) is the
// dominant D-copy STW sub-cost (~O(young space); ~26 ms with ~500 young regions),
// because the JIT elides the write barrier on young->young init stores so these
// edges are in no remembered set and MUST be found by scanning. The scan is
// embarrassingly parallel: regions are disjoint, movedRanges/forwarding are read
// only, and two lanes never write the same field. Each lane owns a private
// DCopyFixupCtx and strides the snapshotted young-region list.
struct DCopy4aYoungCtx
{
    std::vector<std::pair<uint8_t*, uint8_t*>>* regions;   // snapshot [start, cend)
    std::unordered_map<Object*, Object*>*       forwarding;
    std::vector<DCopyMovedRange>*               movedRanges; // sorted by oldStart
    int64_t*                                    laneForwarded; // [lanes], merged after
};
static void DCopy4aYoungFn(int lane, int lanes, void* ctxp)
{
    DCopy4aYoungCtx* ctx = (DCopy4aYoungCtx*)ctxp;
    DCopyFixupCtx fx;
    fx.forwarding = ctx->forwarding;
    fx.movedRanges = ctx->movedRanges;
    auto rebaseField = [&fx](Object** f) { fx.Rebase(f); };
    const std::vector<std::pair<uint8_t*, uint8_t*>>& regions = *ctx->regions;
    for (size_t i = (size_t)lane; i < regions.size(); i += (size_t)lanes)
    {
        uint8_t* p = regions[i].first;
        uint8_t* cend = regions[i].second;
        while (p < cend)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) break;
            p += sz;
            if (fx.IsMovedSource(o))
                continue; // relocated source granule (dead): copy scanned by 4a
            GCScanObjectRefs(o, sz, rebaseField);
        }
    }
    ctx->laneForwarded[lane] = fx.forwarded;
}

// Parallel 4b fix-up: replay the per-evacuation-region remembered set (mature->young
// incoming edges). This is the dominant D-copy sub-cost because SlotCommitted() issues
// a VirtualQuery syscall per scattered stale slot (the remset over-captures: most
// entries name referrers whose region was decommitted since, or targets that have
// since died/promoted, and are pruned). Buckets keyed by distinct region-slots are
// disjoint, so lanes striding a deduped bucket-index list never touch the same bucket
// (and thus never the same slot); each lane owns a private VirtualQuery cache + counters.
struct DCopy4bCtx
{
    std::vector<size_t>*                  bucketIdx;    // deduped region-slot indices to replay
    std::unordered_map<Object*, Object*>* forwarding;
    std::vector<DCopyMovedRange>*         movedRanges;  // sorted by oldStart
    std::vector<std::pair<uint8_t*, uint8_t*>>* committed; // sorted [Start,cend) committed regions
    int64_t*                              laneForwarded;
    int64_t*                              laneRemsetDelta; // entries pruned per lane
};
static void DCopy4bFn(int lane, int lanes, void* ctxp)
{
    DCopy4bCtx* ctx = (DCopy4bCtx*)ctxp;
    DCopyFixupCtx fx;
    fx.forwarding = ctx->forwarding;
    fx.movedRanges = ctx->movedRanges;
    const std::vector<std::pair<uint8_t*, uint8_t*>>& committed = *ctx->committed;
    int64_t delta = 0;
    const std::vector<size_t>& idx = *ctx->bucketIdx;
    for (size_t k = (size_t)lane; k < idx.size(); k += (size_t)lanes)
    {
        std::vector<Object**>& bucket = g_dcopyRemsetBuckets[idx[k]];
        if (bucket.empty())
            continue;
        size_t before = bucket.size();
        std::sort(bucket.begin(), bucket.end());
        bucket.erase(std::unique(bucket.begin(), bucket.end()), bucket.end());
        size_t keepW = 0;
        for (Object** slot : bucket)
        {
            uint8_t* sa = (uint8_t*)slot;
            // (i) Slot inside a moved source region: relocated (its copy's out-edges
            //     are fixed by 4a) and the source region is freed after this pass. Drop.
            if (!fx.movedRanges->empty())
            {
                size_t lo = 0, hi = fx.movedRanges->size();
                while (lo < hi) { size_t mid = (lo + hi) >> 1; if ((*fx.movedRanges)[mid].oldStart <= sa) lo = mid + 1; else hi = mid; }
                if (lo != 0)
                {
                    const DCopyMovedRange& mr = (*fx.movedRanges)[lo - 1];
                    if (sa >= mr.oldStart && sa < mr.oldEnd)
                        continue;
                }
            }
            // (ii) Referrer region decommitted: the slot's own page is gone. Drop.
            //     In-memory committed check (replaces a per-slot VirtualQuery syscall,
            //     the former dominant 4b cost): a real logged slot is a field of a
            //     mature object, which always lives below its region's alloc/used
            //     watermark -- and that extent is committed iff the region's chunk is
            //     Committed. Decommit is whole-region, so a slot whose containing
            //     [Start,cend) is absent from this snapshot names a freed region. The
            //     snapshot is built once under g_chunkLock before the pool dispatch.
            {
                size_t lo = 0, hi = committed.size();
                while (lo < hi) { size_t mid = (lo + hi) >> 1; if (committed[mid].first <= sa) lo = mid + 1; else hi = mid; }
                if (lo == 0 || sa >= committed[lo - 1].second)
                    continue; // not within any committed region: drop
            }
            // (iii) Redirect any reference into a moved source to its new home.
            fx.Rebase(slot);
            // (iv) PRUNE: keep only entries still naming a movable young target
            //      (may be copied in a later budget-limited pass). IsYoung() also
            //      bounds-checks, so a non-heap/non-young value is dropped.
            Object* cur = *slot;
            if (cur != nullptr && g_poolCollector->IsYoung(cur))
                bucket[keepW++] = slot;
        }
        bucket.resize(keepW);
        delta += (int64_t)(before - keepW);
    }
    ctx->laneForwarded[lane] = fx.forwarded;
    ctx->laneRemsetDelta[lane] = delta;
}

void LXRCollector::CopyYoungSurvivors()
{
    static int s_enabled = -1;
    if (s_enabled < 0)
    {
        const char* e = getenv("LXR_NURSERY_COPY");
        s_enabled = (e != nullptr && e[0] == '0') ? 0 : 1; // default ON (full parity)
    }
    if (!s_enabled)
        return;
    if (!g_youngRC || !g_nurseryActive || g_theGCToCLR == nullptr)
        return;
    // Same coordination guards CollectNursery / ReclaimMatureByRC use: never
    // move/free young under an in-flight concurrent trace, and never promote a
    // young object whose RC may be undercounted by a dropped first-log.
    if (g_traceWindowOpen || g_youngRCIncomplete)
        return;

    bool verbose = getenv("LXR_VERBOSE") != nullptr;

    // Policy knobs (bound the STW copy per pass; regions not reached this pass are
    // re-selected next RC pause).
    static int64_t s_budgetBytes = -1, s_budgetMs = -1;
    if (s_budgetBytes < 0)
    {
        const char* b = getenv("LXR_NURSERY_COPY_BUDGET_MB");
        s_budgetBytes = (b ? _atoi64(b) : 32) * (int64_t)1024 * 1024;
        const char* ms = getenv("LXR_NURSERY_COPY_BUDGET_MS");
        s_budgetMs = ms ? _atoi64(ms) : 20;
    }

    // 1. Collect the raw root + handle referent addresses (interior/byref kept
    //    UNRESOLVED) under the STW pause, exactly as CollectNursery does. A young
    //    object that any root/handle address falls within is PINNED (never moved),
    //    so - like Evacuate - a moved survivor is never referenced by a root and
    //    no root/handle needs fix-up. Using raw addresses (vs. resolving interiors
    //    and bailing on failure like Evacuate) is essential here: the RC pause is
    //    frequent, and bailing the whole pass on any single unresolvable interior
    //    byref would disable copying almost every pause.
    std::vector<uint8_t*> rootAddrs;
    g_nurseryRootAddrs = &rootAddrs;
    LXRGCHandleStore::ForEachLiveHandle(&LXRNurseryCollectHandleAddr, nullptr);
    {
        ScanContext sc; sc.promotion = true;
        g_theGCToCLR->GcScanRoots(&LXRNurseryCollectRootAddr, 2, 2, &sc);
    }
    g_nurseryRootAddrs = nullptr;
    std::sort(rootAddrs.begin(), rootAddrs.end());
    auto rootInRange = [&](uint8_t* start, uint8_t* end) -> bool {
        auto it = std::lower_bound(rootAddrs.begin(), rootAddrs.end(), start);
        return it != rootAddrs.end() && *it < end;
    };

    // A never-incremented young object's RC-table page may be reserved-but-
    // uncommitted (definitionally RC 0). Guard the read with a one-page cache.
    uint8_t* rcCacheBase = nullptr; size_t rcCacheLen = 0; bool rcCacheCommitted = false;
    auto rcValue = [&](Object* o) -> uint8_t {
        uint8_t* slot = RCSlot(o);
        if (slot < rcCacheBase || slot >= rcCacheBase + rcCacheLen)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(slot, &mbi, sizeof(mbi)) == 0) return 0;
            rcCacheBase = (uint8_t*)mbi.BaseAddress; rcCacheLen = mbi.RegionSize;
            rcCacheCommitted = (mbi.State == MEM_COMMIT);
        }
        return rcCacheCommitted ? *slot : (uint8_t)0;
    };

    // 2. Snapshot the young source regions (retired, committed, young). Skip the
    //    active (Owner!=nullptr) alloc chunks - moving out from under a live alloc
    //    context would dangle its bump watermark; those young objects are handled
    //    once the context retires. Snapshot before registering any dest chunk so a
    //    g_chunks realloc cannot invalidate the list (indices stay valid).
    struct SrcRegion { size_t index; uint8_t* start; uint8_t* usedEnd; };
    std::vector<SrcRegion> srcs;
    EnterCriticalSection(&g_chunkLock);
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr || c.FreeRun || c.UsedEnd <= c.Start)
            continue;
        if (!IsYoung((Object*)c.Start))
            continue;
        srcs.push_back({ i, c.Start, c.UsedEnd });
    }
    LeaveCriticalSection(&g_chunkLock);
    if (srcs.empty())
    {
        if (verbose) { fprintf(stderr, "LXRGC: [nursery-copy] no young source regions\n"); fflush(stderr); }
        return;
    }

    // 3. Copy live (RC>0), non-pinned young survivors into fresh MATURE dest
    //    chunks; record forwarding old->new. Pinned / marked survivors stay put.
    std::unordered_map<Object*, Object*> forwarding;
    std::vector<DCopyMovedRange> movedRanges;
    uint8_t* destPtr = nullptr;
    uint8_t* destEnd = nullptr;
    int      curDestIndex = -1;
    auto destAlloc = [&](size_t sz) -> uint8_t*
    {
        if (destPtr == nullptr || destPtr + sz > destEnd)
        {
            if (curDestIndex >= 0)
                g_chunks[curDestIndex].UsedEnd = destPtr;
            size_t claim = (sz > CONTEXT_ALLOC_QUANTUM) ? sz : CONTEXT_ALLOC_QUANTUM;
            claim = (claim + g_pageSize - 1) & ~((size_t)g_pageSize - 1);
            uint8_t* base = g_lxrGCHeap->ClaimBlocks(claim);
            if (base == nullptr)
                return nullptr;
            size_t committed = CommitRange(base, claim);
            InterlockedExchangeAdd64(&g_committedInUse, (int64_t)committed);
            curDestIndex = RegisterChunk(base, claim, nullptr);
            // Promote: the destination hosts MATURE objects, not this window's
            // nursery. Override RegisterChunk's StampBornEpoch (which would leave
            // the copies young and re-copy them every pause).
            StampMatureEpoch(base, claim);
            EnsureMarkCommitted(base + claim);
            EnsureLoggedUpTo(base + claim); // future barrier stores into promoted objects must not fault
            destPtr = base;
            destEnd = base + claim;
        }
        uint8_t* r = destPtr;
        destPtr += sz;
        return r;
    };

    std::vector<size_t> freeableSrcIndices;
    // Regions we actually entered the evacuation loop for this pass (bounded by the
    // per-pass byte/time budget). Only these regions' remembered-set buckets are
    // replayed in 4b -- the paper's scoped per-evac-block remset processing.
    std::vector<SrcRegion> evacuatedSrcs;
    int64_t movedObjs = 0, movedBytes = 0, pinnedKept = 0;
    LARGE_INTEGER clkFreq, clk0;
    QueryPerformanceFrequency(&clkFreq);
    QueryPerformanceCounter(&clk0);
    int64_t budgetTicks = (s_budgetMs > 0) ? (s_budgetMs * clkFreq.QuadPart / 1000) : 0;
    int64_t byteBudget = s_budgetBytes;
    for (const SrcRegion& sr : srcs)
    {
        if (budgetTicks > 0)
        {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (now.QuadPart - clk0.QuadPart >= budgetTicks)
                break; // out of time this pause; region left for next RC pause
        }
        if (byteBudget <= 0)
            break;
        evacuatedSrcs.push_back(sr);
        size_t skipped = 0, moved = 0;
        uint8_t* p = sr.start;
        while (p < sr.usedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) { skipped++; break; }   // unparseable: never free this region
            p += sz;
            // Marked (a marker holds/held a ref) or root/handle-referenced (raw
            // address falls within the object, interior included) survivors must
            // stay in place; RC 0 young are implicitly dead (CollectNursery frees
            // them with the region) and are neither moved nor kept.
            if (IsMarked(o)) { skipped++; continue; }
            if (rootInRange((uint8_t*)o, (uint8_t*)o + sz)) { skipped++; continue; }
            if (rcValue(o) == 0)
                continue; // dead young: leave; region freed only if nothing kept
            uint8_t* d = destAlloc(sz);
            if (d == nullptr) { skipped++; continue; } // out of dest space: keep in place
            memcpy(d, o, sz);
            forwarding.emplace(o, (Object*)d);
            movedRanges.push_back({ (uint8_t*)o, (uint8_t*)o + sz, d });
            CommitPageFor(RCSlot((Object*)d));
            CommitPageFor(RCSlot(o));
            *RCSlot((Object*)d) = *RCSlot(o); // preserve the survivor's reference count
            *RCSlot(o) = 0;                   // source granule retired
            // The promoted copy is now MATURE but may hold young->young field
            // edges the JIT never barriered (elided intra-nursery init stores).
            // Such an edge would become an invisible mature->young reference:
            // absent from the old->young remembered set (4b) and skipped by the
            // young-space scan (4a-young, this object is mature) in a FUTURE pass
            // when its young target relocates. Record the DEST field slots that
            // currently point to a young object into the persistent remembered set
            // so 4b covers them thereafter. The dest slot address is stable
            // (mature space); bounded by the count of surviving young->young edges.
            if (!g_dcopyRemsetOverflow)
            {
                GCScanObjectRefs((Object*)d, sz, [&](Object** f)
                {
                    Object* t = *f;
                    if (t == nullptr) return;
                    if ((uint8_t*)t < m_heapBase || (uint8_t*)t >= m_heapBase + m_heapBytes) return;
                    if (!IsYoung(t)) return;
                    DCopyRemsetAppend(f, t, m_heapBase, m_heapBytes);
                });
            }
            // Item F (§3.3.4): the promoted copy is a MATURE object produced by
            // memcpy, so its outgoing inter-block references never fired the write
            // barrier and are therefore ABSENT from the evacuation remembered set.
            // If any such mature target is later evacuated, this promoted referrer
            // would be missed by the scoped fix-up and dangle. Register the copy's
            // inter-block out-edges into the evac remset now (dest slot address is
            // stable mature space), exactly as Evacuate step (a) does for evac
            // copies. Bounded by the promoted survivors' inter-block edge count.
            if (g_remsetActive)
            {
                GCScanObjectRefs((Object*)d, sz, [&](Object** f)
                {
                    Object* nv = *f;
                    if (nv == nullptr || !InHeap(nv)) return;
                    uintptr_t sb = (uintptr_t)f  & ~(lxr::kBlockSize - 1);
                    uintptr_t tb = (uintptr_t)nv & ~(lxr::kBlockSize - 1);
                    if (sb != tb) RecordRemsetEdge(f);
                });
            }
            moved++;
            movedObjs++;
            movedBytes += (int64_t)sz;
            byteBudget -= (int64_t)sz;
        }
        pinnedKept += (int64_t)skipped;
        // Free the source region only if every live object was relocated (nothing
        // pinned/marked/unparseable kept it, and at least one object moved). A
        // region with kept survivors stays; its moved sources become dead space
        // reclaimed at the next trace.
        if (skipped == 0 && moved > 0)
            freeableSrcIndices.push_back(sr.index);
    }
    if (curDestIndex >= 0)
        g_chunks[curDestIndex].UsedEnd = destPtr;
    LARGE_INTEGER clkAfterCopy; QueryPerformanceCounter(&clkAfterCopy);

    if (movedObjs == 0)
    {
        if (verbose) { fprintf(stderr, "LXRGC: [nursery-copy] no survivors moved (pinnedKept=%lld)\n", (long long)pinnedKept); fflush(stderr); }
        InterlockedIncrement64(&g_lxrCounters.NurseryCopyPasses);
        InterlockedExchangeAdd64(&g_lxrCounters.NurseryCopyPinned, pinnedKept);
        return;
    }

    // 4. Fix up every heap reference to a moved survivor. Sort moved ranges by old
    //    address for interior/byref binary search (a heap byref may point into a
    //    moved object's interior, not just its start).
    std::sort(movedRanges.begin(), movedRanges.end(),
              [](const DCopyMovedRange& a, const DCopyMovedRange& b) { return a.oldStart < b.oldStart; });
    DCopyFixupCtx fx;
    fx.forwarding = &forwarding;
    fx.movedRanges = &movedRanges;
    auto rebaseField = [&fx](Object** f) { fx.Rebase(f); };

    static int s_scopedFixup = -1;
    if (s_scopedFixup < 0)
        s_scopedFixup = (getenv("LXR_NURSERY_COPY_SCOPED_FIXUP") != nullptr && getenv("LXR_NURSERY_COPY_SCOPED_FIXUP")[0] == '0') ? 0 : 1;
    // Scoped fix-up replays the epoch's modified slots (the complete incoming-edge
    // set for this-epoch young survivors, captured by ProcessModifiedBuffers) plus
    // the moved copies' own out-edges. Complete precisely because D-copy self-skips
    // when a first-log was dropped (g_youngRCIncomplete). Fall back to the sound
    // O(heap) walk only if capture was somehow unavailable.
    bool useScoped = s_scopedFixup && g_dcopyCaptureModified && !g_dcopyRemsetOverflow;

    // Dormant parity fallback: scoped nursery-copy fix-up was requested but the
    // old->young remembered set overflowed its cap, forcing the sound O(heap) walk.
    if (s_scopedFixup && !useScoped)
        LXRReportFallback(&g_reportedDCopyFullWalk, "nursery-copy remembered-set -> full-heap walk",
                          g_dcopyRemsetOverflow ? "D-copy old->young remset exceeded its cap"
                                                : "D-copy modified-slot capture unavailable");

    LARGE_INTEGER clkSub4a{}, clkSub4ay{};
    if (useScoped)
    {
        // 4a. Outgoing edges of moved survivors: scan each destination copy. This
        //     is the only source that catches a moved survivor's field pointing at
        //     ANOTHER moved survivor (its source slot is skipped by 4b as it lies
        //     inside a moved range). Bounded by the moved live bytes.
        for (const DCopyMovedRange& r : movedRanges)
            GCScanObjectRefs((Object*)r.newStart, (size_t)(r.oldEnd - r.oldStart), rebaseField);
        QueryPerformanceCounter(&clkSub4a);
        // 4a-young. Young->young incoming edges are NOT in the remembered set: the
        //     JIT elides the write barrier on stores that initialize a freshly-
        //     allocated (nursery) object, since an intra-generational young->young
        //     store needs no card mark. The paper finds these by scanning the young
        //     space itself (bounded by the nursery size, NOT the heap). Rebase the
        //     out-edges of every object still resident in a committed YOUNG region
        //     -- the retired sources' kept-in-place survivors AND the active alloc
        //     chunks (up to alloc_ptr) -- skipping relocated sources (their mature
        //     copies are covered by 4a) and our mature dest chunks (IsYoung false).
        //
        //     Snapshot the young-region [start, cend) ranges under the chunk lock,
        //     then rebase in parallel across the mark worker pool (the scan is
        //     region-disjoint and read-only except for the fields it rewrites). The
        //     lock is released BEFORE RunOnPool (which takes g_poolLock) to avoid a
        //     g_chunkLock->g_poolLock ordering; g_chunks is stable here anyway (no
        //     allocation/registration happens during fix-up under STW).
        {
            static int s_no4aYoung = (getenv("LXR_DCOPY_NO_4AYOUNG") != nullptr) ? 1 : 0;
            static int s_par4a = -1;
            if (s_par4a < 0)
            {
                const char* e = getenv("LXR_DCOPY_PARALLEL_4AYOUNG");
                s_par4a = (e != nullptr && e[0] == '0') ? 0 : 1; // default ON
            }
            std::vector<std::pair<uint8_t*, uint8_t*>> youngRegions;
            if (!s_no4aYoung)
            {
                EnterCriticalSection(&g_chunkLock);
                youngRegions.reserve(g_chunkCount);
                for (size_t i = 0; i < g_chunkCount; i++)
                {
                    ChunkRegion& c = g_chunks[i];
                    if (!c.Committed || c.FreeRun || c.UsedEnd <= c.Start)
                        continue;
                    if (!IsYoung((Object*)c.Start))
                        continue; // mature (incl. this pass's promotion dests): via 4a/4b
                    uint8_t* cend = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
                    youngRegions.push_back({ c.Start, cend });
                }
                LeaveCriticalSection(&g_chunkLock);
            }
            // Parallelize only when the pool exists and there is enough work to
            // amortize the wake/join; otherwise a single lane runs inline.
            int lanes = (s_par4a && g_poolWorkers > 0 && youngRegions.size() >= 16)
                            ? (g_poolWorkers + 1) : 1;
            std::vector<int64_t> laneForwarded((size_t)lanes, 0);
            DCopy4aYoungCtx yctx{ &youngRegions, &forwarding, &movedRanges, laneForwarded.data() };
            RunOnPool(lanes, &DCopy4aYoungFn, &yctx);
            for (int l = 0; l < lanes; l++)
                fx.forwarded += laneForwarded[(size_t)l];
        }
        QueryPerformanceCounter(&clkSub4ay);
        // 4b. Incoming edges from MATURE referrers, replayed PER EVACUATED REGION.
        //     The paper (§3.3) keeps a per-evacuation-BLOCK remembered set and, at
        //     evacuation time, processes only the remsets of the blocks in the
        //     evacuation set. We mirror that: g_dcopyRemsetBuckets is keyed by the
        //     TARGET young object's 128KB region, and D-copy evacuates only a
        //     budget-limited SUBSET of young regions per pause (evacuatedSrcs). So
        //     replay ONLY those regions' buckets; incoming edges to young regions
        //     deferred to a later pass stay in their buckets untouched. This bounds
        //     4b work to O(incoming edges of the evacuated regions) instead of the
        //     old O(all mature->young edges) flat replay.
        //
        //     A young object is stationary until the pass that moves it, so every
        //     edge to an object that lived in region [sr.start, sr.usedEnd) was
        //     captured into a bucket in that region's slot range -- replaying that
        //     range covers all of the region's incoming edges (soundness gate:
        //     LXR_VERIFY_TRACE below asserts no field still points at a moved
        //     source). Within each bucket we keep the same sort+dedup (SlotCommitted
        //     single-region VirtualQuery cache locality) and replay+prune as the
        //     former flat set, so stale entries (target promoted/died) are dropped
        //     and steady-state buckets stay small.
        //
        //     The replay is parallelized across the mark worker pool: gather the
        //     deduped set of region-slot bucket indices covered by the evacuated
        //     regions, then stride them across lanes. Buckets are disjoint per
        //     region-slot so no two lanes touch the same bucket/slot; SlotCommitted
        //     VirtualQuery latency (the dominant cost on scattered stale entries)
        //     overlaps across threads.
        if (!g_dcopyRemsetBuckets.empty())
        {
            size_t nbuckets = g_dcopyRemsetBuckets.size();
            std::vector<size_t> bucketIdx;
            bucketIdx.reserve(evacuatedSrcs.size());
            for (const SrcRegion& sr : evacuatedSrcs)
            {
                size_t rs0 = (size_t)(sr.start - m_heapBase) / CONTEXT_ALLOC_QUANTUM;
                size_t rs1 = (size_t)((sr.usedEnd - 1) - m_heapBase) / CONTEXT_ALLOC_QUANTUM;
                if (rs1 >= nbuckets) rs1 = nbuckets - 1;
                for (size_t rs = rs0; rs <= rs1; rs++)
                    if (!g_dcopyRemsetBuckets[rs].empty())
                        bucketIdx.push_back(rs);
            }
            // Dedup: two sub-region evacuees can share a 128KB region-slot; a shared
            // bucket must be processed by exactly one lane (else double prune/rebase).
            std::sort(bucketIdx.begin(), bucketIdx.end());
            bucketIdx.erase(std::unique(bucketIdx.begin(), bucketIdx.end()), bucketIdx.end());

            // Committed-region snapshot for the in-memory referrer-committed check
            // (step (ii) in DCopy4bFn), replacing a per-slot VirtualQuery syscall.
            // Snapshot [Start,cend) of every committed chunk under g_chunkLock, then
            // sort by Start for binary search; released before RunOnPool (which takes
            // g_poolLock) to avoid a g_chunkLock->g_poolLock inversion. g_chunks is
            // stable during fix-up (mutators suspended, no alloc/registration).
            std::vector<std::pair<uint8_t*, uint8_t*>> committedRanges;
            {
                EnterCriticalSection(&g_chunkLock);
                committedRanges.reserve(g_chunkCount);
                for (size_t i = 0; i < g_chunkCount; i++)
                {
                    ChunkRegion& c = g_chunks[i];
                    if (!c.Committed) continue;
                    uint8_t* cend = (c.Owner != nullptr) ? c.Owner->alloc_ptr : c.UsedEnd;
                    if (cend > c.Start)
                        committedRanges.push_back({ c.Start, cend });
                }
                LeaveCriticalSection(&g_chunkLock);
            }
            std::sort(committedRanges.begin(), committedRanges.end());

            static int s_par4b = -1;
            if (s_par4b < 0)
            {
                const char* e = getenv("LXR_DCOPY_PARALLEL_4B");
                s_par4b = (e != nullptr && e[0] == '0') ? 0 : 1; // default ON
            }
            int lanes = (s_par4b && g_poolWorkers > 0 && bucketIdx.size() >= 16)
                            ? (g_poolWorkers + 1) : 1;
            std::vector<int64_t> laneForwarded((size_t)lanes, 0), laneDelta((size_t)lanes, 0);
            DCopy4bCtx bctx{ &bucketIdx, &forwarding, &movedRanges, &committedRanges, laneForwarded.data(), laneDelta.data() };
            RunOnPool(lanes, &DCopy4bFn, &bctx);
            for (int l = 0; l < lanes; l++)
            {
                fx.forwarded += laneForwarded[(size_t)l];
                g_dcopyRemsetCount -= laneDelta[(size_t)l];
            }
            if (verbose)
                fprintf(stderr, "LXRGC:   [4b-detail] evacSrcs=%zu buckets=%zu lanes=%d nbuckets=%zu\n",
                        evacuatedSrcs.size(), bucketIdx.size(), lanes, nbuckets);
        }
    }
    else
    {
        InterlockedIncrement64(&g_lxrCounters.NurseryCopyFullWalks);
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
                if (fx.IsMovedSource(o))
                    continue; // dead source
                GCScanObjectRefs(o, sz, rebaseField);
            }
        }
    }
    LARGE_INTEGER clkAfterFixup; QueryPerformanceCounter(&clkAfterFixup);
    if (verbose)
    {
        fprintf(stderr, "LXRGC: [copy-breakdown] copyloop=%lldus fixup=%lldus scoped=%d remset=%zu evacRegions=%zu srcRegions=%zu\n",
                (long long)((clkAfterCopy.QuadPart - clk0.QuadPart) * 1000000 / clkFreq.QuadPart),
                (long long)((clkAfterFixup.QuadPart - clkAfterCopy.QuadPart) * 1000000 / clkFreq.QuadPart),
                (int)useScoped, g_dcopyRemsetCount, evacuatedSrcs.size(), srcs.size());
        if (useScoped)
            fprintf(stderr, "LXRGC:   [fixup-sub] 4a=%lldus 4a-young=%lldus 4b=%lldus\n",
                    (long long)((clkSub4a.QuadPart - clkAfterCopy.QuadPart) * 1000000 / clkFreq.QuadPart),
                    (long long)((clkSub4ay.QuadPart - clkSub4a.QuadPart) * 1000000 / clkFreq.QuadPart),
                    (long long)((clkAfterFixup.QuadPart - clkSub4ay.QuadPart) * 1000000 / clkFreq.QuadPart));
        fflush(stderr);
    }

    // Empirical soundness check: after fix-up NO live field may still point at a
    // moved source (that would dangle once the source region is decommitted).
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
                if (forwarding.find(o) != forwarding.end())
                    continue; // moved source
                // Only LIVE referrers matter: a dead (RC==0, unmarked, un-rooted)
                // object is unreachable, so a dangling field in it is never
                // followed. Skipping them matches Evacuate's verify and avoids
                // false positives from dead young garbage left in kept regions.
                bool live = (rcValue(o) != 0) || IsMarked(o) || rootInRange(op, p);
                if (!live)
                    continue;
                GCScanObjectRefs(o, sz, [&](Object** f)
                {
                    if (forwarding.find(*f) != forwarding.end())
                    {
                        if (misses < 20)
                            fprintf(stderr, "LXRGC: [verify-nursery-copy] UNFORWARDED ref: referrer=%p mt=%p region=%zu owner=%d young=%d off=%lld -> stale %p\n",
                                    (void*)o, (void*)o->GetGCSafeMethodTable(), i,
                                    (int)(c.Owner != nullptr), (int)IsYoung(o),
                                    (long long)((uint8_t*)f - op), (void*)*f);
                        misses++;
                    }
                });
            }
        }
        if (misses > 0)
        {
            fprintf(stderr, "LXRGC: [verify-nursery-copy] TOTAL unforwarded refs = %lld (freeable %zu source regions) fixup=%s\n",
                    (long long)misses, freeableSrcIndices.size(), useScoped ? "scoped" : "fullwalk");
            fflush(stderr);
        }
    }

    // 5. Free the fully-evacuated young source regions (all survivors relocated).
    int64_t regionsFreed = 0, bytesFreed = 0;
    EnterCriticalSection(&g_chunkLock);
    for (size_t idx : freeableSrcIndices)
    {
        ChunkRegion& c = g_chunks[idx];
        if (!c.Committed)
            continue;
        RecordFreedYoung(c.Start, c.Start + c.Size, g_lxrCounters.NurseryCopyPasses);
        ClearRCRange(c.Start, c.UsedEnd);   // reclaimed => RC 0 (mirror the sweep decommit site)
        ClearLoggedRange(c.Start, c.UsedEnd); // reused range must start unlogged
        bytesFreed += ReclaimRegionMemory(idx); // decommit deferred off-pause
        regionsFreed++;
    }
    LeaveCriticalSection(&g_chunkLock);

    InterlockedIncrement64(&g_lxrCounters.NurseryCopyPasses);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryCopyObjects, movedObjs);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryCopyBytes, movedBytes);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryCopyPinned, pinnedKept);
    InterlockedExchangeAdd64(&g_lxrCounters.NurseryCopyRegionsFreed, regionsFreed);
    if (verbose)
    {
        fprintf(stderr, "LXRGC: [nursery-copy] srcRegions=%zu moved=%lld bytes=%lld pinnedKept=%lld freed=%lld(%lld B) forwarded=%lld fixup=%s\n",
                srcs.size(), (long long)movedObjs, (long long)movedBytes, (long long)pinnedKept,
                (long long)regionsFreed, (long long)bytesFreed, (long long)fx.forwarded,
                useScoped ? "scoped" : "fullwalk");
        fflush(stderr);
    }
}

// ===========================================================================
//  Item ★ — primary-RC MATURE reclamation at the RC pause (paper §3.3).
//
//  Before this, ALL mature memory return happened at the (occasional) backup
//  trace via SweepAndSelectDefrag / CarveFreeRuns / Evacuate; DrainZeroCountWorkList
//  only decremented counts and never returned a byte. That inverts LXR's thesis:
//  RC is meant to be the PRIMARY reclaimer, the trace only an occasional cyclic
//  backstop. ReclaimMatureByRC restores that: at every RC pause it decommits /
//  recycles mature regions whose reference counts have all dropped to zero.
//
//  Soundness (outside a trace window RC is authoritative):
//   * The pluggable barrier counts EVERY ref store (incl. the patched Interlocked
//     CAS/Exchange, SpanHelpers.ClearWithReferences, bulk moves, VM
//     SetObjectReferenceUnchecked), so a heap-reachable mature object has RC>=1.
//   * ProcessModifiedBuffers has already run this pause, so RC is fully reconciled
//     and the zero-count free cascade drained.
//   * A raw root (incl. unresolvable interior/byref that RC deferral skipped) into
//     a region pins it.
//   * Young objects legitimately sit at RC 0 -> young regions are skipped (the
//     nursery's domain).
//   * Stuck (saturated 0xFF) objects are non-zero -> kept for the trace to resolve.
//   * Dead CYCLES keep RC>=1 (mutual refs) -> kept for the trace (correct: RC can't
//     collect cycles, by design).
//  Therefore an RC==0, non-root, non-young mature region is provably dead.
//
//  CRITICAL: unlike CollectNursery, this must NOT consult mark bits. Outside a
//  trace window marks are STALE (set at the last trace, only cleared at the next
//  ResetMarks), so a mature object that died since the last trace still carries its
//  old mark; AnyMarkedInRange would over-retain and block nearly all mature RC
//  reclamation, defeating the whole point of ★. RC is the authority here.
//
//  Deferred entirely while g_traceWindowOpen (marker mutating marks / RC not yet
//  reconciled) or g_youngRCIncomplete (a first-log was dropped -> possible RC
//  undercount) -- the same guards the nursery uses.
// ===========================================================================
void LXRCollector::ReclaimMatureByRC()
{
    static int s_rcReclaim = -1;
    if (s_rcReclaim < 0)
    {
        const char* e = getenv("LXR_RC_RECLAIM");
        s_rcReclaim = (e != nullptr && e[0] == '0') ? 0 : 1; // default ON
    }
    if (!s_rcReclaim || g_theGCToCLR == nullptr)
        return;

    InterlockedIncrement64(&g_lxrCounters.MatureRCPasses);

    static int s_dbg = (getenv("LXR_RC_RECLAIM_DBG") != nullptr) ? 1 : 0;

    // Marks in flux / RC not reconciled -> defer to a later, quiescent RC pause.
    if (g_traceWindowOpen || g_youngRCIncomplete)
    {
        if (s_dbg)
        {
            fprintf(stderr, "LXRGC: [rc-reclaim] DEFER pass=%lld traceWindow=%d youngIncomplete=%d\n",
                    (long long)g_lxrCounters.MatureRCPasses, (int)g_traceWindowOpen, (int)g_youngRCIncomplete);
            fflush(stderr);
        }
        return;
    }
    if (s_dbg)
    {
        fprintf(stderr, "LXRGC: [rc-reclaim] RUN pass=%lld\n", (long long)g_lxrCounters.MatureRCPasses);
        fflush(stderr);
    }

    // Raw root referent addresses (interior/byref UNresolved) under the STW pause.
    // Any region a root points into is pinned, exactly as the nursery does.
    std::vector<uint8_t*> rootAddrs;
    g_nurseryRootAddrs = &rootAddrs;
    LXRGCHandleStore::ForEachLiveHandle(&LXRNurseryCollectHandleAddr, nullptr);
    ScanContext rsc;
    rsc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRNurseryCollectRootAddr, 2, 2, &rsc);
    g_nurseryRootAddrs = nullptr;
    std::sort(rootAddrs.begin(), rootAddrs.end());
    auto rootInRange = [&](uint8_t* start, uint8_t* end) -> bool {
        auto it = std::lower_bound(rootAddrs.begin(), rootAddrs.end(), start);
        return it != rootAddrs.end() && *it < end;
    };

    int64_t regionsReclaimed = 0, bytesReclaimed = 0, runsCarved = 0, carveBytes = 0;
    // Item ★ Stage 2 (RC line-carving) rides the Immix free-run allocator, so it is
    // only useful when line reuse is enabled; gate on it (env resolved elsewhere).
    bool carve = (g_lineReuse > 0);
    EnterCriticalSection(&g_chunkLock);
    size_t sweepCount = g_chunkCount;
    for (size_t i = 0; i < sweepCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr || c.FreeRun)
            continue;                       // uncommitted, active, or a carved free run
        if (c.UsedEnd <= c.Start)
            continue;

        // Young regions belong to the nursery (young objects legitimately sit at
        // RC 0). Skip any region that contains a young block.
        bool anyYoung = false;
        for (uint8_t* b = (uint8_t*)((uintptr_t)c.Start & ~((uintptr_t)lxr::kBlockSize - 1));
             b < c.UsedEnd; b += lxr::kBlockSize)
        {
            if (IsYoung((Object*)b)) { anyYoung = true; break; }
        }
        if (anyYoung)
            continue;

        bool wholeDead = !AnyRCNonZeroInRange(c.Start, c.UsedEnd);
        bool rooted = rootInRange(c.Start, c.UsedEnd);

        if (wholeDead && !rooted)
        {
            // Whole-region dead by RC: reclaim its memory (decommit deferred
            // off-pause by ReclaimRegionMemory) + recycle it. NO mark bits -- they
            // are stale outside a trace window.
            // Reclaimed => RC 0 invariant + reused range must start unlogged (same
            // as the sweep/nursery decommit sites; closes the decommit-vs-decrement
            // race together with DrainZeroCountWorkList's committed-VirtualQuery
            // guard).
            ClearRCRange(c.Start, c.UsedEnd);
            ClearLoggedRange(c.Start, c.UsedEnd);
            bytesReclaimed += ReclaimRegionMemory(i);
            regionsReclaimed++;
        }
        else if (carve)
        {
            // Partially-dead mature region: carve its dead object-runs for reuse by
            // RC authority (Stage 2). Root-covered live objects are protected
            // per-object inside CarveDeadRunsByRC, so a rooted region can still
            // yield its dead runs. NOTE: this may realloc g_chunks -> 'c' is dead
            // after this call; we do not touch it again this iteration.
            int64_t cb = CarveDeadRunsByRC(i, rootAddrs);
            if (cb > 0) { carveBytes += cb; runsCarved++; }
        }
    }
    LeaveCriticalSection(&g_chunkLock);

    InterlockedExchangeAdd64(&g_lxrCounters.MatureRCRegionsReclaimed, regionsReclaimed);
    InterlockedExchangeAdd64(&g_lxrCounters.MatureRCBytesReclaimed, bytesReclaimed);
    InterlockedExchangeAdd64(&g_lxrCounters.MatureRCRunsCarved, runsCarved);
    InterlockedExchangeAdd64(&g_lxrCounters.MatureRCCarveBytes, carveBytes);
    if (regionsReclaimed > 0 || carveBytes > 0)
    {
        fprintf(stderr, "LXRGC: [rc-reclaim] mature regions=%lld bytes=%lld carvedRuns=%lld carvedBytes=%lld "
                "(totRegions=%lld totBytes=%lld totCarveBytes=%lld)\n",
                (long long)regionsReclaimed, (long long)bytesReclaimed,
                (long long)runsCarved, (long long)carveBytes,
                (long long)g_lxrCounters.MatureRCRegionsReclaimed,
                (long long)g_lxrCounters.MatureRCBytesReclaimed,
                (long long)g_lxrCounters.MatureRCCarveBytes);
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
    {
        const char* e = getenv("LXR_DEFER_DECOMMIT");
        g_deferDecommit = (e != nullptr) ? (atoi(e) != 0) : 1; // default ON
    }
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
        // Watchdog: on by default (LXR_WATCHDOG=0 opts out). Cheap 2s-poll thread
        // that, if a stop-the-world phase stalls >20s, prints the stuck phase and
        // dumps every thread's native stack -- so the rare early-startup EventPipe
        // suspension hang self-diagnoses instead of hanging silently.
        if (getenv("LXR_WATCHDOG") == nullptr || _atoi64(getenv("LXR_WATCHDOG")) != 0)
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
        // Item E survival-rate RC-pause trigger (paper §3.2.2): scale the budget by
        // the young-survival EWMA so the pause fires sooner when survivors (hence
        // recursive-increment + young-copy work) are predicted high, and later when
        // survival is low (cheap pause, most young die). Bounded band 0.5x..1.5x
        // (survival 100% -> 0.5x budget, 0% -> 1.5x), mirroring DecidePhase's
        // epoch-cap survival pacing. This targets the EXPECTED per-pause cost.
        if (g_survivalTrigger > 0 && g_lxrCounters.YoungSurvivalPctEwma >= 0)
        {
            budget = (budget * (150 - g_lxrCounters.YoungSurvivalPctEwma)) / 100;
            if (budget < g_gcTriggerBytes / 2) budget = g_gcTriggerBytes / 2; // never below floor/2
        }
        int64_t grown = g_committedInUse - live;
        // Item E increment-count RC trigger (paper §3.2.2): also fire a pause once
        // enough reference-count increments have been logged since the last pause,
        // so a mutation-heavy but allocation-light phase (which would grow the
        // heap slowly, delaying the growth trigger) still gets its RC processed and
        // its dead young reclaimed promptly. Reuses the existing ModifiedBuffer-
        // Entries counter (delta since g_incrementBaseline) - no new barrier cost.
        bool incTrigger = g_incrementTrigger > 0 &&
            (g_lxrCounters.ModifiedBufferEntries - g_incrementBaseline) >= g_incrementTrigger;
        if (grown >= budget || incTrigger)
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

    // NOTE: total allocation volume is tallied in FinalizeChunk (whole [Start,
    // usedEnd) used extent per retired chunk), NOT here -- the mutator bump-
    // allocates most objects inline without re-entering AllocateSlow, so a per-
    // slow-path increment would only ever see the first object of each chunk.

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
    if (index) *index = (uint64_t)g_lxrCounters.Collections;
    if (generation) *generation = 0;
    // Surface the real pause telemetry we already accumulate (previously hard-
    // wired to 0, which is why GC.GetGCMemoryInfo().PauseTimePercentage always
    // read 0 despite real multi-ms STW pauses). The managed GCMemoryInfo scales
    // pauseTimePct by /100 (so it is percent*100 basis points) and treats the
    // pause-duration array as TimeSpan ticks (100 ns), hence micros*10.
    if (pauseTimePct) *pauseTimePct = (uint32_t)(g_lxrCounters.LastGCPercentTimeInGC * 100);
    if (isCompaction) *isCompaction = (g_lxrCounters.EvacPasses > 0);
    if (isConcurrent) *isConcurrent = (g_concurrentEnabled != 0);
    if (genInfoRaw) memset(genInfoRaw, 0, sizeof(uint64_t) * 8);
    if (pauseInfoRaw)
    {
        pauseInfoRaw[0] = (uint64_t)(g_lxrCounters.LastPauseMicros * 10);
        pauseInfoRaw[1] = (uint64_t)(g_lxrCounters.MaxPauseMicros * 10);
    }
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
    if (g_incrementTrigger < 0)
    {
        // Item E increment-count RC trigger. Default ON: an RC pause is forced
        // once this many reference-count increments have been logged since the
        // last pause, bounding per-pause RC work on mutation-heavy/alloc-light
        // phases where the allocation-growth trigger alone would fire too rarely.
        const char* e = getenv("LXR_INCREMENT_TRIGGER");
        g_incrementTrigger = e ? _atoi64(e) : 2000000; // ~2M increments (0 disables)
    }
    if (g_wastageTriggerPct < 0)
    {
        // Item E wastage-based trace trigger (paper default 5%). A trace is
        // escalated once the projected floating garbage reaches this % of the
        // committed heap. 0 disables (pace traces purely by epoch cap + growth).
        const char* e = getenv("LXR_WASTAGE_PCT");
        g_wastageTriggerPct = e ? _atoi64(e) : 5;
    }
    if (g_survivalTrigger < 0)
    {
        // Item E survival-rate RC-pause trigger (paper §3.2.2). Default ON: the
        // allocation budget is modulated by the young-survival EWMA so pauses
        // fire sooner when survival (hence per-pause copy/increment work) is
        // predicted high, bounding the EXPECTED pause cost. =0 opts out (budget
        // paced purely by growth + increment count).
        const char* e = getenv("LXR_SURVIVAL_TRIGGER");
        g_survivalTrigger = (e && _atoi64(e) == 0) ? 0 : 1;
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

    // Item E wastage predictor (paper §3.2.5): escalate to a trace once the
    // PROJECTED floating garbage since the last trace reaches g_wastageTriggerPct
    // of the committed heap. Floating garbage is the dead memory RC cannot reclaim
    // (cycles + stuck counts); we estimate its accrual rate as WastagePctEwma (the
    // biased-decay average of the fraction each past trace actually recovered) and
    // apply it to the memory allocated since the last trace. This fires a trace
    // exactly when RC-uncollectable garbage is predicted to have grown "too large"
    // - the paper's wastage-driven trigger - rather than on a fixed schedule.
    if (g_wastageTriggerPct > 0 && g_lxrCounters.WastagePctEwma > 0 && g_committedInUse > 0)
    {
        int64_t projectedWaste = (growth * g_lxrCounters.WastagePctEwma) / 100;
        if (projectedWaste * 100 >= g_committedInUse * g_wastageTriggerPct)
            return LXRPhase::TracePause;
    }

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
    // Item F (§3.3.4): LXR copies ONLY during stop-the-world pauses, and moving an
    // object requires every live referrer to be forwarded by Evacuate's fix-up
    // pass. Referrers come from two complete sources at a trace finish: (1) the
    // just-completed mark (marked objects' out-fields are scanned+forwarded), and
    // (2) the PERSISTENT, barrier-maintained, line-reuse-tagged remembered set,
    // which captures inter-block mature->* edges the mark alone would miss (e.g.
    // stores into already-marked objects). Because that remset is complete at
    // EVERY trace finish - concurrent, multi-epoch, or STW - evacuation runs in
    // the finish pause of any trace, with no dedicated every-Nth fully-STW backup
    // trace. All trace-finish pauses suspend the EE and park the marker, so the
    // copy is race-free; Evacuate self-limits by fragmentation + a copy budget.
    // --- Multi-epoch trace: classify this epoch's role BEFORE the evac/legacy-
    //     concurrent decisions (item C). While a trace is MARKING, every epoch is
    //     an ordinary RC pause (a "spanned" epoch) except the one that finalizes
    //     it, so force phase=RCPause here to keep evac/legacy-concurrent from
    //     starting a second trace mid-flight.
    bool meStart = false, meFinish = false;
    if (g_multiEpoch && g_concurrentEnabled && g_theGCToCLR != nullptr && doTrace &&
        g_traceState == TRACE_MARKING)
    {
        if (g_markerQuiescent)
            meFinish = true;                       // marker parked -> finalize now
        else
            InterlockedIncrement64(&g_multiEpochSpans);
        phase = LXRPhase::RCPause;
    }

    // Item F (§3.3.4): with the PERSISTENT, barrier-maintained, line-reuse-tagged
    // remembered set, evacuation no longer needs a dedicated fully-STW backup
    // trace to derive its fix-up set - the set is complete at every trace finish.
    // So evacuate in ANY trace-finish pause (STW, concurrent, or multi-epoch) and
    // let the trace stay concurrent. Evacuate() self-limits by fragmentation and a
    // copy budget, so "every finish" costs nothing when there is little to defrag.
    bool doEvac = ((phase == LXRPhase::TracePause) || meFinish) && g_evacActive && doTrace;
    // A trace COMPLETES this pause when it is a synchronous TracePause OR the
    // multi-epoch finalize pause (meFinish). Both must age the nursery, bump the
    // trace epoch, reset the D-copy old->young remembered set, and reset the trace
    // predictors -- otherwise a multi-epoch trace (which finalizes with phase forced
    // to RCPause) would complete without ever promoting the just-traced young
    // window, leaving mature objects pointing at still-young survivors whose
    // mature->young edges are absent from the barrier-maintained remset.
    bool traceCompleted = (phase == LXRPhase::TracePause) || meFinish;
    // Begin a multi-epoch trace at this RC pause when idle and a trace is due.
    if (g_multiEpoch && g_concurrentEnabled && g_theGCToCLR != nullptr && doTrace &&
        g_traceState == TRACE_IDLE && phase == LXRPhase::TracePause)
    {
        meStart = true;
    }
    bool useConcurrent = (phase == LXRPhase::TracePause) && doTrace &&
                         g_concurrentEnabled && g_theGCToCLR != nullptr &&
                         !meStart;

    int64_t pauseMicros = 0;

    if (meStart)
    {
        // Snapshot piggybacked on this RC pause: process this epoch's RC buffers,
        // take the SATB snapshot (reset marks, arm the deletion barrier, seed
        // roots, open the trace window), then launch the background marker to
        // mark the closure across the coming RC epochs. Reclaims nothing here.
        QueryPerformanceCounter(&t0);
        LXRSetPhase("me:suspend-snapshot");
        LXRSuspendEE();
        LXRSetPhase("me:snapshot-buffers");
        LARGE_INTEGER tb0, tb1, tb2;
        QueryPerformanceCounter(&tb0);
        // Detach this epoch's RC buffers cheaply (bounded copy, no RC arithmetic /
        // no recursive free) exactly like the legacy concurrent snapshot; the
        // marker replays them off-pause via ProcessSnapshotDecrements. Falls back
        // to full STW processing only when concurrent decrements are disabled.
        if (doBuffers)
        {
            if (g_concDecrements)
                g_lxrCollector.SnapshotModifiedBuffers();
            else
                g_lxrCollector.ProcessModifiedBuffers();
        }
        QueryPerformanceCounter(&tb1);
        LXRSetPhase("me:snapshot");
        g_lxrCollector.ConcurrentTraceSnapshot();
        QueryPerformanceCounter(&tb2);
        // Item F: candidate-scoping selected this trace's evac candidates inside
        // ConcurrentTraceSnapshot. When active, the concurrent mark repopulates the
        // evac remembered set for the CURRENT candidates (g_evacEdgeLogs), supplying
        // the pre-existing candidate incoming edges the (candidate-scoped) barrier
        // no longer records heap-wide. Reset + arm recording before the marker runs.
        if (doEvac && g_evacCandidateScope) { ResetEvacEdges(); InterlockedExchange(&g_recordEvacEdges, 1); }
        if (verbose) { fprintf(stderr, "LXRGC: [stage]   snapshot breakdown: bufs=%lldus snap=%lldus\n", (long long)((tb1.QuadPart-tb0.QuadPart)*1000000/freq.QuadPart), (long long)((tb2.QuadPart-tb1.QuadPart)*1000000/freq.QuadPart)); fflush(stderr); }
        LXREnsureMarkerThread();
        InterlockedExchange(&g_traceCompleteThisCycle, 0);
        InterlockedExchange(&g_markerQuiescent, 0);
        InterlockedExchange(&g_snapshotConsumed, 0);
        InterlockedExchange(&g_traceState, TRACE_MARKING);
        SetEvent(g_markerStartEvent);
        LXRSetPhase("me:restart-snapshot");
        LXRRestartEE();
        LXRSetPhase("idle");
        QueryPerformanceCounter(&t1);
        pauseMicros = (int64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
        phase = LXRPhase::RCPause;   // account the snapshot epoch as an RC pause
        if (verbose) { fprintf(stderr, "LXRGC: [stage] multi-epoch trace snapshot (pause=%lldus)\n", (long long)pauseMicros); fflush(stderr); }
    }
    else if (meFinish)
    {
        // The marker has drained to quiescence and parked, so the driver is the
        // sole owner of the mark stack: finalize the trace at this RC pause
        // (residual SATB + allocate-black + final root rescan + reconciliation),
        // then the mark-authoritative sweep. Closes the trace window.
        QueryPerformanceCounter(&t0);
        LXRSetPhase("me:suspend-finish");
        LXRSuspendEE();
        LXRSetPhase("me:finish");
        LARGE_INTEGER tf0, tf1, tf2, tf3;
        QueryPerformanceCounter(&tf0);
        g_lxrCollector.ConcurrentTraceFinish();
        QueryPerformanceCounter(&tf1);
        // Lazy decrements (paper §3.2.5): do NOT replay the marking-window RC +
        // recursive free cascade synchronously at the pause - that made the finish
        // pause O(window mutations) and produced multi-second STW cliffs when E's
        // aggressive cadence enlarged the window. Instead detach the buffers CHEAPLY
        // here under STW (SnapshotModifiedBuffers, a bounded copy) and replay the RC
        // arithmetic + free cascade OFF-PAUSE after RestartEE (ProcessSnapshot-
        // Decrements below). ConcurrentTraceFinish already consumed the buffers'
        // NEW values for SATB reconciliation (MarkModifiedNewValues, read-only),
        // so detaching them now is safe. This is the same lazy-decrement machinery
        // the concurrent snapshot path uses, applied to the finish pause.
        LXRSetPhase("me:finish-buffers");
        if (doBuffers)
            g_lxrCollector.SnapshotModifiedBuffers();
        QueryPerformanceCounter(&tf2);
        // Item F: evacuate in this concurrent-trace finish pause (marks are
        // complete; EE is suspended and the marker is parked). The persistent
        // remembered set supplies the incoming-edge fix-up set - no dedicated STW
        // backup trace needed. Must precede the sweep (which frees dead regions).
        if (doEvac)
        {
            LXRSetPhase("me:evacuate");
            g_lxrCollector.Evacuate();
        }
        // Item F: recording window closes once Evacuate has consumed the scoped
        // remembered set (ConcurrentTraceFinish above recorded the final-drain edges).
        if (doEvac && g_evacCandidateScope) InterlockedExchange(&g_recordEvacEdges, 0);
        LARGE_INTEGER tfEvac; QueryPerformanceCounter(&tfEvac);
        LXRSetPhase("me:finish-sweep");
        if (doSweep)
            g_lxrCollector.SweepAndSelectDefrag();
        QueryPerformanceCounter(&tf3);
        if (verbose) { fprintf(stderr, "LXRGC: [stage]   finish breakdown: finish=%lldus bufs=%lldus evac=%lldus sweep=%lldus\n", (long long)((tf1.QuadPart-tf0.QuadPart)*1000000/freq.QuadPart), (long long)((tf2.QuadPart-tf1.QuadPart)*1000000/freq.QuadPart), (long long)((tfEvac.QuadPart-tf2.QuadPart)*1000000/freq.QuadPart), (long long)((tf3.QuadPart-tfEvac.QuadPart)*1000000/freq.QuadPart)); fflush(stderr); }
        // Item F: PRUNE the persistent remembered set (drop stale/duplicate
        // entries; rebuild from marks on prior overflow) instead of clearing it -
        // clearing would drop the evac copies' just-recorded memcpy out-edges that
        // the barrier will never re-log. Runs AFTER Evacuate consumed it + AFTER
        // the sweep bumped reclaimed lines' reuse versions.
        if (g_remsetActive)
            g_lxrCollector.CompactRemsets();
        InterlockedExchange(&g_traceState, TRACE_IDLE);
        LXRSetPhase("me:restart-finish");
        LXRRestartEE();
        QueryPerformanceCounter(&t1);   // TRUE pause end: mutators run from here
        pauseMicros = (int64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
        // Off-pause: replay the detached marking-window RC increments/decrements +
        // recursive zero-count free cascade while mutators run. Sound even though
        // the sweep above may have decommitted reclaimed chunks: reclaimed regions
        // have RC cleared to 0 (ClearRCRange at every decommit site) and Drain-
        // ZeroCountWorkList guards every deref with a committed-VirtualQuery check
        // (commit 80b6ffb), so a deferred decrement into a swept chunk neither
        // underflows nor faults. Single-driver serialization keeps the next
        // collection from starting until this replay completes.
        LXRSetPhase("me:finish-decrements");
        LARGE_INTEGER td0, td1;
        QueryPerformanceCounter(&td0);
        if (doBuffers)
            g_lxrCollector.ProcessSnapshotDecrements();
        QueryPerformanceCounter(&td1);
        // Off-pause: physically decommit the regions the STW sweep deferred (the
        // dominant former in-pause cost). Mutators are live; limbo regions are not
        // yet on the free list so the allocator can't hand them out mid-drain.
        g_lxrCollector.DrainPendingDecommit();
        LXRSetPhase("idle");
        phase = LXRPhase::TracePause; // account the finish epoch as a trace pause
        if (verbose) { fprintf(stderr, "LXRGC: [stage] multi-epoch trace finish (pause=%lldus offpauseDec=%lldus spans=%lld)\n",
                               (long long)pauseMicros, (long long)((td1.QuadPart-td0.QuadPart)*1000000/freq.QuadPart), (long long)g_multiEpochSpans); fflush(stderr); }
    }
    else if (useConcurrent)
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
        // Item F: arm candidate-scoped evac-remset repopulation for the concurrent
        // mark (see the multi-epoch snapshot path for rationale).
        if (doEvac && g_evacCandidateScope) { ResetEvacEdges(); InterlockedExchange(&g_recordEvacEdges, 1); }
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
        // Item F: evacuate in this concurrent-trace finish pause (STW, marks
        // complete). The persistent remembered set supplies the fix-up set.
        if (doEvac)
        {
            LXRSetPhase("conc:evacuate");
            g_lxrCollector.Evacuate();
        }
        if (doEvac && g_evacCandidateScope) InterlockedExchange(&g_recordEvacEdges, 0);
        LXRSetPhase("conc:finish-sweep");
        if (doSweep)
            g_lxrCollector.SweepAndSelectDefrag();
        // Item F: PRUNE the persistent remembered set (drop stale/duplicate
        // entries; rebuild from marks on prior overflow) rather than clearing it,
        // so the evac copies' just-recorded memcpy out-edges survive for the next
        // evacuation. Runs after Evacuate consumed it and after the sweep bumped
        // reclaimed lines' reuse versions.
        if (g_remsetActive)
            g_lxrCollector.CompactRemsets();
        LXRSetPhase("conc:restart-finish");
        LXRRestartEE();
        // Off-pause: physically decommit the regions the STW sweep deferred.
        g_lxrCollector.DrainPendingDecommit();
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
        // Multi-epoch spanned RC epoch: DRAIN the modified buffers accumulated so
        // far in the marking window (paper: RC pauses continue during a spanning
        // trace). Safe to run a full ProcessModifiedBuffers once the marker has
        // consumed the meStart snapshot (g_snapshotConsumed) - the root-deferral
        // rotation is then clean, ProcessModifiedBuffers takes m_collectLock while
        // ConcurrentTraceDrain takes none (no contention), and DrainZeroCountWork-
        // List never decommits inside the trace window (g_traceWindowOpen), so the
        // concurrent marker never races a free. Incremental draining bounds the
        // finish pause. Only in the tiny window BEFORE the snapshot is consumed do
        // we defer (skip) to avoid corrupting the outstanding snapshot's rotation.
        bool skipBuffersForSpan = g_multiEpoch && g_traceState == TRACE_MARKING && !g_snapshotConsumed;
        if (doBuffers && !skipBuffersForSpan)
        {
            LARGE_INTEGER tpb0; QueryPerformanceCounter(&tpb0);
            g_lxrCollector.ProcessModifiedBuffers();
            LARGE_INTEGER tpb1; QueryPerformanceCounter(&tpb1);
            if (verbose) { fprintf(stderr, "LXRGC: [stage] ProcessModifiedBuffers done (%lldus)\n",
                                   (long long)((tpb1.QuadPart - tpb0.QuadPart) * 1000000 / freq.QuadPart)); fflush(stderr); }
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
                // Item F (F3): keep the legacy STW-mark-derived evac edge log as an
                // A/B fallback (consumed by Evacuate only when LXR_EVAC_PERSIST=0).
                // The default path uses the PERSISTENT barrier-maintained remset and
                // ignores these logs. Reset+record inter-block edges during closure.
                if (doEvac) { ResetEvacEdges(); InterlockedExchange(&g_recordEvacEdges, 1); }
                g_lxrCollector.BackupTrace();
                if (doEvac) InterlockedExchange(&g_recordEvacEdges, 0);
                if (verbose) { fprintf(stderr, "LXRGC: [stage] BackupTrace done\n"); fflush(stderr); }
            }
            if (doEvac)
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
            // Item F: PRUNE (not clear) the persistent remembered set - preserve the
            // evac copies' recorded memcpy out-edges; drop stale/duplicate entries;
            // rebuild from marks on prior overflow. Runs after Evacuate + sweep.
            if (g_remsetActive)
                g_lxrCollector.CompactRemsets();
        }
        else
        {
            // Item D: young/nursery collection at the RC pause (paper §3.3). Young
            // objects are reference-counted from birth; after this pause's
            // increments (modbuf new-values + deferred root captures) and
            // decrements are applied, any young object still at RC 0 is implicitly
            // dead and its region is reclaimed. Guards on g_traceWindowOpen so it
            // never frees under an in-flight concurrent trace.
            if (g_youngRC && g_nurseryActive)
            {
                // Item D-copy: promote/defragment the young SURVIVORS first, so a
                // region emptied of survivors is freed here; then CollectNursery
                // mops up the regions of purely implicitly-dead young.
                LXRSetPhase("stw:nursery-copy");
                LARGE_INTEGER tnc0; QueryPerformanceCounter(&tnc0);
                g_lxrCollector.CopyYoungSurvivors();
                LXRSetPhase("stw:nursery");
                LARGE_INTEGER tnc1; QueryPerformanceCounter(&tnc1);
                g_lxrCollector.CollectNursery();
                LARGE_INTEGER tnc2; QueryPerformanceCounter(&tnc2);
                if (verbose)
                {
                    fprintf(stderr, "LXRGC: [rc-breakdown] copy=%lldus nursery=%lldus\n",
                            (long long)((tnc1.QuadPart - tnc0.QuadPart) * 1000000 / freq.QuadPart),
                            (long long)((tnc2.QuadPart - tnc1.QuadPart) * 1000000 / freq.QuadPart));
                    fflush(stderr);
                }
            }
            // Item ★: primary-RC MATURE reclamation at the RC pause (paper §3.3).
            // ProcessModifiedBuffers above has fully reconciled RC + drained the
            // zero-count free cascade, so any mature region with no RC>0 object is
            // dead and is returned NOW by RC authority -- not deferred to the
            // occasional backup trace. This makes RC the primary reclaimer.
            LXRSetPhase("stw:rc-reclaim");
            LARGE_INTEGER trr0; QueryPerformanceCounter(&trr0);
            g_lxrCollector.ReclaimMatureByRC();
            LARGE_INTEGER trr1; QueryPerformanceCounter(&trr1);
            if (verbose)
            {
                fprintf(stderr, "LXRGC: [rc-breakdown] rc-reclaim=%lldus (chunks=%zu)\n",
                        (long long)((trr1.QuadPart - trr0.QuadPart) * 1000000 / freq.QuadPart),
                        g_chunkCount);
                fflush(stderr);
            }
        }

        LXRSetPhase("stw:restart");
        if (suspended)
            LXRRestartEE();
        // Off-pause: physically decommit the regions the STW sweep deferred.
        g_lxrCollector.DrainPendingDecommit();
        LXRSetPhase("idle");
        QueryPerformanceCounter(&t1);
        pauseMicros = (int64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    }
    InterlockedExchangeAdd64(&g_lxrCounters.TotalPauseMicros, pauseMicros);
    // Pause-time telemetry (feeds GetTotalPauseDuration / GetLastGCPercentTimeInGC,
    // consumed by the dotnet.gc.pause.time meter + "% Time in GC" EventCounter, and
    // now GetGCMemoryInfo().PauseTimePercentage). Report the CUMULATIVE fraction of
    // wall-clock spent in STW pauses (TotalPauseMicros / elapsed-since-first-pause),
    // NOT the instantaneous "this pause / interval since the previous pause end"
    // ratio: under concurrent tracing the snapshot/finish/RC pauses cluster within
    // a few ms of each other, so the instantaneous ratio spikes to ~100% and wildly
    // over-reported time-in-GC (e.g. 80% on a workload actually spending <1%). The
    // cumulative figure is the honest, stable "% time in GC".
    {
        static LARGE_INTEGER s_firstPauseQpc = { 0 };
        LARGE_INTEGER nowQpc; QueryPerformanceCounter(&nowQpc);
        g_lxrCounters.LastPauseMicros = pauseMicros;
        if (pauseMicros > g_lxrCounters.MaxPauseMicros)
            g_lxrCounters.MaxPauseMicros = pauseMicros;
        if (s_firstPauseQpc.QuadPart == 0)
            s_firstPauseQpc = nowQpc;
        int64_t elapsedMicros =
            (int64_t)((nowQpc.QuadPart - s_firstPauseQpc.QuadPart) * 1000000 / freq.QuadPart);
        if (elapsedMicros > 0)
        {
            int64_t pct = g_lxrCounters.TotalPauseMicros * 100 / elapsedMicros;
            if (pct > 100) pct = 100;
            g_lxrCounters.LastGCPercentTimeInGC = pct;
        }
    }
    InterlockedIncrement64(&g_lxrCounters.Epochs);
    g_lxrCounters.LastCollectCommitted = g_committedInUse;
    // Item E: reset the increment-count trigger accumulator at every pause (both
    // RC and trace consume the logged increments), so the next pause is paced by
    // increments logged from here on.
    g_incrementBaseline = g_lxrCounters.ModifiedBufferEntries;

    // Item E survival-rate predictor (paper §3.2.2): update the YOUNG-object
    // survival EWMA from this pause's nursery outcome. Young survival % =
    // (survivor bytes copied) / (survivor bytes copied + dead young bytes
    // reclaimed) over the deltas since the last pause. Uses the paper's
    // asymmetric biased decay (¾ new on a rise, ¼ new on a fall) so the survival
    // trigger reacts fast to a survival spike (expensive pauses ahead) and decays
    // slowly, never under-provisioning. Drives the budget modulation in Alloc.
    {
        int64_t copied    = g_lxrCounters.NurseryCopyBytes    - g_youngSurvCopiedBaseline;
        int64_t reclaimed = g_lxrCounters.NurseryBytesReclaimed - g_youngSurvReclaimedBaseline;
        g_youngSurvCopiedBaseline    = g_lxrCounters.NurseryCopyBytes;
        g_youngSurvReclaimedBaseline = g_lxrCounters.NurseryBytesReclaimed;
        int64_t examined = copied + reclaimed;
        if (examined > 0)
        {
            int64_t youngSurvPct = (copied * 100) / examined;
            if (youngSurvPct > 100) youngSurvPct = 100;
            if (youngSurvPct < 0)   youngSurvPct = 0;
            int64_t prevY = g_lxrCounters.YoungSurvivalPctEwma;
            if (prevY <= 0)                     g_lxrCounters.YoungSurvivalPctEwma = youngSurvPct;
            else if (youngSurvPct > prevY)      g_lxrCounters.YoungSurvivalPctEwma = (3 * youngSurvPct + prevY) / 4;
            else                                g_lxrCounters.YoungSurvivalPctEwma = (youngSurvPct + 3 * prevY) / 4;
        }
    }

    if (phase == LXRPhase::TracePause)
    {
        InterlockedIncrement64(&g_lxrCounters.TracePauses);
        InterlockedExchangeAdd64(&g_lxrCounters.TracePausePauseMicros, pauseMicros);
    }
    else
    {
        InterlockedIncrement64(&g_lxrCounters.RCPauses);
        InterlockedExchangeAdd64(&g_lxrCounters.RCPausePauseMicros, pauseMicros);
    }
    if (traceCompleted)
    {
        // Item E predictors, both using the paper's asymmetric ("biased")
        // exponential decay (§3.2.5): react fast to a rising signal (¾ new, ¼ old)
        // and decay slowly on a falling one (¼ new, ¾ old) so the collector never
        // under-provisions. Survival % paces the trace cadence (DecidePhase epoch
        // cap); wastage % (the floating garbage each trace recovers) drives the
        // wastage-based trace trigger.
        if (committedBefore > 0)
        {
            int64_t survivalPct = (g_committedInUse * 100) / committedBefore;
            if (survivalPct > 100) survivalPct = 100;
            if (survivalPct < 0) survivalPct = 0;
            int64_t prevS = g_lxrCounters.SurvivalPctEwma;
            if (prevS <= 0)                    g_lxrCounters.SurvivalPctEwma = survivalPct;
            else if (survivalPct > prevS)      g_lxrCounters.SurvivalPctEwma = (3 * survivalPct + prevS) / 4;
            else                               g_lxrCounters.SurvivalPctEwma = (survivalPct + 3 * prevS) / 4;

            int64_t wastagePct = 100 - survivalPct; // % of the pre-trace heap this trace reclaimed (RC-uncollectable garbage)
            if (wastagePct < 0) wastagePct = 0;
            int64_t prevW = g_lxrCounters.WastagePctEwma;
            if (prevW <= 0)                    g_lxrCounters.WastagePctEwma = wastagePct;
            else if (wastagePct > prevW)       g_lxrCounters.WastagePctEwma = (3 * wastagePct + prevW) / 4;
            else                               g_lxrCounters.WastagePctEwma = (wastagePct + 3 * prevW) / 4;
        }
        g_lastTraceCommitted = g_committedInUse;
        g_epochsSinceTrace = 0;
        // #6: age the nursery. Regions born in the window just ended (their blocks
        // stamped with the pre-bump g_traceEpoch) are no longer young after this
        // trace has had the chance to mark/reclaim them, so RC resumes for them.
        InterlockedIncrement64(&g_traceEpoch);
        // Item D-copy: the trace has aged the just-ended window's young to mature,
        // so every slot in the "references to young" remembered set now points to a
        // mature object (or dead memory). Drop it: the next inter-trace window
        // rebuilds it from scratch, and clearing any overflow re-enables scoped
        // fix-up.
        if (g_dcopyCaptureModified)
        {
            DCopyRemsetClearAll();
            InterlockedExchange(&g_dcopyRemsetOverflow, 0);
        }
        // A complete trace has re-established liveness mark-authoritatively and
        // aged the just-ended window's young to mature, so any RC increment lost to
        // a rare free-list exhaustion is now moot: clear the incompleteness flag so
        // the nursery may reclaim RC=0 young again.
        InterlockedExchange(&g_youngRCIncomplete, 0);
    }
    else
    {
        InterlockedIncrement64(&g_epochsSinceTrace);
    }
    // Legacy "Collections" counter continues to count full reclaiming cycles so
    // existing runtime GC counters / reports keep reporting real collections.
    if (traceCompleted)
        InterlockedIncrement64(&g_lxrCounters.Collections);

    if (verbose) { fprintf(stderr, "LXRGC: [stage] restarted (phase=%s pause=%lldus survivalEwma=%lld%% youngSurvEwma=%lld%% wastageEwma=%lld%% satbEntries=%lld satbMarks=%lld remsetEntries=%lld)\n",
                           phase == LXRPhase::TracePause ? "trace" : "rc",
                           (long long)pauseMicros, (long long)g_lxrCounters.SurvivalPctEwma,
                           (long long)g_lxrCounters.YoungSurvivalPctEwma,
                           (long long)g_lxrCounters.WastagePctEwma,
                           (long long)g_lxrCounters.SatbEntries, (long long)g_lxrCounters.SatbMarks,
                           (long long)g_lxrCounters.RemsetEntries); fflush(stderr); }

    if (verbose) {
        size_t liveChunkBytes = 0, liveChunks = 0, freeChunks = 0;
        EnterCriticalSection(&g_chunkLock);
        for (size_t i = 0; i < g_chunkCount; i++) {
            if (g_chunks[i].Committed) { liveChunks++; liveChunkBytes += g_chunks[i].Size; }
            else freeChunks++;
        }
        LeaveCriticalSection(&g_chunkLock);
        int64_t committed = g_committedInUse;
        fprintf(stderr, "LXRGC: [committed] total=%lldMB liveChunks=%llu(%lldMB) runOverhang=%lldMB freeDecommittedChunks=%llu freeChunkStack=%llu\n",
                (long long)(committed >> 20), (unsigned long long)liveChunks,
                (long long)((int64_t)liveChunkBytes >> 20),
                (long long)((committed - (int64_t)liveChunkBytes) >> 20),
                (unsigned long long)freeChunks, (unsigned long long)g_freeChunkTop);
        fflush(stderr);
    }

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
    static int s_all = -1;
    if (s_all < 0) s_all = (getenv("LXR_AV_ALL") != nullptr) ? 1 : 0;
    bool wild = (s_any && faultAddr >= (uint8_t*)0x10000);
    if (!inHeap && !wild && !s_all)
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
    // Dump key registers so we can see which pointer was the wild -1 value and
    // what object was being dereferenced. Printed BEFORE the (slow, huge) minidump
    // so a torn-down stderr pipe never loses it.
    {
        CONTEXT* c = ep->ContextRecord;
        fprintf(stderr, "LXRGC: [AV] rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                (void*)c->Rax, (void*)c->Rbx, (void*)c->Rcx, (void*)c->Rdx, (void*)c->Rsi, (void*)c->Rdi);
        fprintf(stderr, "LXRGC: [AV] r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p rbp=%p rsp=%p\n",
                (void*)c->R8, (void*)c->R9, (void*)c->R10, (void*)c->R11, (void*)c->R12,
                (void*)c->R13, (void*)c->R14, (void*)c->R15, (void*)c->Rbp, (void*)c->Rsp);
        fflush(stderr);
    }

    // Walk the faulting thread's stack from the exception CONTEXT (copy it -
    // StackWalk64 mutates the CONTEXT it is given). Printed BEFORE the minidump so
    // it is never lost, flushing per frame, guarded so a walk fault cannot recurse.
    __try
    {
        CONTEXT ctxw = *ep->ContextRecord;
        STACKFRAME64 sf; memset(&sf, 0, sizeof(sf));
        sf.AddrPC.Offset = ctxw.Rip;    sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctxw.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctxw.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        HANDLE thw = GetCurrentThread();
        for (int frame = 0; frame < 50; frame++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thw, &sf, &ctxw,
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
                fprintf(stderr, "LXRGC: [AV]     %-16s 0x%llx (base+0x%llx)\n", modname,
                        (unsigned long long)sf.AddrPC.Offset,
                        (unsigned long long)(modbase ? sf.AddrPC.Offset - modbase : 0));
            fflush(stderr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    fprintf(stderr, "LXRGC: [AV] ===== end native stack =====\n");
    fflush(stderr);

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

    // (legacy duplicate stack walk removed; the guarded pre-dump walk above is
    // authoritative.)
    if (false)
    {
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
    } // end if(false) legacy walk
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
            // Dump every thread's native stack so we can see which thread is
            // failing to make progress (a mutator not reaching a safepoint, or a
            // lock cycle). Previously gated to "suspend" phases only; broadened so
            // the rare early-startup EventPipe hang is captured whatever phase the
            // collector is parked in.
            LXRDumpAllThreadStacks();
        }
    }
}

// Body of the dedicated, non-suspendable GC thread: wait for a request, run one
// full collection, then publish completion so synchronous waiters (GC.Collect)
// can observe it.
static void LXRCollectorThreadProc(void*)
{
    // Announce readiness: we are alive and about to park on the request event, so
    // SuspendEE will now run on THIS non-suspendable thread (never a random
    // cooperative mutator). Triggers gate on this flag (see RequestLXRCollection).
    InterlockedExchange(&g_collectorReady, 1);
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
    // Startup-safety gate: until the dedicated collector thread has announced it is
    // parked on the request event, NO collection may run. The only alternative is
    // to drive SuspendEE synchronously from the calling thread -- which, when that
    // caller is a cooperative-mode mutator (e.g. the EventPipe/dotnet-counters
    // poll thread allocating during NativeRuntimeEventSource init), is itself a
    // suspension target and deadlocks (the intermittent early-startup hang). It is
    // always safe to DROP an early trigger: the heap is freshly reserved and the
    // budget/increment trigger will simply fire again once the collector is up.
    if (!g_collectorReady || g_collectRequestEvent == nullptr)
    {
        if (!wait)
            return;                              // fire-and-forget: just skip this one
        // wait==true (explicit GC.Collect): give the collector a bounded moment to
        // come up rather than hanging forever; if it never does, drop the request.
        for (int spins = 0; spins < 200 && (!g_collectorReady || g_collectRequestEvent == nullptr); ++spins)
            Sleep(5);
        if (!g_collectorReady || g_collectRequestEvent == nullptr)
            return;
    }

    if (forceTrace)
        InterlockedExchange(&g_requestTrace, 1);

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
            "LXRGC: GC(gen=%d) -> RC inc=%lld dec=%lld, backupTraces=%lld, collections=%lld, reclaimed this GC=%lld bytes (total=%lld) rcApply[par=%lld ser=%lld]\n",
            generation,
            (long long)g_lxrCounters.RCIncrements,
            (long long)g_lxrCounters.RCDecrements,
            (long long)g_lxrCounters.BackupTraces,
            (long long)g_lxrCounters.Collections,
            (long long)reclaimed,
            (long long)g_lxrCollector.ReclaimedBytes(),
            (long long)g_parRCApplies,
            (long long)g_serRCApplies);
    fflush(stderr);
    return S_OK;
}

unsigned LXRGCHeap::GetMaxGeneration() { return 2; }
void LXRGCHeap::SetFinalizationRun(Object* obj) { }
bool LXRGCHeap::RegisterForFinalization(int gen, Object* obj) { return true; }
int LXRGCHeap::GetLastGCPercentTimeInGC() { return (int)g_lxrCounters.LastGCPercentTimeInGC; }
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

// TimeSpan ticks are 100ns; TotalPauseMicros is in microseconds -> x10.
int64_t LXRGCHeap::GetTotalPauseDuration() { return g_lxrCounters.TotalPauseMicros * 10; }

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
