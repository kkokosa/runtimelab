// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// LXRGC.h
//
// An experimental attempt to implement the LXR garbage collector
// ("Low-Latency, High-Throughput Garbage Collection", Zhao, Blackburn &
// McKinley, PLDI 2022 - https://arxiv.org/abs/2210.17175) as a *standalone*
// CoreCLR GC, i.e. WITHOUT modifying the runtime, loaded via
// DOTNET_GCName=LXRGC.dll.
//
// It is a sibling experiment to ZeroGC
// (https://github.com/kkokosa/runtimelab/tree/feature/ZeroGC) and reuses the
// same proven standalone-GC ABI scaffolding (allocation contexts, handle
// table, card-table wiring) as its starting point.
//
// LXR's design in one paragraph
// -----------------------------
// LXR reclaims memory primarily with *reference counting* (RC), backed by an
// Immix block/line heap so that RC can free whole lines/blocks cheaply and so
// that the collector can *evacuate* (copy) to defragment. Two things make
// LXR's RC affordable and correct:
//
//   1. A *field-logging write barrier* (a.k.a. coalescing RC barrier, after
//      Levanoni & Petrank). On the FIRST mutation of an object's reference
//      field within an epoch, the barrier records BOTH the slot and the OLD
//      reference value it is about to overwrite into a per-mutator
//      "modified"/decrement buffer. At GC time LXR replays that buffer:
//      decrement the old referents, increment the new referents. Without the
//      OLD value there is no way to know what to decrement.
//
//   2. A periodic *backup trace* (a mark-sweep style cycle collection) that
//      reclaims dead cycles which pure RC can never collect.
//
// This header lays out the data structures LXR needs. See FEASIBILITY.md for
// the punchline: item (1) is exactly what the CoreCLR standalone-GC ABI
// cannot express, because the JIT-emitted write barrier is hard-wired to
// card marking and never surfaces the old field value to the GC.
//
#ifndef __LXRGC_H__
#define __LXRGC_H__

#ifndef BUILD_AS_STANDALONE
#define BUILD_AS_STANDALONE
#endif
#ifndef FEATURE_STANDALONE_GC
#define FEATURE_STANDALONE_GC
#endif

#include <stdint.h>
#include <stddef.h>
#include <windows.h>

#include "gcenv.structs.h"
#include "gcenv.base.h"
#include "gcenv.os.h"
#include "gcenv.interlocked.h"
#include "gcenv.interlocked.inl"
#include "gcenv.object.h"
#include "gcenv.sync.h"
#include "gcenv.ee.h"
#include "gcinterface.h"

// gcdesc.h (pulled in by gcobjscan.h) uses _ASSERTE -> ASSERT, which the GC host
// is expected to supply. LXRGC has no debug-assert facility, so define a no-op.
#ifndef ASSERT
#define ASSERT(expr) ((void)0)
#endif
#include "gcobjscan.h"   // generic runtime object-scanning facility (dotnet/runtime #12809)

// Total size in bytes of a managed object, matching the runtime's own formula
// (base size + component count * component size, aligned up to the pointer
// granule). Used both as the 'size' argument to GCScanObjectRefs and to walk a
// bump-allocated region object-by-object during the backup trace / sweep.
size_t LXRObjectSize(Object* o);

// Forwarded from the runtime; set once in GC_Initialize.
extern IGCToCLR* g_theGCToCLR;
extern VersionInfo g_runtimeSupportedVersion;
extern bool g_oldMethodTableFlags;

// ===========================================================================
//                        LXR heap geometry (Immix)
// ===========================================================================
// These match the sizes used by the LXR / Immix papers. A block is the unit
// of reclamation and evacuation; a line is the unit of marking and recycling.
namespace lxr
{
    static const size_t kBlockSize = 32 * 1024;   // 32 KiB Immix block
    static const size_t kLineSize = 256;          // 256 B Immix line
    static const size_t kLinesPerBlock = kBlockSize / kLineSize; // 128
    static const size_t kObjectGranule = 8;       // RC side-table granularity

