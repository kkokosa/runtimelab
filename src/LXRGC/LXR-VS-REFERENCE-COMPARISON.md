# LXR: Paper & Reference Implementation vs. Our Implementation — Phase-by-Phase Comparison

**Purpose.** Our LXR GC (`native/LXRGCHeap.cpp`) is functionally working but underperforms the
built-in .NET GC on **every** axis in the 120 s event-based benchmark run (tag
`working-but-underperf`): pauses are always worse (with multi-second spikes), footprint is
2–3× larger, and throughput parity is therefore no consolation. This document is an honest,
grounded, phase-by-phase comparison of three sources to locate the flaws:

1. **Paper** — Zhao, Blackburn, McKinley, *"Low-Latency, High-Throughput Garbage Collection"*, PLDI 2022 (arXiv:2210.17175). Cited as *(paper §…)*; local copy in `session files/LXR-paper-2210.17175.txt`.
2. **Reference** — the author's MMTk implementation, `wenyuzhao/mmtk-core` branch `lxr-x/simplified`. Cited as *(ref `file.rs`)*.
3. **Ours** — `src/LXRGC/native/LXRGCHeap.cpp` / `LXRGC.h`. Cited as *(ours `LXRGCHeap.cpp:NNNN`)*.

> **Thesis (one sentence).** The paper's entire thesis is that **no phase is a whole-heap
> stop-the-world operation** — every pause does either *bounded* work (RC increments capped by a
> young-size trigger; evacuation capped by a copy budget) or *concurrent* work (SATB trace,
> decrements, cycle & block sweeping run **between** pauses). **We preserved the algorithm but
> collapsed its concurrency/bounding into a periodic heavyweight STW "trace-finish" that performs
> whole-heap work** (an O(live-heap) allocate-black object parse, a whole-window RC replay, and an
> O(heap) sweep). That single structural divergence explains both the pause spikes and the
> footprint bloat.

---

## 0. Benchmark symptoms we must explain

From `results/results-full.json` (120 s, event-based `dotnet-trace` pauses):

| Scenario | LXRGC pause avg / p99 / max (ms) | Best builtin avg / max | LXRGC WSet | builtin WSet |
|---|---|---|---|---|
| webapi | 13.9 / 22.6 / 27.8 | 0.7 / 2.3 | 291 MB | 69 MB |
| gcperfsim-cache | 29.5 / 118 / 118 | 3.4 / 9.7 | 881 MB | 539 MB |
| gcperfsim-churn | 21.7 / 226 / 226 | 2.3 / 27 | 770 MB | 182 MB |
| gcperfsim-webserver | 53 / 246 / 246 | 3.8 / 17.5 | 1738 MB | 723 MB |
| mt-throughput (16-thread storm) | 244 / 1727 / 1727 | 1.8 / 17.9 | 7402 MB | 533 MB |

Symptom classes to root-cause. Note that **(A) has two distinct sub-classes with different fixes** —
the *baseline* pause level (what the reference keeps **sub-millisecond**) and the *spike tails* (what
the reference keeps to a *few ms* but we let run to *seconds*):

- **(A1) Baseline pause too high** — our common `RefCount`-equivalent pause is single-digit-to-tens of
  ms where the reference is **sub-ms**. This is about the *frequent, light* pause.
- **(A2) Multi-second spikes** — our trace *finish* pauses spike to hundreds of ms / seconds where the
  reference's `InitialMark`/`FinalMark` stay a *few ms*. This is about the *heavy, occasional* pause.
- **(B) Footprint 2–10× larger.**

The distinction matters because **A1 and A2 are fixed by different mechanisms** (see §12/§13): A1 by
deferring decrements + capping/parallelizing per-pause RC work; A2 by removing whole-heap work from the
finish.

---

## 1. Heap organization

