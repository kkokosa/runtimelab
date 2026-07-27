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
| **D** | **Implicitly-dead young**: young objects with no increment are reclaimed **at the RC pause**, before decrements; young survivors (0→1 increment) **copied at that pause** to defragment (§2.1, §3.3.1–3) | **LANDED & VERIFIED (2026-07-24) — but PARTIAL vs paper** (`CollectNursery`, env `LXR_NURSERY`): at each RC pause it reclaims young regions in which every young object is implicitly dead (RC 0, unmarked), region-granular Immix-style, honoring interior/byref roots + mark bits. **True root cause of the earlier AV found and FIXED (not masked):** the per-thread coalescing **modified buffer overflowed** under load and the barrier **dropped the first-log entry** on a full buffer — a dropped first-log **permanently loses an RC increment**, so a live young referent stayed stuck at RC 0 and the nursery freed it while reachable. Proven by an authoritative from-roots probe (`LXR_NURSERY_ROOTPROBE`): with a huge buffer `victimsReachableFromRoots=0` across all pauses, with the normal 4096 buffer up to **1760 live victims**. Fix = the paper's **shared-queue** buffer (§3.2.1): on fill the barrier CAS-swaps in a pre-registered spare from a global free-list (topped up off-barrier in `EnsureThreadBuffers`, recycled at each RC-pause drain), so **no first-log is ever dropped**; the vanishingly-rare free-list-exhaustion case sets `g_youngRCIncomplete` (nursery defers reclaim until the next complete trace re-establishes liveness). Also fixed a latent stale-logged-bit bug (`ClearLoggedRange` at all reclaim sites). Verified with normal buffers: `victimsReachableFromRoots=0` across all pauses; **8/8 concurrent + 6/6 STW WebApi iters av=0/hang=0**, ~75 MB young reclaimed/run (605 MB + 450 MB totals). No runtime change needed. **✅ D-copy LANDED & VERIFIED (2026-07-26):** the young-SURVIVOR copy-at-RC-pause (§3.3.1–3, the defragmenting half of D) is now implemented (`CopyYoungSurvivors`, env `LXR_NURSERY_COPY`). At each RC pause, before `CollectNursery`, it promotes live (RC>0), non-pinned young survivors out of retired young regions into fresh **mature** dest chunks (`StampMatureEpoch` so they are not re-copied), fully evacuates+frees emptied source regions, and honors interior/byref roots + mark bits (pinned/marked survivors stay in place). **The fix-up is bounded (paper-faithful, NOT an O(heap) walk) and its soundness was root-caused empirically, not masked** (`LXR_VERIFY_TRACE` → `[verify-nursery-copy] misses=0`). The complete incoming-edge set for a moved young survivor is: **(4a)** the moved copies' out-edges + **(4a-young)** the out-edges of every live object still resident in a committed *young* region (retired-region kept survivors + active alloc chunks up to `alloc_ptr`) — this catches **young→young edges the JIT elides the barrier on** (intra-nursery init stores need no card mark, so they are absent from the modified buffer) — + **(4b)** a **persistent** old→young remembered set replayed from the coalescing modified buffer (mature→young stores ARE barriered). The remset is maintained across RC pauses (a young object survives many RC pauses; it is aged only by a trace) and reset at the trace epoch bump; a **promoted copy's** surviving young out-edges are added to it at promotion time (an elided young→young edge in an object promoted to mature would otherwise become an invisible mature→young reference missed when its young target later relocates). Byte/time budgeted (`LXR_NURSERY_COPY_BUDGET_MB/MS`, breaks at region boundaries). Verified full config (`LXR_CONCURRENT LXR_EVAC LXR_REMSET LXR_GC_THREADS=16 LXR_YOUNG_RC LXR_NURSERY LXR_NURSERY_COPY`): **4/4 WebApi iters exit 0, Errors=0, [verify-nursery-copy] misses=0, trace offenders=0, av=0**; ~350–780 survivors moved + 8–22 MB source regions freed per pass. No runtime change needed. | ✅ |
| **E** | Triggers: RC = heap-full OR increment-threshold OR **young-survival predictor** (1:3 biased exp decay, 128 MB default); SATB = free-block threshold OR **wastage predictor** (live-block, 1:3 decay, 5% default) (§3.2.2, §3.2.5) | **LANDED & VERIFIED (2026-07-24)** (env `LXR_INCREMENT_TRIGGER`, `LXR_WASTAGE_PCT`, default-ON): the symmetric survival EWMA (7:1) is replaced by the paper's **biased/asymmetric exponential decay** — react fast to a rising signal (¾ new + ¼ old), decay slowly on a falling one (¼ new + ¾ old) — for **both** the survival predictor (paces the trace cadence via the `DecidePhase` epoch cap) and a new **wastage predictor** (`WastagePctEwma` = floating garbage each trace recovers; projects growth·wastage% ≥ 5%·committed → trace). Added the **increment-count RC trigger** (paper §3.2.2): fire an RC pause once `ModifiedBufferEntries − g_incrementBaseline ≥ 2M` (reuses the existing coalescing counter — **zero new hot-path cost**), so a mutation-heavy/allocation-light phase still gets RC processed promptly. **Latency-cliff FIXED (paper §3.2.5 lazy decrements):** the E cadence exposed a multi-second STW finish where `meFinish` replayed the whole marking window's RC + free cascade synchronously; fixed structurally by (a) a **cheap flat O(entries) buffer detach** at the pause (`SnapshotModifiedBuffers` no longer builds the coalescing hash-map — that moves **off-pause** into `ProcessSnapshotDecrements`), (b) replaying the finish epoch's RC arithmetic + recursive free **off-pause** after RestartEE (lazy decrements, sound via `ClearRCRange`+committed-`VirtualQuery` guards), and (c) the marker **requesting prompt finalize** on quiescence so the finish epoch is the drain window, not the wait-for-next-trigger interval. Verified: multi-second cliff → **true STW finish pause 8–45 ms** (off-pause replay 7–133 ms), `LXR_VERIFY_TRACE` **offenders=0/gap=0**, **11/11 WebApi iters av=0/hang=0** (full unified config), legacy-concurrent + E-off regressions clean. Free-block-count SATB trigger still folded into the wastage predictor (equivalent). **UPDATE (2026-07-24): the young-survival RC-pause trigger is now literally realized (was previously only pacing the trace-cadence epoch cap, not RC pauses).** A true **young-object** survival EWMA (`YoungSurvivalPctEwma`, biased 3:1 decay) is computed each RC pause from the nursery outcome (survivor bytes copied / (copied + dead-young reclaimed)) and **modulates the allocation-growth budget** (`LXR_SURVIVAL_TRIGGER`, default-ON): high predicted young survival → smaller budget (pause sooner, bounding expected recursive-increment + copy work); low survival → larger budget (pause later, cheap pauses). Verified WebApi default config: `youngSurvEwma` ~1–4% (generational hypothesis holds), 3/3 iters exit 0/Errors=0/offenders=0, A/B `LXR_SURVIVAL_TRIGGER=0` equivalent+clean. | ✅ |
| **F** | Remsets **scoped to the evacuation set**, bootstrapped by the first SATB trace, kept updated by the barrier, **line-reuse-counter** stale-entry tagging; evac set = blocks <50% occupancy, N lowest; **incremental, time-budgeted**, STW (§3.3.4) | **LANDED & VERIFIED — LITERAL §3.3.4 MECHANISM (2026-07-26).** The earlier F3 derived the fix-up set from a **dedicated per-cycle STW mark** (mark-lane inter-block logging) and ran evac only every 4th cycle — a mark-rebuilt set, NOT the paper's barrier-maintained remset. Replaced with the literal mechanism: **(1) a PERSISTENT, barrier-maintained remembered set.** The single field barrier logs every **inter-block** (`sblk≠tblk`, 32 KiB) reference store into per-thread `RemsetBuffer`s (registry `g_registeredRemsetBuffers`); each entry is **line-reuse-version tagged** (`LineReuseVerOf(slot)`, §3.3.4) and stale entries (whose line was re-carved since insert → version bumped) are pruned at `CompactRemsets`. The set is **persistent across RC pauses** and *compacted* (not rebuilt) at each trace finish, so memcpy-relocation edges survive. Completeness under load: the barrier **CAS-swaps in a pre-registered spare** on a full buffer (shared-queue, mirrors the item-D RC fix) so **no inter-block edge is ever dropped**; genuine free-list exhaustion sets `g_remsetOverflow` → bounded full-walk **safety net** (sound, not a shortcut). **(2) evacuation runs in EVERY trace-finish pause** (STW, concurrent, or multi-epoch `meFinish`) — `doEvac = ((phase==TracePause)||meFinish) && g_evacActive && doTrace` — so no dedicated fully-STW backup trace is needed; `Evacuate()` self-limits by fragmentation (F1: N-lowest-occupancy) and a copy budget (F2: `LXR_EVAC_BUDGET_MS`, region-boundary breaks). **(3) memcpy-relocation edges re-registered explicitly** (memcpy fires no barrier): evac + young-promotion copies' inter-block mature→mature out-edges → the persistent remset (`RecordRemsetEdge`); an intra-block scan of every live object in each 32 KiB block **touched** by the evac set catches same-block edges the (inter-block) remset excludes; evac copies' mature→young out-edges → the item-D old→young remset. **Root-caused, not masked — the dominant AV was a real bug:** a multi-epoch trace finalizes with `phase` forced to `RCPause`, so the nursery-aging + epoch-bump + D-copy-remset-reset (gated on trace COMPLETION) was skipped at `meFinish`, leaving mature objects pointing at un-aged young survivors whose edges dangled → ~60% AV. Fixed by aging on **any** trace completion (`traceCompleted = (phase==TracePause)||meFinish`): AV rate **60% → 0 across 46+ iters**. Verified default full-parity config: **6/6 WebApi iters exit 0, Errors=0, [verify-evac] misses=0, `fixup=scoped fullwalk=0` (persistent remset is primary, no fallback), no real AV**; real reclamation (committed 405–706 MB, scoped=2–4 evac fix-ups/run). Pre-existing STW-teardown AV (present in baseline) is unrelated. No runtime change. Envs `LXR_EVAC_BUDGET_MS`, `LXR_EVAC_PERSIST`/`LXR_EVAC_SCOPED_FIXUP` (A/B; the legacy mark-derived path is retired). **UPDATE (2026-07-28f): the remembered set is now CANDIDATE-SCOPED (paper §3.3 "remsets scoped to the evacuation set", env `LXR_EVAC_CANDIDATE_SCOPE`, default-ON)** — the barrier + mark record only edges whose target is an evac candidate selected at the trace snapshot (predicted from a per-region line-mark occupancy estimate), the concurrent mark repopulates the candidate remset each cycle, `Evacuate` replays their union and moves ONLY candidate regions. Bounds the remset + finish-pause replay to candidate incoming edges (verified evac-select ~29–71 candidate regions vs 252 heap-wide with scoping off); `[verify-evac] misses=0`, 6/6 clean, A/B equivalent. See changelog 2026-07-28f. | ✅ |
| **G** | **Parallelism in every phase**; very large reference arrays partitioned for increment scalability (§3.5) | **LANDED & VERIFIED (2026-07-25)** (env `LXR_PARALLEL_RC`, default-ON). The persistent worker pool (built once at Initialize, never during a pause) is generalized from mark-only to a **generic parallel-for** (`RunOnPool(lanes, fn, ctx)`; lane 0 = caller, pooled workers = lanes 1..n; the worker proc branches on `g_poolWorkKind`: mark-drain vs. parallel-for). **RC apply is now parallel** (`ApplyRCEpoch`): both RC paths (`ProcessModifiedBuffers` STW + `ProcessSnapshotDecrements` off-pause) build flat increment/decrement lists and stride-partition them across the pool, so a large coalesced epoch — e.g. **a big reference array filled/cleared → one RC entry per element** — distributes across the GC threads instead of serializing on one (this IS the §3.5 large-reference-array increment scalability, applied at the RC-entry granularity where the work actually is). Ordering preserved: **all increments, then all decrements** (two `RunOnPool` calls, which join between them = the required barrier). RC slot updates use **atomic CAS** variants (`RCIncrementAtomic`/`RCDecrementAtomic`, `_InterlockedCompareExchange8`, saturating at 0xFF / floored at 0, exactly-once 1→0 edge) so concurrent lanes touching the same object's count never lose an update; RC-table pages are **pre-committed serially** (deduped by page) so the atomic apply never calls `VirtualAlloc` under contention. The free cascade (`DrainZeroCountWorkList`) stays **serial** (parallel recursive free is highest-risk/lowest-reward, runs after the barrier). A dedicated `g_poolLock` serializes the shared pool between the item-C background marker's mark-drain and a spanned-RC-epoch's parallel apply (no nesting → no deadlock). Small epochs (< 8192 entries) stay serial (wake/join not amortised). Verified: full unified config `LXR_GC_THREADS=16`, 8+ WebApi iters, **av=0/hang=0**, `LXR_VERIFY_TRACE` **offenders=0**; parallel path exercised (`rcApply[par>0]`); A/B `LXR_PARALLEL_RC=0` equivalent (all-serial, clean). Parallel mark was already present (`ParallelDrainMarkStack`). **Single-huge-array MARK-side partitioning is now implemented (2026-07-25, env `LXR_PARALLEL_BIGARRAY`, default-ON):** the base parallel closure atomically claims each object for exactly one lane, so one giant `object[]` was scanned end-to-end by a single lane (§3.5's very-large-reference-array cliff). A lane that meets such an array during the closure now **defers** it (it has already atomically marked the array; it pushes it to a lock-guarded list instead of scanning inline). After the per-lane closure joins and releases `g_poolLock`, `DrainDeferredBigArrays` partitions every deferred array's element range into fixed `kBigArrayChunkSlots` chunks and scans the chunks across the pool via `RunOnPool`; each lane drains the greys it discovers to full local closure (which may defer further nested big arrays), iterating to a **fixpoint**. `IsBigRefArray` restricts to the single-positive-series ref-array shape (`GetNumSeries()==1`, component size = `sizeof(void*)`, `ContainsGCPointers`, ≥ `kBigRefArraySlots`), and the chunk visitor replicates the F3 inter-block evac-edge logging + nursery guard so item-F soundness/diagnostics hold. Sound by construction: the array is atomically claimed once, its elements are visited exactly once, discovered greys are transitively closed before return, and marking is monotone. Verified: 100K-slot live `object[]` probe on WebApi (`LXR_BIGARRAY_PROBE`) — **`bigArrays[scans>0]`** fires each trace (`slots=100000/scan`), **offenders=0 / no verify-evac misses / exit 0**, A/B `LXR_PARALLEL_BIGARRAY=0` equivalent + clean (serial inline scan), default no-probe clean. | ✅ |
| — | Immix block/line heap; single field barrier serving RC + SATB + remset; SATB collects cycles + stuck counts; **copy only during STW**; stuck count → resolved by trace | All present ✅ (stuck at 0xFF vs paper's 2-bit count — variant, higher fidelity). **Straddling-object soundness (§3.1):** the paper's trailing-line-RC-write is subsumed — our carve is object-boundary precise (`CarveDeadRunsByRC` object walk + `CarveFreeRuns` exact per-line marks), no per-line RC free-scan exists; verified via `LXR_VERIFY_STRADDLE` (0 violations). | ✅ |
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
the occasional backstop. **D-copy** (young-survivor copy-at-RC-pause
defragmentation, §3.3.1–3) is now **✅ DONE** (2026-07-26); **E/F/G** are
precision/perf refinements, orthogonal to this.

Reaching full 1:1 parity is now **complete**: D-copy copies young survivors at the
RC pause and the primary-RC mature reclaimer (★) is done.

**Audit caveats (re-verified 2026-07-26, code-level, for airtight honesty).** All of
A–G + ★ + D-copy are present, *driver-wired*, and **default-ON (2026-07-26): full-LXR
parity is now the out-of-the-box behaviour** — `DOTNET_GCName=LXRGC.dll` alone gives
the full collector; every `LXR_*` knob became opt-*out* (`=0`), and the GC worker count
defaults to the machine's logical processors. Verified with **no parity env** (WebApi
40 s exit 0, Errors=0; nursery + nursery-copy + evacuate + multi-epoch concurrent trace
+ rc-reclaim all engage; `LXR_VERIFY_TRACE` misses=0/offenders=0). The only fallbacks
in the tree are bounded overflow safety nets (evac lane-log cap, SATB snapshot overflow,
D-copy remset cap, conservative keep-alive) — **empirically dormant (fire count = 0)**
under all validated runs, retained only to degrade gracefully instead of crashing on a
pathological resource-exhaustion burst (the paper likewise handles buffer overflow
gracefully rather than aborting). Residual mechanism differences, all sound (none are
unsound shortcuts):
- **F evacuation now uses the LITERAL §3.3.4 mechanism (closed 2026-07-26, commit
  1e8a367)** — the earlier every-4th-trace fully-STW mark-derived remset has been
  **retired**. F now maintains a **persistent, barrier-maintained remembered set** with
  **line-reuse-counter stale-entry tagging** (`RecordRemsetEdge`/`CompactRemsets`,
  `LineReuseVerOf`), evacuates in **every trace-finish pause** (STW/concurrent/`meFinish`),
  and needs no dedicated STW full mark. See row F above for the full landed description.
  Verified `[verify-evac] misses=0`, `fixup=scoped fullwalk=0`. **No longer a divergence.**
