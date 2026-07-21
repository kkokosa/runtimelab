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
static ModifiedBuffer* g_registeredBuffers = nullptr;
static CRITICAL_SECTION g_buffersLock;

// --- SATB (snapshot-at-the-beginning) deletion buffers (P2) ----------------
// While a trace window is open (g_satbActive), the field-logging barrier
// appends every overwritten referent here. A trace consumes them via
// DrainSatbBuffers so a concurrent marker (P4) cannot miss an object that a
// mutator unlinks mid-trace. Persist across RC epochs (unlike the RC modified
// buffer) until a trace drains them.
struct SatbBuffer
{
    static const size_t kCapacity = 4096;
    Object* Entries[kCapacity];
    size_t Count = 0;
    SatbBuffer* NextRegistered = nullptr;
};
static thread_local SatbBuffer* t_satbBuffer = nullptr;
static SatbBuffer* g_registeredSatbBuffers = nullptr;
static CRITICAL_SECTION g_satbLock;
static volatile LONG g_satbActive = 0; // logging gate: open while a trace window is live

// --- Remembered sets: inter-block pointer slots (P2) -----------------------
// While enabled (g_remsetActive), the barrier records slots that come to hold a
// pointer into a different Immix block than the slot's own block. Evacuation
// (P3) walks these to fix up references into a moved block. Entries persist
// (a remembered set must retain a slot until its target region is collected);
// stale/duplicate entries are filtered at evacuation by re-reading the slot.
struct RemsetBuffer
{
    static const size_t kCapacity = 4096;
    Object** Entries[kCapacity];
    size_t Count = 0;
    RemsetBuffer* NextRegistered = nullptr;
};
static thread_local RemsetBuffer* t_remsetBuffer = nullptr;
static RemsetBuffer* g_registeredRemsetBuffers = nullptr;
static CRITICAL_SECTION g_remsetLock;
static volatile LONG g_remsetActive = 0; // logging gate: open while evacuation is enabled
static volatile LONG g_evacActive = 0;   // STW evacuation gate (P3)

// A tiny zero-count work list used by recursive decrements. In a full LXR
// this is a bounded work packet processed incrementally; here it is a simple
// growable stack (the engine is dormant, so simplicity beats scalability).
static Object** g_zeroCountStack = nullptr;
static size_t g_zeroCountTop = 0;
static size_t g_zeroCountCap = 0;

// Backup-trace mark stack (grown on demand; STW so no locking needed while draining).
static Object** g_markStack = nullptr;
static size_t g_markTop = 0;
static size_t g_markCap = 0;

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

// Dedicated collector thread. Driving SuspendEE from a random cooperative-mode
// allocating thread deadlocks under high concurrency (the initiator can end up
// waiting on threads that are in turn waiting on it). Real LXR runs its trace on
// its own GC threads; we do the same: a single non-suspendable GC thread owns
// every stop-the-world cycle, and allocation/GC.Collect just post a request to
// it. Because the collector thread is created with is_suspendable=false it is
// never itself a suspension target, so SuspendEE from it is deadlock-free.
static HANDLE g_collectRequestEvent = nullptr; // auto-reset: wake the collector
static HANDLE g_collectDoneEvent = nullptr;    // auto-reset: pulsed after each cycle
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
};
static ChunkRegion* g_chunks = nullptr;
static size_t g_chunkCount = 0;
static size_t g_chunkCap = 0;
static size_t* g_freeChunks = nullptr;   // stack of reclaimed (decommitted) chunk indices
static size_t g_freeChunkTop = 0;
static size_t g_freeChunkCap = 0;
static CRITICAL_SECTION g_chunkLock;

// Total object size in bytes, matching the runtime's Align(base + comps*compSize).
size_t LXRObjectSize(Object* o)
{
    MethodTable* mt = o->GetGCSafeMethodTable();
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
    LeaveCriticalSection(&g_chunkLock);
    return idx;
}

