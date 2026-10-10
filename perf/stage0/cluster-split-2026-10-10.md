# Cluster light list staged split — 2026-10-10

The per-frame cluster light list update (`ClusterLightLists::SetSources`, the `clusters` / `clust
lists` rows) was the largest single CPU row on the owner demos: ~7.4-7.7 ms/frame of a ~26 ms
serial frame, of which the per-cluster top-up loop measured ~4.0-4.6 ms (mark 0.10, grid 0.02 after
the sub-slot split in `3c8e735e`). The stage now runs as three graph nodes when `r_tasks 1`; the
serial branch runs the same three stages inline with one slice.

Artifacts: commit `2491c038`; Debug executable SHA256
`2D97B5C69374240E09554050AD9229011E2B9975A024D3B691005005ECA490B8`
(`build/Debug/qray-build.json`, build_win.ps1, revision `3c8e735e` plus this commit's nine source
files); demo `ad_particle_heavy` SHA256 `2653D5878152894B2400514C282D7462094B1CBBCEEEA73FD10C5FEA5AD93C0F`;
raw blocks in `build/Debug/ad/benchmark.log`, per-frame captures in
`build/Debug/ad/benchmark-frames-20261010-*.csv`, and the assert console capture
`build/Debug/qconsole.log` from the `-condebug` run below.

## What changed

- `renderer/Source/ClusterLightLists.{h,cpp}`: `SetSources` splits into `BeginSources`
  (ingest, change detection, shape decision, pass 1, dirty marking, grid), `RunTopUpSlice`
  (per-cluster top-up over the frame's work list) and `FinishSources` (slice merge, per-shape
  sums, `FillLists`, tail rebuild/overflow, validate, publication, report). Per-slice counter
  deltas, tail-dirty marks and additive counters are merged in the finish; the routing pointer is
  `thread_local` (`activeSliceTls`), since slices run concurrently.
- `renderer/Include/qray/qray.h`, `qray.cpp`, `VulkanDevice.{h,cpp}`:
  `qrBeginClusterLightSources(info, sliceCount)`, `qrRunClusterLightSourceSlice(slice, sliceCount)`,
  `qrFinishClusterLightSources()`; `qrUploadClusterLightSources` composes the three with
  `sliceCount = 1`.
- `Quake/gl_rlight.c`: `RT_ClusterLightListsUpload` splits into
  `RT_ClusterLightListsPrepare/Slice/Finish`; the `RT_PROF_CLUSTERS` bracket spans prepare..finish;
  the stats/grants readback moved to the finish; the old name remains as the serial composition.
- `Quake/gl_rmain.c`: the cluster work leaves `R_DrawViewModelTask`; new tasks
  `cluster_prepare_task` (depends on the viewmodel task), `cluster_topup_task` (indexed,
  `RT_CLUSTER_SLICES = 16`, depends on prepare) and `cluster_finish_task` (depends on top-up, feeds
  `draw_done_task`); the serial branch calls the composition after `R_DrawViewModelTask (NULL)`.
- Under `rt_cluster_assert 1` the finish also checks that `granted[li]` equals the number of slots
  naming the light after the merge (the invariant the delta scheme relies on).

## Measured runs (owner demo `ad_particle_heavy`, points-on config, cache on)