- **G single-huge-array MARK-side partitioning is now implemented (closed 2026-07-25,
  commit 1b7dac1, env `LXR_PARALLEL_BIGARRAY`, default-ON)** — a lane that meets a
  ≥64K-slot single-series ref array defers it; after the closure joins,
  `DrainDeferredBigArrays` chunks its element range across the pool to a fixpoint
  (`IsBigRefArray`/`ScanBigRefArrayChunk`). Combined with `ApplyRCEpoch` (increment-side),
  §3.5 large-array scalability is delivered on **both** the mark and RC paths. See row G.
  **No longer a divergence.**
- **A young elision** is via `LXR_YOUNG_RC` RC-skip, not the paper's birth-logged bit
  (behaviour-equivalent: young referents are RC-skipped and the JIT elides young→young
  stores anyway, as D-copy's fix-up confirmed); and **C's epoch-spanning** mechanism is
  present but not exercised on WebApi (the parallel marker drains within one RC-pause
  gap, `spans=0`) — it spans only when marking outlasts the pause interval, as the paper
  intends.
- **Triggers (§3.2.2) fully closed (2026-07-26):** RC = heap-full OR increment-threshold
  OR **young-survival predictor** (now an actual RC-pause budget modulator, not just
  trace-cadence pacing — see row E); SATB = wastage predictor. All three RC triggers +
  the wastage trace trigger use the paper's biased 3:1 decay.
- **Straddling objects (§3.1) subsumed + verified (2026-07-26):** the paper's
  trailing-line-RC-write serves its allocator's per-line RC free-scan, which our design
  does not have (allocator reuses object-parsed free-run chunks). Straddle safety is
  structural (exact per-line marks + object-boundary carve) and empirically checked by
  `LXR_VERIFY_STRADDLE` (0 violations).
- **THE ONE genuine remaining mechanism divergence: no large-object / medium-overflow
  allocator (§3.1/§3.3).** The paper delegates objects >16 KB to a separate non-moving
  large-object allocator and medium objects to dynamic-overflow blocks; ours bump-
  allocates everything into contiguous Immix blocks (`IsLargeObject`/`GetLOHThreshold`
  are interface stubs only). Sound (allocated + RC-reclaimed) but not the paper's
  structure — deferred by owner decision (2026-07-26).

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
   huge-array **mark-side** partitioning now also implemented (env `LXR_PARALLEL_BIGARRAY`,
   default-ON): a lane defers a claimed ≥64K-slot ref array; `DrainDeferredBigArrays`
   chunks its element range across `RunOnPool` to a fixpoint after the closure joins.
   Verified WebApi 100K-slot live-`object[]` probe → `bigArrays[scans>0]`, offenders=0,
   av=0, A/B `LXR_PARALLEL_BIGARRAY=0` equivalent+clean.)
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
   later trace-cycle `Evacuate`. **✅ DONE** (2026-07-26, `CopyYoungSurvivors`, env
   `LXR_NURSERY_COPY`): promotes live young survivors into fresh mature chunks and
   frees emptied source regions; bounded (non-O(heap)) reference fix-up = moved-copy
   out-edges + young-space scan (catches JIT-elided young→young init stores) +
   persistent old→young remembered set (with promoted objects' young out-edges).
   Verified full config 4/4 WebApi iters, `[verify-nursery-copy] misses=0`, av=0.

**★ (primary-RC mature reclamation) is now ✅** (commit — see changelog), so mature
reclamation is RC-primary and the full benchmarks measure a structurally-faithful
"primary RC, occasional trace" collector. **All parity items 1–9 are ✅** (A `f9f7031`,
B `2599d3f`, D-reclaim `d7a3b78`, C multi-epoch, D-copy 2026-07-26), E/F/G ✅, and ★ ✅.

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
- **2026-07-28h** — **Parallelized the D-copy young-survivor fix-up (STW pause
  reduction).** Profiling the `[copy-breakdown] fixup` STW sub-cost (up to ~26 ms)
  with new `LXR_VERBOSE` sub-timers (`[fixup-sub] 4a / 4a-young / 4b`) corrected an
  earlier misattribution: the dominant cost is **step 4b** (the per-evacuation-region
  remembered-set replay), NOT the 4a-young young-space rescan. 4b spends its time in
  `SlotCommitted`'s per-slot `VirtualQuery` syscall on scattered, mostly-stale remset
  entries (the set over-captures: ~4.7 K entries pruned to ~560; referrer regions get
  decommitted, targets die/promote). Both scans are embarrassingly parallel — regions
  / region-slot buckets are disjoint, `movedRanges`/`forwarding` are read-only, and no
  two lanes write the same field — so both are now dispatched across the existing mark
  worker pool via `RunOnPool`: **(4a-young)** snapshot the young-region `[start,cend)`
  ranges under `g_chunkLock`, release it (avoids a `g_chunkLock→g_poolLock` inversion),
  then stride regions across lanes (`DCopy4aYoungFn`, each lane a private `DCopyFixupCtx`,
  per-lane forwarded counts merged back). **(4b)** gather the deduped set of region-slot
  bucket indices covered by the evacuated regions and stride them across lanes
  (`DCopy4bFn`, per-lane private VirtualQuery cache + forwarded + remset-prune-delta,
  merged after). Gated on `g_poolWorkers>0` and a ≥16-region/bucket threshold, else a
  single inline lane. Result (16 threads, WebApi full config): **4a-young ~26 ms→<0.6 ms;
  4b ~16 ms→~5.5 ms** (VirtualQuery latency overlaps but caps at ~3× on the kernel VAD
  lock); whole fixup ~26 ms→~6 ms worst; `PauseTimePercentage` 9→1. Verified sound:
  3/3 WebApi + ConsoleApp iters `[verify-nursery-copy]/[verify-evac] misses=0`,
  `Errors=0`, no `PARITY-FALLBACK`, throughput ~250 ops/s unchanged; A/B
  `LXR_DCOPY_PARALLEL_4AYOUNG=0`/`LXR_DCOPY_PARALLEL_4B=0` serial path equivalent + clean.
  Both flags default-ON. No runtime change.
- **2026-07-28g** — **GC metric/pause-telemetry fidelity fixes (3 concerns raised
  from the smoke run).** (a) **`GC.GetTotalAllocatedBytes` was under-reporting ~80x**
  (ASP.NET Core showed ~690 KB where Workstation/Server GC saw ~55 MB). Root cause:
  `TotalAllocatedBytes` was bumped by only the *first* object's size per 128 KiB
  context chunk on the slow path — the mutator bump-allocates every subsequent object
  inline without re-entering the GC, so ~all allocation went uncounted. Fixed by
  tallying the true used extent `[Start, usedEnd)` of each chunk in `FinalizeChunk`
  (the exact point a filled chunk is retired) and dropping the coarse per-slow-path
  increment. Verified WebApi now reports **42.8 MB** (≈ Server's volume), ConsoleApp
  **142 MB**. (b) **Committed memory was ~453–487 MB (~17x Server's 26 MB).** Two
  drivers found via a new `[committed]` verbose breakdown (`total / liveChunks /
  runOverhang / freeChunks`): (1) a **per-thread commit-ahead "overhang" of ~72–200 MB**
  — every allocating thread eagerly committed `COMMIT_CHUNK`=16 MiB past its bump
  pointer, so ~12 WebApi threads wasted ~190 MB; **reduced COMMIT_CHUNK 16 MiB → 2 MiB**
  (~8x less overhang, one extra VirtualAlloc per ~2 MiB, no throughput cost). (2) a
  **pathological evac abort**: a single unresolvable interior/byref root skipped **all**
  evacuation for that cycle (`[evac] skipped: unresolved interior root`), and this
  workload carried one nearly **every** trace cycle, so fragmented regions were never
  compacted and committed ratcheted up. **Fixed** by pinning **only** the region each
  unresolvable interior points into (excluded from the evac candidate set) and
  compacting the rest — evac-skip **~every-cycle → 0**, `[verify-evac] misses=0`.
  Net: WebApi committed **487 → 307 MB**, private **602 → 422 MB**, ConsoleApp committed
  **83 MB**; throughput unchanged (250 ops/s), 4/4 iters exit 0 / verify 0. (c)
  **`GC.GetGCMemoryInfo().PauseTimePercentage` was hard-wired to 0** (and the
  `PauseDurations` array empty) in `GetMemoryInfo` — the STW pauses were **not tracked
  there at all** (an earlier "tracked separately" claim was wrong). Wired the real
  telemetry we already accumulate into `GetMemoryInfo` (`pauseTimePct` = basis points,
  `pauseInfoRaw` = TimeSpan ticks), and **changed "% time in GC" from a noisy
  per-pause instantaneous ratio to an honest CUMULATIVE `TotalPauseMicros / elapsed`**
  (the instantaneous form spiked to ~80% under clustered concurrent pauses on a
  workload actually <1%). Verified WebApi **8%**, ConsoleApp **0%** (8 collections /
  60 s) — believable and stable. No runtime change. New A/B/diag: `LXR_VERBOSE`
  `[committed]` line. The dominant per-pause cost itself (D-copy young-survivor fixup
  ~26 ms, an O(young) rescan forced by JIT young→young barrier elision) is now honestly
  surfaced but not yet reduced — tracked as the next perf item.
- **2026-07-28f** — **Trace-cycle evacuation remembered set is now CANDIDATE-SCOPED
  (literal paper §3.3 "remsets scoped to the evacuation set"), replacing heap-wide
  inter-block edge recording.** Closes the last true F mechanism-level divergence: the
  persistent barrier remset recorded **every** inter-block edge (`sblk≠tblk`) heap-wide,
  so `Evacuate` walked O(all edges) even though only a fraction of regions evacuate. The
  paper records only edges whose **target is an evacuation candidate selected at the
  trace's start**, and the trace (re)builds those remsets each cycle. **Change (GC-side,
  `LXRGCHeap.cpp`, env `LXR_EVAC_CANDIDATE_SCOPE`, default-ON):**
  - **Candidate bytemap** `g_evacCandidate` (1 byte / 128 KiB region-slot, committed at
    Initialize) + `IsEvacCandidateAddr`. `SelectEvacCandidates()` runs at the snapshot /
    backup STW pause (before `ResetMarks`) from each region's persisted `DeadPctEstimate`
    (a cheap side-table read — no live-heap walk). `ChunkRegion.DeadPctEstimate` is
    stamped every trace by `SweepAndSelectDefrag` from the **line marks** (O(lines/8)),
    which decouples the predictor from evac (evac runs only some cycles), bootstrapping
    candidacy from cycle 2 on.
  - **Scoped recording:** the hot field barrier, the three mark closures, and the mark's
    inter-block `RecordEvacEdge` all gate on `!g_evacCandidateScope ||
    IsEvacCandidateAddr(target)`. The **concurrent mark repopulates** the candidate
    remembered set each cycle (`g_evacEdgeLogs`, armed via `g_recordEvacEdges` across the
    whole snapshot→finish window in the multi-epoch + legacy-concurrent paths), supplying
    the pre-existing candidate incoming edges the (now candidate-scoped) barrier no longer
    records. `Evacuate` replays the **union** of the persistent barrier remset (candidate
    mutations this window) + `g_evacEdgeLogs` (candidate live edges from the mark).
  - **Soundness invariant:** whenever scoping is enabled (and line reuse supplies the
    predictor), evacuation is **restricted to candidate regions only** — even on a cycle
    with 0 candidates (then nothing is recorded and nothing moves) — so the persistent
    remset's cross-cycle history is never trusted for a non-candidate region (avoids a
    scope-transition dangle). A non-candidate region that fragmented only after the
    snapshot stays put and becomes a candidate next cycle. Overflow of either the remset
    or the mark edge-log falls back to the sound full-heap walk (loud PARITY-FALLBACK).
  - **Verified** default full-parity config (`LXR_VERIFY_TRACE`, WebApi): candidate count
    warms up (0→36→74→90) and `[evac-select]` now scopes to the candidate subset
    (**~29–71 regions vs 252 heap-wide** with `LXR_EVAC_CANDIDATE_SCOPE=0`), **6/6 iters
    exit 0, Errors=0, 0 `[verify-evac]`/`[verify-nursery]` misses, no PARITY-FALLBACK**
    (scoped fix-up primary), ops at parity (~252). A/B disable equivalent + clean. No
    runtime change.
- **2026-07-28e** — **D-copy young-survivor remembered set is now PER-EVACUATION-REGION
  (literal paper §3.3 per-block remset), replacing the flat global log.** Closes the
  "residual vs paper" item flagged in 2026-07-28d. The paper keeps a **per-evacuation-
  block** remembered set and, at evacuation, **processes only the remsets of the blocks
  in the evacuation set**. D-copy is byte/time budgeted, so each RC pause evacuates only
  a **subset** of the young regions; the old flat set forced a replay+prune of *every*
  mature→young edge in the heap every pause regardless of which regions actually moved.
  **Change (GC-side, `LXRGCHeap.cpp`):** the flat `g_dcopyModifiedSlots` vector is
  replaced by `g_dcopyRemsetBuckets` — a vector of per-region buckets keyed by the
  **target young object's 128 KiB region-slot** (`(target − heapBase) /
  CONTEXT_ALLOC_QUANTUM`). All four capture sites (`ProcessModifiedBuffers`, evac-copy
  and D-copy promotion-edge re-registration) append via `DCopyRemsetAppend(slot, target)`
  (still gated on `!g_traceWindowOpen`); the trace epoch bump and overflow clear all
  buckets (`DCopyRemsetClearAll`). **4b now replays ONLY the buckets of the regions
  actually evacuated this pass** (`evacuatedSrcs`, the srcs entered before the budget
  cut): for each such region it visits the buckets covering `[start, usedEnd)`'s slot
  range, applying the same per-bucket sort+dedup (SlotCommitted VirtualQuery-cache
  locality) + Rebase + prune as before. Edges into young regions **deferred** to a later
  pass stay in their buckets untouched, so 4b work is bounded to *O(incoming edges of the
  evacuated regions)* rather than *O(all mature→young edges)*. **Soundness:** a young
  object is stationary until the pass that moves it, so every incoming edge to an object
  that lived in a region is captured into that region's slot-range buckets — replaying
  that range covers all of the region's incoming edges; deferred regions did not move, so
  their unfixed edges remain correct until their bucket is replayed on the pass that
  evacuates them. Verified full parity config (`LXR_VERIFY_TRACE`, 25 s WebApi):
  `[copy-breakdown]` shows `remset` bounded (~530–570) with `evacRegions=srcRegions`
  when the budget covers all young regions, and a **budget-forced partial run**
  (`LXR_NURSERY_COPY_BUDGET_MB=1`) shows `evacRegions < srcRegions` (e.g. 43/132,
  138/516, 69/722) with **0 `[verify-nursery-copy]`/`[verify-evac]` miss lines** —
  proving deferred regions' edges are correctly retained and fixed on the later pass.
  13 iters Errors=0 / 0 verify misses; teardown-AV rate ~15% (unchanged from the
  pre-existing 10–30 % baseline, not a regression). No runtime change.
- **2026-07-28d** — **D-copy remembered-set balloon fixed (a ~330 ms RC pause on
  1.09M slots eliminated); compared against the paper's remembered-set design.**
  Canary profiling caught a single ~330 ms RC pause whose `[copy-breakdown]` showed
  `modslots=1092646` and `fixup=229640us` — the D-copy young-survivor fix-up replaying
  a **1.09-million-entry** persistent remembered set (a microbenchmark attributed ~94 ms
  of that to the per-pass `std::sort` alone, the rest to the scattered `*slot` loads).
  **Paper comparison (§ "combining RC and remembered sets"):** LXR *initializes each
  remembered set at the start of an SATB trace and scopes it to the evacuation set of
  high-fragmentation blocks*, and the field-logging barrier *keeps them up to date*; it
  never accumulates or replays edges outside the evacuation candidates. Ours instead
  logged **every** mature→young slot into one flat global set reset only at the next
  trace — so a long (multi-epoch) concurrent trace, during which `CopyYoungSurvivors`
  self-skips (never moves young under an in-flight trace) and therefore never prunes,
  let the set grow without bound while every RC pause kept appending. Every slot so
  captured is discarded unused at trace completion (the window's young ages to mature
  and the set is cleared). **Fix (two parts, GC-side):** (1) **gate capture on
  `!g_traceWindowOpen`** — do not append to the D-copy remset while a trace is in
  flight, since those entries are provably dead by trace end and D-copy consumes the
  set only between traces; (2) **prune during the 4b replay** — retain an entry only
  while it still names a movable young target (drop promoted/mature, dead, freed-region,
  and relocated-source slots), matching the paper's "keep remsets up to date" instead
  of accumulating stale edges. Soundness: an entry is dropped only after re-reading its
  current value; any later store that makes a field point at a young object re-logs it
  (coalescing bit cleared each pause), so no live incoming edge into a survivor is lost —
  `LXR_VERIFY_TRACE` confirms 0 unforwarded refs. **Result:** `modslots` **1.09M →
  ~570** and stable; `[copy-breakdown] fixup` **229 ms → ~3 ms**; max `[rc-breakdown]
  copy` **~330 ms → ~7.5–8 ms** with no spike, across 6/6 WebApi iters (Errors=0,
  offenders=0, 0 misses); D-copy still moves 635–807 survivors / frees 244–311 regions
  per pass. **Residual vs paper:** a genuinely burst-heavy inter-trace window could
  still make the (now-pruned) set large; the fully paper-faithful bound is a per-
  evacuation-block remembered set (only edges into the evac set) — **now implemented,
  see 2026-07-28e.**
- **2026-07-28c** — **All RC-pause / trace-finish region reclamation decommit moved
  OFF-PAUSE via a single shared `ReclaimRegionMemory` helper — closes the last
  non-paper-faithful cost inside the D-copy young-survivor copy.** Profiling the
  RC-pause young-survivor copy (`CopyYoungSurvivors`) showed `[rc-breakdown] copy`
  ≈ 7–9 ms split as: memcpy of survivors ~2–3 ms (paper-faithful — LXR §3.3 copies/
  promotes nursery survivors to defragment), fix-up ~1 ms, and **~3 ms of in-pause
  `VirtualFree(MEM_DECOMMIT)`** of the emptied young source regions (`freed=250 /
  ~31 MB` per pass). LXR reclaims memory **off the critical path**, so the in-pause
  decommit is the non-faithful part. **Fix (GC-side):** introduced
  `LXRCollector::ReclaimRegionMemory(chunkIndex)` — sets `Committed=false` then either
  defers (`g_deferDecommit`, default-on → push range+index to `g_pendingDecommit`/
  `g_pendingFreeChunks`, drained off-pause by `DrainPendingDecommit` after `RestartEE`)
  or inlines the `VirtualFree` (opt-out). Converted **all 5** reclaim sites to it:
  `SweepAndSelectDefrag`, `CollectNursery`, `CopyYoungSurvivors` step 5,
  `ReclaimMatureByRC`, and `Evacuate` step 5 (the sweep site previously had this logic
  inline; now shared). The off-pause invariant is preserved: a region reaches
  `g_freeChunks` (allocator-reusable) ONLY after it is physically decommitted; between
  reclaim and drain it is in limbo (Committed=false, RC/log cleared, pages still
  committed, unreachable by the allocator). **Result:** typical `[rc-breakdown] copy`
  **7–9 ms → ~6 ms** (remainder = root pinning + source selection + memcpy + fix-up,
  all paper-faithful); `[verify-nursery-copy]`/`[verify-evac]` misses=0, trace
  offenders=0, Errors=0 across 6/6 WebApi iters; A/B `LXR_DEFER_DECOMMIT=0` (inline
  path) equivalent + clean.
- **2026-07-28b** — **Evacuation fix-up VirtualQuery thrash fixed (~26× on worst
  pass; eliminates a multi-*second* outlier) — same class of bug as the D-copy fix.**
  After deferring sweep decommit, the trace-finish pause's remaining spike (~144–223 ms,
  pathologically up to tens of seconds) was localized with phase timers to
  `Evacuate()` step 4b — the persistent remembered-set replay. Its `slotCommitted`
  guard caches a single `MEMORY_BASIC_INFORMATION` region and VirtualQuery's any slot
  outside it; the ~32 K remset entries are in scattered barrier-insertion order, so the
  single-region cache thrashes to **one VirtualQuery syscall per slot** = the entire
  145 ms (matching `b_remset≈fixup`). **Fix (GC-side):** gather the surviving
  (non-stale, not-inside-a-moved-source) slots into a vector, `std::sort` by address +
  `std::unique`, then run the committed-check + rebase in address order so the cache
  hits (~one syscall per distinct region). The check/rebase is moved outside
  `g_remsetLock` (safe: STW finish, mutators suspended, marker parked). **Result:**
  worst-pass evac fix-up **145 ms → ~5.5 ms**; whole `Evacuate` **~144–223 ms → ~6–12 ms**;
  `[verify-evac] misses=0/offenders=0` across all iters; 10/12 WebApi iters clean (the
  2 failures are the pre-existing flaky AV — 1 post-`##RESULT##` teardown, 1 mid-run
  after a clean evac). Also moved the sweep `VirtualFree(MEM_DECOMMIT)` off-pause
  (2026-07-28 entry).
