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
| **C** | SATB trace **spans multiple RC epochs**; completes **concurrently, no STW finish** (checked at next RC pause); invariant: RC may never delete an unmarked object mid-trace → **mark+scan any mature object RC kills** if not already marked (§3.2.3) | **LANDED & VERIFIED (2026-07-24)** (env `LXR_MULTIEPOCH`): the trace is now structured as **snapshot piggybacked on an RC pause → background marker across subsequent RC epochs → finalize at a later RC pause** — **no dedicated trace pauses**. A persistent marker thread (`LXRMarkerThreadProc`) runs `ConcurrentTraceDrain` + off-pause `ProcessSnapshotDecrements` while mutators (and spanned RC pauses) run; the driver only *checks* `g_markerQuiescent` at each RC pause and finalizes (`meFinish`: residual SATB + allocate-black + final root rescan + mark-authoritative sweep) when the marker has parked, so the finish is a brief **RC-pause-piggybacked** finalize, not a separate STW trace pause. The **no-delete-unmarked-mid-trace** invariant is enforced by **reclamation deferral** (`g_traceWindowOpen` gates ALL decommit/reuse — nursery, chunk, free-run, sweep — off until the finish), which is the paper's sound-deletion rule realised as deferral rather than eager mark-on-death (equivalent soundness; only the collector mutates RC, and no RC-zero object is freed until the trace completes). Snapshot uses the cheap `SnapshotModifiedBuffers` detach (RC arithmetic off-pause on the marker); spanned RC epochs skip buffer processing to keep the outstanding snapshot's root-deferral rotation intact, draining at the finish. Verified: snapshot pauses **1.4–8.4 ms** (was 27–31 ms before the detach fix; matches the legacy concurrent snapshot), `LXR_VERIFY_TRACE` **offenders=0 / gap=0** every finish, **8/8 WebApi iters av=0/hang=0**, ~70 MB reclaimed/run. NOTE: on WebApi the 16-thread parallel marker drains the closure *within one* RC-pause gap, so `spans=0` in practice (no epoch is actually spanned) — the spanning **mechanism** is present and correct; the paper spans only when marking outlasts the pause interval. | ✅ |
| **D** | **Implicitly-dead young**: young objects with no increment are reclaimed **at the RC pause**, before decrements; young survivors (0→1 increment) **copied at that pause** to defragment (§2.1, §3.3.1–3) | **LANDED & VERIFIED (2026-07-24)** (`CollectNursery`, env `LXR_NURSERY`): at each RC pause it reclaims young regions in which every young object is implicitly dead (RC 0, unmarked), region-granular Immix-style, honoring interior/byref roots + mark bits. **True root cause of the earlier AV found and FIXED (not masked):** the per-thread coalescing **modified buffer overflowed** under load and the barrier **dropped the first-log entry** on a full buffer — a dropped first-log **permanently loses an RC increment**, so a live young referent stayed stuck at RC 0 and the nursery freed it while reachable. Proven by an authoritative from-roots probe (`LXR_NURSERY_ROOTPROBE`): with a huge buffer `victimsReachableFromRoots=0` across all pauses, with the normal 4096 buffer up to **1760 live victims**. Fix = the paper's **shared-queue** buffer (§3.2.1): on fill the barrier CAS-swaps in a pre-registered spare from a global free-list (topped up off-barrier in `EnsureThreadBuffers`, recycled at each RC-pause drain), so **no first-log is ever dropped**; the vanishingly-rare free-list-exhaustion case sets `g_youngRCIncomplete` (nursery defers reclaim until the next complete trace re-establishes liveness). Also fixed a latent stale-logged-bit bug (`ClearLoggedRange` at all reclaim sites). Verified with normal buffers: `victimsReachableFromRoots=0` across all pauses; **8/8 concurrent + 6/6 STW WebApi iters av=0/hang=0**, ~75 MB young reclaimed/run (605 MB + 450 MB totals). No runtime change needed. Young-survivor copy-at-RC-pause still deferred (survivors stay young, compacted by the trace-cycle `Evacuate`). | ✅ |
| **E** | Triggers: RC = heap-full OR increment-threshold OR **young-survival predictor** (1:3 biased exp decay, 128 MB default); SATB = free-block threshold OR **wastage predictor** (live-block, 1:3 decay, 5% default) (§3.2.2, §3.2.5) | **LANDED & VERIFIED (2026-07-24)** (env `LXR_INCREMENT_TRIGGER`, `LXR_WASTAGE_PCT`, default-ON): the symmetric survival EWMA (7:1) is replaced by the paper's **biased/asymmetric exponential decay** — react fast to a rising signal (¾ new + ¼ old), decay slowly on a falling one (¼ new + ¾ old) — for **both** the survival predictor (paces the trace cadence via the `DecidePhase` epoch cap) and a new **wastage predictor** (`WastagePctEwma` = floating garbage each trace recovers; projects growth·wastage% ≥ 5%·committed → trace). Added the **increment-count RC trigger** (paper §3.2.2): fire an RC pause once `ModifiedBufferEntries − g_incrementBaseline ≥ 2M` (reuses the existing coalescing counter — **zero new hot-path cost**), so a mutation-heavy/allocation-light phase still gets RC processed promptly. **Latency-cliff FIXED (paper §3.2.5 lazy decrements):** the E cadence exposed a multi-second STW finish where `meFinish` replayed the whole marking window's RC + free cascade synchronously; fixed structurally by (a) a **cheap flat O(entries) buffer detach** at the pause (`SnapshotModifiedBuffers` no longer builds the coalescing hash-map — that moves **off-pause** into `ProcessSnapshotDecrements`), (b) replaying the finish epoch's RC arithmetic + recursive free **off-pause** after RestartEE (lazy decrements, sound via `ClearRCRange`+committed-`VirtualQuery` guards), and (c) the marker **requesting prompt finalize** on quiescence so the finish epoch is the drain window, not the wait-for-next-trigger interval. Verified: multi-second cliff → **true STW finish pause 8–45 ms** (off-pause replay 7–133 ms), `LXR_VERIFY_TRACE` **offenders=0/gap=0**, **11/11 WebApi iters av=0/hang=0** (full unified config), legacy-concurrent + E-off regressions clean. Free-block-count SATB trigger still folded into the wastage predictor (equivalent). | ✅ |
| **F** | Remsets **scoped to the evacuation set**, bootstrapped by the first SATB trace, kept updated by the barrier, **line-reuse-counter** stale-entry tagging; evac set = blocks <50% occupancy, N lowest; **incremental, time-budgeted**, STW (§3.3.4) | **LANDED & VERIFIED (2026-07-25).** Three parts, all paper-faithful: **(F1) N-lowest-occupancy evac set** — gather every region ≥`LXR_EVAC_FRAG_PCT` dead, sort by occupancy ratio (live/total) ascending, take the most-fragmented first under the byte budget (max compaction per live byte), not first-over-threshold. **(F2) time-budgeted incremental copy** (`LXR_EVAC_BUDGET_MS`, default 20 ms) checked only at region **boundaries** so a region is never left half-moved (dangling). **(F3) trace-bootstrapped, evac-scoped fix-up** — the O(live-heap) whole-heap field-fix-up walk is **eliminated**: during the evac cycle's STW mark (which already scans every live object's fields once) each mark lane logs the **inter-block reference slots** it sees into its own thread-local vector (lock-free; registry touched once/thread). The mark runs in the SAME pause immediately before `Evacuate()`, so the union is a **complete remembered set of live inter-block edges** at evac time. Fix-up = (a) scan moved objects' destination copies (outgoing edges), (b) replay the recorded inter-block slots (incoming edges from non-moved referrers, skipping slots inside a moved source), (c) scan the evac regions' **in-place survivors** — pinned/alloc-failed objects that hold *intra*-block edges to same-block neighbours that DID move (evac is per-OBJECT). All three are bounded by the evac-set size, NOT the heap. Automatic full-walk **fallback** on lane-log overflow or a conservative-keep-alive cycle (objects marked without a field scan). Verified: 11/11 WebApi iters full config `LXR_VERIFY_TRACE` **[verify-evac] misses=0, av=0**, `fixup=scoped fullwalk=0`; the (c) gap (16-17 unforwarded pinned-referrer intra-block edges) was **root-caused and fixed**, not masked. `EnumerateRemsetSlots` per-interval remset still consumed by the nursery (D). No runtime change. Envs `LXR_EVAC_BUDGET_MS`, `LXR_EVAC_SCOPED_FIXUP` (A/B). | ✅ |
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
4. **C —** multi-epoch SATB + mark-on-RC-death; drop the STW finish closure. ✅ **DONE**
   (env `LXR_MULTIEPOCH`: snapshot piggybacks an RC pause → background marker across
   epochs → finalize at a later RC pause; no dedicated trace pauses; invariant held by
   `g_traceWindowOpen` reclamation-deferral. Spanning mechanism present; `spans=0` on
   WebApi because the parallel marker drains within one pause gap.)
