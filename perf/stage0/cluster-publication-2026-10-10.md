# Cluster light-list publication cost — baseline and plan (2026-10-10)

Owner directive: the cluster pipeline must not rebuild more than the light changes; the
incremental path (`rt_cluster_incremental 1`) and a correct "nothing changed" skip must always
work by default; everything is tested with `r_tasks 1` (the preparation for multithreading);
temporal amortization (publish at most once per N frames, structural changes immediately) is part
of the plan. This record pins the baseline and the batch order; the implementation record follows
in this file once the batches land.

## Baseline (build `BD84BC99…15E1`, HEAD `a7049bd3`, clean tree)

Harness `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1`, saves `qr_ad_start` / `qr_fuma_start`,
15 s captures, medians of window maxima (upper-middle), one instance, machine guard held.

| arm | clusters | upload | fill | topup | wait | frame | fps |
|---|---:|---:|---:|---:|---:|---:|---:|
| start task-1 | 5.09 | 2.28 | 1.51 | 0.85 | 11.56 | 11.58 | 67.3 |
| start task-2 | 4.98 | 2.23 | 1.36 | 0.86 | 11.38 | 11.40 | 36.9 (outlier) |
| start task-3 | 5.03 | 2.24 | 1.50 | 0.84 | 11.66 | 11.68 | 66.4 |
| start serial-1 | 7.16 | 2.01 | 1.02 | 3.34 | 0.00 | 21.50 | 53.2 |
| start serial-2 | 6.52 | 1.95 | 1.03 | 2.68 | 0.00 | 21.50 | 57.1 |
| start task assert | 10.60 | 2.16 | 1.39 | 0.79 | 17.22 | 17.24 | 48.9 |
| tfuma task | 0.16 | 0.01 | 0.00 | 0.00 | 16.17 | 16.18 | 44.4 |
| demo `ad_particle_heavy` task | – | – | – | – | – | 9.94 | 67.2 (4663 frames) |