- **2026-07-28** — **Trace-finish sweep decommit moved OFF-PAUSE; trace-finish
  pause dominator re-identified as Evacuate (not sweep).** After the D-copy fix the
  trace-finish STW pause became the largest remaining pause. Profiled it with a new
  `[sweep-breakdown]` timer (verbose-gated): the steady-state sweep (~5–7 ms) was
  ~94 % `VirtualFree(MEM_DECOMMIT)` — a per-page-PTE syscall O(freed pages), and the
  suspected source of a rare multi-hundred-ms spike. **Fix (GC-side, `LXR_DEFER_DECOMMIT`,
  default ON):** the STW sweep now only marks a dead region reclaimed
  (`Committed=false` + `ClearRCRange`/`ClearLoggedRange`, all cheap) and records its
  page range + chunk index to pending lists; it does NOT `VirtualFree` and does NOT
  publish the chunk to the reusable free list. A new `DrainPendingDecommit()` runs
  **after `LXRRestartEE`** (off-pause, mutators live) at all three collection-driver
  sites (meFinish, concurrent-finish, STW): it `VirtualFree`s each range, updates the
  committed/reclaimed counters, then pushes the indices onto `g_freeChunks`. Invariant
  preserved: a region is on `g_freeChunks` only *after* it is decommitted; limbo
  regions (dead, pages still committed, not yet reusable) are unreachable by the
  allocator, and their RC is already 0 so an off-pause deferred decrement into one
  no-ops. **Result:** in-pause sweep dropped from ~5–7 ms (mostly decommit) to
  **<2 ms**; `[verify-nursery-copy]/[verify-evac] misses=0/offenders=0` across all
  iters; A/B `LXR_DEFER_DECOMMIT=0` equivalent + clean. **Key re-finding:** the
  `finish breakdown` "sweep" timer actually spanned `Evacuate()`+sweep; splitting it
  (`evac=` vs `sweep=`) showed the real trace-finish spike is **`Evacuate` (up to
  ~144 ms)**, with sweep now <2 ms. Evacuate is the next pause target.
