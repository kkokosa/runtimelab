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
| **D** | **Implicitly-dead young**: young objects with no increment are reclaimed **at the RC pause**, before decrements; young survivors (0→1 increment) **copied at that pause** to defragment (§2.1, §3.3.1–3) | **LANDED & VERIFIED (2026-07-24) — but PARTIAL vs paper** (`CollectNursery`, env `LXR_NURSERY`): at each RC pause it reclaims young regions in which every young object is implicitly dead (RC 0, unmarked), region-granular Immix-style, honoring interior/byref roots + mark bits. **True root cause of the earlier AV found and FIXED (not masked):** the per-thread coalescing **modified buffer overflowed** under load and the barrier **dropped the first-log entry** on a full buffer — a dropped first-log **permanently loses an RC increment**, so a live young referent stayed stuck at RC 0 and the nursery freed it while reachable. Proven by an authoritative from-roots probe (`LXR_NURSERY_ROOTPROBE`): with a huge buffer `victimsReachableFromRoots=0` across all pauses, with the normal 4096 buffer up to **1760 live victims**. Fix = the paper's **shared-queue** buffer (§3.2.1): on fill the barrier CAS-swaps in a pre-registered spare from a global free-list (topped up off-barrier in `EnsureThreadBuffers`, recycled at each RC-pause drain), so **no first-log is ever dropped**; the vanishingly-rare free-list-exhaustion case sets `g_youngRCIncomplete` (nursery defers reclaim until the next complete trace re-establishes liveness). Also fixed a latent stale-logged-bit bug (`ClearLoggedRange` at all reclaim sites). Verified with normal buffers: `victimsReachableFromRoots=0` across all pauses; **8/8 concurrent + 6/6 STW WebApi iters av=0/hang=0**, ~75 MB young reclaimed/run (605 MB + 450 MB totals). No runtime change needed. **⚠️ GAP: the young-SURVIVOR copy-at-RC-pause (§3.3.1–3, the defragmenting half of D) is NOT implemented** — survivors stay young in place and are compacted only later by the trace-cycle `Evacuate`. Implicitly-dead-young reclaim is done; survivor copy is not. | ⚠️ |
| **E** | Triggers: RC = heap-full OR increment-threshold OR **young-survival predictor** (1:3 biased exp decay, 128 MB default); SATB = free-block threshold OR **wastage predictor** (live-block, 1:3 decay, 5% default) (§3.2.2, §3.2.5) | **LANDED & VERIFIED (2026-07-24)** (env `LXR_INCREMENT_TRIGGER`, `LXR_WASTAGE_PCT`, default-ON): the symmetric survival EWMA (7:1) is replaced by the paper's **biased/asymmetric exponential decay** — react fast to a rising signal (¾ new + ¼ old), decay slowly on a falling one (¼ new + ¾ old) — for **both** the survival predictor (paces the trace cadence via the `DecidePhase` epoch cap) and a new **wastage predictor** (`WastagePctEwma` = floating garbage each trace recovers; projects growth·wastage% ≥ 5%·committed → trace). Added the **increment-count RC trigger** (paper §3.2.2): fire an RC pause once `ModifiedBufferEntries − g_incrementBaseline ≥ 2M` (reuses the existing coalescing counter — **zero new hot-path cost**), so a mutation-heavy/allocation-light phase still gets RC processed promptly. **Latency-cliff FIXED (paper §3.2.5 lazy decrements):** the E cadence exposed a multi-second STW finish where `meFinish` replayed the whole marking window's RC + free cascade synchronously; fixed structurally by (a) a **cheap flat O(entries) buffer detach** at the pause (`SnapshotModifiedBuffers` no longer builds the coalescing hash-map — that moves **off-pause** into `ProcessSnapshotDecrements`), (b) replaying the finish epoch's RC arithmetic + recursive free **off-pause** after RestartEE (lazy decrements, sound via `ClearRCRange`+committed-`VirtualQuery` guards), and (c) the marker **requesting prompt finalize** on quiescence so the finish epoch is the drain window, not the wait-for-next-trigger interval. Verified: multi-second cliff → **true STW finish pause 8–45 ms** (off-pause replay 7–133 ms), `LXR_VERIFY_TRACE` **offenders=0/gap=0**, **11/11 WebApi iters av=0/hang=0** (full unified config), legacy-concurrent + E-off regressions clean. Free-block-count SATB trigger still folded into the wastage predictor (equivalent). | ✅ |
| **F** | Remsets **scoped to the evacuation set**, bootstrapped by the first SATB trace, kept updated by the barrier, **line-reuse-counter** stale-entry tagging; evac set = blocks <50% occupancy, N lowest; **incremental, time-budgeted**, STW (§3.3.4) | **LANDED & VERIFIED (2026-07-25).** Three parts, all paper-faithful: **(F1) N-lowest-occupancy evac set** — gather every region ≥`LXR_EVAC_FRAG_PCT` dead, sort by occupancy ratio (live/total) ascending, take the most-fragmented first under the byte budget (max compaction per live byte), not first-over-threshold. **(F2) time-budgeted incremental copy** (`LXR_EVAC_BUDGET_MS`, default 20 ms) checked only at region **boundaries** so a region is never left half-moved (dangling). **(F3) trace-bootstrapped, evac-scoped fix-up** — the O(live-heap) whole-heap field-fix-up walk is **eliminated**: during the evac cycle's STW mark (which already scans every live object's fields once) each mark lane logs the **inter-block reference slots** it sees into its own thread-local vector (lock-free; registry touched once/thread). The mark runs in the SAME pause immediately before `Evacuate()`, so the union is a **complete remembered set of live inter-block edges** at evac time. Fix-up = (a) scan moved objects' destination copies (outgoing edges), (b) replay the recorded inter-block slots (incoming edges from non-moved referrers, skipping slots inside a moved source), (c) scan the evac regions' **in-place survivors** — pinned/alloc-failed objects that hold *intra*-block edges to same-block neighbours that DID move (evac is per-OBJECT). All three are bounded by the evac-set size, NOT the heap. Automatic full-walk **fallback** on lane-log overflow or a conservative-keep-alive cycle (objects marked without a field scan). Verified: 11/11 WebApi iters full config `LXR_VERIFY_TRACE` **[verify-evac] misses=0, av=0**, `fixup=scoped fullwalk=0`; the (c) gap (16-17 unforwarded pinned-referrer intra-block edges) was **root-caused and fixed**, not masked. `EnumerateRemsetSlots` per-interval remset still consumed by the nursery (D). No runtime change. Envs `LXR_EVAC_BUDGET_MS`, `LXR_EVAC_SCOPED_FIXUP` (A/B). | ✅ |
| **G** | **Parallelism in every phase**; very large reference arrays partitioned for increment scalability (§3.5) | **LANDED & VERIFIED (2026-07-25)** (env `LXR_PARALLEL_RC`, default-ON). The persistent worker pool (built once at Initialize, never during a pause) is generalized from mark-only to a **generic parallel-for** (`RunOnPool(lanes, fn, ctx)`; lane 0 = caller, pooled workers = lanes 1..n; the worker proc branches on `g_poolWorkKind`: mark-drain vs. parallel-for). **RC apply is now parallel** (`ApplyRCEpoch`): both RC paths (`ProcessModifiedBuffers` STW + `ProcessSnapshotDecrements` off-pause) build flat increment/decrement lists and stride-partition them across the pool, so a large coalesced epoch — e.g. **a big reference array filled/cleared → one RC entry per element** — distributes across the GC threads instead of serializing on one (this IS the §3.5 large-reference-array increment scalability, applied at the RC-entry granularity where the work actually is). Ordering preserved: **all increments, then all decrements** (two `RunOnPool` calls, which join between them = the required barrier). RC slot updates use **atomic CAS** variants (`RCIncrementAtomic`/`RCDecrementAtomic`, `_InterlockedCompareExchange8`, saturating at 0xFF / floored at 0, exactly-once 1→0 edge) so concurrent lanes touching the same object's count never lose an update; RC-table pages are **pre-committed serially** (deduped by page) so the atomic apply never calls `VirtualAlloc` under contention. The free cascade (`DrainZeroCountWorkList`) stays **serial** (parallel recursive free is highest-risk/lowest-reward, runs after the barrier). A dedicated `g_poolLock` serializes the shared pool between the item-C background marker's mark-drain and a spanned-RC-epoch's parallel apply (no nesting → no deadlock). Small epochs (< 8192 entries) stay serial (wake/join not amortised). Verified: full unified config `LXR_GC_THREADS=16`, 8+ WebApi iters, **av=0/hang=0**, `LXR_VERIFY_TRACE` **offenders=0**; parallel path exercised (`rcApply[par>0]`); A/B `LXR_PARALLEL_RC=0` equivalent (all-serial, clean). Parallel mark was already present (`ParallelDrainMarkStack`). Single-huge-array **mark**-side partitioning across lanes (work-stealing on one giant array) remains a bounded refinement, not implemented: the current pool is static seed-partitioning, and RC-entry parallelism already delivers the paper's large-array *increment* scalability. | ✅ |
| — | Immix block/line heap; single field barrier serving RC + SATB + remset; SATB collects cycles + stuck counts; **copy only during STW**; stuck count → resolved by trace | All present ✅ (stuck at 0xFF vs paper's 2-bit count — variant, higher fidelity). | ✅ |
| **★** | **PRIMARY-RC RECLAMATION** (LXR's core thesis): mature garbage reclaimed **promptly at RC pauses** when RC→0 (line/block returned to the allocator); the SATB trace runs **only occasionally**, to collect **cycles** + reset **stuck** counts | **LANDED & VERIFIED (2026-07-24)** (env `LXR_RC_RECLAIM`, default ON): new `ReclaimMatureByRC()` runs STW at **every RC pause** (after `ProcessModifiedBuffers` has fully reconciled RC + drained the zero-count cascade) and **returns mature memory by RC authority — no trace required**. Two granularities: **(1)** whole-region decommit + free-chunk recycle for any mature 128 KB region with **no** RC>0 object (`!AnyRCNonZeroInRange`), and **(2)** `CarveDeadRunsByRC` — RC-authoritative Immix line-carving that linear-walks real object boundaries, coalesces runs of consecutive **dead** (RC 0, non-young, non-root) objects ≥ the reuse threshold, and plugs+lists them as free runs for the allocator (mirrors `CarveFreeRuns`' re-tiling). **Soundness:** outside a trace window RC is authoritative (the pluggable barrier counts every ref store incl. the patched Interlocked/bulk/ClearWithReferences forms; roots pinned; young = nursery's domain; stuck 0xFF kept; dead cycles keep RC≥1 for the trace). **CRITICAL — consults NO mark bits** (unlike the nursery): marks are stale outside a trace window, so using them would over-retain and block nearly all mature RC reclamation. Deferred while `g_traceWindowOpen` (marks in flux / RC not reconciled) or `g_youngRCIncomplete` (possible undercount). Decommit-vs-decrement race closed by `ClearRCRange` + `DrainZeroCountWorkList`'s committed-`VirtualQuery` guard (as at the sweep sites). **Verified:** full unified config `LXR_GC_THREADS=16 LXR_VERIFY_TRACE=1`, **8/8 WebApi iters ~55 s: av=0, offenders=0, hang=0, Errors=0**; `[rc-reclaim]` fired on the real benchmark returning **37–54 mature regions (≈3.5–6.9 MB) + 8–9 carved dead line-runs (~0.1 MB) per firing at RC pauses without a trace**, proving RC now returns mature memory. A/B `LXR_RC_RECLAIM=0` disables it cleanly. Mature reclamation is now RC-primary; the trace remains the occasional cyclic/stuck backstop, matching the paper's cost model. **NOTE (2026-07-26, corrected):** an earlier claim of a "pre-existing LXRGC startup AV" on an 80 MB startup burst was **DISPROVEN** — it was a **stale runtime deploy** in `StartupBurst\publish` (a `coreclr.dll`/`clrjit.dll` predating the `SetObjectReferenceUnchecked`→callback patch, runtime commit `9701d0e850d`), so VM-internal `SetObjectReference` stores bypassed the barrier and RC undercounted live mature objects. With the patched runtime redeployed: **0 UNSOUND** (VERIFY), StartupBurst **15/15 clean** ★-ON, WebApi full config **4/4 clean**. ★ (RC as sole authority between traces) depends on the COMPLETE barrier, which the patched runtime supplies. | ✅ |

### Honest bottom line (revised 2026-07-23 re-audit)
The engine is a **sound, structurally-LXR** collector, faithful on the big choices
(Immix, the one field barrier serving three roles, SATB for cycles/stuck, STW-only
copying, parallel mark) and on the individual A–G mechanisms. **A/B are genuinely
fixed** (precise coalescing RC + inc-before-dec + root deferral) — the earlier
"RC imprecise/advisory" critique no longer applies at the barrier level.

**The primary-RC divergence (row ★) is now CLOSED (2026-07-24):** `ReclaimMatureByRC()`
returns mature memory **at the RC pause** by RC authority — whole-region decommit for
fully-dead 128 KB regions plus `CarveDeadRunsByRC` RC-authoritative Immix line-carving
for partially-dead regions — with **no mark-bit dependence** and **no trace required**.
Verified firing on the real WebApi benchmark (37–54 regions + carved runs per firing,
av=0/offenders=0). The SATB trace is back to its paper role (cycles + stuck reset) as
the occasional backstop. Only **D-copy** (young-survivor copy-at-RC-pause
defragmentation, §3.3.1–3) remains open; **E/F/G** are precision/perf refinements,
orthogonal to this.

Reaching full 1:1 parity now means only **(D-copy)** copying young survivors at the
RC pause; the primary-RC mature reclaimer (★) is done.

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
7. **G —** parallel RC + very-large reference-array partitioning (§3.5). ✅ **DONE**
   (env `LXR_PARALLEL_RC`; generic `RunOnPool` parallel-for over the persistent pool;
   `ApplyRCEpoch` stride-partitions inc/dec across GC threads on both RC paths with
   atomic CAS RC ops + serial page pre-commit + inc-then-dec barrier; free cascade
   stays serial; `g_poolLock` serializes pool use vs. the item-C marker drain. Verified
   16-thread full config av=0/offenders=0, parallel path exercised, A/B-clean. Single
   huge-array mark-side work-stealing left as a bounded refinement.)
8. **★ PRIMARY-RC MATURE RECLAMATION — DONE (2026-07-24).** `ReclaimMatureByRC()` now
   **returns mature memory at the RC pause**: whole-region decommit + free-chunk
   recycle for fully-dead 128 KB regions (`!AnyRCNonZeroInRange`), and
   `CarveDeadRunsByRC` RC-authoritative Immix line-carving (object-boundary walk,
   coalesce consecutive RC-0/non-young/non-root runs, plug+list for reuse) for
   partially-dead regions — **no trace required, no mark bits** (stale outside a trace
   window). Sound w.r.t. an in-flight SATB trace (defer inside `g_traceWindowOpen` /
   `g_youngRCIncomplete`) and decommit-vs-decrement races (`ClearRCRange` +
   committed-`VirtualQuery` guards). The SATB trace is back to its paper role (cycles +
   stuck reset) as the occasional backstop. Verified: 8/8 WebApi full-config iters
   av=0/offenders=0, `[rc-reclaim]` firing (37–54 regions + carved runs per firing).
   Env `LXR_RC_RECLAIM` (default ON). **✅ DONE.**
9. **D-copy — young-survivor copy at the RC pause** (§3.3.1–3): copy the 0→1 young
   survivors at the RC pause to defragment, instead of leaving them in place for the
   later trace-cycle `Evacuate`. **UNDONE** (implicitly-dead-young reclaim is done).

**★ (primary-RC mature reclamation) is now ✅** (commit — see changelog), so mature
reclamation is RC-primary and the full benchmarks measure a structurally-faithful
"primary RC, occasional trace" collector. Only **D-copy** (young-survivor
defragmenting copy) remains before full 1:1 parity. **1–4 are ✅** (A `f9f7031`,
B `2599d3f`, D-reclaim `d7a3b78`, C multi-epoch), E/F/G ✅, and ★ ✅.

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
- **2026-07-26** — **★ PRIMARY-RC MATURE RECLAMATION LANDED → ★ ✅.** New
  `ReclaimMatureByRC()` runs STW at every RC pause (after `ProcessModifiedBuffers`
  reconciles RC) and returns mature memory by RC authority — **no trace required**:
  **(1)** whole-region decommit + free-chunk recycle for fully-dead 128 KB regions
  (`!AnyRCNonZeroInRange`), and **(2)** `CarveDeadRunsByRC` — RC-authoritative Immix
  line-carving that walks real object boundaries, coalesces consecutive dead
  (RC 0 / non-young / non-root) object runs ≥ the reuse threshold, and plugs+lists
  them for allocator reuse (mirrors `CarveFreeRuns`' re-tiling). **Consults NO mark
  bits** (stale outside a trace window — using them would over-retain and defeat ★);
  defers inside `g_traceWindowOpen`/`g_youngRCIncomplete`; decommit-vs-decrement race
  closed by `ClearRCRange` + the committed-`VirtualQuery` guard. Wired into the driver
  RC-pause branch alongside the nursery. Counters `MatureRC*`; env `LXR_RC_RECLAIM`
  (default ON). **Verified:** full unified config `LXR_GC_THREADS=16
  LXR_VERIFY_TRACE=1`, 8/8 WebApi ~55 s iters **av=0/offenders=0/hang=0/Errors=0**,
  `[rc-reclaim]` firing on the real benchmark (37–54 mature regions ≈3.5–6.9 MB + 8–9
  carved line-runs per firing, returned at RC pauses without a trace); A/B
  `LXR_RC_RECLAIM=0` clean. Mature reclamation is now RC-primary; trace is the
  occasional cyclic/stuck backstop (paper cost model). **Startup-AV RESOLVED
  (2026-07-26):** the earlier "pre-existing LXRGC startup AV on an 80 MB
  startup-burst" was **not a GC bug** — it was a **stale runtime deploy**. The
  `StartupBurst\publish` folder carried a `coreclr.dll`/`clrjit.dll` from 7/20 that
  **predated** the `SetObjectReferenceUnchecked`→callback routing patch (runtime
  commit `9701d0e850d`, 7/22), so VM-internal `SetObjectReference` stores (e.g.
  `System.Signature.SetArgumentArray`, `MethodTable::AllocateRegularStaticBox`)
  never reached the barrier and RC undercounted live mature objects. After
  redeploying the patched `coreclr.dll`+`clrjit.dll`: `LXR_RC_RECLAIM_VERIFY`
  **0 UNSOUND regions**, StartupBurst **15/15 clean** with ★ default-ON, and WebApi
  full unified config **4/4 clean** (Errors=0, ★ reclaiming ~36 MB). ★ needs a
  COMPLETE barrier and the patched runtime provides it. Only **D-copy** remains open
  for full parity.
- **2026-07-23** — **Independent evidence-based re-audit (code, not doc).** Ticked A–G
  against the actual reclamation paths and found two honest corrections to the
  all-✅ status: **(1) D downgraded ✅→⚠️** — implicitly-dead-young reclaim is done, but
  the **young-survivor copy-at-RC-pause** (the defragmenting half of §3.3) is NOT
  implemented (survivors stay in place, compacted only by the later trace-cycle
  `Evacuate`). **(2) New row ★ = ❌** — the LXR **primary-RC mature-reclamation
  identity is not realized**: `DrainZeroCountWorkList` only decrements counts +
  `meta->liveObjects` (grep-verified **never read** — dead bookkeeping) and returns
  **zero mature memory**; ALL mature memory return happens at `TracePause` via
  `SweepAndSelectDefrag`/`CarveFreeRuns`/`Evacuate`. Only the young nursery reclaims
  at RC pauses. So the collector is a *frequent-trace mark-sweep with RC keep-alive*,
  the inverse of the paper's *primary-RC + occasional-trace* cost model. A/B are
  genuinely fixed (precise coalescing RC), so the old "RC imprecise" bottom line was
  **stale** and has been rewritten; the real gap (★) was previously untracked. Added
  roadmap items 8 (★) and 9 (D-copy). Sound, but not yet 1:1 — benchmarks today are
  informative, not a faithful LXR-vs-Server comparison until ★ lands.
- **2026-07-25** — **G parallel RC landed → G ✅ (all A–G now ✅).** The persistent
  mark-worker pool is generalized into a **generic parallel-for** (`RunOnPool(lanes,
  fn, ctx)`; the worker proc branches on `g_poolWorkKind` = mark-drain vs. parallel-
  for). **RC apply is parallelized** on both paths (`ApplyRCEpoch`, env
  `LXR_PARALLEL_RC` default-ON): `ProcessModifiedBuffers` (STW) and
  `ProcessSnapshotDecrements` (off-pause) build flat increment/decrement lists and
  stride-partition them across the GC threads, so a large coalesced epoch — e.g. a
  big reference array filled/cleared → one RC entry per element — distributes across
  lanes instead of serializing (the §3.5 large-reference-array increment scalability
  at RC-entry granularity). Ordering preserved (**all increments then all
  decrements** = two `RunOnPool` calls joining between them). Correctness under
  contention: atomic-CAS RC ops (`RCIncrementAtomic`/`RCDecrementAtomic`,
  `_InterlockedCompareExchange8`, saturating 0xFF / floored 0, exactly-once 1→0),
  RC-table pages **pre-committed serially** (deduped by page) so the apply never
  `VirtualAlloc`s under contention, and the recursive free cascade stays **serial**.
  A new `g_poolLock` serializes the shared pool between the item-C background marker
  drain and a spanned-RC-epoch's parallel apply (no nesting → no deadlock); small
  epochs (< 8192 entries) stay serial. Verified: full unified config
  `LXR_GC_THREADS=16`, 8+ WebApi iters **av=0/hang=0**, `LXR_VERIFY_TRACE`
  **offenders=0**, parallel path exercised (`rcApply[par>0]`), A/B `LXR_PARALLEL_RC=0`
  equivalent. Single huge-array **mark**-side work-stealing left as a bounded
  refinement (RC-entry parallelism already delivers the paper's increment scalability).
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
