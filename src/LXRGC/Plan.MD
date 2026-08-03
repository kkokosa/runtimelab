# LXRGC — Execution Plan to Reach Parity with .NET GC

Derived from `LXR-VS-REFERENCE-COMPARISON.md` §13. This is the actionable, ordered work
plan; the comparison doc holds the paper/MMTk-reference/ours evidence and citations that
justify each item. **Guiding rule: paper-faithful, no shortcuts — every change grounded in
the paper (arXiv:2210.17175), the MMTk reference (`wenyuzhao/mmtk-core` @ `lxr-x/simplified`),
or measured run-data, with citations.**

## Target symptoms (from §0 of the comparison doc)

- **A1 — baseline RC pause too high.** Reference is sub-ms; ours is single-digit-to-tens of ms.
- **A2 — multi-second trace-finish spikes.** Reference finish is a few ms; ours funnels
  whole-heap allocate-black parse + whole-window RC replay + O(heap) sweep into one STW pause
  (measured 2–3.4 s via `LXR_FINISH_PROFILE`).
- **B — footprint.** Ours is 2–10× the reference; positive-feedback growth cadence.

## Fix → symptom matrix

| Fix | A1 (sub-ms pause) | A2 (no spikes) | B (footprint) |
|---|:---:|:---:|:---:|
| 1. Remove allocate-black whole-heap parse; young = block/line-RC | — | ✅ primary | ✅ |
| 2. Lazy/concurrent decrements + zero-count cascade | ✅ primary | ✅ | — |
| 3. Cycle/mature sweep off-pause | — | ✅ primary | ✅ |
| 4. Hard young-size cap (`MAX_SURVIVAL_MB` analogue) | ✅ | ✅ | ✅ |
| 5. Re-enable nursery copy (soundly) | — | — | ✅ primary |
| 6. Bounded + parallel increment processing | ✅ the sub-ms step | — | — |
| 7. (Secondary) Narrow RC to 2 bits | — | — | ✅ (metadata tax) |

No single fix delivers sub-ms; A1 = 2 + 4 + 6, A2 = 1 + 3 + 4, B = 1 + 3 + 4 + 5.

---

## Execution order & tasks

Ordered by leverage and dependency. Each task lists the reference mechanism to port, the
paper section, the code anchor(s) in `native/LXRGCHeap.cpp` / `native/LXRGC.h`, and how to
verify. **Build (~60–90 s):**
`build.ps1 -RuntimeRepo C:\github\runtime -Configuration Release` (from `native/`, vcvars64).

### Fix 1 — Eliminate allocate-black whole-heap parse; make "young" a block/line-RC property
- **Port:** `BlockState::Nursery` + RC-table semantics (ref `block.rs`, `gc_work/rc.rs
  dont_evacuate`). New objects are RC=0 in nursery blocks; reclaim by reading the RC table,
  never by walking objects.
- **Paper:** §1 "implicitly dead young objects."
- **Ours:** `ConcurrentTraceFinish:5669` (allocate-black whole-heap parse) — remove; replace
  with block-state nursery reclaim.
- **Effect:** removes the dominant O(heap) STW cost and its footprint coupling.
- **Verify:** `LXR_FINISH_PROFILE` allocblk term → ~0; A2 spikes drop; growing-cache no longer
  pause-dominated. Re-run cache/churn/webserver pause percentiles.

### Fix 2 — Lazy & concurrent decrements + zero-count free cascade (default)
- **Port:** `LAZY_DECREMENTS=true` — decs run in the `Concurrent` bucket between pauses (ref
  `mod.rs`, `gc_work/rc.rs`).
- **Paper:** §3.2.5.
- **Ours:** flip default from `g_onPauseRC` on-pause to `SnapshotModifiedBuffers` + off-pause
  replay; `ProcessModifiedBuffers:4798`.
- **Effect:** removes the measured 2–3.4 s finish-buffers spike from the pause.
- **Verify:** `[finish-prof]` buffers term drops off the pause; A1 baseline falls; soundness via
  `LXR_VERIFY_TRACE offenders=0`.

### Fix 4 — Hard young-size trigger (`MAX_SURVIVAL_MB` analogue)
- **Port:** "pause when predicted young-survival ≥ fixed cap" (ref `global.rs
  collection_required`, `mod.rs`).