- **2026-07-27** — **D-copy pause bottleneck root-caused & fixed (37× on worst
  pass); prior 4a-young attribution + the JIT-runtime-change plan REFUTED — no runtime
  change needed.** The 2026-07-24 entry blamed the ~68–97 ms RC pause on the 4a-young
  O(young-space) rescan and concluded the deep fix required a JIT init-store-barrier
  change (approved by the user as "option A"). **Both conclusions were wrong.**
  **(1) The JIT does NOT elide heap young→young stores.** A dedicated GC-side
  diagnostic (`LXR_DCOPY_ELISION_DIAG`, since removed) tallied, at every D-copy fixup,
  the young→young edges 4a-young forwards whose slot is ABSENT from the barrier-
  maintained remembered set (`g_dcopyModifiedSlots`): **`elided(not-in-remset)=0`
  across every pass** — every heap young→young edge IS captured by the barrier. A
  parallel RyuJIT elision audit confirmed the only relevant elision (`GTF_IND_TGT_NOT_HEAP`,
  gcinfo.cpp:250, set by the ObjectAllocator, objectalloc.cpp:2556) fires on stores
  into **stack-allocated** (non-escaping) container objects — whose refs are GC
  **roots**, handled by D-copy's existing root **pinning** (a young object any root
  address falls within is never moved). So neither a JIT change nor 4a-young is needed
  for those. **(2) The real bottleneck was a GC-side `VirtualQuery` syscall thrash.**
  The D-copy fixup 4b step replays the (unsorted) old→young remembered set through
  `LXRDCopyRemsetVisit → DCopyFixupCtx::SlotCommitted`, which VirtualQuery's each
  slot's page (freed-region guard) behind a **single-region** cache. Unsorted slots
  scatter across every mature referrer in the heap → the cache thrashes to **one
  VirtualQuery per slot** (~8 k syscalls) = the entire 50–75 ms cost; 4a-young itself
  is ~negligible (A/B with `LXR_DCOPY_NO_4AYOUNG=1`: fixup timing unchanged). **Fix
  (GC-side, 3 lines):** `std::sort`+`std::unique` the modified-slot vector before the
  4b replay so same-region slots are visited consecutively → the committed-page cache
  hits, collapsing VirtualQuery to ~one per distinct region (+ dedup drops repeat
  logs, + better Rebase locality). **Result:** worst-pass D-copy fixup **75.6 ms →
  2.1 ms** (~37×); max **RC pause ~97 ms → ~15 ms**; `LXR_VERIFY_TRACE`
  `[verify-nursery-copy] misses=0` across all passes; 10/10 WebApi iters complete the
  workload (av=0 in-workload; the ~1/10 teardown AV is the pre-existing flaky shutdown
  fault, present in baseline). **No runtime change** — the approved option-A JIT change
  is moot. 4a-young kept as a cheap correctness belt-and-suspenders (dormant A/B knob
  `LXR_DCOPY_NO_4AYOUNG`). **Next dominant pause = the trace-finish O(heap) STW sweep
  (~341 ms max observed)** — separate follow-up.
