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

## Evidence pointers

- Baselines: tags `cl-b1-*`; per-frame CSVs `benchmark-frames-20261010-*.csv` (start task
  205555/211642/211718/211754, serial 143715/211830/211906, tfuma 205631).
- Prior records: `perf/stage0/cluster-split-2026-10-10.md` (the staged split; names publication
  as the next candidate), `docs/cluster-lighting-spec.md`, `docs/cluster-implementation-report.md`.