// Reuse a previously reclaimed region (block reuse); returns its (zeroed,
// recommitted) start and sets *outIndex, or nullptr if the free list is empty.
static uint8_t* ReuseChunk(gc_alloc_context* owner, int* outIndex)
{
    uint8_t* start = nullptr;
    EnterCriticalSection(&g_chunkLock);
    while (g_freeChunkTop > 0)
    {
        size_t idx = g_freeChunks[--g_freeChunkTop];
        if (idx >= g_chunkCount || g_chunks[idx].Committed)
            continue;
        ChunkRegion& c = g_chunks[idx];
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
    return start;
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

    // Per-block metadata: one entry per 32 KiB block. Small enough to commit.
    m_blockCount = heapReservedBytes / lxr::kBlockSize;
    size_t metaBytes = m_blockCount * sizeof(lxr::BlockMeta);
    m_blockMeta = (lxr::BlockMeta*)VirtualAlloc(nullptr, metaBytes, MEM_RESERVE, PAGE_READWRITE);
    if (m_blockMeta == nullptr)
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

uint8_t* LXRCollector::RCSlot(Object* obj) const
{
    size_t idx = ((uint8_t*)obj - m_heapBase) / lxr::kObjectGranule;
    return &m_rcTable[idx];
}

void LXRCollector::RCIncrement(Object* obj)
{
    if ((uint8_t*)obj < m_heapBase || (uint8_t*)obj >= m_heapBase + m_heapBytes)
        return; // not our heap (frozen segment, boot object, etc.)
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
    uint8_t* slot = RCSlot(obj);
    CommitPageFor(slot);
    InterlockedIncrement64(&g_lxrCounters.RCDecrements);
    if (*slot == 0 || *slot == 0xFF)
        return false; // already zero, or stuck-high (resolved by backup trace)
    (*slot)--;
    return (*slot == 0);
}

// LXR write-barrier slow path. Reached from the runtime's generic Callback
// write barrier (WriteBarrierKind::Callback) on every in-heap reference-field
// store, carrying the OLD value the runtime just overwrote.
void LXRCollector::LogModifiedField(Object** slot, Object* oldValue, Object* newValue)
{
    // (1) Coalescing-RC modified buffer: record (slot, oldValue) on mutation.
    ModifiedBuffer* buf = t_modifiedBuffer;
    if (buf == nullptr)
    {
        buf = new (nothrow) ModifiedBuffer();
        if (buf == nullptr)
            return;
        t_modifiedBuffer = buf;
        EnterCriticalSection(&g_buffersLock);
        buf->NextRegistered = g_registeredBuffers;
        g_registeredBuffers = buf;
        LeaveCriticalSection(&g_buffersLock);
    }
    if (buf->Count < ModifiedBuffer::kCapacity)
    {
        buf->Entries[buf->Count].Slot = slot;
        buf->Entries[buf->Count].OldValue = oldValue;
        buf->Count++;
        InterlockedIncrement64(&g_lxrCounters.ModifiedBufferEntries);
    }
    // A production LXR flushes a full buffer into a shared queue; omitted here.
    // Once full we simply stop recording further entries this epoch.

    // (2) SATB deletion barrier (Yuasa): while a trace window is open, retain the
    //     overwritten referent so a concurrent marker (P4) cannot miss an object
    //     unlinked mid-trace. Over-retention for one cycle is always safe.
    if (g_satbActive && oldValue != nullptr && InHeap(oldValue))
    {
        SatbBuffer* sb = t_satbBuffer;
        if (sb == nullptr)
        {
            sb = new (nothrow) SatbBuffer();
            if (sb != nullptr)
            {
                t_satbBuffer = sb;
                EnterCriticalSection(&g_satbLock);
                sb->NextRegistered = g_registeredSatbBuffers;
                g_registeredSatbBuffers = sb;
                LeaveCriticalSection(&g_satbLock);
            }
        }
        if (sb != nullptr && sb->Count < SatbBuffer::kCapacity)
        {
            sb->Entries[sb->Count++] = oldValue;
            InterlockedIncrement64(&g_lxrCounters.SatbEntries);
        }
    }

    // (3) Remembered set: record slots that now hold an inter-block pointer, so
    //     evacuation (P3) can locate and rewrite references into a moved block.
    if (g_remsetActive && newValue != nullptr && InHeap(newValue) && InHeap((Object*)slot))
    {
        uintptr_t sblk = (uintptr_t)slot     & ~(lxr::kBlockSize - 1);
        uintptr_t tblk = (uintptr_t)newValue & ~(lxr::kBlockSize - 1);
        if (sblk != tblk)
        {
            RemsetBuffer* rb = t_remsetBuffer;
            if (rb == nullptr)
            {
                rb = new (nothrow) RemsetBuffer();
                if (rb != nullptr)
                {
                    t_remsetBuffer = rb;
                    EnterCriticalSection(&g_remsetLock);
                    rb->NextRegistered = g_registeredRemsetBuffers;
                    g_registeredRemsetBuffers = rb;
                    LeaveCriticalSection(&g_remsetLock);
                }
            }
            if (rb != nullptr && rb->Count < RemsetBuffer::kCapacity)
            {
                rb->Entries[rb->Count++] = slot;
                InterlockedIncrement64(&g_lxrCounters.RemsetEntries);
            }
        }
    }
}

void LXRCollector::SetSatbActive(bool active) { InterlockedExchange(&g_satbActive, active ? 1 : 0); }
bool LXRCollector::IsSatbActive() const { return g_satbActive != 0; }

// Mark every SATB-logged referent (over-retention within a cycle is safe) and
// empty the buffers. Called under STW today; drivable from a concurrent marker
// in P4. Assumes the mark stack is being (or will be) drained by the caller.
void LXRCollector::DrainSatbBuffers()
{
    EnterCriticalSection(&g_satbLock);
    for (SatbBuffer* sb = g_registeredSatbBuffers; sb != nullptr; sb = sb->NextRegistered)
    {
        for (size_t i = 0; i < sb->Count; i++)
        {
            Object* o = sb->Entries[i];
            if (o != nullptr)
            {
                PushMark(o);
                InterlockedIncrement64(&g_lxrCounters.SatbMarks);
            }
        }
        sb->Count = 0;
    }
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

void LXRCollector::ResetRemsets()
{
    EnterCriticalSection(&g_remsetLock);
    for (RemsetBuffer* rb = g_registeredRemsetBuffers; rb != nullptr; rb = rb->NextRegistered)
        rb->Count = 0;
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
    while (g_zeroCountTop > 0)
    {
        Object* dead = g_zeroCountStack[--g_zeroCountTop];
        uint8_t* blk = (uint8_t*)((uintptr_t)dead & ~(lxr::kBlockSize - 1));
        lxr::BlockMeta* meta = MetaForBlock(blk);
        if (meta != nullptr && meta->liveObjects > 0)
            meta->liveObjects--;

        // Recursive decrement: dropping 'dead' releases one reference from each
        // object it points at. Any referent that hits zero cascades.
        GCScanObjectRefs(dead, LXRObjectSize(dead), [this](Object** ref)
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
        return false; // beyond the reset range (above heap high-water) -> nothing to mark
    uint8_t* byte = &m_markTable[byteIdx];
    uint8_t bit = (uint8_t)(1u << (granule & 7));
    if (*byte & bit)
        return false; // already marked
    *byte |= bit;
    return true;
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
            return; // best-effort; a dropped push only risks over-retention via re-scan
        g_markStack = grown;
        g_markCap = newCap;
    }
    g_markStack[g_markTop++] = obj;
}

void LXRCollector::DrainMarkStack()
{
    while (g_markTop > 0)
    {
        Object* o = g_markStack[--g_markTop];
        GCScanObjectRefs(o, LXRObjectSize(o), [this](Object** ref)
        {
            PushMark(*ref);
        });
    }
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
        if (interior >= c.Start && interior < usedEnd)
        {
            region = c;
            region.UsedEnd = usedEnd;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_chunkLock);
    if (!found)
        return nullptr;

    uint8_t* p = region.Start;
    while (p < region.UsedEnd)
    {
        Object* o = (Object*)p;
        size_t sz = LXRObjectSize(o);
        if (sz == 0)
            break;
        if (interior >= p && interior < p + sz)
            return o;
        p += sz;
    }
    return nullptr;
}

void LXRCollector::ProcessModifiedBuffers()
{
    // Coalescing reference counting (Levanoni-Petrank): for every logged
    // (slot, oldValue), increment the NEW referent currently in the slot and
    // decrement the OLD referent that was there when first logged. Objects
    // whose count hits zero are queued for recursive freeing.
    EnterCriticalSection(&m_collectLock);
    EnterCriticalSection(&g_buffersLock);
    for (ModifiedBuffer* buf = g_registeredBuffers; buf != nullptr; buf = buf->NextRegistered)
    {
        for (size_t i = 0; i < buf->Count; i++)
        {
            Object* newValue = *(buf->Entries[i].Slot);
            Object* oldValue = buf->Entries[i].OldValue;
            if (newValue != nullptr)
                RCIncrement(newValue);
            if (oldValue != nullptr && RCDecrement(oldValue))
                EnqueueZeroCount(oldValue);
        }
        buf->Count = 0; // epoch consumed
    }
    LeaveCriticalSection(&g_buffersLock);
    DrainZeroCountWorkList();
    LeaveCriticalSection(&m_collectLock);
}

// Root callback for handle scanning: mark the referent of a live handle.
static void LXRMarkHandleRef(Object** ref, void* /*ctx*/)
{
    g_lxrCollector.PushMark(*ref);
}

// promote_func for GcScanRoots: mark a stack/static/finalizer root. Interior
// pointers are resolved to their containing object so nothing reachable is
// missed; unresolvable interior roots are ignored (their region stays live via
// any precise root, or is simply not reclaimed this cycle).
static void LXRPromoteRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        o = g_lxrCollector.ResolveInterior((uint8_t*)o);
        if (o == nullptr)
            return;
    }
    g_lxrCollector.PushMark(o);
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
    g_theGCToCLR->GcScanRoots(&LXRPromoteRoot, maxgen, maxgen, &sc);

    // 2b. SATB deletion set: while a trace window is open, referents unlinked by
    //     mutators since the snapshot must be kept live for this trace (Yuasa).
    //     Under STW this is a no-op unless LXR_SATB exercises it; P4 drives it
    //     from a concurrent marker.
    if (g_satbActive)
        DrainSatbBuffers();

    // 3. Transitive closure over reachable objects.
    DrainMarkStack();
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
    for (size_t i = 0; i < g_chunkCount; i++)
    {
        ChunkRegion& c = g_chunks[i];
        if (!c.Committed || c.Owner != nullptr)
            continue; // uncommitted, or an active (still-allocating) region

        bool anyLive = false;
        uint8_t* p = c.Start;
        while (p < c.UsedEnd)
        {
            Object* o = (Object*)p;
            size_t sz = LXRObjectSize(o);
            if (sz == 0) { anyLive = true; break; } // parse failure -> keep it
            if (IsMarked(o)) { anyLive = true; break; }
            p += sz;
        }
        if (anyLive)
            continue;

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
static void LXRPinRoot(PTR_PTR_Object ppObj, ScanContext* /*sc*/, uint32_t flags)
{
    Object* o = *ppObj;
    if (o == nullptr)
        return;
    if (flags & GC_CALL_INTERIOR)
    {
        o = g_lxrCollector.ResolveInterior((uint8_t*)o);
        if (o == nullptr)
            return;
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
    g_evacPinned = &pinned;
    ScanContext sc; sc.promotion = true;
    g_theGCToCLR->GcScanRoots(&LXRPinRoot, 2, 2, &sc);
    g_evacPinned = nullptr;
    LXRGCHandleStore::ForEachLiveHandle(&LXRPinHandle, &pinned);

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
            GCScanObjectRefs(o, sz, [&forwarding](Object** f)
            {
                auto it = forwarding.find(*f);
                if (it != forwarding.end())
                {
                    *f = it->second;
                    InterlockedIncrement64(&g_lxrCounters.EvacFieldsForwarded);
                }
            });
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
    QueryPerformanceCounter(&t0);

    bool suspended = false;
    if (g_theGCToCLR != nullptr)
    {
        g_theGCToCLR->SuspendEE(SUSPEND_FOR_GC);
        suspended = true;
    }
    bool verbose = getenv("LXR_VERBOSE") != nullptr;
    if (verbose) { fprintf(stderr, "LXRGC: [stage] phase=%s suspended=%d\n",
                           phase == LXRPhase::TracePause ? "trace" : "rc", (int)suspended); fflush(stderr); }

    bool doBuffers = getenv("LXR_NO_BUFFERS") == nullptr;
    bool doTrace   = getenv("LXR_NO_TRACE")   == nullptr;
    bool doSweep   = getenv("LXR_NO_SWEEP")   == nullptr;

    if (doBuffers)
    {
        g_lxrCollector.ProcessModifiedBuffers();
        if (verbose) { fprintf(stderr, "LXRGC: [stage] ProcessModifiedBuffers done\n"); fflush(stderr); }
    }
    if (phase == LXRPhase::TracePause)
    {
        if (doTrace)
        {
            g_lxrCollector.BackupTrace();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] BackupTrace done\n"); fflush(stderr); }
        }
        if (g_evacActive)
        {
            g_lxrCollector.Evacuate();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] Evacuate done\n"); fflush(stderr); }
        }
        if (doSweep)
        {
            g_lxrCollector.SweepAndSelectDefrag();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] SweepAndSelectDefrag done\n"); fflush(stderr); }
        }
    }

    if (suspended)
        g_theGCToCLR->RestartEE(true);
    QueryPerformanceCounter(&t1);
    int64_t pauseMicros = (int64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
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
bool LXRGCHeap::IsGCInProgressHelper(bool bConsiderGCStart) { return false; }
unsigned LXRGCHeap::GetGcCount() { return 0; }
bool LXRGCHeap::IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number) { return true; }
bool LXRGCHeap::IsEphemeral(Object* object) { return true; }
uint32_t LXRGCHeap::WaitUntilGCComplete(bool bConsiderGCStart) { return 0; }
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