5. **E —** biased survival + wastage predictors + increment-count RC trigger. ✅ **DONE**
   (env `LXR_INCREMENT_TRIGGER`/`LXR_WASTAGE_PCT`; asymmetric ¾/¼ decay; the E-induced
   multi-second finish cliff fixed structurally via cheap flat pause-detach + off-pause
   lazy decrements + prompt marker-requested finalize → true STW finish 8–45 ms.)
6. **F —** evac-set scoping (N-lowest-occupancy) + incremental time-budgeted evac +
   trace-bootstrapped evac-scoped fix-up (O(live-heap) walk eliminated). ✅ **DONE**
   (envs `LXR_EVAC_BUDGET_MS`, `LXR_EVAC_SCOPED_FIXUP`; verified [verify-evac] misses=0).
7. **G —** parallel RC + very-large reference-array partitioning (§3.5).

Only after 1–4 (identity-restoring) land do the full benchmarks become a faithful
LXR-vs-Server/Workstation comparison. **1–4 are now all ✅** (A `f9f7031`, B `2599d3f`,
D `d7a3b78`, C multi-epoch); remaining divergences E/F/G are precision/perf refinements.

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
`LXR_YOUNG_RC=1 LXR_NURSERY=1 LXR_MULTIEPOCH=1 LXR_GC_THREADS=<#cores>` + `DOTNET_ReadyToRun=0`.
vs Server GC and Workstation GC. Regenerate `results/report.html`.