| | Paper | Reference | Ours |
|---|---|---|---|
| Substrate | Immix (paper §1) | `ImmixSpace` + LOS (ref `plan/lxr/global.rs`) | Immix-style regions/chunks |
| Block | 32 KB | 32 KB, `Block::LOG_BYTES=15` (ref `policy/immix/block.rs`) | **32 KB**, `kBlockSize` (ours `LXRGC.h:90`) |
| Line | 256 B | 256 B, `Line::LOG_BYTES=8`, 128 lines/block (ref `policy/immix/line.rs`) | **256 B**, `kLineSize`, `kLinesPerBlock=128` (ours `LXRGC.h:91-92`) |
| RC width | **2 bits/object**, N_rc=2 (paper §3.2.1) | **2 bits** per 8-byte granule, `MAX_REF_COUNT=3` sticky (ref `util/rc.rs`) | **1 byte/object** `RCSlot`→`uint8_t*` (ours `LXRGC.h:244`) |
| Liveness meta | line mark + RC | `RC_TABLE` (2-bit) read a whole line (u64) at a time; `BlockState` byte; `RC_STRADDLE_LINES` for spanning objects (ref `util/rc.rs`, `block.rs`) | per-object RC byte; line bitmap + line version table (ours `LXRGCHeap.cpp:1923-1932`) |

**Assessment.** Geometry is faithful. Two real differences: (1) our RC is a **byte** not 2 bits — 4× the
RC metadata (minor footprint tax, not a root cause); (2) the reference reads liveness a **whole line
(64 RC bits) per word** and tracks block state as a single byte, so "is this line/block dead?" is a
handful of word loads (ref `RCArray::is_dead`, `block.rc_dead()` reads the block as `[u128]`). Our
sweep instead walks objects (see §7/§9). **This is the seed of the pause problem: we scan objects where
the reference scans bitmaps.**

---

## 2. Write barrier

| | Paper | Reference | Ours |
|---|---|---|---|
| Kind | single field barrier combining coalescing-RC + SATB + remset (paper §2.2, §3) | `FieldBarrier`/`LXRFieldBarrierSemantics`, per-**slot** unlog bit (ref `plan/lxr/barrier.rs`, `mutator.rs`) | field-logging barrier, per-slot, coalesced at processing (ours `LXRGCHeap.cpp:4798+`) |
| Fast path | 1 unlogged-bit test | 1 side-metadata byte load; skip if already logged (ref `barrier.rs`) | pluggable runtime barrier → callback; log slot+old value |
| What is logged | old value (dec) + new value (inc) + SATB | pushes `old`→`decs`, `slot`→`incs`, weak→`refs`; on full → flush (ref `barrier.rs`) | slot + first old value per epoch; coalesced into one (old,*slot) pair per slot (ours `4798-4830`) |
| Flush cadence | on buffer full & at pause | on `VectorQueue::is_full()` and `flush()` at every STW start (ref `barrier.rs`) | at pause via `ProcessModifiedBuffers`/`SnapshotModifiedBuffers` |

**Assessment.** Barrier design matches (per-slot coalescing RC + SATB). **But note the destination
of the flushed work — that is where we diverge sharply (§3):** the reference routes `incs` to an STW
bucket and `decs` to a **concurrent, deferred** bucket. We process both at the pause.

---

## 3. Reference counting — increments & decrements