    // Per-block allocation/collection state.
    enum class BlockState : uint8_t
    {
        Free = 0,        // fully empty, on the free list
        Recyclable = 1,  // partially free lines available for bump allocation
        Unavailable = 2, // no free lines; skipped by the allocator
        Evacuating = 3   // selected as a defragmentation source this cycle
    };

    // Metadata kept per 32 KiB block. Stored in a side array indexed by
    // (address - heapBase) / kBlockSize, never inline in the block itself.
    struct BlockMeta
    {
        BlockState state;
        uint8_t    liveLines;   // number of marked lines (0..128)
        uint16_t   liveObjects; // objects with RC > 0 in this block
        uint32_t   deadBytes;   // approx. reclaimable bytes (defrag heuristic)
    };
}

// ===========================================================================
//                       Global counters / observability
// ===========================================================================
struct LXRCounters
{
    volatile int64_t TotalAllocatedBytes;
    volatile int64_t BlocksAllocated;
    volatile int64_t LiveHandleCount;
    volatile int64_t PeakHandleCount;
    volatile int64_t InducedCollectRequests;

    // LXR RC engine counters (see LXRCollector). Non-zero only if/when the RC
    // engine can actually be driven - see FEASIBILITY.md.
    volatile int64_t RCIncrements;
    volatile int64_t RCDecrements;
    volatile int64_t ModifiedBufferEntries; // field-log entries observed
    volatile int64_t BackupTraces;          // cycle-collection passes run
};
extern LXRCounters g_lxrCounters;

// ===========================================================================
//              LXR reference-counting engine + Immix substrate
// ===========================================================================
// This object owns the RC side table, the per-block metadata array and the
// mutator log buffers, and implements the RC increment/decrement,
// coalescing-replay and backup-trace algorithms. It is fully written out so
// that the *only* missing ingredient is visible: something has to feed
// LogModifiedField() with (slot, oldValue) pairs from mutator writes.
class LXRCollector
{
public:
    bool Initialize(uint8_t* heapBase, size_t heapReservedBytes);

    // --- RC side table (1 byte of saturating count per 8-byte granule) ---
    uint8_t* RCSlot(Object* obj) const;
    void RCIncrement(Object* obj);
    bool RCDecrement(Object* obj); // returns true if the count reached zero

    // --- Coalescing / field-logging write barrier support ---
    //
    // LogModifiedField is the LXR write-barrier slow path. It MUST be called
    // by mutator reference-field writes, BEFORE the slot is overwritten, with
    // the value currently in the slot (the "old" value). It records the pair
    // into the calling thread's modified buffer for replay at GC time.
    //
    // *** Under the CoreCLR standalone-GC ABI there is no way to make the
    //     runtime call this on ordinary field writes. See FEASIBILITY.md. ***
    void LogModifiedField(Object** slot, Object* oldValue);

    // Replays every mutator's modified buffer: increment new referents,
    // decrement old referents, then process the resulting zero-count work
    // list (recursive decrements). This is coalescing RC.
    void ProcessModifiedBuffers();

    // Periodic mark-sweep over the whole heap to reclaim dead cycles that RC
    // leaks. Uses IGCToCLR root/stack enumeration (which the ABI *does*
    // expose) plus per-object GCDesc traversal.
    void BackupTrace();

    // Reclaim fully-dead lines/blocks after RC/trace, and pick evacuation
    // candidates for the next cycle (Immix defragmentation).
    void SweepAndSelectDefrag();

    lxr::BlockMeta* MetaForBlock(uint8_t* blockAddr);

