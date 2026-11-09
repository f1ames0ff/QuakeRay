# id Tech 8 tuning: feature matrix and AD blocker

Status: first measurements. Date: 2026-11-09.

Branch: `feat/idtech8-tuning`, stacked on `feat/idtech8-restir-gi`.

## Build and scene

- Windows Release `ci-windows-42545111353` (grid + direct ReSTIR + volumes + quality tiers +
  local volumes + radiance cache + GI camera reprojection), Proton 11.0 / RADV RX 9070 XT.
- Map `e1m1` (`the Slipgate Complex`), spawn view, 1280 x 720, screenshot after the scene settles.
- Metric: mean RGB and the fraction of near-black samples (max channel < 8) over an 8-pixel grid.
  This is an image-stability metric, not a frame-time measurement.

## Feature matrix

| configuration | mean RGB | near-black |
|---|---|---:|
| baseline (grid 1, ReSTIR 1, volume 1, cache 0, GI 0) | `[64.0, 41.7, 35.9]` | 0.001 |
| grid 0 | `[65.4, 41.6, 35.8]` | 0.002 |
| ReSTIR 0 | `[47.5, 38.1, 33.8]` | 0.002 |
| volume 0 | `[67.2, 29.5, 19.9]` | 0.007 |
| GI 1 (camera reprojection) | `[63.7, 41.4, 35.7]` | 0.001 |
| radiance cache 1, age 30, cell 16 (`ci-windows-42540803460`) | `[63.2, 41.8, 36.0]` | 0.001 |
| `rt_light_reach_static 15` | `[63.4, 41.7, 36.0]` | 0.001 |
| `rt_light_reach_static 10` | `[64.3, 41.8, 36.0]` | 0.001 |

Observations:

- The grid's full-cell fallback keeps the spawn frame within noise of the cluster path
  (`[64.0, ...]` vs `[65.4, ...]`); the torch loss that motivated the fallback is not visible in
  either configuration.
- Direct ReSTIR is the largest single contribution: turning it off drops mean R from 64 to 47.5.
- The volume path adds the warm tint (R/G/B `[64.0, 41.7, 35.9]` vs `[67.2, 29.5, 19.9]`); the
  0.25 weight is the tested first slice.
- The world radiance cache and GI are close to the noise floor at this spawn (2.0 and 2.4 mean
  absolute channel difference against the same-configuration run-to-run 1.8). Their selection is
  proven by the debug-tint builds (green history/cache counters), not by the mean.
- Reach 10/15/25 m changes the mean by less than 1.5 at the spawn; the near-black fraction stays
  0.001, so the current reach is not visibly cutting lights in this room.

## AD measurements

Arcane Dimensions 1.50 is installed under `<basedir>/ad` and the maps load without saves
(`map ad_tfuma`, `map ad_swampy`). The legacy map-light path has to be selected with
`rt_truelight 0`; the default `rt_truelight 1` skips `RT_UploadAllElights` entirely, so a plain
`"light"` entity mod map stayed black.

The map-light fix (`feat/idtech8-ad-lights`) also accepts a plain `"light"` entity with a positive
intensity and carries its `_color`. Before the fix `ad_tfuma` reported one grid source and mean
`[3.6, 5.4, 5.4]` (90.1% near-black); after it and with `rt_truelight 0`:

| map | grid sources | mean RGB | near-black |
|---|---:|---|---:|
| `ad_tfuma` (Terror Fuma) | 651 | `[44.7, 35.6, 23.6]` | 0.037 |
| `ad_swampy` | 1577 | `[49.5, 36.5, 17.7]` | 0.037 |

The `ad_swampy` feature matrix (grid 1, ReSTIR 1, volume 1, cache 0, GI 0):

| configuration | mean RGB | near-black |
|---|---|---:|
| baseline | `[49.5, 36.5, 17.7]` | 0.037 |
| grid 0 | `[52.0, 39.3, 21.3]` | 0.019 |
| ReSTIR 0 | `[61.1, 45.7, 22.5]` | 0.013 |
| volume 0 | `[49.5, 36.1, 16.7]` | 0.045 |

Observations:

- The plain-light fix plus `rt_truelight 0` is what turns the AD maps from black to lit; the
  651/1577 grid sources match the AD light counts.
- The grid path is slightly darker on `ad_swampy` (`[49.5, ...]`, 3.7% near-black) than the
  cluster path (`[52.0, ...]`, 1.9%); the full-cell fallback keeps every light reachable, but the
  grid ranking (`power` is zero for the legacy map lights, see `RT_LIGHT_Emit`) still changes the
  sampled set. This is the deferred grid-capacity/reach tuning target.
- ReSTIR off is brighter than on (`[61.1, ...]` vs `[49.5, ...]`): with 1577 lights the reservoir
  loses energy relative to the non-ReSTIR path. The `id1` direction was the opposite, so the AD
  target/candidate weighting needs a separate tuning pass (the zero map-light power is the first
  suspect).
- The volume path changes little here (`[49.5, 36.1, 16.7]` vs `[49.5, 36.5, 17.7]`); weight
  tuning needs a torch-corridor/reference view.
- Frame times are not captured yet; `rt_stats_dump` is the next measurement step.

## Deferred tuning

- The grid capacity question is closed by `docs/idtech8-phase0.md`: 64 IDs/cell is below the AD
  p95, 128 IDs is insufficient at 64/128 u, and the shipped 64-ID grid relies on the full-cell
  fallback. A tail buffer or a 128-ID near-field layout needs the AD scene to judge.
- Cell size/reach/cascade count should be swept on AD with frame-time capture
  (`rt_stats_dump` -> `qperfdump.log`), not only mean RGB; the current runs did not capture frame
  times.
- Volume weight, local-volume bounds and GI history age need a scene with stronger local-light
  variation than the `e1m1` spawn; AD or a torch corridor is the next target.
