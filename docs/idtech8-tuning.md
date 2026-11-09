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

The map-light power fix (`RT_LIGHT_Emit` now uses luminance(resolved.color)) makes the cluster
power range nonzero: `power 0.0000..36,736,236` instead of `0..0`. The post-fix `ad_swampy` matrix
(build `ci-windows-42586988356`, grid 1, ReSTIR 1, volume 1, cache 0, GI 0):

| configuration | mean RGB | near-black |
|---|---|---:|
| baseline | `[50.5, 37.8, 19.6]` | 0.028 |
| grid 0 | `[52.0, 39.3, 21.3]` | 0.019 |
| ReSTIR 0 | `[61.7, 46.3, 23.4]` | 0.013 |
| ReSTIR 1, candidates 32 | `[32.6, 26.1, 17.3]` | 0.091 |
| ReSTIR 1, grid 0 | `[41.7, 32.2, 20.9]` | 0.058 |
| volume 0 | `[49.5, 36.1, 16.7]` | 0.045 |

Frame time from `rt_stats_dump` on `ad_swampy` (build `ci-windows-42586988356`, baseline):
`gpu.frame 10.85 ms`, `gpu.pass direct 3.36 ms`, `indirect 0.78 ms`, `clouds 2.66 ms`,
`setup 2.49 ms`, `compose 0.56 ms`; the remaining passes are below 0.3 ms.

Observations:

- The plain-light fix plus `rt_truelight 0` is what turns the AD maps from black to lit; the
  651/1577 grid sources match the AD light counts.
- The grid path is slightly darker on `ad_swampy` (`[49.5, ...]`, 3.7% near-black) than the
  cluster path (`[52.0, ...]`, 1.9%); the full-cell fallback keeps every light reachable, but the
  grid ranking (`power` is zero for the legacy map lights, see `RT_LIGHT_Emit`) still changes the
  sampled set. This is the deferred grid-capacity/reach tuning target.
- ReSTIR off is still brighter than on (`[61.7, ...]` vs `[50.5, ...]`): the map-light power fix
  did not remove the energy loss. Raising the candidates to 32 makes it worse (`[32.6, ...]`) and
  the global-array path without the grid is still darker (`[41.7, ...]`), so the loss is in the
  target/pdf weighting or the light-count/candidate distribution, not in the grid candidate count.
- The volume path changes little here (`[49.5, 36.1, 16.7]` vs `[49.5, 36.5, 17.7]`); weight
  tuning needs a torch-corridor/reference view.
- Frame times are not captured yet; `rt_stats_dump` is the next measurement step.

## AD ReSTIR isolation

The AD energy gap is not a light-count cap: `rt_light_report` shows 1577 registered lights
(< the 4096-entry array), 0 dropped, and the power range is `0..36.7M` after the map-light fix.
The gap also does not follow the candidate count or the grid: 32 candidates gives
`[32.6, 26.1, 17.3]` and the global-array path gives `[41.7, 32.2, 20.9]`, both darker than the
8-candidate grid path `[50.5, 37.8, 19.6]`.

The remaining difference is the source-PDF floor. `q2SampleGridLights` selects with
`q2FastMass(m, stratumMax) = max(m, 0.001 * stratumMax)`; the non-ReSTIR path divides the selected
shade by that floored `lightPdf`, so the tiny AD map lights (radius about 0.01 m) are over-weighted
relative to their true solid-angle contribution. The ReSTIR target is
`globalLightTargetUnfloored` (exact mass), so the reservoir estimates the unfloored contribution
and is darker than the floored reference. The earlier global proxy floor was removed because it
over-brightened the grid target; the next tuning is a grid-local target floor that matches
`q2FastMass` without reintroducing the global proxy.

### Floored-mass target fix

`q2SampleGridLights` now returns the floored selection mass of the winner, and the ReSTIR target
uses `mass * luminance(color)` instead of the unfloored `q2LightSelectionMass`. This makes
`target/pdf` match the grid source pdf for the tiny AD map lights.

`ad_swampy`, build `ci-windows-42590816027`, `rt_truelight 0`:

| configuration | mean RGB | near-black |
|---|---|---:|
| ReSTIR 1 (floored mass) | `[43.9, 36.1, 25.3]` | 0.028 |
| ReSTIR 0 | `[46.4, 38.0, 26.0]` | 0.024 |

The gap is now ~2.5/1.9/0.7 instead of ~11/8/7 before the fix; the remaining difference is light
style timing and reservoir variance. The absolute means differ from the earlier `[50.5]`/`[61.7]`
pair because the map's flickering lights were captured in a different phase; the on/off gap is the
comparable metric.

## Motion-vector GI: final decision

The two screen-space motion images were tested for the GI history in the indirect pass: the
regular image gave `rt_restir_gi 1` mean `[72.9, 49.5, 41.9]` and the DLSS image `[72.0, 47.5,
40.5]`, against `[64.3, 41.6, 35.8]` with the cache off. Both reproject to wrong history pixels for
this pass and over-brighten the reused field. The camera projection (`volumeViewProj_Prev`) is the
shipped path: build `ci-windows-42549702028` gives `[64.5, 40.2, 34.6]` vs `[63.5, 41.6, 35.8]`
and `ci-windows-42550491186` (half rows) gives `[64.4, 40.3, 34.7]` vs `[66.4, 42.7, 36.7]`. The
motion set 10 stays allocated but is not read; a future fix needs the exact motion convention for
the indirect pass, not a different image.

## Deferred tuning

- The grid capacity question is closed by `docs/idtech8-phase0.md`: 64 IDs/cell is below the AD
  p95, 128 IDs is insufficient at 64/128 u, and the shipped 64-ID grid relies on the full-cell
  fallback. A tail buffer or a 128-ID near-field layout needs the AD scene to judge.
- Cell size/reach/cascade count should be swept on AD with frame-time capture
  (`rt_stats_dump` -> `qperfdump.log`), not only mean RGB; the current runs did not capture frame
  times.
- Volume weight, local-volume bounds and GI history age need a scene with stronger local-light
  variation than the `e1m1` spawn; AD or a torch corridor is the next target.