    // --- Backup trace (stop-the-world mark) + Immix reclamation ---
    //
    // BackupTrace marks every object reachable from the roots (stacks, statics,
    // handles) transitively via GCScanObjectRefs, then SweepAndSelectDefrag
    // reclaims (decommits) any allocation region that ends up with no marked
    // object. This is the correctness backstop of LXR (it also collects the
    // cycles pure RC leaks) and the mechanism that actually returns memory.
    bool MarkObject(Object* obj);       // sets the mark bit; true if newly marked
    bool IsMarked(Object* obj) const;
    void ResetMarks();                  // decommits the mark side-table (all bits -> 0)
    void PushMark(Object* obj);         // MarkObject + push onto the mark stack
    void DrainMarkStack();              // transitive closure via GCScanObjectRefs
    Object* ResolveInterior(uint8_t* interior); // interior pointer -> containing object

    int64_t ReclaimedBytes() const { return m_reclaimedBytes; }

private:
    void EnqueueZeroCount(Object* obj);
    void DrainZeroCountWorkList();

    bool InHeap(Object* obj) const
    {
        return (uint8_t*)obj >= m_heapBase && (uint8_t*)obj < m_heapBase + m_heapBytes;
    }

    uint8_t*        m_heapBase = nullptr;
    size_t          m_heapBytes = 0;
    uint8_t*        m_rcTable = nullptr;     // 1 byte / 8 heap bytes
    uint8_t*        m_markTable = nullptr;   // 1 bit / 8 heap bytes (backup-trace marks)
    size_t          m_markCommittedBytes = 0; // committed+zeroed mark-table prefix (bytes)
    lxr::BlockMeta* m_blockMeta = nullptr;   // 1 entry / 32 KiB block
    size_t          m_blockCount = 0;
    volatile int64_t m_reclaimedBytes = 0;   // cumulative bytes decommitted by sweeps
    CRITICAL_SECTION m_collectLock{};
};
extern LXRCollector g_lxrCollector;

// ===========================================================================
//                                LXRGCHeap
// ===========================================================================
// IGCHeap implementation. Allocation is a real Immix-style per-thread block
// bump allocator (blocks carved from one large reservation). The RC/collection
// machinery above is present but dormant, because it cannot be driven without
// the field-logging barrier (FEASIBILITY.md).
class LXRGCHeap : public IGCHeap
{
public:
    static LXRGCHeap* CreateAndInitialize();

    // Hosting
    bool IsValidSegmentSize(size_t size) override;
    bool IsValidGen0MaxSize(size_t size) override;
    size_t GetValidSegmentSize(bool large_seg = false) override;
    void SetReservedVMLimit(size_t vmlimit) override;

