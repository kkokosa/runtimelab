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
#include <vector>

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
        int64_t    bornTraceEpoch; // inter-trace window this block was allocated in
                                   // (young-object nursery, LXR difference #6). 0 =
                                   // never stamped (old / pre-first-trace).
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
    volatile int64_t Collections;           // total STW collections performed
    volatile int64_t TotalPauseMicros;      // cumulative STW pause time (us)
    volatile int64_t LastCollectCommitted;  // committed-in-use at last GC (bytes)

    // LXR phase model (P1): most epochs are cheap RC pauses; a full backup
    // trace + sweep runs only occasionally, paced by a survival-rate predictor.
    volatile int64_t Epochs;                // total RC epochs (light + full)
    volatile int64_t RCPauses;              // light epochs (ProcessModifiedBuffers only)
    volatile int64_t TracePauses;           // full epochs (trace + sweep) == BackupTraces
    volatile int64_t SurvivalPctEwma;       // EWMA of survivor % across traces (0..100)
    volatile int64_t WastagePctEwma;        // biased-decay EWMA of floating-garbage % recovered per trace (item E wastage predictor)
    volatile int64_t RCPausePauseMicros;    // cumulative light-pause time (us)
    volatile int64_t TracePausePauseMicros; // cumulative full-pause time (us)

    // P2 barrier extensions: SATB deletion barrier + remembered sets.
    volatile int64_t SatbEntries;           // old referents logged to SATB buffers
    volatile int64_t SatbMarks;             // SATB entries consumed (marked) by a trace
    volatile int64_t SatbOverflowRetraces;  // finish-pause full re-traces due to SATB overflow
    volatile int64_t RemsetEntries;         // inter-block pointer slots logged
    volatile int64_t RemsetFixups;          // remset slots rewritten during evacuation
    volatile int64_t RemsetRebuilds;        // item F: persistent-remset rebuilds from mark (post-overflow)
    volatile int64_t RemsetStaleSkipped;    // item F: remset entries skipped as stale (line reuse ver advanced)

    // P3 STW evacuation (moving defragmentation).
    volatile int64_t EvacPasses;            // evacuation phases run
    volatile int64_t EvacRegions;           // fragmented regions evacuated
    volatile int64_t EvacObjects;           // live objects relocated
    volatile int64_t EvacBytesCopied;       // bytes relocated
    volatile int64_t EvacFieldsForwarded;   // heap references rewritten to moved targets
    volatile int64_t EvacPinnedSkipped;     // live objects left in place (root/handle-pinned)
    volatile int64_t EvacTimeBudgetHits;    // evac passes cut short by the wall-clock budget
    volatile int64_t EvacRemsetFixups;      // moved-object refs forwarded via the scoped remset
    volatile int64_t EvacFullWalkFallbacks; // evac cycles that fell back to the O(heap) fixup walk

    // P4 concurrency: SATB backup trace whose transitive mark runs while the
    // mutators execute, bracketed by two brief stop-the-world pauses.
    volatile int64_t ConcurrentTraces;      // concurrent trace cycles run
    volatile int64_t ConcMarkedObjects;     // objects marked during the concurrent drain
    volatile int64_t ConcAllocBlack;        // objects retained by allocate-black (born mid-trace)
    volatile int64_t ClosureGapMarked;      // live objects the conc trace missed, marked by closure-completion
    volatile int64_t FinalRescanMarked;     // live root-reachable objects rescued by the final root rescan
    volatile int64_t ConcSnapshotMicros;    // cumulative STW snapshot-pause time (us)
    volatile int64_t ConcFinishMicros;      // cumulative STW finish-pause time (us)
    volatile int64_t ConcDrainMicros;       // cumulative concurrent (non-pause) drain time (us)

    volatile int64_t MarkStackDrops;        // mark-stack pushes lost to allocation failure (0 == complete)

    // Item D: young/nursery collection at RC pauses.
    volatile int64_t NurseryPasses;         // nursery collections run (RC pauses that reclaimed young)
    volatile int64_t NurserySkipped;        // nursery collections skipped (remset overflow / trace window)
    volatile int64_t NurseryRegionsReclaimed; // young regions reclaimed (no live young)
    volatile int64_t NurseryBytesReclaimed; // young bytes decommitted by the nursery
    volatile int64_t NurseryLiveYoung;      // live young objects retained at the last nursery pass

    // Item D-copy: young-survivor copy-at-RC-pause (paper §3.3.1-3). Live young
    // (RC>0) survivors are copied out of young regions into fresh MATURE space at
    // the RC pause to defragment + promote, letting the emptied young regions be
    // recycled instead of leaving survivors in place for the later trace Evacuate.
    volatile int64_t NurseryCopyPasses;      // survivor-copy passes run
    volatile int64_t NurseryCopyObjects;     // young survivors relocated (promoted)
    volatile int64_t NurseryCopyBytes;       // survivor bytes copied
    volatile int64_t NurseryCopyPinned;      // live young survivors left in place (root/handle-pinned)
    volatile int64_t NurseryCopyRegionsFreed;// young regions freed after full survivor evacuation
    volatile int64_t NurseryCopyFieldsForwarded; // heap refs rewritten to promoted survivors
    volatile int64_t NurseryCopyFullWalks;   // passes that fell back to the O(heap) fixup walk

    // Item ★ (primary-RC mature reclamation): mature memory returned at the RC
    // pause by RC authority (RC==0), NOT by the backup trace's mark-sweep. This
    // is what makes RC the PRIMARY reclaimer (paper §3.3) rather than the trace.
    volatile int64_t MatureRCPasses;          // RC-pause mature-reclamation passes run
    volatile int64_t MatureRCRegionsReclaimed;// mature regions decommitted by RC (whole-region dead)
    volatile int64_t MatureRCBytesReclaimed;  // mature bytes decommitted at RC pauses by RC
    volatile int64_t MatureRCRunsCarved;      // mature dead line-runs carved for reuse at RC pauses
    volatile int64_t MatureRCCarveBytes;      // mature bytes recovered by RC line-carving
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

    uint8_t* HeapBase()  const { return m_heapBase; }
    size_t   HeapBytes() const { return m_heapBytes; }

    // --- RC side table (1 byte of saturating count per 8-byte granule) ---
    uint8_t* RCSlot(Object* obj) const;
    void RCIncrement(Object* obj);
    bool RCDecrement(Object* obj); // returns true if the count reached zero
    // Item G (§3.5): thread-safe variants for parallel RC apply. Use a CAS loop on
    // the byte RC slot so concurrent workers touching the same object's count race
    // safely (preserving the 0xFF stuck-high sentinel and the no-decrement-below-0
    // rule). The RC page must be pre-committed (RC apply pre-commits serially), so
    // these never call VirtualAlloc.
    void RCIncrementAtomic(Object* obj);
    bool RCDecrementAtomic(Object* obj); // returns true iff THIS worker drove it to zero

    // --- Coalescing / field-logging write barrier support ---
    //
    // LogModifiedField is the LXR write-barrier slow path. It MUST be called
    // by mutator reference-field writes, with the OLD value the slot held (for
    // coalescing RC + the SATB deletion barrier) and the NEW value being stored
    // (for the remembered set). It records into the calling thread's buffers for
    // replay/consumption at GC time.
    //
    // *** Under the CoreCLR standalone-GC ABI there is no way to make the
    //     runtime call this on ordinary field writes. See FEASIBILITY.md. ***
    void LogModifiedField(Object** slot, Object* oldValue, Object* newValue);

    // Coalescing "unlogged bit" (paper A(ii), §3.4). TryFirstLogField returns true
    // exactly once per field per epoch (the first store), so the barrier logs each
    // modified field once (one RC dec of the t_n referent + one SATB snapshot
    // entry), matching Levanoni-Petrank coalescing at the source instead of at
    // processing time. ClearLoggedBit resets a field's bit as its buffer entry is
    // consumed at the RC pause (O(modified fields), not O(heap)); ResetLoggedTable
    // wholesale-clears the committed prefix on a buffer-overflow epoch.
    bool TryFirstLogField(Object** slot);
    void ClearLoggedBit(Object** slot);
    void ClearLoggedRange(uint8_t* start, uint8_t* end);
    void ResetLoggedTable();

    // Extend the committed logged-table (unlogged-bit) prefix to cover
    // [heapBase, addrEnd). Called on the allocation/heap-commit path so the
    // cooperative-mode barrier never has to commit a reserved bitmap page.
    void EnsureLoggedUpTo(uint8_t* addrEnd);

    // Replays every mutator's modified buffer: increment new referents,
    // decrement old referents, then process the resulting zero-count work
    // list (recursive decrements). This is coalescing RC.
    void ProcessModifiedBuffers();
    // #1 concurrent/lazy decrements: STW snapshot of the modified buffers, then
    // off-pause replay of the coalescing-RC increments/decrements + recursive free.
    void SnapshotModifiedBuffers();
    void ProcessSnapshotDecrements();

    // Deferred reference counting for ROOTS (Deutsch-Bobrow; paper §2.1/§3.2.1).
    // Root->heap pointers are not barriered, so at each RC pause LXR scans the
    // roots, applies ONE increment to each root-reachable object, and buffers a
    // matching decrement for the NEXT pause. This keeps a root-only-reachable
    // (mature) object's count >= 1 for the epoch it is rooted, making RC
    // self-standing instead of relying on the mark trace to protect roots.
    // CaptureRoots collects the current unique in-heap root+handle referents; it
    // MUST be called under STW (GcScanRoots requires a suspended EE).
    void CaptureRoots(std::vector<Object*>& out);

    // --- SATB (snapshot-at-the-beginning) deletion barrier (P2) ---
    //
    // While a (future concurrent, P4) trace window is open, the barrier logs the
    // overwritten referent so a Yuasa snapshot cannot miss an object unlinked
    // mid-trace. SetSatbActive opens/closes that window; DrainSatbBuffers marks
    // every logged referent (over-retention within one cycle is always safe) and
    // empties the buffers.
    void SetSatbActive(bool active);
    bool IsSatbActive() const;
    void DrainSatbBuffers();

    // --- Concurrent SATB backup trace (P4) ---
    //
    // The paper's backup trace marks concurrently with the mutators, bracketed by
    // two brief stop-the-world pauses. ConcurrentTraceSnapshot (STW) resets the
    // marks, opens the SATB window, seeds the mark stack from the roots and
    // records a per-region allocation high-water so objects born mid-trace can be
    // retained (allocate-black). ConcurrentTraceDrain runs OUTSIDE any pause,
    // marking the transitive closure while mutators log deletions via the SATB
    // barrier. ConcurrentTraceFinish (STW) consumes residual SATB, finishes the
    // closure, and applies allocate-black. Correctness rests on the existing
    // write-barrier old-value capture (no runtime read barrier is required).
    void ConcurrentTraceSnapshot();
    void ConcurrentTraceDrain();
    void ConcurrentTraceFinish();
    void ResetSatbBuffers(); // clear SATB buffers+cursors at end of a trace (STW)

    // --- Remembered sets: inter-block pointer slots (P2), for evacuation (P3) ---
    //
    // While enabled, the barrier records slots that come to hold a pointer into a
    // different Immix block, so evacuation can find and rewrite references into a
    // moved block without a full-heap scan. EnumerateRemsetSlots visits every
    // recorded slot (evacuation re-reads each to filter stale/duplicate entries).
    void SetRemsetActive(bool active);
    bool IsRemsetActive() const;
    void EnumerateRemsetSlots(void (*visit)(Object** slot, void* ctx), void* ctx);
    void ResetRemsets();
    // Item F (§3.3.4): make the remembered set PERSISTENT with reuse-version
    // pruning instead of clearing it each trace. CompactRemsets (called at trace
    // finish, after Evacuate has consumed the set) drops entries whose source line
    // reuse version has advanced (stale) and de-duplicates the survivors, keeping
    // the set bounded to the live inter-block edge working set. On a prior barrier
    // overflow it rebuilds completeness from the just-completed mark. RecordRemset-
    // Edge lets the collector register edges created by GC-internal relocation
    // (evac copies / young-survivor promotions), whose memcpy stores never fire
    // the barrier. Both run STW.
    void CompactRemsets();
    void RecordRemsetEdge(Object** slot);

    // --- STW incremental evacuation / copying (P3) ---
    //
    // Inside the stop-the-world trace pause (after BackupTrace has marked the
    // live objects), relocate the live objects out of the most fragmented
    // regions into fresh space and free those regions, defragmenting the heap.
    // Because it runs fully STW no read barrier is needed: every reference to a
    // moved object is fixed up within the pause. Root/handle referents are pinned
    // (never moved) so only heap references need forwarding. Gated off by default
    // (LXR_EVAC=1); driven from the trace pause.
    void SetEvacActive(bool active);
    bool IsEvacActive() const;
    void Evacuate();

    // Item F (paper §3.3.4): per-line reuse versioning for the persistent evac
    // remembered set. LineReuseVerOf reads a line's current reuse version;
    // BumpLineReuseRange increments the version of every line overlapping
    // [start,end) and is called at every reclaim site (carve, sweep decommit,
    // evac free). EnsureLineReuseCommitted commits the covering table prefix.
    uint32_t LineReuseVerOf(void* addr) const;
    void BumpLineReuseRange(uint8_t* start, uint8_t* end);
    void EnsureLineReuseCommitted(size_t usedBytes);

    // Periodic mark-sweep over the whole heap to reclaim dead cycles that RC
    // leaks. Uses IGCToCLR root/stack enumeration (which the ABI *does*
    // expose) plus per-object GCDesc traversal.
    void BackupTrace();

    // Reclaim fully-dead lines/blocks after RC/trace, and pick evacuation
    // candidates for the next cycle (Immix defragmentation).
    void SweepAndSelectDefrag();

    // Item D: young/nursery collection at an RC pause. Reclaims young (this-epoch)
    // regions proven dead by a bounded closure over roots + handles + the complete
    // mature->young remembered set (item F) + young->young edges. Reclaim-only
    // (region-granular, no copying): survivors stay young and are compacted /
    // promoted by the trace-cycle Evacuate as today. Self-guards: no-op unless the
    // remset is complete (!g_remsetOverflow) and no concurrent trace window is
    // open, so freeing young can never dangle a live mature->young edge.
    void CollectNursery();

    // Item D-copy (paper §3.3.1-3): at the RC pause, copy the live young (RC>0)
    // SURVIVORS out of young regions into fresh MATURE destination space, then
    // recycle the emptied young regions. This is the defragmenting/promoting half
    // of item D (the reclaim-only half is CollectNursery). Liveness is RC (marks
    // are stale outside a trace window); reference fix-up to moved survivors uses
    // the inter-block remembered set (item F) for incoming edges + the moved
    // copies' outgoing edges + in-place pinned survivors, with an O(heap) full-walk
    // fallback when the remset is unavailable/overflowed. Root/handle referents are
    // pinned (not moved); interior-root-unresolved cycles are skipped. Runs before
    // CollectNursery so fully-evacuated regions are freed here and all-dead regions
    // are mopped up there. Self-guards on g_traceWindowOpen / g_youngRCIncomplete.
    void CopyYoungSurvivors();

    // Item ★ (primary-RC mature reclamation, paper §3.3): at each RC pause,
    // return MATURE memory whose reference count has dropped to zero, WITHOUT
    // waiting for the backup trace. RC is authoritative outside a trace window
    // (all ref stores are counted by the coalescing barrier, roots are pinned,
    // young is the nursery's domain, stuck 0xFF objects are kept), so a mature
    // region/line with no RC>0 object is dead and is decommitted / carved for
    // reuse here. This is the change that makes RC the PRIMARY reclaimer rather
    // than the trace. Deferred entirely while a concurrent trace window is open
    // (marks in flux) or an RC undercount is possible (g_youngRCIncomplete).
    void ReclaimMatureByRC();

    lxr::BlockMeta* MetaForBlock(uint8_t* blockAddr);
    // Young-object nursery (LXR difference #6). StampBornEpoch marks the blocks
    // spanned by a freshly (re)registered allocation region with the current
    // inter-trace window id; IsYoung reports whether an object still lives in the
    // window it was born in (no trace has aged it yet). Young objects are decoupled
    // from reference counting (the generational hypothesis: most die young, so
    // paying RC inc/dec for them is wasted) and are kept alive by the trace /
    // allocate-black instead. Sound because reclamation is mark-authoritative on
    // every trace cycle and RC-only pauses never reclaim.
    void StampBornEpoch(uint8_t* start, size_t size);
    // Stamp blocks as MATURE (aged out of the current window) so IsYoung() is
    // false there. Used for CopyYoungSurvivors promotion destinations.
    void StampMatureEpoch(uint8_t* start, size_t size);
    bool IsYoung(Object* obj);

    // --- Backup trace (stop-the-world mark) + Immix reclamation ---
    //
    // BackupTrace marks every object reachable from the roots (stacks, statics,
    // handles) transitively via GCScanObjectRefs, then SweepAndSelectDefrag
    // reclaims (decommits) any allocation region that ends up with no marked
    // object. This is the correctness backstop of LXR (it also collects the
    // cycles pure RC leaks) and the mechanism that actually returns memory.
    bool MarkObject(Object* obj);       // sets the mark bit; true if newly marked
    bool IsMarked(Object* obj) const;
    // True if any mark bit is set at a granule-aligned start within [start,end).
    // Parse-independent region liveness for the sweep (see SweepAndSelectDefrag).
    bool AnyMarkedInRange(uint8_t* start, uint8_t* end) const;
    // RC-authoritative liveness: true if any object-start granule in [start,end)
    // has a non-zero reference count. Parse-free (scans the RC side table like
    // AnyMarkedInRange scans the mark table); skips uncommitted RC pages, which
    // are necessarily all-zero (never had an RC set). Used by the sweep so a
    // region holding any RC>=1 object is never reclaimed - the guarantee that
    // makes concurrent-trace incompleteness harmless (missed-live objects have
    // RC>=1 and so are protected without relying on the trace).
    bool AnyRCNonZeroInRange(uint8_t* start, uint8_t* end) const;
    // Zero the RC side-table bytes for every granule in [start,end). Called when
    // a region is reclaimed (decommitted) by the sweep/evac: a reclaimed region's
    // memory is gone, so its objects' reference counts MUST become zero to keep
    // the invariant "reclaimed => RC 0". Without this a stale RC>0 survives in a
    // decommitted range; a later decrement (e.g. a modified-buffer old value
    // captured before the sweep) drives it to zero, enqueues the now-dangling
    // pointer, and the recursive-free scan reads the decommitted page -> AV.
    // Skips uncommitted RC pages (already all-zero); never faults.
    void ClearRCRange(uint8_t* start, uint8_t* end);
    void VerifyTraceComplete();         // diagnostic: LXR_VERIFY_TRACE=1
    int64_t CompleteClosureOverMarked(); // finish pause: close closure over all marked objects
    int64_t MarkModifiedNewValues();     // finish pause: reconcile concurrent-marking race via modified set
    // Immix line marking (LXR_LINE_REUSE): record every 256 B line touched by a
    // live object [obj, obj+size) in the line-mark side table. Accumulated at the
    // object-scan sites (drain/closure) where the size is already known, so the
    // sweep can find fully-dead line runs inside otherwise-live regions in
    // O(lines) - no whole-heap object parse. Thread-safe (atomic bit set).
    void MarkLines(Object* obj, size_t size);
    // First marked (live) object-start at or after 'from', bounded by 'end'
    // (returns 'end' if none). Cheap mark-table bit scan used to snap a free
    // line run's end to a real object boundary so the following live segment
    // stays linearly parseable after line reuse.
    uint8_t* FirstMarkedAtOrAfter(uint8_t* from, uint8_t* end) const;
    // Carve fully-dead line runs out of retained region g_chunks[i] into their
    // own reusable FreeRun regions (Immix line recycling). Reads only line marks
    // (O(lines)); splits the region at object boundaries; may realloc g_chunks,
    // so the caller must not touch a prior g_chunks reference afterwards.
    void CarveFreeRuns(size_t regionIndex);
    // Item ★ Stage 2: RC-authoritative dead-object-run carving for a partially
    // dead MATURE region at an RC pause. Linear-walks real object boundaries
    // (parse-safe, like the nursery/sweep) classifying each object live (RC>0 /
    // stuck / root-covered / young) or dead, coalesces runs of consecutive dead
    // objects >= the reuse threshold, and plugs+lists them for reuse -- returning
    // mature memory by RC authority WITHOUT waiting for the mark-driven trace.
    // Uses NO mark bits (stale outside a trace window). Returns bytes carved.
    // Caller holds g_chunkLock. rootSorted must be the sorted raw-root address vec.
    int64_t CarveDeadRunsByRC(size_t regionIndex, const std::vector<uint8_t*>& rootSorted);
    void ResetMarks();                  // decommits the mark side-table (all bits -> 0)
    void PushMark(Object* obj);         // MarkObject + push onto the mark stack
    void DrainMarkStack();              // transitive closure via GCScanObjectRefs
    void ParallelDrainMarkStack(int workers); // P5: parallel transitive closure
    void DrainSliceLocal(std::vector<Object*>& local); // drain one worker's grey set
    void DrainClosure();                // parallel or serial closure per LXR_GC_THREADS
    // Item G (§3.5): partition the scan of a single very large reference array
    // across the mark pool. A lane that meets such an array defers it (marks it,
    // records it) instead of scanning its elements serially; after the per-lane
    // closure joins, DrainDeferredBigArrays chunks every deferred array's element
    // range and scans the chunks in parallel, iterating to a fixpoint.
    bool IsBigRefArray(Object* o, size_t osz, size_t* outSlots) const;
    void ScanBigRefArrayChunk(Object* o, size_t slotStart, size_t slotEnd, std::vector<Object*>& local);
    void DrainDeferredBigArrays(int workers);
    Object* ResolveInterior(uint8_t* interior); // interior pointer -> containing object
    // Conservative fallback when an in-heap interior/byref root cannot be
    // resolved to its base object: keep the containing region alive this cycle so
    // a live-byref object is never swept. Returns true if a keep-alive bit was set.
    bool ConservativelyKeepAliveInterior(uint8_t* interior);

    int64_t ReclaimedBytes() const { return m_reclaimedBytes; }

