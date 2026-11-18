# id Tech 8 Transparency Froxel Irradiance Volume

Status: active implementation (metric 32.0 m box, pre-`ReflRefr` dispatch, particle `inViewDepth` reconstruction; updated 2026-11-17).

Branch: `feat/idtech8-transparency-froxels` (merged), updated in `fix/idtech8-correctness`.

## Scope and architecture

The engine shares one camera-centered froxel irradiance volume across transparents (particles and
glass) and fills it during frame preparation before `ReflRefr` runs:

- **Grid**: camera-centered `32.0 m` (`Q2_TRANSPARENCY_FROXEL_BOX_SIZE = 32.0`, matching
  `QR_QUAKE_UNIT_IN_METERS = 0.025f`) world-space box. Three tiers share the layout: low
  32 x 16 x 32, medium 64 x 32 x 64 (the CPU oracle) and high 128 x 64 x 128 (48 B/probe =
  50.3 MB max allocation); active dimensions come from `transparencyFroxelParams.yzw`
  (`rt_transparency_froxel_quality`, default 1 = medium).
- **Probe**: 4-coefficient RGB SH (L0, L1x, L1y, L1z), 48 bytes.
- **Lookup**: trilinear blend of the eight surrounding froxels and the Lambertian SH convolution
  (`0.886227` for L0, `1.023328` for L1).
- **SH helpers**: `AccumulateRadiance`, `NormalizeSh` (4*pi / sampleCount) and
  `EvaluateIrradiance`, the same math as the probe volumes.

`renderer/Source/TransparencyFroxelVolume.h/.cpp` is the oracle; `qray_transparency_froxel_tests`
(16 checks, 0 failures) covers it.

## GPU pipeline and pass ordering

`RhiRtDirectPass` owns the probe buffer at set 6 binding 20, allocated once and cleared on creation.
`Shaders/TransparencyFroxels.hlsli` carries the GPU layout, SH helpers and trilinear
`q2SampleTransparencyFroxel`.

`CmBuildTransparencyFroxels.comp.hlsl` fills the active probe grid using eight Fibonacci
directions per probe (`samplePosition = position + direction * 1.6` m), selecting one grid light
per direction and applying smooth radius falloff into SH4. `RhiRtDirectPass::EnsureLightSet`
takes `pUniformBuffer` (`worldUniformBuffer.Get()` from `NvrhiFrameSkeleton.cpp`) and dispatches
`PrepareUniformSet`, `BuildLightGrid` and `DispatchTransparencyFroxels` before `ReflRefr` runs,
keeping both glass and primary/direct passes synchronized with the current frame's lights and froxels.

The glass consumer is `GlassPaint.hlsli`: when `rt_transparency_froxel` is 1 it adds the froxel
irradiance divided by pi multiplied by `transparencyFroxelBlendParams.x` (`rt_transparency_froxel_blend`).
The particle consumer in `RsParticle.frag.hlsl` reconstructs the particle's world position from its
own fragment view-space depth (`inViewDepth` via camera `invProjection` and `invView`) instead of
the opaque wall depth behind the particle.

## Runtime evidence (Proton 11.0, RADV RX 9070 XT)

- Glass build `ci-windows-42552478516`, `e1m1`: `rt_transparency_froxel 0` mean RGB
  `[64.2, 41.8, 36.0]`, `1` `[63.9, 41.7, 36.0]` (difference 2.0), `cvarlist` 1, 0
  `NVRHI::ERROR`.
- Particle build `ci-windows-42553218368`: off `[64.1, 41.7, 35.9]`, on `[64.5, 41.6, 35.9]`
  (difference 2.0), 0 `NVRHI::ERROR`.
- Both spawn-view deltas are near the same-configuration run-to-run noise (1.8): the fill and both
  consumers run, but the view has little glass/particle coverage. A dedicated view or a debug tint
  is the next visual proof.
- Quality tiers on `ad_swampy` (`rt_truelight 0`, `rt_transparency_froxel 1`, build
  `ci-windows-42585380398`): low `[49.4, 36.4, 17.7]` (0.044 near-black), medium
  `[49.6, 36.5, 17.9]` (0.039), high `[49.8, 36.7, 17.9]` (0.036); `cvarlist` confirms 0/1/2 and
  `NVRHI::ERROR` is 0 for all three. The high tier's 50.3 MB buffer and 16k-group fill run
  without a crash.

- Developer proof: build `ci-windows-42591987829` with `rt_transparency_froxel 2` (green replaces
  the froxel contribution) gives 0.9% green-dominant pixels on `ad_swampy` and 0.8% on `ad_tfuma`,
  with 0 `NVRHI::ERROR`; the small fraction is the glass/particle coverage in the spawn view.

