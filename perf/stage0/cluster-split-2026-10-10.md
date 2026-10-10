# Cluster light list staged split — 2026-10-10

The per-frame cluster light list update (`ClusterLightLists::SetSources`, the `clusters` / `clust
lists` rows) was the largest single CPU row on the owner demos: ~7.4-7.7 ms/frame of a ~26 ms
serial frame, of which the per-cluster top-up loop measured ~4.0-4.6 ms (mark 0.10, grid 0.02
after the 3c8e735e sub-slot split). The stage now runs as three graph nodes when `r_tasks 1`; the
serial branch runs the same three stages inline with one slice.

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

## Measured (owner demo `ad_particle_heavy`, SHA256 2653D587…, points-on config, cache on)

| mode | metric | before (pre-split, this day) | after |
|---|---:|---:|---:|
| `r_tasks 1` | `clust topup` avg | 4.04 | **0.84-0.85** |
| `r_tasks 1` | `clust topup` max | 54.4 | 26-27 |
| `r_tasks 1` | `frame` avg | 18.95 | 17.47-17.67 |
| `r_tasks 1` | fps | 41.8 | 44.3-48.1 |
| `r_tasks 1` | `clust fill` / `clust upload` | 0.75 / 2.02 | 0.69-0.71 / 2.03-2.04 |
| `r_tasks 0` | `clust topup` avg | 4.26-4.41 | 4.34-4.63 (unchanged path, K=1) |
| `r_tasks 0` | `frame` avg | 26.3-29.0 | 26.3-30.9 (session band) |
| `r_tasks 1` | `ad_particle_idle` `clust topup` | 1.97 (serial band) | 0.40 |

The serial numbers are the same code path as before (one slice through the same stages); the
spread is the session's own noise. The task numbers come from the same binary and session; the
`topup` row is the finish-to-prepare wall of the slice phase, so it is what the frame pays for the
per-cluster pass.

`clusters` (the pipeline wall) is **not** comparable to the pre-split row: it now spans
prepare..finish across tasks and can overlap the GUI task; on these runs it reads 9.2-9.9 in task
mode while the frame got faster. `clust mark` / `clust grid` keep their meaning (prepare phase).

## Verification

- `.\build_win.ps1 Debug` clean; `ctest --test-dir build\Debug` 4/4.
- `r_tasks 1` plus `rt_cluster_assert 1`: full run clean — no `cluster validation:` lines and no
  grant-counter mismatch from the new check.
- Serial and task runs on both owner demos; no crash, no warning new to the split.
- Two independent read-only reviews of the diff (facts-in-code and adversarial) confirmed the
  slice partition (exactly once per cluster), the routing completeness, the merge order and the
  DAG; findings fixed before this record: a shared `activeSlice` pointer (cross-slice race,
  replaced by `thread_local`), a slice-count validation gap, and the `numClusters == 0` underflow
  in `ComposeBegin` (guarded; the old loop was vacuous).

## Known limits

- Pass 1 (`GrantSource`), the tail rebuild and the publication stay serial; the publication
  (~2.0 ms/frame) is the next candidate (an exact sliced map). The `Compose` spike's top-up share
  halves (54 → 27 max) but the compose path itself stays serial.
- Cross-mode byte identity is still not guaranteed at the baseline (arrival-ordered light
  registration under `r_tasks 1`); the acceptance for this change is the validator plus the
  per-cluster equivalence argument, not a byte diff. See the earlier review record.
- `r_tasks` stays opt-in; the serial path is unchanged.