| | Paper | Reference | Ours |
|---|---|---|---|
| Increments | processed **at the pause**, bounded (paper §3.2.2 "frequent light RC pauses") | STW in `RCProcessIncs` bucket; packets of **1024 slots**; pause fires when predicted survival ≥ **128 MB** (`MAX_SURVIVAL_MB`) so per-pause inc work is bounded (ref `gc_work/rc.rs`, `global.rs`, `mod.rs`) | STW `ProcessModifiedBuffers`; increment-count trigger `g_incrementTrigger=32768` (ours `LXRGCHeap.cpp:10133`) |
| Decrements | **lazy, concurrent** (paper §3.2.5 "lazy processing of decrements") | **Lazy by default** `LAZY_DECREMENTS=true`: prev-GC root decs + barrier decs run **between** pauses in `Concurrent` bucket; only `Full` runs decs STW (ref `mod.rs`, `global.rs`, `gc_work/rc.rs`) | **On-pause by default** (`g_onPauseRC`): `ProcessModifiedBuffers` applies the dec + zero-count free **cascade at the pause**; `SnapshotModifiedBuffers` off-pause path exists but is not the default (ours `10527`, `4798+`) |
| Death (RC→0) | reclaim free lines/blocks | `process_dead_object`: recursive child decs, add block to `possibly_dead_mature_blocks` (lazy sweep), LOS freed immediately (ref `gc_work/rc.rs`) | zero-count cascade frees young + carves runs at the pause (ours `ProcessModifiedBuffers`, `CollectNursery`) |
| Root decs | prev/curr root snapshot swap | `prev_roots`↔`curr_roots` swap; prev decs enqueued at **next** pause start, into concurrent bucket (ref `global.rs process_prev_roots`) | processed on-pause |

**Root cause (A) #1 — decrements are STW for us, concurrent for the reference.** The reference's
default `LAZY_DECREMENTS=true` means the recursive dec + free cascade (potentially O(dead subgraph))
runs on GC worker threads **between** pauses. Ours runs it **inside** the pause. On a churny or
multi-epoch window this cascade is exactly the *"O(window mutations)"* cost our own code comments call
out, and the measured **2–3.4 s `finish-buffers` dominator** (`LXR_FINISH_PROFILE`). We even added
`SnapshotModifiedBuffers` to defer it, but the default path (`g_onPauseRC`) still pays it at the pause.

---

## 4. Concurrent marking / SATB trace

| | Paper | Reference | Ours |
|---|---|---|---|
| Trigger | survival/wastage heuristic; trace when profitable (paper §3.2.2) | `garbage_mature ≥ 20% total` (`TRACE_THRESHOLD`) or `< 4 MiB free` (`CYCLE_TRIGGER_THRESHOLD`), via EWMA predictor (ref `global.rs`, `mod.rs`) | epoch-span cap + growth + wastage 5% + survival EWMA (ours `10154-10168`, `DecidePhase`) |
| Concurrency | concurrent, spans multiple RC pauses (paper §3, novelty i) | `LXRConcurrentTraceObjects` in `ConcurrentResumable` — **runs between pauses**, preemptible; only the wavefront tail drains at FinalMark (ref `gc_work/tracing.rs`, `global.rs`) | separate marker thread + pool; drains between pauses **but the FINISH is a heavy STW pause** (ours `LXRMarkerThreadProc`, `ConcurrentTraceFinish:5669`) |
| Cycle detection | SATB (Yuasa) finds dead cycles & stuck-RC objects (paper §3) | `SweepDeadCycles` after FinalMark: `rc!=0 && !marked` ⇒ dead cycle; **lazy/concurrent, chunk-parallel** (ref `mature_sweeping.rs`) | mark-authoritative `SweepAndSelectDefrag` **STW at the finish** (ours `10539`, `SweepAndSelectDefrag`) |
| Roots | STW at InitialMark/FinalMark | InitialMark & FinalMark scan roots STW; FinalMark re-scans via `LXRStopTheWorldProcessEdges` (ref `gc_work/rc.rs`, `tracing.rs`) | STW snapshot + STW final rescan in `ConcurrentTraceFinish` (ours `5669+`) |

**Root cause (A) #2 & (B) #1 — our trace *finish* is STW and heavy; theirs is concurrent and light.**
In the reference, FinalMark is a *light* pause: increments for roots, drain the residual CM wavefront
(the bulk was done concurrently), drain SATB buffers, and run the *budgeted* mature-evac closure. The
**cycle sweep that actually frees mature garbage (`SweepDeadCycles`) is deferred to the concurrent
`Concurrent`/lazy bucket and runs chunk-parallel between pauses.** Ours performs the equivalent
`SweepAndSelectDefrag` **inside** the STW finish, over the whole heap. Deferring the window's finish
too long (our `g_traceMaxSpan`, growth cadence) also means (B) mature garbage waits for that rare heavy
finish instead of being reclaimed promptly & concurrently at a 20%-garbage trigger.