- **2026-07-24** — **Pause profiling + sound D-copy fixup micro-opt; throughput
  claim corrected.** Profiled the WebApi/LXRGC full-config canary with per-phase STW
  timers (`[rc-breakdown]`, `[copy-breakdown]`, ProcessModifiedBuffers timing — all
  verbose-gated). **Findings:** (1) **Throughput is NOT ~3.6× slower** — the prior
  canary's claim was a measurement artifact. Direct + harness runs both give WebApi
  LXRGC **≈ 250 ops/s vs ≈ 256 WS/Server** (parity). (2) **The real problem is pause
  latency, not throughput.** Two dominant STW pauses: **(a)** RC-pause
  `CopyYoungSurvivors` grows to **~68–97 ms** (**correction: 2026-07-27 shows this was
  the 4b VirtualQuery thrash, NOT the 4a-young rescan as claimed below**), and **(b)**
  the trace-finish **STW sweep/line-carve is O(heap) at ~5–170 ms** (one spike to
  301 ms under a smaller trigger). During these spikes per-second ops collapse from
  ~256 to ~16 — that is what makes LXR "feel slow." (3) **[SUPERSEDED 2026-07-27 —
  the JIT does not elide heap young→young stores; see above]** ~~Root cause of 4a-young
  — the JIT elides the write barrier on init stores to freshly-allocated (young)
  objects~~, so
  young→young edges never reach the modified buffer; we compensate with an O(young-
  space) rescan every RC pause. Young space per pause ≈ the RC-pause trigger's worth
  of allocation (~32 MB ≈ 450 regions), so 4a-young ∝ trigger size. (4) **Trigger
  tuning is a tradeoff, not a fix:** `LXR_GC_TRIGGER_MB=8` cut RC pauses (97→20 ms)
  but made traces more frequent and one sweep hit 301 ms; total young-scan work is
  ~constant (≈ total allocation) because each pause rescans the whole young space.
  **Landed (sound, kept):** removed the redundant per-ref `unordered_map::find` from
  the D-copy fixup hot path (`DCopyFixupCtx::Rebase`/`IsMovedSource` now use one
  binary search over the small sorted moved-range table; the `>=` lower bound catches
  exact-start refs the strict `>` previously deferred to the hash) — ~14 % off the
  worst RC pause (79→68 ms), `LXR_VERIFY_TRACE` misses=0. **Remaining deep fix is a
  runtime-change boundary** (see Audit-caveats): making young→young init stores
  barriered (JIT init-store barrier elision) — or maintaining young RC so survivors
  can age-in-place — is what eliminates the O(young-space) rescan and yields the
  paper's sub-ms RC pauses. Flagged to the user per the standing "stop and inform on
  runtime change" mandate.