Assert run clean (no `cluster validation:` / `grant counters do not match` lines). Tags
`cl-b1-*` under `%LOCALAPPDATA%\Temp\opencode\`; blocks in `build/Debug/ad/benchmark.log`.

## Frame shape and root cause (verified from per-frame CSVs and code)

- AD start runs **0 full compositions** per window; ~52% pure-reuse frames and ~48% incremental
  frames. tfuma is 736/736 reuse.
- The whole-list publication (`clust upload`) is **paid on every frame in task mode** (100% of
  frames > 0.5 ms, median ~2.0 ms; 235k list words resolved, ~940 KB of stores plus the offsets
  and tail tables). On serial reuse frames it is correctly skipped (0.004-0.022 ms); the skip
  fires on only 26-33% of frames instead of the 47-54% reuse share, because the predicate
  compares against the slot's own record instead of the device's.
- Two independent reasons break the skip on start:
  1. In `r_tasks 1` the light array indices are assigned in first-registration order while
     entity draws register lights from parallel tasks; the uid sequence changes every frame, so
     the published index words genuinely differ and republication is required (not a false
     negative of the test). tfuma registers only from single-threaded passes and skips.
  2. `FillLists` bumps `listGeneration` unconditionally, so an incremental frame whose packed
     packet is byte-identical still republishes.

## Plan (owner-approved order)

1. **Batch 1 - the skip must work.** Skip-reason counters (per-frame bitmask of the failing
   term, `publicationSkips/publicationCopies` in the cluster stats); device-first predicate (skip
   when the device already holds the identical publication); content-based generation (compare
   the ordered packet - offsets, words, tail offsets/beta, tail entries, counts - and bump only
   when it differs); cheap trims (fill `live = fill - slotHoles`; no redundant writes on skip
   frames). Both modes.
2. **Batch 2 - deterministic registration order.** Flush the entity-pass light registrations in
   a deterministic order at one single-threaded point (the pattern the world/model lights already
   use), so a reuse frame in task mode keeps the uids at their indices and the skip fires there
   too.
3. **Batch 3 - sliced publication.** Move the entry-level resolve/fill work into the existing
   16-slice cluster phase with the V-A amendments: exclusive ownership of every published word
   (including `offsets[hi]` boundaries), full-range initialization (the RHI copies the full
   `listOffsets` 32772 B and `tailOffsets` 65540 B), a Plan/Commit publication API, an
   assert-gated coverage check of the staged bytes, and the serial path recomposed to run the
   same stage functions (today `SetSources` is dead code).
4. **Batch 4 - amortization.** Publish at most once per N frames with immediate publication of
   structural changes (owner idea: 16 ms of light latency is invisible, the saving is ~50%).
   Owner refinement (2026-10-10): spread the refresh with an interleaved (checkerboard) scheme -
   evaluate/rebuild the sources whose stable key has parity P only on frames of parity P, so each
   light is refreshed every two frames while every frame carries half the work (no "double batch"
   frame). Rules: structural changes (a light appears or disappears) are handled immediately; the
   parity key is a stable id (the uid, or the index once Batch 2 lands), never the arrival-ordered
   index; the cheap per-frame light-value upload stays per-frame (flicker latency); the interleave
   applies to the list-side evaluation, marking, top-up and publication. Per-light list staleness
   is bounded by two frames (~33 ms). The content-based skip stays in force: when a frame's half
   evaluates to no change, the packet is identical and the publication still skips entirely, and
   with Batch 3 the publication cost scales with the changed half.

## Verification notes carried from the design falsifiers

- The correct mapping primitive is the multiset of `(uid, arrayIndex)` over the uids the packet
  references (the index-sequence memcmp alone misses a swap).
- The device-first comparison removes the per-slot reuse-run-head false negatives; the device
  record is written at copy-record time, not at GPU completion (keep that assumption stated).
- A tail generation is redundant while `FillLists` bumps unconditionally; with a content
  generation the shadow must compare the ordered packet, tails included, or it can skip real
  changes.
- Known independent defects noticed on this path: the sun uid collides with custom light 0
  (`gl_rmain.c`), dropping the sun when a custom light 0 exists; `SetSources` is dead code.

## Batch 3 - parallel sliced publication (commit `c8a6a945`, build `B5C3F71D…B2C3`)

- The publication splits into `PrepareClusterListPublication` (predicate, range initialization,
  offsets and tail beta), `RunClusterListPublishSlice` (the uid resolve of a disjoint word range,
  one per `RT_CLUSTER_SLICES` task with a per-slice cache; a sampled self-check re-resolves the
  first and last word of every slice against the staging and asserts on mismatch) and
  `CommitClusterListPublication` (tail entries, unresolved handling, device bookkeeping). The
  task graph gains `cluster_publish_task` (indexed 16, after finish) and `cluster_commit_task`
  (before `draw_done`); the serial composition runs the same stages with `sliceCount` 1, so the
  serial publication is byte-identical by construction (no executed byte-diff exists; the
  reviewers verified the source-level equivalence and the sampled check covers the runtime path).
- Row re-scoping: `clusters`/`clust lists` still include the prepare (`clust upload` measures it,
  ~0.02 ms); what moved out of the bracket is the resolve+commit wall, recorded as the new
  `clust publish` row and only on copy frames.
- Measured on AD start task mode: the copy-frame resolve 2.1 -> **0.45-0.47 ms** across three
  runs (dump upper-median; per-frame CSV central tendency ~0.39-0.40, p90 ~0.47); the clean run
  `cl-b4-task-3` shows the `clusters` row 5.03 (baseline band) -> **2.96** and **70.6 fps**
  (1083 frames / 15.33 s) with publication skips 557/526 preserved; tfuma unchanged; assert run
  clean (`clusters` 8.51 + `publish` 0.48).
- Serial is **not literally unchanged**: `clusters` + `publish` = 7.57/7.62 ms against the batch-2
  band 7.34-7.40 (+0.17..+0.23 ms, +2.3-3%) and the isolated publication stage 2.29-2.33 against
  the batch-2 `upload` 1.89-2.01 (+0.3-0.4 ms). Both are within the historical serial spread
  (6.52-7.56 across all pre-batch-3 runs) and the isolated-stage delta has no identified
  mechanism beyond codegen/measurement-bracket differences; re-measure on a quiet machine before
  signing it off. No owner-facing serial regression was observed in the frame totals.
- Scope note: the fill fold ("fold the fill into the publish slices") was part of this batch's
  original scope in the batch-2 plan; it did not land here and moves to the next batch explicitly
  (fill is still 1.32-1.37 ms on copy frames and runs serially in `FinishLists`).
- Statistics and evidence: dump rows are upper-middle (`sorted[n//2]`) values of the per-window
  maxima series; runs `cl-b4-*` under `%LOCALAPPDATA%\Temp\opencode\`, per-frame CSVs
  `benchmark-frames-20261010-2224*.csv`, blocks in `build/Debug/ad/benchmark.log`.
- The low-fps samples (`cl-b4-task-1/2` ~38 fps, `cl-b4-serial-2` 27.9, the demo at 50.8 fps)
  carry an outside-frame gap of +16-17 ms with every changed slot normal; the shape matches the
  engine's 16 ms focus/pause sleep path (`main_sdl.c`) and repeats on pre-change builds
  (`cl-b1-task-2`), so they stand as an environment/harness condition, not as a regression;
  re-run the demo and the repeat arms on a quiet machine before accepting them.
- Remaining: the fill fold (fill 1.32-1.37 ms on copy frames), the published-span reuse (the
  serial resolve lever, `clust publish` 2.3; design and measurement still missing) and the parity
  amortization.

## Evidence pointers

- Baselines: tags `cl-b1-*`; per-frame CSVs `benchmark-frames-20261010-*.csv` (start task
  205555/211642/211718/211754, serial 143715/211830/211906, tfuma 205631).
- Prior records: `perf/stage0/cluster-split-2026-10-10.md` (the staged split; names publication
  as the next candidate), `docs/cluster-lighting-spec.md`, `docs/cluster-implementation-report.md`.

## Batch 1 - the skip fires where the device already holds the lists (commit `505e122c`, build `05DE1BAA…5E85`)

- The publication predicate now compares the current generation and mapping against the device's
  recorded publication (the reuse-run-head skips the slot-local check missed), the packet
  generation is content-based (the ordered packet, tails included, is compared against a shadow
  and bumped only when it differs; reset, overflow toggle and table rebuilds invalidate the
  shadow), and the diagnostics are in: a reason bitmask (`QR_CLUSTER_PUB_*`), `pubskips`/
  `pubcopies` in the `cpu.cluster lists` block line, the dump columns and the cluster panel.
- Measured on AD start: serial skips 26-33% of frames -> 52-55% (the copies settle on the reuse
  share); task mode stayed at 0 skips (the mask reports GEN+PLACES+ORDER+COPY - the arrival-order
  registration); tfuma unchanged; assert run clean.

## Batch 2 - deterministic registration order (commit `5cfe95fe`, build `D4049C51…3528`)

- The light uploads of the parallel draw tasks are captured per slot (world 0, entity slices
  1..NUM_ENTITIES_CBX, alpha NUM_ENTITIES_CBX+1) and flushed in slot order inside the viewmodel
  task, before the map, world-model and teleport uploads and before the cluster prepare. The
  registration sequence repeats for a stable light set, so the device-first skip fires in task
  mode as well; serial mode runs the same capture and flush through the same task bodies.
- Measured on AD start task mode: publication skips 45-64% of frames (three runs: 705/400,
  614/493, 475/580 skips/copies), mask settles on `skip device`; fps 74.4 / 73.5 / 69.5 against the
  baseline band 66.4-67.3; serial unchanged (~50% skips); tfuma unchanged; assert run clean
  (skips 434 / copies 381); demo `ad_particle_heavy` 68.1 fps (baseline 67.2, batch 1 65.3). Two
  serial/tfuma runs (55.2 -> 28.8 fps, 46.0 -> 28.9 fps) coincided with external machine load and
  are recorded as contention outliers, not as a regression.
- Remaining on the table: the copy frames (400-580 per run) still pay the full ~2.1 ms
  publication and ~1.4 ms fill; Batch 3 (sliced publication with the V-A amendments, fill folded
  into the publish slices) and Batch 4 (checkerboard amortization) are next.