- **Paper:** §3.2.2.
- **Ours:** replace/augment the growth-ratio budget at `10120-10168` (`g_incrementTrigger=32768`,
  survival/wastage triggers).
- **Effect:** breaks the positive-feedback loop; bounds per-pause work independent of heap size.
- **Verify:** footprint peak bounded on gcperfsim `-tagb 3`; collection cadence regular (not
  5–6 collections for 3 GB).

### Fix 3 — Move cycle/mature sweep off the pause
- **Port:** `SweepDeadCycles` (rc!=0 && !marked ⇒ dead) as concurrent chunk-parallel after
  FinalMark; `LazySweepingJobsCounter` page-return guarantee (ref `mature_sweeping.rs`,
  `global.rs`).
- **Paper:** §3.2.
- **Ours:** `SweepAndSelectDefrag` (O(heap) at finish) → off-pause; keep only a light FinalMark
  STW (root incs + CM-tail drain + budgeted mature-evac closure).
- **Effect:** removes O(heap) sweep from the pause; prompt reclaim → footprint.
- **Verify:** finish pause no longer scales with heap; footprint tracks live set; no
  decommit-of-reachable AV (guard with committed-validated scan).

### Fix 6 — Bound and parallelize per-pause increment processing (the sub-ms step)
- **Port:** parallel fixed-size inc packets (1024 slots/packet, work-stolen) like `ProcessIncs`
  (ref `gc_work/rc.rs`).
- **Paper:** §3.2.2 "frequent light RC pauses."
- **Ours:** lower `g_incrementTrigger` (currently 32768 ≈ ~16 ms, `LXRGCHeap.cpp:10133`);
  replace the serial `ProcessModifiedBuffers` coalesce (under `m_collectLock`) with parallel
  packets.
- **Effect:** takes the common RC pause from single-digit ms to sub-ms (2 + 4 alone leave it at
  a few ms).
- **Verify:** RC pause p50/p99 sub-ms on webapi/cache; throughput not regressed.

### Fix 5 — Re-enable nursery copy (soundly)
- **Port:** copy RC=0 nursery survivors every pause with a proper per-block remset + to-space
  pressure guard (`NO_EVAC`) (ref `gc_work/rc.rs`, `mature_evac.rs`).
- **Paper:** §3.
- **Ours:** D-copy disabled (`LXR_NURSERY_COPY=0`) for remset-completeness bugs (stored
  memories); adopt block-state + reuse-count remset validation as the sound path.
- **Effect:** directly attacks footprint (B).
- **Verify:** storm (tc4/tc16) clean, `[verify-nursery-copy]` unforwarded=0, footprint near
  Server.

### Fix 7 — (Secondary) Narrow RC to 2 bits
- **Port:** 2-bit RC saturating at 3 (ref `util/rc.rs`).
- **Paper:** §3.2.1.
- **Ours:** `RCSlot`→`uint8_t*` (`LXRGC.h:244`) → 2-bit packed table. **Only after 1–6 land.**
- **Effect:** sheds ~4× RC-metadata tax (footprint).
- **Verify:** metadata footprint drops; no RC correctness regression.

---

## Cross-cutting invariants (do not regress)

- **Soundness:** mark-and-scan-before-free SATB invariant; committed-validated object scan
  (`LXR_SCAN_SAFE`) around any concurrent reclaim; `LXR_VERIFY_TRACE offenders=0`.
- **Reclaim only when safe:** paper frees at STW RC pauses / next pause after trace; never free
  while the tracer may read (see stored reclamation-architecture memories).
- **No runtime change** unless unavoidable — if a fix requires it, STOP and surface the exact
  primitive + cost before proceeding.

## Validation harness

- Targeted: gcperfsim (cache/churn/webserver), webapi (Kestrel), growing-cache, storm
  (mt-throughput tc4/tc16). Pauses are **event-based** (`dotnet-trace`, out-of-process) →
  `PauseDistMsStats` in `results-full.json`.
- Full: `run-benchmarks.ps1` (11 scenarios × WS/Server/LXRGC). Redeploy fresh LXRGC.dll to
  **sample publish dirs** (harness only refreshes the dotllm root).
- Compare against tag `working-but-underperf` as the pre-fix baseline.