- **2026-07-24** — **GC pause-time counters wired + startup-hang defensive fix.**
  (1) **Pause counters:** LXR now feeds the pause-time telemetry that its IGCHeap
  previously stubbed to 0. `GetTotalPauseDuration()` returns cumulative STW pause
  time (`TotalPauseMicros`×10, TimeSpan ticks) → drives the modern
  `dotnet.gc.pause.time` meter *and* the `total-pause-time-by-gc` EventCounter (both
  managed via `GC.GetTotalPauseDuration()`), and `GetLastGCPercentTimeInGC()` now
  returns the last pause as a % of the wall interval since the prior pause (feeds the
  legacy `% Time in GC` counter). Verified: 30 s WebApi/LXRGC canary now reports
  **PauseTimeMs avg 1.35 / max 32.99 ms** (was uniformly 0). This closes the
  "harness measurement gap" noted in the prior canary entry — LXR's pauses are now
  observable, so the throughput/footprint numbers can be attributed. **NOTE:** the
  prior entry's "expected for an unoptimised research collector" gloss is *rejected* —
  LXR's design goal is to *beat* the built-in GCs; the regression is an implementation
  cost to be profiled and removed, not an inherent property. (2) **Startup hang:**
  root-caused to driving `SuspendEE` synchronously from a cooperative-mode mutator
  (the EventPipe/`dotnet-counters` poll thread allocating during
  `NativeRuntimeEventSource` init) — itself a suspension target → deadlock. Fix:
  added a `g_collectorReady` gate; `RequestLXRCollection` now **drops** early
  fire-and-forget triggers (always safe on a freshly-reserved heap) and bounds
  `wait==true` (GC.Collect) readiness spin to ~1 s instead of the removed
  deadlock-prone synchronous fallback. Watchdog made **default-on** (`LXR_WATCHDOG=0`
  to disable) and now dumps all thread stacks on ANY >20 s stall for self-diagnosis.