    // Concurrent GC
    void WaitUntilConcurrentGCComplete() override;
    bool IsConcurrentGCInProgress() override;
    void TemporaryEnableConcurrentGC() override;
    void TemporaryDisableConcurrentGC() override;
    bool IsConcurrentGCEnabled() override;
    HRESULT WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout) override;

    // Finalization
    size_t GetNumberOfFinalizable() override;
    Object* GetNextFinalizable() override;

    // BCL routines
    void GetMemoryInfo(uint64_t* highMemLoadThresholdBytes,
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
                       int kind) override;
    uint32_t GetMemoryLoad() override;
    int GetGcLatencyMode() override;
    int SetGcLatencyMode(int newLatencyMode) override;
    int GetLOHCompactionMode() override;
    void SetLOHCompactionMode(int newLOHCompactionMode) override;
    bool RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage) override;
    bool CancelFullGCNotification() override;
    int WaitForFullGCApproach(int millisecondsTimeout) override;
    int WaitForFullGCComplete(int millisecondsTimeout) override;
    unsigned WhichGeneration(Object* obj) override;
    int CollectionCount(int generation, int get_bgc_fgc_coutn = 0) override;
    int StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC) override;
    int EndNoGCRegion() override;
    size_t GetTotalBytesInUse() override;
    uint64_t GetTotalAllocatedBytes() override;
    HRESULT GarbageCollect(int generation = -1, bool low_memory_p = false, int mode = collection_blocking) override;
    unsigned GetMaxGeneration() override;
    void SetFinalizationRun(Object* obj) override;
    bool RegisterForFinalization(int gen, Object* obj) override;
    int GetLastGCPercentTimeInGC() override;
    size_t GetLastGCGenerationSize(int gen) override;

    // Misc
    HRESULT Initialize() override;
    bool IsPromoted(Object* object) override;
    bool IsHeapPointer(void* object, bool small_heap_only = false) override;
    unsigned GetCondemnedGeneration() override;
    bool IsGCInProgressHelper(bool bConsiderGCStart = false) override;
    unsigned GetGcCount() override;
    bool IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number) override;
    bool IsEphemeral(Object* object) override;
    uint32_t WaitUntilGCComplete(bool bConsiderGCStart = false) override;
    void FixAllocContext(gc_alloc_context* acontext, void* arg, void* heap) override;
    size_t GetCurrentObjSize() override;
    void SetGCInProgress(bool fInProgress) override;
    bool RuntimeStructuresValid() override;
    void SetSuspensionPending(bool fSuspensionPending) override;
    void SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor) override;
    void Shutdown() override;

    size_t GetLastGCStartTime(int generation) override;
    size_t GetLastGCDuration(int generation) override;
    size_t GetNow() override;

    // Allocation
    Object* Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags) override;
    void PublishObject(uint8_t* obj) override;
    void SetWaitForGCEvent() override;
    void ResetWaitForGCEvent() override;

    // Heap verification
    bool IsLargeObject(Object* pObj) override;
    void ValidateObjectMember(Object* obj) override;
    Object* NextObj(Object* object) override;
    Object* GetContainingObject(void* pInteriorPtr, bool fCollectedGenOnly) override;

    // Diagnostics
    void DiagWalkObject(Object* obj, walk_fn fn, void* context) override;
    void DiagWalkObject2(Object* obj, walk_fn2 fn, void* context) override;
    void DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) override;
    void DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number = -1) override;
    void DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn) override;
    void DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context) override;
    void DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context) override;
    void DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context) override;
    void DiagDescrGenerations(gen_walk_fn fn, void* context) override;
    void DiagTraceGCSegments() override;
    void DiagGetGCSettings(EtwGCSettingsInfo* settings) override;

    bool StressHeap(gc_alloc_context* acontext) override;

    segment_handle RegisterFrozenSegment(segment_info* pseginfo) override;
    void UnregisterFrozenSegment(segment_handle seg) override;
    bool IsInFrozenSegment(Object* object) override;

    void ControlEvents(GCEventKeyword keyword, GCEventLevel level) override;
    void ControlPrivateEvents(GCEventKeyword keyword, GCEventLevel level) override;

    unsigned int GetGenerationWithRange(Object* object, uint8_t** ppStart, uint8_t** ppAllocated, uint8_t** ppReserved) override;

    int64_t GetTotalPauseDuration() override;
    void EnumerateConfigurationValues(void* context, ConfigurationValueFunc configurationValueFunc) override;
    void UpdateFrozenSegment(segment_handle seg, uint8_t* allocated, uint8_t* committed) override;
    int RefreshMemoryLimit() override;
    enable_no_gc_region_callback_status EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem* callback, uint64_t callback_threshold) override;
    FinalizerWorkItem* GetExtraWorkForFinalization() override;
    uint64_t GetGenerationBudget(int generation) override;
    size_t GetLOHThreshold() override;
    void DiagWalkHeapWithACHandling(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) override;
    void NullBridgeObjectsWeakRefs(size_t length, void* unreachableObjectHandles) override;

    uint8_t* HeapBase() const { return m_heapBase; }
    uint8_t* HeapHighWater() const { return min(m_heapNextFree, m_heapReservedEnd); }
    uint8_t* HeapReservedEnd() const { return m_heapReservedEnd; }