---

## 5. "Allocate-black" / handling of objects born during a trace

| | Paper | Reference | Ours |
|---|---|---|---|
| New-object liveness | implicit: young objects live in fresh lines, reclaimed by RC if never incremented (paper §1, §3 "implicitly dead young objects") | New objects are **RC=0 in `BlockState::Nursery` blocks**. No per-object "allocate-black" walk; nursery blocks are swept by reading their RC table (ref `block.rs rc_sweep_nursery`, `gc_work/rc.rs dont_evacuate`) | **`ConcurrentTraceFinish` walks EVERY object in EVERY committed region** to mark those born ≥ region snapshot high-water (ours `LXRGCHeap.cpp:5669+`, the allocate-black parse) |

**Root cause (A) #3 — the allocate-black whole-heap object parse.** This is the clearest single flaw.
The reference **never parses the heap object-by-object to find "new" objects** — "new" is encoded in
`BlockState::Nursery` + RC=0, and reclaimed by reading the block's 2-bit RC table (a few `u128`
loads/line). We instead added an **O(live-heap) parse over all committed regions at each trace finish**
(profiled at ~125–200 ms via `LXR_FINISH_PROFILE`, growing with footprint). Our own comment calls it "a
dominant heavy-cycle trace-finish cost." Parallelizing it (`LXR_PARALLEL_ALLOCBLACK`) reduces the
constant but not the O(heap) scaling — and it grows precisely as footprint grows, coupling (A) to (B).

---

## 6. Evacuation / defragmentation

| | Paper | Reference | Ours |
|---|---|---|---|
| Young (nursery) copy | judicious young copy each pause to defrag blocks (paper §3) | copy RC=0 nursery objects during `ProcessIncs`, `NURSERY_EVACUATION=true`; disabled for the pause if to-space pressure (`NO_EVAC`) (ref `gc_work/rc.rs`) | `CopyYoungSurvivors`/D-copy **default OFF** (unsound remset); young defrag rides STW `Evacuate` (ours `CollectNursery:7959`, memory: nursery-copy off) |
| Mature evac | incremental, evac sets, **time budget** (paper "at each STW pause … may use a time budget") | select blocks ≥ **50% dead**, greedy to a **copy-byte budget** (`defrag_headroom`); remset-driven fix-up at FinalMark; `RCEvacuateMature` (ref `mature_evac.rs`) | `Evacuate` STW at finish; adaptive **copy budget** `LXR_EVAC_BUDGET_MB` (8 MB floor→256 MB under pressure) + region cap (ours `LXRGCHeap.cpp:5319-5360`, `Evacuate:6753`) |
| Copy timing | STW only (paper §1) | STW at pause (nursery in RCProcessIncs; mature closure at FinalMark) | STW at finish/RC pause |

