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
| **A** | **Coalescing field-logging barrier**: per-field **unlogged bit**; log each field **once per epoch** (first write pushes old→`decbuf`, addr→`modbuf`); ignore intermediate referents; unlogged bit reset at RC pause; new objects born "logged" so barrier elides young (§3.4, Fig.3) | **A(i) landed** (`parity-a1`): both RC paths now **coalesce per field at processing time** — one decrement of the first-logged old value + one increment of the final `*slot` per modified field — so counts are **precise** (`ProcessModifiedBuffers`/`SnapshotModifiedBuffers`). **A(ii) pending:** barrier still logs every store (no per-field unlogged bit), so it does redundant work + can overflow the buffers → O(heap) fallback. | ⚠️ |
| **B** | RC pause applies **all increments, then all decrements** (§3.2.1); **root deferral** — increment root-reachable at tₙ, buffer matching decrement for tₙ₊₁ (§2.1) | **Ordering fixed** (`ProcessModifiedBuffers` now applies all increments before any decrement, matching the concurrent path). **Root deferral still pending** — roots kept live only by the mark trace, so RC cannot yet stand alone. | ⚠️ |
| **C** | SATB trace **spans multiple RC epochs**; completes **concurrently, no STW finish** (checked at next RC pause); invariant: RC may never delete an unmarked object mid-trace → **mark+scan any mature object RC kills** if not already marked (§3.2.3) | Single snapshot→drain→**STW finish pause** per trace cycle (`ConcurrentTraceFinish`); does not span epochs; uses a final root+handle rescan + (overflow-only) closure instead of the mark-on-RC-death rule. Sound, but structurally different. | ⚠️ |
| **D** | **Implicitly-dead young**: young objects with no increment are reclaimed **at the RC pause**, before decrements; young survivors (0→1 increment) **copied at that pause** to defragment (§2.1, §3.3.1–3) | Young excluded from RC (`IsYoung`), kept alive by trace/allocate-black, reclaimed only on a **trace/sweep** cycle; no young-survivor copy-at-RC-pause. Leans on the trace. | ⚠️ |
| **E** | Triggers: RC = heap-full OR increment-threshold OR **young-survival predictor** (1:3 biased exp decay, 128 MB default); SATB = free-block threshold OR **wastage predictor** (live-block, 1:3 decay, 5% default) (§3.2.2, §3.2.5) | Allocation-growth trigger + a single **survival EWMA (7:1)** scaling the epoch cap + epochs-since-trace. No increment/wastage/free-block predictors. | ⚠️ |
| **F** | Remsets **scoped to the evacuation set**, bootstrapped by the first SATB trace, kept updated by the barrier, **line-reuse-counter** stale-entry tagging; evac set = blocks <50% occupancy, N lowest; **incremental, time-budgeted**, STW (§3.3.4) | Remset logs **all** inter-block pointers continuously while active; evac every 4th trace cycle, STW; stale filtered by re-reading slot; evac-set selection + incrementalism differ. Sound (over-broad). | ⚠️ |
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

1. **A — precise coalescing RC** (restores LXR's core identity):
   - **A(i) correctness:** dedup per field at processing — first logged `oldValue` +
     one increment of the final `*slot` — so counts are precise even with the current
     per-store log. Small, isolated change in the RC processing loops.
   - **A(ii) full parity:** an **unlogged-bit side table** so the barrier logs each
     field **once per epoch** (matches the paper's 1.6% overhead, eliminates buffer
     bloat, and **subsumes the SATB/modified-buffer overflow → O(heap) fallback**).
2. **B —** inc-before-dec on the STW RC path + **root deferral** (RC accounts roots).
3. **D —** implicitly-dead-young reclaim + young-survivor copy at the RC pause.
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
`LXR_YOUNG_RC=1 LXR_GC_THREADS=<#cores>` + `DOTNET_ReadyToRun=0`. vs Server GC and
Workstation GC. Regenerate `results/report.html`.

---

## Changelog
- **2026-07-23** — Initial paper-conformance audit (this document). A ❌, B ❌, C/D/E/F/G ⚠️.
  Starting fix roadmap at item A.
- **2026-07-23** — **A(i) precise coalescing RC landed.** Both RC paths
  (`ProcessModifiedBuffers` STW + `SnapshotModifiedBuffers` concurrent) now coalesce
  per modified field at processing time (one dec of first-logged old value + one inc
  of final `*slot`), and `ProcessModifiedBuffers` applies all increments before any
  decrement. RC counts are now precise. A ❌→⚠️ (A(ii) unlogged bit still pending),
  B ❌→⚠️ (ordering fixed; root deferral pending). Verified: ConsoleApp reclaim smoke
  exit 0, WebApi 6/6 clean (av=0) under the full unified config.
