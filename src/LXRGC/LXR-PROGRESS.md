# LXR implementation progress — paper-conformance source of truth

**Paper:** "Low-Latency, High-Throughput Garbage Collection" (LXR), Zhao, Blackburn &
McKinley, PLDI 2022. arXiv:2210.17175 (Extended Version).
Canonical HTML: <https://ar5iv.labs.arxiv.org/html/2210.17175>

**Goal:** implement LXR **1:1 with the paper** as a CoreCLR standalone GC (branch
`feature/LXRGC`) on top of the generic pluggable write-barrier + object-scan facility
(fork `kkokosa/runtime`, branch `feature/pluggable-write-barrier`). No STW-only
fallbacks, no O(live-heap) perf crutches on the hot path, no unsound masking. Runtime
changes are allowed only if generic + minimal + flagged.

This document is the **reference source of truth** for how faithfully each LXR mechanism
is implemented. Update the status table + changelog whenever a divergence is closed.

Legend: ✅ conformant · ⚠️ partial / approximated · ❌ divergent (shortcut) · ⬜ not started

---

## Conformance status (audited 2026-07-23 against paper §2–§3)

| # | Paper mechanism (citation) | Our code | Status |
|---|---|---|---|
| **A** | **Coalescing field-logging barrier**: per-field **unlogged bit**; log each field **once per epoch** (first write pushes old→`decbuf`, addr→`modbuf`); ignore intermediate referents; unlogged bit reset at RC pause; new objects born "logged" so barrier elides young (§3.4, Fig.3) | **A(i)+A(ii) landed** (`parity-a1`, `parity-a2`): the barrier now carries a per-field **unlogged-bit side table** (`m_loggedTable`, 1 bit/8-byte slot) — `TryFirstLogField` test-and-sets it so RC-`modbuf` + SATB append **exactly once per field per epoch** (`LogModifiedField`); bits are cleared per-consumed-field at each RC pause (`ClearLoggedBit`, O(modified fields)) or wholesale on a buffer-overflow epoch (`ResetLoggedTable`). Logged pages are committed off the barrier on the alloc path (`EnsureLoggedUpTo`). Coalescing is now at the **source** (Levanoni-Petrank), matching the paper; this drastically cuts buffer traffic and the overflow→O(heap) fallback frequency. (Young still handled via `LXR_YOUNG_RC` RC-skip, not birth-logged elision — see D.) | ✅ |
| **B** | RC pause applies **all increments, then all decrements** (§3.2.1); **root deferral** — increment root-reachable at tₙ, buffer matching decrement for tₙ₊₁ (§2.1) | **Ordering + root deferral landed** (`parity-b`): all increments precede all decrements on both RC paths, and `CaptureRoots` now scans handles + stack/static/finalizer roots at each RC STW pause, increments each unique in-heap referent, and buffers a matching decrement for the next pause (`m_rootDeferredPrev`/`m_rootDeferredSnap`, rotated in `ProcessModifiedBuffers`/`ProcessSnapshotDecrements`). RC is now **self-standing** (root-reachable mature objects hold RC ≥ 1 for their rooted epoch) instead of relying on the mark trace to protect roots — the prerequisite for young-at-RC-pause reclaim (D). (Young referents are RC-skipped, per D.) | ✅ |
| **C** | SATB trace **spans multiple RC epochs**; completes **concurrently, no STW finish** (checked at next RC pause); invariant: RC may never delete an unmarked object mid-trace → **mark+scan any mature object RC kills** if not already marked (§3.2.3) | Single snapshot→drain→**STW finish pause** per trace cycle (`ConcurrentTraceFinish`); does not span epochs; uses a final root+handle rescan + (overflow-only) closure instead of the mark-on-RC-death rule. Sound, but structurally different. | ⚠️ |
| **D** | **Implicitly-dead young**: young objects with no increment are reclaimed **at the RC pause**, before decrements; young survivors (0→1 increment) **copied at that pause** to defragment (§2.1, §3.3.1–3) | **LANDED & VERIFIED (2026-07-24)** (`CollectNursery`, env `LXR_NURSERY`): at each RC pause it reclaims young regions in which every young object is implicitly dead (RC 0, unmarked), region-granular Immix-style, honoring interior/byref roots + mark bits. **True root cause of the earlier AV found and FIXED (not masked):** the per-thread coalescing **modified buffer overflowed** under load and the barrier **dropped the first-log entry** on a full buffer — a dropped first-log **permanently loses an RC increment**, so a live young referent stayed stuck at RC 0 and the nursery freed it while reachable. Proven by an authoritative from-roots probe (`LXR_NURSERY_ROOTPROBE`): with a huge buffer `victimsReachableFromRoots=0` across all pauses, with the normal 4096 buffer up to **1760 live victims**. Fix = the paper's **shared-queue** buffer (§3.2.1): on fill the barrier CAS-swaps in a pre-registered spare from a global free-list (topped up off-barrier in `EnsureThreadBuffers`, recycled at each RC-pause drain), so **no first-log is ever dropped**; the vanishingly-rare free-list-exhaustion case sets `g_youngRCIncomplete` (nursery defers reclaim until the next complete trace re-establishes liveness). Also fixed a latent stale-logged-bit bug (`ClearLoggedRange` at all reclaim sites). Verified with normal buffers: `victimsReachableFromRoots=0` across all pauses; **8/8 concurrent + 6/6 STW WebApi iters av=0/hang=0**, ~75 MB young reclaimed/run (605 MB + 450 MB totals). No runtime change needed. Young-survivor copy-at-RC-pause still deferred (survivors stay young, compacted by the trace-cycle `Evacuate`). | ✅ |
| **E** | Triggers: RC = heap-full OR increment-threshold OR **young-survival predictor** (1:3 biased exp decay, 128 MB default); SATB = free-block threshold OR **wastage predictor** (live-block, 1:3 decay, 5% default) (§3.2.2, §3.2.5) | Allocation-growth trigger + a single **survival EWMA (7:1)** scaling the epoch cap + epochs-since-trace. No increment/wastage/free-block predictors. | ⚠️ |
| **F** | Remsets **scoped to the evacuation set**, bootstrapped by the first SATB trace, kept updated by the barrier, **line-reuse-counter** stale-entry tagging; evac set = blocks <50% occupancy, N lowest; **incremental, time-budgeted**, STW (§3.3.4) | **Completeness + scoping landed** (`g_remsetOverflow`, `ResetRemsets` at every trace): the barrier now sets an **overflow flag** on a dropped inter-block edge (was a silent drop) — safety-critical for young reclaim — and the remset is **scoped per inter-trace interval** (reset under STW at each trace, which ages all young to mature and rebuilds completeness). Per-interval reset makes stale entries *impossible* rather than filtering them (equivalent to, and simpler than, the paper's persistent-remset + reuse-counter tagging: reclamation/decommit only happen at traces, and consumers drain under the STW pause before any decommit, so a recorded slot's source is always still committed). `EnumerateRemsetSlots` consumed by the nursery (D). Evac-set scoping + incremental time-budget still differ. | ⚠️ |
| **G** | **Parallelism in every phase**; very large reference arrays partitioned for increment scalability (§3.5) | Parallel **mark** pool (`ParallelDrainMarkStack`); RC increment/decrement processing is **serial**; no large-array partitioning. | ⚠️ |
| — | Immix block/line heap; single field barrier serving RC + SATB + remset; SATB collects cycles + stuck counts; **copy only during STW**; stuck count → resolved by trace | All present ✅ (stuck at 0xFF vs paper's 2-bit count — variant, higher fidelity). | ✅ |

### Honest bottom line
The engine is a **sound, structurally-LXR** collector, faithful on the big choices
(Immix, the one field barrier serving three roles, SATB for cycles/stuck, STW-only
copying, parallel mark). But **A** breaks LXR's core thesis — *"high-performance
parallel reference counting as the **primary** mechanism,"* tracing only occasional —
because per-store logging makes RC **imprecise/advisory**, so reclamation leans on the
periodic mark trace far more than the paper. **B/C/D** compound this (RC cannot stand
alone without the trace). **E/F/G** are precision/perf refinements, not soundness or
identity issues. Reaching 1:1 parity means restoring **precise, primary RC** first.

---

## Fix roadmap (in priority order)

1. **A — precise coalescing RC** (restores LXR's core identity): ✅ **DONE**
   - **A(i) correctness:** dedup per field at processing — first logged `oldValue` +
     one increment of the final `*slot` — so counts are precise even with the current
     per-store log. Small, isolated change in the RC processing loops. ✅
   - **A(ii) full parity:** an **unlogged-bit side table** so the barrier logs each
     field **once per epoch** (matches the paper's 1.6% overhead, eliminates buffer
     bloat, and **subsumes the SATB/modified-buffer overflow → O(heap) fallback**). ✅
2. **B —** inc-before-dec on the STW RC path + **root deferral** (RC accounts roots). ✅ **DONE**
3. **D —** implicitly-dead-young reclaim at the RC pause. ✅ **DONE** (young-survivor
   copy-at-RC-pause still deferred to the trace-cycle `Evacuate`).
4. **C —** multi-epoch SATB + mark-on-RC-death; drop the STW finish closure.
5. **E / F / G —** survival + wastage predictors; evac-set-scoped remsets with
   line-reuse tagging + incremental time-budgeted evac; parallel RC + array partition.

Only after 1–4 (identity-restoring) land do the full benchmarks become a faithful
LXR-vs-Server/Workstation comparison.

---

## Already-fixed stability blockers (prerequisites, all sound)

| Item | Commit | Status |
|---|---|---|
| SuspendEE livelock (blocking `WaitUntilGCComplete`) | `55b1ef9` | ✅ hang=0 / 200+ iters |
| In-heap `DrainZeroCountWorkList` decommit-UAF (RC-vs-trace authority desync) | `80b6ffb` | ✅ av=0 / 100 iters |
| Concurrent-SATB `DispatchContinuations` AV (live obj reachable only from a mutator root) | `56825eb` | ✅ av=0 / 136 iters |

---

## Full-LXR benchmark config (once parity reached)

`LXR_CONCURRENT=1 LXR_EVAC=1 LXR_REMSET=1 LXR_LINE_REUSE=1 LXR_CONC_DECREMENTS=1`
`LXR_YOUNG_RC=1 LXR_NURSERY=1 LXR_GC_THREADS=<#cores>` + `DOTNET_ReadyToRun=0`. vs Server GC
and Workstation GC. Regenerate `results/report.html`.

---

## Changelog
- **2026-07-24** — **D implicitly-dead-young reclaim landed → D ✅.** Root-caused the
  nursery AV to the coalescing **modified buffer overflow dropping first-log entries**
  (a dropped first-log permanently loses an RC increment, leaving a live young object
  stuck at RC 0 for the nursery to free). Proven decisively by an authoritative
  from-roots probe (`LXR_NURSERY_ROOTPROBE`): huge buffer → `victimsReachableFromRoots=0`
  everywhere; normal 4096 buffer → up to 1760 live victims. Fixed with the paper's
  **shared-queue** buffer (§3.2.1): the barrier CAS-swaps a pre-registered spare from a
  global free-list on fill (spares topped up off-barrier in `EnsureThreadBuffers`,
  recycled at each RC-pause drain in `ProcessModifiedBuffers`/`SnapshotModifiedBuffers`),
  so no first-log is ever dropped; the rare free-list-exhaustion path sets
  `g_youngRCIncomplete` and the nursery defers reclaim until the next complete trace.
  Also fixed a latent stale-logged-bit bug (`ClearLoggedRange` at all reclaim sites).
  The prior "GC-side nursery-vs-trace race" / "remembered-set incompleteness" theories
  were both wrong. Verified with normal buffers: rootprobe `victimsReachableFromRoots=0`
  across all pauses; **8/8 concurrent + 6/6 STW WebApi iters av=0/hang=0**, ~75 MB young
  reclaimed per run. No runtime change needed.
- **2026-07-23** — Initial paper-conformance audit (this document). A ❌, B ❌, C/D/E/F/G ⚠️.
  Starting fix roadmap at item A.
- **2026-07-23** — **A(i) precise coalescing RC landed.** Both RC paths
  (`ProcessModifiedBuffers` STW + `SnapshotModifiedBuffers` concurrent) now coalesce
  per modified field at processing time (one dec of first-logged old value + one inc
  of final `*slot`), and `ProcessModifiedBuffers` applies all increments before any
  decrement. RC counts are now precise. A ❌→⚠️ (A(ii) unlogged bit still pending),
  B ❌→⚠️ (ordering fixed; root deferral pending). Verified: ConsoleApp reclaim smoke
  exit 0, WebApi 6/6 clean (av=0) under the full unified config.
- **2026-07-23** — **A(ii) unlogged-bit side table landed → A complete.** Added a
  per-field logged bitmap (`m_loggedTable`, 1 bit/8-byte slot, mirrors the mark
  table). The barrier (`LogModifiedField`) now gates the RC modified-buffer append
  and the SATB deletion append on `TryFirstLogField` (atomic test-and-set), so each
  field is logged **exactly once per epoch** (source-level Levanoni-Petrank
  coalescing); the remset append stays per-store (its membership depends on the new
  value). Bits are cleared per consumed field at each RC pause (`ClearLoggedBit`) or
  wholesale on a modified-buffer overflow (`ResetLoggedTable`), keeping the
  set-bits==buffered-fields invariant so no stale bit crosses an epoch/trace start.
  Bitmap pages are committed off the barrier on the allocation path
  (`EnsureLoggedUpTo`) — never from the cooperative-mode barrier (which cannot safely
  VirtualAlloc). A ⚠️→✅. Verified: build green, ConsoleApp smoke exit 0, WebApi 6/6
  clean (av=0) under the full unified config.
- **2026-07-23** — **B root deferral landed → B complete.** Added `CaptureRoots`,
  which scans handles + stack/static/finalizer roots (`GcScanRoots`, resolving
  interior pointers) into a de-duplicated set at each RC STW pause. Each RC pause
  now increments the current root referents and decrements the previous pause's set
  (`m_rootDeferredPrev`, rotated in `ProcessModifiedBuffers` on the STW/finish path
  and via `m_rootDeferredSnap` across `SnapshotModifiedBuffers`→`ProcessSnapshot-
  Decrements` on the concurrent path). Root-reachable mature objects now hold RC ≥ 1
  for their rooted epoch, so RC is self-standing rather than reliant on the mark
  trace to protect roots (Deutsch-Bobrow deferral, paper §2.1/§3.2.1). The deferral
  chain is balanced across every processing pause (each captured set is decremented
  exactly once at the following pause). B ⚠️→✅. Verified: build green, ConsoleApp
  smoke exit 0, WebApi 6/6 clean (av=0) under the full unified config.
- **2026-07-24** — **F remset completeness + scoping landed (partial).** The barrier
  now sets `g_remsetOverflow` on a dropped inter-block edge (was a silent drop —
  completeness is safety-critical for young reclaim), and the remembered set is
  scoped per inter-trace interval: `ResetRemsets` (which also clears the overflow
  flag) is called under the STW pause at every trace cycle (both the STW and
  concurrent-finish paths). Per-interval reset makes stale entries impossible
  (reclamation/decommit happen only at traces; consumers drain under the pause
  before any decommit), a stronger, simpler invariant than the paper's persistent
  remset + reuse-counter tagging. Evac-set scoping + incremental time-budget still
  differ (F stays ⚠️). Verified: build green, WebApi 5/5 clean (av=0) full config.
- **2026-07-24** — **D root cause CORRECTED: no write-barrier gap, no runtime change
  needed.** Added a crash-safe classification diagnostic (`LXR_NURSERY_DIAG` →
  `RunNurseryDiag`) that, at each RC pause, compares the remembered-set young-live
  closure against a full-heap closure and — decisively — runs a **from-roots
  full-transitive reachability** probe over the difference. Result across passes:
  `missedReachableFromRoots=0/17`, `0/138` → **every young object the remset closure
  omits is unreachable from roots (dead)**, so the remembered set / write barrier is
  **complete** for young liveness. The earlier A/B (`LXR_NURSERY_FULLSCAN` av=0 vs
  remset-only crash) was a **false signal**: FULLSCAN merely over-retains dead young
  (reachable only from dead, unswept objects), decommitting fewer regions and masking
  the AV. `youngMarked=0`/`youngDeadMarked=0` at RC pauses also rules out a
  nursery-vs-SATB-mark gap at pause time. Corrected the prior "incomplete barrier"
  claim. The residual nursery AV is therefore a **GC-side nursery-vs-trace
  reclamation/recycling race**, to be fixed with **C** (no runtime change). Also
  recorded that the remset-closure nursery is itself a paper deviation (paper reclaims
  implicitly-dead young via **RC=0**, not a generational remset; young are currently
  RC-skipped) — faithful redesign tracked under D.
- **2026-07-24** — **D nursery collection implemented; gated OFF pending a complete
  barrier.** `CollectNursery` (env `LXR_NURSERY`) runs at each RC pause: it builds a
  bounded young-liveness closure (roots + handles + the complete mature→young remset
  (F) + transitive young→young) and reclaims young regions holding no reachable young
  object (region-granular, mirrors the sweep decommit path). **Root-caused a
  soundness blocker and did NOT ship it:** authoritative young reclaim requires a
  *complete* mature→young remset, i.e. a write barrier capturing 100% of ref stores;
  our pluggable Callback barrier has residual completeness gaps (a store form bypasses
  `LogModifiedField`; RC/SATB tolerate this via the from-roots trace backstop, the
  nursery cannot). Proven by A/B: `LXR_NURSERY_FULLSCAN` (O(heap) complete-seed mode)
  is av=0 and reclaims ~25 MB/pass, while the remset-only mode faults in the trace's
  `DrainMarkStack`/`GCScanObjectRefs` on a decommitted young region (0xC0000005).
  Nursery therefore OFF by default (default full config stays av=0, 5/5); enabling it
  by default **needs a runtime change** (route the remaining store form(s) through the
  Callback barrier, or maintain the card table in the Callback barrier so the nursery
  can do a cheap dirty-card mature→young scan instead of O(heap) fullscan). Survivor
  copy-at-RC-pause deferred (survivors stay young → compacted by the trace-cycle
  `Evacuate`). D stays ⚠️.
