# id Tech 8 Cascaded Irradiance Volumes

Status: first slice. Date: 2026-11-07.

Branch: `feat/idtech8-volumes`, stacked on `feat/idtech8-restir`.

## Scope of this slice

The volume ABI and CPU oracle came first (same pattern as the grid's CPU candidate histogram).
The GPU path is now added on top:

- 6 nested cascades x 16^3 probes, first cascade 1,280 u (32 m) and every next one doubling.
- 10^3 hand-placed local volumes, up to 128 slots.
- Probe position = cell centre; cascade centre snapped to 640 u (the grid's rebuild snap).
- 4-coefficient RGB SH per probe (L0, L1x, L1y, L1z), 48 B per probe.
- Interleaved update schedule: one cascade and one local volume per frame.
- Trilinear lookup with local volumes taking precedence over cascades; the smallest containing
  local volume wins.
- SH helpers: `AccumulateRadiance`, `NormalizeSh` (4*pi / sampleCount) and the Lambertian
  convolution `EvaluateIrradiance` (0.886227 for L0, 1.023328 for L1).

## GPU path

- `RhiRtDirectPass` owns a 152,576-probe `ShLightVolumeProbe` atlas (7.3 MB) bound at set 6
  binding 18. It is allocated once and cleared when the 640 u camera snap moves.
- A second ray-tracing pipeline (`RtLightVolume.rgen.hlsl`) runs one dispatch of 4,096 probes per
  frame, for `cascade = frameId % 6`. Each probe traces 16 directions, shades the hit with the sun
  NEE and emission, uses the uniform sky fallback on a miss, and stores the SH4 integration.
- The update dispatch is 4,096 cascade probes plus one 10^3 local volume per frame; the local slot
  is `frameId % localVolumeCount`, so the report's one-cascade + one-volume schedule is kept. The
  atlas is cleared when the local-volume list changes.
- The indirect raygen (`RtQ2Indirect.rgen.hlsl`) samples the atlas at the surface position with the
  same trilinear lookup as the CPU oracle, converts the integrated coefficients to the screen SH
  layout `(L0, Ly, Lz, Lx)` and adds them with the host weight. A point inside a local volume
  reads the local 10^3 field; the smallest containing local volume wins over the cascades.
- Host cvars: `rt_light_volume` (default 0), `rt_light_volume_weight` (default 0.25, clamped to
  0..1; the volume SH is added on top of the screen-space GI in this first slice) and
  `rt_light_volume_rays` (default 16, clamped to 16..64). The report's quality tiers are 64/32/16
  visibility rays per probe; the shader loops and normalizes by the actual count, so 32 is also
  accepted as an intermediate step.
- Hand-placed local volumes: `rt_light_volume_local_add [bounds]` places a 10^3 volume at the
  current view origin (default bounds 512 u), `rt_light_volume_local_clear` drops the list and
  `rt_light_volume_local_list` prints it. Up to 8 slots; the list is session state.
- The volume pipeline and table are optional: if the shader/pipeline cannot be created, the
  direct/indirect path keeps working and the atlas stays zero.

Probe cull fix (`fix/idtech8-volume-probe-hits`): the probe rays inherited the primary pass's
`RAY_FLAG_CULL_FRONT_FACING_TRIANGLES`. A diagnostic that traced one camera ray and one probe ray
per invocation with `RAY_FLAG_NONE` hit both (volume-on deltas: `SHADOW_DIRECT` +5,229,
`SHADOW_INDIRECT` +2,233, against +0 with the culled `traceIndirectRay`). The probe trace now uses
a dedicated `traceVolumeRay` with `RAY_FLAG_NONE` and the same indirect cull mask; the
primary/direct/indirect passes keep their cull.

Runtime on `test/idtech8-volume-probe-fix-runtime` shows the fix is necessary but not sufficient:
with the probe trace on `RAY_FLAG_NONE`, the cascade probes at the e1m1 spawn still produce only
~55-60 cache hits over 30 frames. A camera-local volume bounds sweep does not change that — 128:
59, 256: 47, 512: 55 hits — while a local volume at the world origin gives 1,549. The trace and
cache work and shrinking the probe cube does not help, so the remaining loss is the sampled ray
directions or the per-probe position mapping, not the cull and not the cube size. The volume still
reads mostly sky. The next diagnostic is a fixed-direction (axis) probe-ray hit counter, which
needs a new build; GitHub Actions was in a major outage at this point.

Limitations of this GPU slice: the local volumes are session-placed through the console, not yet
authored in a level file; the atlas is cleared on a snap change and refills one cascade and one
local volume per frame. Hit shading uses sun NEE, one grid/cluster light NEE and emission; the sky
miss uses the uniform sky fallback. The volume SH is added on top of the screen-space GI (not a
replacement); 0.25 is the tested first-slice weight.

## Probe-unit fix (2026-11-09)

The probe path was not a cull problem after all. The renderer scene is metric: the camera uniform
reads `x = 12` for Quake `x = 480`, while `Q2_LIGHT_VOLUME_BASE_BOUNDS`/`_SNAP` were the report's
Quake values 1280/640. The probe cube was therefore 1280 m across (~40x the map), every cascade
origin snapped to 0, and every probe sat outside the level; the volume saw only sky.

Measured with a shader that encoded the first probe's position into a ray-stat:
`probePosition.x = -600` for a camera at 12 m, i.e. `origin = 0`, `bounds = 1280`.

Fix:

- `LightVolumeGpu.hlsli`: `Q2_LIGHT_VOLUME_BASE_BOUNDS 32.512`, `_SNAP 16.256` (1280/640 Quake
  units x 0.0254 m).
- `LightVolume.h`: `kBaseBounds = 32.512f`, `kSnap = 16.256f`.
- `gl_vidsdl.c`: local-volume origins and bounds are converted with `QUAKEUNIT_IN_METERS` before
  the upload.
- `LightVolumeTests.cpp`: literals moved to meters; 85 checks, 0 failures.

Hot-swap verification on the existing cull-fix build (`ci-windows-42671545279`, e1m1,
`rt_radiance_cache 1`, split 0): world-radiance-cache hits over 30 frames went from ~55 to
**58,582** (max 984/frame, 501 misses, 0 `NVRHI::ERROR`). The shader constants are verified in the
runtime; the host-side local-volume conversion needs the next build.

## Runtime evidence (Proton 11.0, RADV RX 9070 XT)

Build `ci-windows-42412543551`, `e1m1` (`the Slipgate Complex`), `rt_light_grid 1 rt_restir 1
rt_light_volume 1 rt_light_volume_weight 0.25`:

- `NVRHI::ERROR` count: 0; the volume pipeline and shader table were created (no
  "light-volume update pipeline is unavailable" warning).
- `rt_light_volume` reported `1`, `rt_light_volume_weight` reported `0.25` through `cvarlist`
  (the earlier `RT_LockCvar` ROM lock silently forced these command-line values to 0 and is removed).
- Mean RGB on the same spawn: volume on `[64.5, 41.5, 35.9]`, volume off `[66.3, 29.3, 19.8]`;
  the world-space volume therefore reaches the final gather. At weight 1/4 the tint is much
  stronger, which is why the default is 0.25 and the feature stays opt-in.

## Runtime evidence: quality tiers and local volumes (Proton 11.0, RADV RX 9070 XT)

Build `ci-windows-42538583016` (quality tiers + local volumes), `e1m1`, `the Slipgate Complex`,
`rt_light_grid 1 rt_restir 1 rt_light_volume 1 rt_light_volume_weight 0.25`:

- `rt_light_volume_rays 16`: mean RGB `[64.3, 41.7, 35.9]`.
- `rt_light_volume_rays 64`: mean RGB `[64.8, 41.7, 36.0]`, mean absolute channel difference 2.1
  against the 16-ray frame. `cvarlist` reports `64`; `NVRHI::ERROR` count 0.
- Local volume at the view origin (`rt_light_volume_local_add 512`): mean RGB
  `[65.2, 41.9, 36.2]` vs `[64.3, 41.7, 35.9]` without it, mean absolute channel difference 3.0.
  The run-to-run difference of the same configuration is 1.8, so the 512 u volume changes only the
  surfaces inside its 256 u half-extent.

The local lookup itself is proven by a temporary debug build (renderer `20530a3`, release
`ci-windows-42539730885`) that replaces every local-volume sample with a green SH: with
`rt_light_volume_local_add 16384` the frame mean is `[11.9, 121.3, 9.2]` and 98% of samples are
green-dominant, while the same build without a local volume stays neutral. The production shader
keeps the normal SH.

## Constants

| | value |
|---|---:|
| cascades | 6 |
| cascade probes | 16^3 = 4,096 |
| first cascade bounds | 1,280 u |
| cascade 5 bounds | 40,960 u |
| local volume probes | 10^3 = 1,000 |
| local volume slots | 128 |
| total probe slots | 152,576 |
| probe record | 48 B (SH4 RGB) |

Cascade `c` has `bounds = 1280 * 2^c`, `cell = bounds / 16`. The camera origin is snapped to
640 u on every axis, so a moving camera crosses a rebuild boundary at most once per 640 u.

## Update schedule

`GetUpdateSchedule(frameIndex, cascade, localVolume)` returns `cascade = frameIndex % 6` and the
`(frameIndex % activeLocalVolumes)`-th enabled local volume (or `-1`). One cascade and one local
volume are updated per frame, matching the report's interleaved update.

## Lookup

- `FindCascade` returns the finest cascade that contains the point (`-1` outside all).
- `FindLocalVolume` returns the smallest enabled local volume that contains the point, or `-1`.
- `GetCascadeLookup` / `GetLocalLookup` return the `(0,0,0)` probe and the eight weights indexed
  `[z][y][x]`, matching the probe layout `(z * dim + y) * dim + x`.
- `SampleIrradiance` prefers a local volume, then the cascade, and trilinearly blends the eight
  probes with `EvaluateIrradiance`. It returns false outside all volumes; the final gather keeps
  the existing screen-space SH as the fallback.

## Tests

`qray_light_volume_tests` (85 checks locally) covers:

- cascade bounds and the finest-cascade choice, including the inclusive edge and the outside case;
- probe positions for the first, last and centre probe of cascade 0, and cascade 5's cell size;
- trilinear weight sums and the probe-centre delta;
- local-volume containment, precedence, base corner and probe layout;
- the corner mapping — a probe-centre sample reads exactly that probe, and halfway samples blend
  the correct two probes on each axis;
- constant-environment SH (pi * radiance), a directional lobe, and zero-sample normalization;
- the interleaved update schedule with zero, one and two active local volumes.

## Next

1. Local-volume GPU update: the same raygen with the local 10^3 probe grid and its origin/bounds.
2. The world radiance cache: the report splits visibility sampling, cache shading and the volume
   update; this slice still shades the probe ray directly.
3. A final-gather replacement/blend policy once the cascade field is tuned, and level-file
   authoring for the local volumes.
4. The 4-coefficient SH can grow to 9 coefficients only after the atlas layout and sampling are
   stable; the report's 2-band SH matches the current choice.