private:
    void EnqueueZeroCount(Object* obj);
    void DrainZeroCountWorkList();
    // Item G (§3.5): apply one coalesced RC epoch -- ALL increments then ALL
    // decrements (paper's ordering) -- across the worker pool when large enough,
    // else serially. Objects that reach zero are enqueued for the (serial) free
    // cascade; the caller then calls DrainZeroCountWorkList(). Shared by the STW
    // ProcessModifiedBuffers and the off-pause ProcessSnapshotDecrements.
    void ApplyRCEpoch(std::vector<Object*>& incs, std::vector<Object*>& decs);

    // Extend the committed+zeroed mark-table prefix to cover [heapBase, addrEnd)
    // without disturbing already-set bits, so marks can be set on freshly claimed
    // evacuation-destination space that sits above the trace-time high-water.
    void EnsureMarkCommitted(uint8_t* addrEnd);


    bool InHeap(Object* obj) const
    {
        return (uint8_t*)obj >= m_heapBase && (uint8_t*)obj < m_heapBase + m_heapBytes;
    }

    uint8_t*        m_heapBase = nullptr;
    size_t          m_heapBytes = 0;
    uint8_t*        m_rcTable = nullptr;     // 1 byte / 8 heap bytes
    uint8_t*        m_markTable = nullptr;   // 1 bit / 8 heap bytes (backup-trace marks)
    size_t          m_markCommittedBytes = 0; // committed+zeroed mark-table prefix (bytes)
    uint8_t*        m_lineMarkTable = nullptr;   // 1 bit / 256 B line (Immix line reuse)
    size_t          m_lineMarkCommittedBytes = 0; // committed+zeroed line-table prefix (bytes)
    // Item F (paper §3.3.4): per-line reuse version. Bumped whenever a line is
    // reclaimed/recycled (carve, sweep decommit, evac free). A persistent evac
    // remembered-set entry records the source line's version at insert; at evac
    // fix-up an entry whose line version has advanced is STALE (its source object
    // was reclaimed and the line possibly reused) and is skipped. This is what
    // lets the barrier-maintained remset stay persistent without dangling on
    // reused slots. 16 bits: wrap needs 65536 reclaims of one line between insert
    // and consume (impossible within a trace interval).
    uint16_t*       m_lineReuseVer = nullptr;    // 1 uint16 / 256 B line
    size_t          m_lineReuseVerCommittedBytes = 0; // committed prefix (bytes)
    uint8_t*        m_loggedTable = nullptr;  // 1 bit / 8 heap bytes: per-field "logged this epoch" (coalescing unlogged bit, paper A(ii))
    size_t          m_loggedCommittedBytes = 0; // committed logged-table prefix (bytes)
    lxr::BlockMeta* m_blockMeta = nullptr;   // 1 entry / 32 KiB block
    size_t          m_blockCount = 0;
    volatile int64_t m_reclaimedBytes = 0;   // cumulative bytes decommitted by sweeps
    // Deferred-RC root buffers (see CaptureRoots). Only ever touched by the single
    // collection thread, at STW pauses or the serialized off-pause drain, so no
    // lock is needed. m_rootDeferredPrev holds the objects incremented at the
    // previous RC pause (to be decremented at the next); m_rootDeferredSnap holds
    // the roots captured at a concurrent snapshot pause for the off-pause replay.
    std::vector<Object*> m_rootDeferredPrev;
    std::vector<Object*> m_rootDeferredSnap;
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

    // Lock-free block-range claim from the reservation. Public so the collector's
    // STW evacuation (P3) can claim fresh destination space.
    uint8_t* ClaimBlocks(size_t bytes);

private:
    LXRGCHeap() = default;

    Object* AllocateSlow(gc_alloc_context* acontext, size_t size, uint32_t flags);

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
