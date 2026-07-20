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
    InitializeCriticalSection(&g_chunkLock);
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
void LXRCollector::LogModifiedField(Object** slot, Object* oldValue)
{
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
static void LXRWriteBarrierCallback(Object** slot, Object* /*newValue*/, Object* oldValue)
{
    g_lxrCollector.LogModifiedField(slot, oldValue);
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
int LXRGCHeap::CollectionCount(int generation, int get_bgc_fgc_coutn) { return 0; }
int LXRGCHeap::StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC) { return start_no_gc_success; }
int LXRGCHeap::EndNoGCRegion() { return end_no_gc_success; }

size_t LXRGCHeap::GetTotalBytesInUse()
{
    int64_t v = g_committedInUse;
    return v > 0 ? (size_t)v : 0;
}
uint64_t LXRGCHeap::GetTotalAllocatedBytes() { return (uint64_t)g_lxrCounters.TotalAllocatedBytes; }

HRESULT LXRGCHeap::GarbageCollect(int generation, bool low_memory_p, int mode)
{
    InterlockedIncrement64(&g_lxrCounters.InducedCollectRequests);
    // An explicit GC.Collect() runs one LXR cycle: replay the coalescing-RC
    // modified buffers, then (for a full collection) a stop-the-world backup
    // trace + Immix sweep that actually reclaims memory. The world is suspended
    // for the whole cycle so root scanning and the linear heap sweep observe a
    // quiescent, parseable heap.
    bool fullGC = (generation < 0 || generation >= (int)GetMaxGeneration());
    int64_t reclaimedBefore = g_lxrCollector.ReclaimedBytes();

    fprintf(stderr,
            "LXRGC: GarbageCollect(gen=%d) captured field-log entries this epoch = %lld\n",
            generation,
            (long long)g_lxrCounters.ModifiedBufferEntries);

    bool suspended = false;
    if (g_theGCToCLR != nullptr)
    {
        g_theGCToCLR->SuspendEE(SUSPEND_FOR_GC);
        suspended = true;
    }
    bool verbose = getenv("LXR_VERBOSE") != nullptr;
    if (verbose) { fprintf(stderr, "LXRGC: [stage] suspended=%d\n", (int)suspended); fflush(stderr); }

    bool doBuffers = getenv("LXR_NO_BUFFERS") == nullptr;
    bool doTrace   = getenv("LXR_NO_TRACE")   == nullptr;
    bool doSweep   = getenv("LXR_NO_SWEEP")   == nullptr;

    if (doBuffers)
    {
        g_lxrCollector.ProcessModifiedBuffers();
        if (verbose) { fprintf(stderr, "LXRGC: [stage] ProcessModifiedBuffers done\n"); fflush(stderr); }
    }
    if (fullGC)
    {
        if (doTrace)
        {
            g_lxrCollector.BackupTrace();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] BackupTrace done\n"); fflush(stderr); }
        }
        if (doSweep)
        {
            g_lxrCollector.SweepAndSelectDefrag();
            if (verbose) { fprintf(stderr, "LXRGC: [stage] SweepAndSelectDefrag done\n"); fflush(stderr); }
        }
    }

    if (suspended)
        g_theGCToCLR->RestartEE(true);
    if (verbose) { fprintf(stderr, "LXRGC: [stage] restarted\n"); fflush(stderr); }

    int64_t reclaimedNow = g_lxrCollector.ReclaimedBytes();
    fprintf(stderr,
            "LXRGC: after cycle -> RC inc=%lld dec=%lld, backupTraces=%lld, reclaimed this GC=%lld bytes (total reclaimed=%lld)\n",
            (long long)g_lxrCounters.RCIncrements,
            (long long)g_lxrCounters.RCDecrements,
            (long long)g_lxrCounters.BackupTraces,
            (long long)(reclaimedNow - reclaimedBefore),
            (long long)reclaimedNow);
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