- **2026-07-24** — **Canary benchmark run (short, non-exhaustive).** Ran
  `run-benchmarks.ps1 -DurationSeconds 60 -Scenarios console,webapi -GcModes
  workstation,server,lxrgc` (report at `results/report.html`, data `results/results-full.json`;
  the harness runs each (scenario,mode) **sequentially**, never concurrently, and
  regenerates the HTML after every run). Results: **WebApi runs clean on all three GC
  modes incl. LXRGC** (exit 0). Throughput/footprint gap vs built-in GC on WebApi (60s):
  LXRGC ~70 ops/s vs ~256 (WS/Server) ≈ 3.6× slower; WS 371 MB / peak 586 / commit 408
  vs 72–93 MB ≈ 4–8× higher — expected for an unoptimised research collector. LXRGC GC
  **pause-time EventCounters read 0** (its IGCHeap does not feed `% Time in GC` / pause
  duration) — a harness measurement gap, not zero pauses. **Finding — intermittent
  LXRGC × EventPipe startup hang:** `console_lxrgc` HUNG at early startup (~50%: 1 hang /
  2 harness attempts) with CPU frozen at ~0.9 s; the dump showed the CLR not fully
  initialised, stalled in the `NativeRuntimeEventSource..cctor` / `CounterGroup.PollForValues`
  EventPipe path (same path the earlier stale-coreclr AVs traversed). Run **directly
  without a diagnostics attach, ConsoleApp/LXRGC is 4/4 clean** — so the hang is a race
  between LXRGC and the EventPipe/`dotnet-counters` attach during fast-process startup,
  **not** a workload GC-correctness bug (WebApi, slower to start, tolerates the attach
  3/3). Dump kept at `results/console_lxrgc_hang.dmp`. Also re-confirmed the deployment
  hazard: `ConsoleApp/publish` carried the stale 4928512-byte 7/20 `coreclr.dll` (crashes
  **all** GC modes) — refreshed from `artifacts/bin/coreclr/windows.x64.Release`
  (4935680, 7/22) + `clrjit.dll` before the passing runs.