private:
    LXRGCHeap() = default;

    Object* AllocateSlow(gc_alloc_context* acontext, size_t size, uint32_t flags);
    uint8_t* ClaimBlocks(size_t bytes); // lock-free block-range claim from reservation

    uint8_t* m_heapBase = nullptr;
    uint8_t* m_heapNextFree = nullptr;   // shared block-claim watermark
    uint8_t* m_heapReservedEnd = nullptr;
    LARGE_INTEGER m_qpcFrequency{};
    LARGE_INTEGER m_startTime{};
};
extern LXRGCHeap* g_lxrGCHeap;

// ===========================================================================
//                      Handle table (reused from ZeroGC)
// ===========================================================================
class LXRGCHandleStore : public IGCHandleStore
{
public:
    LXRGCHandleStore();
    ~LXRGCHandleStore() override;

    void Uproot() override;
    bool ContainsHandle(OBJECTHANDLE handle) override;
    OBJECTHANDLE CreateHandleOfType(Object* object, HandleType type) override;
    OBJECTHANDLE CreateHandleOfType(Object* object, HandleType type, int heapToAffinitizeTo) override;
    OBJECTHANDLE CreateHandleWithExtraInfo(Object* object, HandleType type, void* pExtraInfo) override;
    OBJECTHANDLE CreateDependentHandle(Object* primary, Object* secondary) override;

    struct Slot
    {
        Object* Value;
        Object* Secondary;
        void* ExtraInfo;
        HandleType Type;
        Slot* NextFree;
        bool InUse;
        Slot* NextAll; // intrusive chain of every slot ever allocated (for root scan)
    };

    OBJECTHANDLE AllocSlot(Object* value, HandleType type);
    void FreeSlot(OBJECTHANDLE handle);
    static Slot* SlotFromHandle(OBJECTHANDLE handle) { return reinterpret_cast<Slot*>(handle); }

    // Visit the (Value, Secondary) references of every in-use handle in every
    // store. Used as a GC root source by the backup trace.
    static void ForEachLiveHandle(void (*cb)(Object** ref, void* ctx), void* ctx);

private:
    CRITICAL_SECTION m_lock{};
    Slot* m_freeList = nullptr;
    Slot* m_allSlots = nullptr;              // head of the NextAll chain
    LXRGCHandleStore* m_nextStore = nullptr; // chain of all stores
    static LXRGCHandleStore* s_firstStore;
    static CRITICAL_SECTION s_storesLock;
    static bool s_storesLockInit;
};

class LXRGCHandleManager : public IGCHandleManager
{
public:
    bool Initialize() override;
    void Shutdown() override;
    IGCHandleStore* GetGlobalHandleStore() override;
    IGCHandleStore* CreateHandleStore() override;
    void DestroyHandleStore(IGCHandleStore* store) override;
    OBJECTHANDLE CreateGlobalHandleOfType(Object* object, HandleType type) override;
    OBJECTHANDLE CreateDuplicateHandle(OBJECTHANDLE handle) override;
    void DestroyHandleOfType(OBJECTHANDLE handle, HandleType type) override;
    void DestroyHandleOfUnknownType(OBJECTHANDLE handle) override;
    void SetExtraInfoForHandle(OBJECTHANDLE handle, HandleType type, void* pExtraInfo) override;
    void* GetExtraInfoFromHandle(OBJECTHANDLE handle) override;
    void StoreObjectInHandle(OBJECTHANDLE handle, Object* object) override;
    bool StoreObjectInHandleIfNull(OBJECTHANDLE handle, Object* object) override;
    void SetDependentHandleSecondary(OBJECTHANDLE handle, Object* object) override;
    Object* GetDependentHandleSecondary(OBJECTHANDLE handle) override;
    Object* InterlockedCompareExchangeObjectInHandle(OBJECTHANDLE handle, Object* object, Object* comparandObject) override;
    HandleType HandleFetchType(OBJECTHANDLE handle) override;
    void TraceRefCountedHandles(HANDLESCANPROC callback, uintptr_t param1, uintptr_t param2) override;

private:
    LXRGCHandleStore* m_globalStore = nullptr;
};

#endif // __LXRGC_H__