Mode is inferred from `cpu.wait_ms` (> 0 = task mode; `r_tasks` is not recorded in the log). The
split was built three times during the session (pre-fix, after the thread-local routing fix, and the
frozen build at 02:48:43 that carries this commit's code); runs from different builds are listed
separately and must not be averaged together. The serial path is the same code as before (one slice
through the same stages), so its band tracks the session, not this change.

| time | mode | build | frames | fps | clust topup | frame |
|---|---|---|---:|---:|---:|---:|
| 00:33:47 | task | pre-split | 2855 | 41.1 | 4.15 | 19.23 |
| 01:04:18 | task | pre-split | 2901 | 41.8 | 3.98 | 18.95 |
| 02:39:58 | task | split, pre-fix | 3340 | 48.1 | 0.78 | 15.63 |
| 02:42:42 | task, assert | split, pre-fix | 3205 | 46.2 | 0.82 | 16.69 |
| 02:48:08 | task, assert | split, pre-fix | 3112 | 44.9 | 0.83 | 17.47 |
| 02:50:12 | task, assert | frozen | 3082 | 44.4 | 0.84 | 17.61 |
| 02:51:28 | task | frozen | 3076 | 44.3 | 0.85 | 17.67 |
| 03:08:13 | task, assert, `-condebug` | frozen | 2898 | 41.8 | 0.86 | 19.11 |
| 02:44:52 | task (idle demo) | split, pre-fix | 2544 | 47.1 | 0.40 | 16.21 |
| 01:03:01 | serial | pre-split | 2667 | 38.4 | 4.11 | 25.65 |
| 00:28:22 | serial | pre-split | 2596 | 37.4 | 4.25 | 26.35 |
| 00:29:38 | serial | pre-split | 2553 | 36.8 | 4.33 | 26.77 |
| 02:36:50 | serial | split | 2627 | 37.8 | 4.29 | 26.02 |
| 02:41:26 | serial | split | 2595 | 37.4 | 4.34 | 26.33 |
| 02:43:51 | serial (idle demo) | split | 1947 | 36.0 | 1.97 | 27.51 |
| 02:46:51 | serial | split | 2361 | 34.0 | 4.41 | 29.02 |
| 02:52:45 | serial, assert | frozen | 2221 | 32.0 | 4.63 | 30.87 |

The per-frame captures reproduce the log means (e.g. `clust topup` 3.982 for 01:04:18, 0.785 for
02:39:58, 0.853 for 02:51:28). The 03:08:13 run was launched without the runner's ESC and
foreground handling, so its fps/frame are not comparable; it is the validator evidence below.
Pre-split idle-serial `clust topup` reads 1.80-1.96; the post-split task idle run reads 0.40.

`clusters` and `clust lists` (`stats.totalMs`) now span prepare..finish across tasks and include
the inter-task wait, so they are **not** comparable to the pre-split rows; `clust mark` / `clust
grid` keep their meaning (prepare phase), and `clust topup` is the finish-to-prepare wall of the
slice phase.

## Verification

- `.\build_win.ps1 Debug` produced the frozen executable (SHA256 above); `ctest --test-dir
  build\Debug` ran 4/4 targets clean (`build/Debug/Testing/Temporary/LastTest.log`, 02:48; none of
  the four targets covers `ClusterLightLists`, so the runtime checks below are the guard).
- Assert-clean: the 03:08:13 run carried `r_tasks 1` and `rt_cluster_assert 1` and wrote
  `build/Debug/qconsole.log` via `-condebug`; a grep of that log finds no `cluster validation:` and
  no `grant counters do not match` lines (`config.cfg` archived `rt_cluster_assert "1"` at that
  run's exit). The earlier runner assert runs (02:42:42, 02:50:12) produced no such lines in their
  stdout captures either.
- Two read-only reviews of the diff (facts-in-code and adversarial) confirmed the slice partition
  (each cluster exactly once), the routing completeness, the merge order and the DAG; three final
  reviewers classified the code items CONFIRMED. Their findings fixed before this record: a shared
  `activeSlice` routing pointer (cross-slice race, replaced by `thread_local`), a slice-count
  validation gap, and the `numClusters == 0` underflow in `ComposeBegin` (guarded; the old loop was
  vacuous).

## Known limits

- Pass 1 (`GrantSource`), the tail rebuild and the publication stay single-threaded; the compose
  top-up is sliced like the incremental one, while the compose pass 1 stays serial. The publication
  (~1.8-2.0 ms/frame) is the next candidate (an exact sliced map).
- The counter equivalence rests on an invariant stated nowhere in the code: within a frame, a
  top-up append can never be evicted again (appends run in non-decreasing distance order), so every
  slice-phase removal targets a pre-slice slot. The `rt_cluster_assert` check verifies the end
  state (`granted[li]` vs the slots) but the invariant itself is not asserted.
- `rt_cluster_sampling 1` (overflow/tails) was not exercised by any recorded run of this change;
  the sliced candidate and tail-dirty writes are per-cluster by inspection only.
- Cross-mode byte identity is still not guaranteed at the baseline (arrival-ordered light
  registration under `r_tasks 1`); the acceptance for this change is the validator plus the
  per-cluster equivalence argument, not a byte diff.
- `r_tasks` stays opt-in; the serial path is unchanged.