## Blend policy

The glass and particle consumers multiply the sampled froxel irradiance by
`transparencyFroxelBlendParams.x`, the new host cvar `rt_transparency_froxel_blend` (0..1, default
1). 0 disables the froxel contribution without stopping the fill, 1 keeps the original additive
behavior, and intermediate values mix it with the screen-space GI. The debug value
`rt_transparency_froxel 2` bypasses the weight (it is a proof, not lighting).

## Blend runtime (build `ci-windows-43045581398`)

One e1m1 run, four screenshots with the blend bound to keys (0, 0.5, 1, 1):
means `[64.13, 41.64, 35.89]`, `[65.05, 42.55, 36.77]`, `[64.94, 42.99, 37.05]`,
`[65.62, 43.03, 36.97]`. The frame-to-frame noise between two blend-1 captures is a mean absolute
difference of 6.37 (80.8% of pixels >6); blend 0 vs 1 is 7.30 (82.8%) — not separable. The spawn
view has under 1% glass/particle coverage, so the blend delta is far below the animated-scene
noise. `cvarlist` confirms the cvar, and the earlier green proof shows the froxel value reaches
both consumers; a dedicated glass/particle scene is needed for a visible A/B.

## AD blend A/B attempt

A lit AD scene (`ad_swampy`, `rt_truelight 0`, master `ci-windows-43051875171`) was used for the
blend 0/0.5/1 A/B in one session. The means were `[65.24, 53.60, 38.18]`, `[66.12, 54.44, 38.98]`,
`[65.44, 53.55, 37.83]`; the frame-to-frame noise between two blend-1 captures is a 5.87 mean
absolute difference over 76.5% of pixels, while blend 0 vs 1 is 5.93/75.9% — still not separable.
The froxel-covered glass/particle area is under 1% in these views, so the signal is far below the
animated-light noise. Freezing attempts (`pause`, `host_framerate`) either opened the pause menu
(which swallows the bound keys) or prevented the screenshot sequence. A purpose-built static
glass/particle view is required for the visual A/B; the cvar plumbing is confirmed by `cvarlist`
and the green consumer proof (`rt_transparency_froxel 2`, 0.9%/0.8% green-dominant pixels).

## Historical froxel world box fix (2026-11-11 vs 2026-11-17 metric unification)

Before `fix/idtech8-transparency-froxel-box`, the fill and both consumers used a fixed world box
`(-4096, -2048, -4096)` size `(8192, 4096, 8192)` in Quake units. Moving it to a camera-centered
`32.512 m` cube restored probes to the scene, and on `2026-11-17` (`965f027` + `832573a`) the box
and fill step were unified with `QR_QUAKE_UNIT_IN_METERS = 0.025f` (`32.0 m` cube, `1.6 m` sample
step), pre-`ReflRefr` dispatch ordering (`EnsureLightSet`) was fixed, and particle lookup switched to
camera `invProjection` + `inViewDepth` + `invView`.

`ad_swampy` blend 0/0.5/1 means: `[64.85, 53.21, 37.90]`, `[66.75, 55.00, 39.61]`,
`[65.78, 54.03, 38.67]`. The per-pixel animated noise is still 5.3 and the 0.5 point is not
monotonic, so this is not yet a clean visual A/B; the green coverage is 0.044% at this view. The
unit fix is required regardless: it puts the fill probes back in the scene.

## Static glass A/B (test map)

`tests/maps/idtech8_test_glass.map` is a one-brush static glass view (glass2, one fixed light, the
player start facing the pane), built with ericw-tools qbsp. On the static map the blend no longer
competes with scene animation: blend 0/0.5/1 red means **7.54 / 8.36 / 8.38**, green
91.16/91.15/91.14 and blue 109.27/109.05/109.04 — the blend lifts red by 0.84 while the other
channels stay within 0.02. The scene is static, so this is the dedicated A/B the animated maps
could not provide. The map and its material entry are documented in `tests/maps/README.md`.

## Tests

`qray_transparency_froxel_tests` (16 checks locally):

- 64 x 32 x 64 dimensions, 131,072 probes and the 48-byte probe layout;
- bounds round-trip and outside rejection, including the box corner;
- a constant environment evaluates to `pi * radiance`;
- halfway between two froxels blends L0 by half;
- the SH helpers produce a brighter lobe along its direction and stay non-negative.

## Follow-up

1. Re-verify `tests/maps/idtech8_test_glass.map` and particle transparency on Windows Debug after
   `832573a` (pre-`ReflRefr` froxel dispatch and `inViewDepth` particle world reconstruction).
2. Production blend tuning (`rt_transparency_froxel_blend`) and broader material mapping for mods.