---

## Changelog
- **2026-07-25** — **F evacuation refinements landed → F ✅.** Three paper-faithful
  parts (§3.3.4). **F1 N-lowest-occupancy evac set:** gather every region ≥
  `LXR_EVAC_FRAG_PCT` dead, sort by occupancy ratio ascending, take the most-
  fragmented first under the byte budget (max compaction per live byte copied),
  replacing first-over-threshold. **F2 time-budgeted incremental copy:**
  `LXR_EVAC_BUDGET_MS` (default 20 ms) checked only at region **boundaries** so a
  region is never left half-moved (which would dangle live un-forwarded objects).
  **F3 trace-bootstrapped, evac-scoped fix-up:** the O(live-heap) whole-heap field-
  fix-up walk in `Evacuate()` step 4 is **eliminated**. During an evac cycle's STW
  backup-trace mark (which already scans every live object's fields exactly once),
  each mark lane logs the **inter-block reference slots** it scans into its own
  thread-local vector (`RecordEvacEdge`, lock-free hot path; the registry is touched
  once per thread). Because the mark runs in the SAME pause immediately before
  `Evacuate()` with no mutator in between, the union of the lane logs is a
  **complete remembered set of every live inter-block edge** at evac time (the
  paper's trace-bootstrapped remset). Fix-up is then scoped to three bounded passes:
  **(a)** scan the moved objects' destination copies (outgoing edges); **(b)** replay
  the recorded inter-block slots (incoming edges from non-moved referrers, skipping
  any slot inside a moved source via binary search on `movedRanges`); **(c)** scan
  the evac regions' **in-place survivors** (pinned or alloc-failed objects). Pass (c)
  was added after `[verify-evac]` caught 16-17 unforwarded refs on ~2/5 runs: since
  evacuation is **per-OBJECT**, a pinned object stays in place while its same-block
  neighbour moves, leaving an *intra*-block edge that the (inter-block) remembered
  set deliberately excludes — root-caused and fixed, not masked. Automatic full-walk
  fallback on lane-log overflow or a conservative-keep-alive cycle (objects marked
  without a field scan → their out-edges never recorded). Verified: 11/11 WebApi
  iters full unified config, `LXR_VERIFY_TRACE` **[verify-evac] misses=0, av=0**,
  `fixup=scoped fullwalk=0`, ops nominal. No runtime change. New envs
  `LXR_EVAC_BUDGET_MS`, `LXR_EVAC_SCOPED_FIXUP` (=0 forces the full walk for A/B).

  Replaced the symmetric survival EWMA (7:1) with the paper's **biased/asymmetric
  exponential decay** (¾ new + ¼ old on a rising signal, ¼ new + ¾ old on a falling
  one) for both the **survival** predictor (paces trace cadence) and a new **wastage**
  predictor (`WastagePctEwma`, projects floating garbage → SATB trace at 5%). Added the
  **increment-count RC trigger** (§3.2.2): fires an RC pause at ≥2M coalescing-buffer
  entries since the last pause (reuses the existing counter — zero new barrier cost).
  Envs `LXR_INCREMENT_TRIGGER` / `LXR_WASTAGE_PCT`, default-ON. **The aggressive E
  cadence exposed a multi-second STW finish cliff** (`meFinish` synchronously replayed
  the whole marking-window RC + recursive free cascade). **Fixed structurally, paper-
  faithful (§3.2.5 lazy decrements) — not with a magic constant:** (a) `SnapshotModified-
  Buffers` now does a **cheap flat O(entries) detach** (the coalescing hash-map moved
  **off-pause** into `ProcessSnapshotDecrements`; `RCSnapshotEntry` carries `Slot`);
  (b) `meFinish` replays the finish epoch's RC + free cascade **off-pause after
  RestartEE** (sound over swept/decommitted chunks via the existing `ClearRCRange` +
  committed-`VirtualQuery` guards, commit 80b6ffb); (c) the marker **requests a prompt
  finalize** the moment it quiesces, so the finish epoch is just the drain window rather
  than stretching to the next allocation trigger. Result: multi-second cliff → **true
  STW finish pause 8–45 ms** (off-pause replay 7–133 ms), 5 traces/run (was 3). This
  also improves item C's finish pauses (same shared machinery). Verified: `LXR_VERIFY_
  TRACE` offenders=0/gap=0, **11/11 WebApi iters av=0/hang=0** (full unified config),
  legacy-concurrent + E-off regressions clean. No runtime change needed.
- **2026-07-24** — **C multi-epoch SATB trace landed → C ✅.** Restructured the
  concurrent trace so it **piggybacks on the RC-pause cadence** instead of running two
  dedicated trace pauses inside one monolithic collection call (env `LXR_MULTIEPOCH`):
  a **snapshot** rides an RC pause (cheap `SnapshotModifiedBuffers` detach), a
  **persistent background marker thread** (`LXRMarkerThreadProc`) marks the closure +
  replays RC decrements off-pause across the following RC epochs, and a later RC pause
  **finalizes** (`meFinish`: residual SATB + allocate-black + final root rescan +
  mark-authoritative sweep) once the marker parks. No dedicated trace pauses; the
  no-delete-unmarked-mid-trace invariant is held by `g_traceWindowOpen` reclamation
  deferral (the paper's sound-deletion rule as deferral). Spanned RC epochs skip
  `ProcessModifiedBuffers` so the outstanding snapshot's root-deferral rotation stays
  intact (drained at finish). **Fixed a snapshot-pause regression** (27–31 ms → 1.4–8.4
  ms) by using the detach + off-pause replay instead of full STW `ProcessModifiedBuffers`
  at the snapshot. Verified: `LXR_VERIFY_TRACE` offenders=0/gap=0 every finish, **8/8
  WebApi iters av=0/hang=0**, ~70 MB reclaimed/run. `spans=0` on WebApi (the 16-thread
  parallel marker drains within one pause gap) — the spanning mechanism is present and
  correct; the paper spans only when marking outlasts the pause interval. No runtime
  change needed.
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