**Assessment.** We *have* a copy budget (good, faithful to the paper's time/byte budget). Two gaps:
(1) **nursery copy is disabled** (our default), so a major footprint-reducer the reference runs *every
pause* (defragmenting young survivors so whole blocks become reclaimable) is missing for us — a
contributor to (B). (2) Our mature evac is bundled into the heavy STW finish rather than the
reference's lighter FinalMark + concurrent sweep.

---

## 7. Reclamation cadence — when free lines/blocks return

| | Paper | Reference | Ours |
|---|---|---|---|
| Young | reclaim free young lines/blocks **every pause** (paper §1) | `sweep_nursery_blocks()` in every `release()`; dead nursery block → page resource, holey → `reusable_blocks` (ref `block_allocation.rs`, `block.rs`) | `CollectNursery` reclaims RC-0 young every RC pause (ours `7959+`; works, `winReclaims` confirmed) |
| Mature | lazy concurrent (paper §3.2.5) | `SweepBlocksAfterDecs` (after lazy decs) + `SweepDeadCycles` (after FinalMark) — **all concurrent, between pauses**, then `flush_page_resource()` returns pages at `end_of_lazy` (ref `mature_sweeping.rs`, `global.rs on_lazy_sweeping_finished`) | mature reclaim happens **STW inside the finish** (`SweepAndSelectDefrag`) + on-pause zero-count cascade; region decommit via pending-decommit drain (ours `10539`, memory: DrainPendingDecommit) |
| Page return to OS | — | guaranteed via `LazySweepingJobsCounter` → `flush_page_resource()` when all lazy work done (ref `global.rs`) | region decommit under `g_chunkLock`; growth-triggered, less prompt |

**Root cause (B) #2 — mature reclamation is coupled to a rare heavy STW finish instead of a prompt
concurrent sweep.** The reference reclaims mature memory *continuously* between pauses (lazy decs free
blocks as counts hit 0; `SweepDeadCycles` frees cycles after each CM) and *guarantees* page return via
`LazySweepingJobsCounter`. Ours only reclaims mature at the STW finish, which our cadence makes rare
(long windows, growth budget), so committed memory ratchets up between finishes.

---

## 8. Pause structure — STW vs concurrent, per phase

**Reference (ref `global.rs`, `concurrent/mod.rs`) — four pause kinds, none whole-heap:**

- **`RefCount`** (most frequent, sub-ms target): StopMutators+flush; root incs; `ProcessIncs`
  (nursery evac); nursery block sweep. **Decs deferred concurrent.**
- **`InitialMark`**: RefCount work + reset block states + `schedule_defrag_selection` + start CM.
- **`FinalMark`**: root incs + drain CM tail + SATB drain + **budgeted** mature-evac closure + schedule
  **lazy** cycle/LOS sweep. (Cycle sweep itself is *concurrent after*.)
- **`Full`** (emergency only): decs + full closure + defrag + cycle sweep **all STW**.

**Ours (ours `RunLXRCollection:10317`):**

- **RC pause**: `ProcessModifiedBuffers` (incs **+ decs + free cascade**) + `CollectNursery` (+ evac).
- **Snapshot pause**: STW snapshot to start a window.
- **`meFinish` pause (the heavy one)**: `ConcurrentTraceFinish` (residual SATB + **allocate-black
  whole-heap parse** + final rescan) → `ProcessModifiedBuffers` (**whole-window replay**) →
  `Evacuate` (budgeted) → `SweepAndSelectDefrag` (**O(heap) sweep**). (ours `10500-10560`)
- **`TracePause` (forced/STW trace)**: `BackupTrace` + `Evacuate` + `SweepAndSelectDefrag` STW.

**Root cause (A) — summary.** Our `RefCount` pause is heavier than theirs (it pays decs + free cascade
STW), and our `meFinish`/`TracePause` are **the whole-heap STW operations the paper's design exists to
avoid**. The reference's only "everything STW" pause is the *emergency* `Full`; for us it is the
*normal* trace finish.

---

## 9. Triggering & per-pause work bounding

| | Paper | Reference | Ours |
|---|---|---|---|
| Pause trigger | survival-rate prediction bounds **expected work per epoch** (paper §3.2.2 novelty v) | pause when **predicted survival ≥ 128 MB** (`MAX_SURVIVAL_MB`) or space full; 2× conservative EWMA (ref `global.rs collection_required`, `mod.rs`) | growth budget × survival-EWMA + increment count 32 Ki + wastage 5% (ours `10120-10168`) |
| Hard per-pause cap | implicit via young size | **Yes** — young is capped at ~128 MB survival ⇒ inc/copy work per pause is bounded | **No hard young cap**; work bounded only indirectly by triggers; windows span many epochs (memory: spans 8–243) |
| Cycle-GC trigger | wastage heuristic | `mature garbage ≥ 20%` of heap | wastage 5% + span cap `g_traceMaxSpan=3` |

**Root cause (A) #4 & (B) #3 — no hard young-size cap; work bounded only indirectly.** The reference's
`MAX_SURVIVAL_MB=128` is the *linchpin* that keeps `RefCount` pauses sub-ms: a pause fires before young
grows past a fixed budget, so `ProcessIncs` and nursery copy are bounded **regardless of heap size**.
Our triggers modulate a *growth* budget (a ratio of committed), which the stored `footprint cadence`
memory shows is a **positive-feedback loop** — as committed grows, the budget grows, pauses get rarer
and *heavier*, and both footprint and pause tails blow up. This is the mt-throughput storm result
exactly (244 ms avg, 1.7 s max, 7.4 GB).

---

## 10. Parallelism

| | Reference | Ours |
|---|---|---|
| Model | typed `WorkBucketStage` packets, work-stealing across N workers, every phase parallel (ref `gc_work/*`) | mark pool + shared-stack drain (`DrainMarkStackShared`), parallel allocate-black, parallel sweep lanes (ours; memory: shared-stack fix) |
| Inc/dec | `ProcessIncs`/`ProcessDecs` packets, recursive children work-stolen (ref `gc_work/rc.rs`) | serial-ish `ProcessModifiedBuffers` under `m_collectLock` (ours `4798`) |
| Sweep | chunk-range packets (`SweepDeadCycles`, `SweepBlocksAfterDecs`) work-stolen | lane-partitioned STW sweep |

**Assessment.** Both parallelize. But the reference parallelizes **concurrent** work (between pauses);
we parallelize **STW** work (inside the pause). Parallelism reduces our pause constants but cannot fix
the fact that the work is *on the pause* and *O(heap)*.

---

## 11. Footprint control

| Mechanism | Reference | Ours |
|---|---|---|
| Eager line/hole reuse | dead lines → `reusable_blocks`, mutator recycles before clean block (ref `immixspace.rs get_reusable_block`) | line carving in `rc-reclaim` (`carvedRuns`), `g_lineReuseMinBytes` 8 KiB min run (ours `3183`, `9539`) |
| Nursery copy defrag | **every pause** (`NURSERY_EVACUATION`) | **OFF by default** (unsound remset) |
| Prompt mature reclaim | lazy concurrent decs + 20%-garbage cycle GC | rare STW finish |
| Guaranteed page return | `LazySweepingJobsCounter`→`flush_page_resource` | growth-triggered decommit |
| Young size bound | **128 MB survival cap** | none (growth ratio) |

**Assessment.** Every one of the reference's footprint controls is either weaker or off in ours. The
biggest: **no young-size cap + mature reclaim only at a rare STW finish** ⇒ committed ratchets up.

---

## 12. Root-cause synthesis — mapping symptoms to divergences

**Symptom (A1): baseline `RefCount` pause too high (reference is sub-ms; we are single-digit-to-tens
of ms).** The *frequent, light* pause. Fixed by making our RC pause do only bounded, deferred-dec work:
1. **On-pause decrements + zero-count free cascade** (§3) — the reference runs these **lazy/concurrent**
   by default (`LAZY_DECREMENTS=true`); we pay them STW inside `ProcessModifiedBuffers`. *The single
   biggest lever for the common pause.*
2. **No hard young-size cap** (§9) — the reference bounds per-pause inc/copy/sweep **volume** via
   `MAX_SURVIVAL_MB=128` independent of heap size; our growth-budget cadence lets it grow unbounded.
3. **Increment budget too large + serial coalesce** (§9/§10) — our `g_incrementTrigger=32768` is
   self-documented as **~16 ms** of inc work, and `ProcessModifiedBuffers` coalesces **serially** under
   `m_collectLock`, where the reference processes `ProcessIncs` in **parallel 1024-slot packets**. Even
   after #1–#2, this keeps the common pause at single-digit ms rather than sub-ms.

**Symptom (A2): multi-second trace-*finish* spikes (reference `InitialMark`/`FinalMark` are a few ms).**
The *heavy, occasional* pause. Fixed by removing whole-heap work from the finish:
1. **Allocate-black whole-heap object parse at every trace finish** (§5) — O(live-heap) STW; the
   reference has *no such parse* (nursery = block-state + RC table). *Biggest structural flaw.*
2. **STW cycle/mature sweep at the finish** (§4, §7) — `SweepAndSelectDefrag` is O(heap) STW; the
   reference's `SweepDeadCycles`/`SweepBlocksAfterDecs` are **concurrent, chunk-parallel, between pauses**.
3. **Over-long windows** (§9) — the same missing young/work cap (A1 #2) lets a trace window span dozens
   of epochs, so the finish's residual RC replay + allocate-black + sweep all grow. The hard cap shrinks
   the window, shrinking the finish.

**Symptom (B): footprint 2–10× larger.**
1. **Mature reclamation coupled to a rare STW finish** instead of prompt concurrent lazy sweep (§7).
2. **Nursery copy disabled** (§6, §11) — the reference defragments young **every pause**, turning
   partially-dead blocks into fully-reclaimable ones; we don't, so fragmented blocks accumulate.
3. **Positive-feedback growth cadence** (§9, stored `footprint cadence` memory) — budget scales with
   committed, so collections rarefy as memory grows.
4. **1-byte RC** vs 2-bit (§1) — a fixed ~4× RC-metadata tax (secondary).

**The coupling that makes it feel hopeless:** (A) and (B) reinforce each other. Longer windows / rarer
finishes ⇒ larger heap ⇒ the O(heap) allocate-black parse & sweep get *slower* ⇒ bigger pause spikes ⇒
and meanwhile mature garbage isn't reclaimed ⇒ bigger footprint ⇒ … This is why nothing improves in
isolation and why the mt-throughput storm degenerates to 7.4 GB / 1.7 s.

---

## 13. Prioritized fix plan (highest leverage first)

Each item cites the reference mechanism to port and the paper section that motivates it.

1. **Eliminate the allocate-black whole-heap parse; make "young" a block/line-RC property.**
   Adopt `BlockState::Nursery` + RC-table semantics (ref `block.rs`, `gc_work/rc.rs dont_evacuate`;
   paper §1 "implicitly dead young objects"). New objects are simply RC=0 in nursery blocks; reclaim by
   reading the 2-bit RC table, never by walking objects. *Removes the dominant O(heap) STW cost and its
   footprint coupling.*

2. **Make decrements + the zero-count free cascade lazy & concurrent by default.**
   Flip the default from `g_onPauseRC` on-pause processing to the `SnapshotModifiedBuffers` +
   off-pause replay path, mirroring `LAZY_DECREMENTS=true` (ref `mod.rs`, `gc_work/rc.rs`; paper §3.2.5).
   *Removes the measured 2–3.4 s finish-buffers spike from the pause.*

3. **Move cycle/mature sweep off the pause.**
   Port `SweepDeadCycles` (rc!=0 && !marked ⇒ dead) as a concurrent, chunk-parallel job after FinalMark,
   with a `LazySweepingJobsCounter`-style page-return guarantee (ref `mature_sweeping.rs`, `global.rs`).
   Keep only a *light* FinalMark STW (root incs + CM-tail drain + budgeted mature-evac closure).

4. **Introduce a hard young-size trigger (`MAX_SURVIVAL_MB` analogue).**
   Replace/augment the growth-ratio budget with "pause when predicted young-survival ≥ fixed cap"
   (ref `global.rs collection_required`, `mod.rs`; paper §3.2.2). *Breaks the positive-feedback loop;
   bounds per-pause work independent of heap size.*

5. **Re-enable nursery copy (soundly).**
   The reference copies RC=0 nursery survivors every pause with a proper per-block remset and to-space
   pressure guard (`NO_EVAC`) (ref `gc_work/rc.rs`, `mature_evac.rs`; paper §3). Our D-copy was disabled
   for remset-completeness bugs (stored memories); adopting the reference's block-state + reuse-count
   remset validation is the sound path. *Directly attacks footprint (B).*

6. **Bound and parallelize per-pause increment processing.**
   Lower `g_incrementTrigger` (currently 32768 ≈ **~16 ms** of inc work, `LXRGCHeap.cpp:10133`) and
   replace the **serial** `ProcessModifiedBuffers` coalesce (under `m_collectLock`) with **parallel
   fixed-size inc packets** like the reference's `ProcessIncs` (1024 slots/packet, work-stolen) (ref
   `gc_work/rc.rs`, §10; paper §3.2.2 "frequent light RC pauses"). *This is the item that takes the
   common RC pause from single-digit ms down to sub-ms; #2 and #4 alone leave it at a few ms.*

7. **(Secondary) Narrow RC to 2 bits** with saturation at 3 (ref `util/rc.rs`; paper §3.2.1) to shed the
   ~4× RC-metadata tax — do this only after 1–6 land.

**Expected effect — which fix targets which symptom:**

| Fix | A1: sub-ms baseline RC pause | A2: kill trace-finish spikes | B: footprint |
|---|:---:|:---:|:---:|
| 1. Remove allocate-black whole-heap parse | — | ✅ **primary** | ✅ (unblocks coupling) |
| 2. Lazy/concurrent decrements | ✅ **primary** | ✅ (removes on-pause replay) | — |
| 3. Cycle/mature sweep off-pause | — | ✅ **primary** | ✅ (prompt reclaim) |
| 4. Hard young-size cap | ✅ (bounds pause volume) | ✅ (shrinks windows) | ✅ (breaks feedback loop) |
| 5. Re-enable nursery copy | — | — | ✅ **primary** |
| 6. Bounded + parallel increments | ✅ **the sub-ms step** | — | — |

**Reading the table:** **A1 (sub-ms common pause) = fixes 2 + 4 + 6** — 2 removes the on-pause dec
cascade, 4 bounds the per-pause volume, and 6 is what actually crosses from a few ms to sub-ms. **A2
(no multi-second spikes) = fixes 1 + 3 + 4** — 1 and 3 remove the whole-heap work from the finish, 4
keeps the window (and thus the finish) small. **B (footprint) = fixes 1 + 3 + 4 + 5.** No single fix
delivers sub-ms; the algorithm is sound — our realization turned its concurrent/bounded phases into
whole-heap STW work, and these fixes put them back.

---

## Appendix — source citation index

- **Paper**: arXiv:2210.17175 — §1 (design, Immix, implicit dead young, STW-only copy), §2.2 (barriers),
  §3.2.1 (2-bit coalescing RC), §3.2.2 (RC/survival triggers), §3.2.5 (lazy decrements), §3.3 (RC remsets).
- **Reference** (`wenyuzhao/mmtk-core` @ `lxr-x/simplified`): `plan/lxr/global.rs`,
  `plan/lxr/mod.rs`, `plan/lxr/barrier.rs`, `plan/lxr/gc_work/rc.rs`, `plan/lxr/gc_work/tracing.rs`,
  `plan/lxr/gc_work/mature_evac.rs`, `plan/lxr/gc_work/mature_sweeping.rs`,
  `plan/lxr/gc_work/nursery_sweeping.rs`, `plan/lxr/mature_evac.rs`, `plan/lxr/block_allocation.rs`,
  `plan/lxr/mutator.rs`, `policy/immix/{block,line,immixspace}.rs`, `util/rc.rs`, `plan/concurrent/mod.rs`.
- **Ours**: `src/LXRGC/native/LXRGCHeap.cpp` (`RunLXRCollection:10317`, `ConcurrentTraceFinish:5669`,
  `ProcessModifiedBuffers:4798`, `Evacuate:6753`, `CollectNursery:7959`, evac budget `5319-5360`,
  trigger policy `10120-10168`), `src/LXRGC/native/LXRGC.h` (`kBlockSize:90`, `kLineSize:91`,
  `RCSlot:244`).