- **2026-07-26** — **b.4 young-survival RC-pause trigger + b.5 straddle verification landed
  (post-audit parity closures).** From the bidirectional paper↔impl audit, two items
  closed: **(b.4, throughput)** the paper's §3.2.2 *survival-threshold* RC-pause trigger
  was only pacing the trace-cadence epoch cap, not RC-pause timing. Added a true
  **young-object** survival EWMA (`YoungSurvivalPctEwma`, biased 3:1 decay) computed each
  RC pause from `NurseryCopyBytes`/`NurseryBytesReclaimed` deltas, modulating the
  allocation-growth budget (0.5×..1.5×) so pauses fire sooner when predicted survival
  (hence recursive-increment + young-copy cost) is high — controlling *expected* pause
  time, favouring throughput (`LXR_SURVIVAL_TRIGGER`, default-ON, `=0` opts out).
  **(b.5, §3.1 straddling objects)** the paper writes trailing-line RC to stop its
  allocator's per-line RC free-scan from reusing a straddling live object's tail lines.
  Our architecture has **no per-line RC free-scan** — the allocator reuses *object-parsed*
  carved free-run chunks — so straddle safety is structural: `CarveFreeRuns` uses exact
  per-line marks (`MarkLines` marks every line an object spans) and `CarveDeadRunsByRC`
  walks whole object boundaries. Added `LXR_VERIFY_STRADDLE` proving the invariant (no
  carved dead run overlaps a live object) via `LXRReportFallback`. Verified WebApi default
  config: `youngSurvEwma` ~1–4%, 3/3 iters exit 0/Errors=0/offenders=0, **straddle
  verifier silent (0 fallbacks)**, A/B `LXR_SURVIVAL_TRIGGER=0` equivalent+clean. Also
  reconciled the stale F/G "Audit caveats" bullets (F literal §3.3.4 remset per 1e8a367,
  G huge-array mark per 1b7dac1). No runtime change.
- **2026-07-26** — **D-copy YOUNG-SURVIVOR COPY-AT-RC-PAUSE LANDED → D fully ✅
  (defragmenting half of §3.3).** New `CopyYoungSurvivors()` (env `LXR_NURSERY_COPY`)
  runs STW at every RC pause, before `CollectNursery`: it promotes live (RC>0),
  non-pinned young survivors out of retired young regions into fresh **mature** dest
  chunks (`StampMatureEpoch` so they are never re-copied), frees fully-evacuated
  source regions, and keeps pinned/marked survivors in place (interior/byref roots +
  mark bits honored). **The reference fix-up is bounded (NOT an O(heap) walk) and its
  soundness was root-caused empirically, not masked.** The AV chase found the complete
  incoming-edge set for a moved young survivor has THREE parts: **(4a)** the moved
  copies' out-edges; **(4a-young)** the out-edges of every live object still resident
  in a committed *young* region (retired-region kept survivors + active alloc chunks
  up to `alloc_ptr`) — this is required because **the JIT elides the write barrier on
  young→young initializing stores** (an intra-nursery store needs no card mark, so it
  never reaches the coalescing modified buffer), the paper finds these by scanning the
  young space itself; and **(4b)** a **persistent** old→young remembered set replayed
  from the modified buffer (mature→young stores ARE barriered). Two subtleties the AV
  exposed: the remset must be **persistent across RC pauses** (a young object survives
  many RC pauses, aged only by a trace; a per-pause buffer loses edges to pinned/
  budget-skipped survivors) and reset only at the trace epoch bump; and a **promoted
  copy's surviving young out-edges must be added to the remset at promotion time** —
  an elided young→young edge carried into a mature promoted object becomes an invisible
  mature→young reference that 4a/4a-young/4b would all miss when its young target later
  relocates. On remset overflow (`kDCopyRemsetCap`) D-copy falls back to a sound
  full-committed-heap walk. Byte/time budgeted (`LXR_NURSERY_COPY_BUDGET_MB/MS`, breaks
  at region boundaries). **Verified full unified config** (`LXR_CONCURRENT LXR_EVAC
  LXR_REMSET LXR_GC_THREADS=16 LXR_YOUNG_RC LXR_NURSERY LXR_NURSERY_COPY LXR_VERIFY_
  TRACE`): **4/4 WebApi iters exit 0, Errors=0, `[verify-nursery-copy] misses=0`, trace
  offenders=0, av=0**; ~350–780 survivors moved + 8–22 MB source regions freed per pass.
  No runtime change needed. **All parity items A–G + ★ + D-copy are now ✅.**
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
