# id Tech 8 Transparency Froxel Irradiance Volume (first slice)

Status: first slice. Date: 2026-11-09.

Branch: `feat/idtech8-transparency-froxels`, stacked on `feat/idtech8-restir-gi`.

## Scope of this slice

The report shares one froxel irradiance volume across every transparency (particles, glass, decals)
and stores it during the light-scattering froxel dispatch. This slice adds the ABI and the CPU
sampling oracle; the GPU image and its update pass are the next commits.

- **Grid**: one world-space box (`origin` + `size` per axis), independent of the camera snap.
  Three tiers share the layout: low 32 x 16 x 32, medium 64 x 32 x 64 (the CPU oracle) and high
  128 x 64 x 128. The high tier is the report's ~50 MB budget (128 x 64 x 128 x 48 B = 50.3 MB);
  the GPU buffer is allocated at that high size and the active dimensions come from
  `transparencyFroxelParams.yzw` (`rt_transparency_froxel_quality`, default 1 = medium).
- **Probe**: 4-coefficient RGB SH (L0, L1x, L1y, L1z), 48 bytes. Four coefficients are the
  report's 2-band SH; the medium grid is 6.3 MB and the high grid is 50.3 MB.
- **Lookup**: trilinear blend of the eight surrounding froxels and the Lambertian SH convolution
  (`0.886227` for L0, `1.023328` for L1).
- **SH helpers**: `AccumulateRadiance`, `NormalizeSh` (4*pi / sampleCount) and
  `EvaluateIrradiance`, the same math as the probe volumes.

`renderer/Source/TransparencyFroxelVolume.h/.cpp` is the oracle; `qray_transparency_froxel_tests`
covers it.

## GPU image

`RhiRtDirectPass` owns the 6.3 MB probe buffer at set 6 binding 20, allocated once and cleared on
creation. `Shaders/TransparencyFroxels.hlsli` carries the GPU layout, the SH accumulation helpers
and the trilinear `q2SampleTransparencyFroxel`.

The light-scattering fill is `CmBuildTransparencyFroxels.comp.hlsl`: a compute dispatch over all
131,072 probes, eight Fibonacci directions per probe, one grid light per direction (the strongest
in the cell, up to four candidates), and a smooth radius falloff. The result is the SH4 integral
stored in the probe buffer. The pipeline declares the seven shader set positions; set 2 is the
global uniform and set 6 is the shared light layout, so the fill reads the same grid and light
sources as the RT passes.

The glass consumer is `GlassPaint.hlsli`: when `rt_transparency_froxel` is 1 it adds the froxel
irradiance divided by pi to the direct glass lighting. The particle consumer in
`RsParticle.frag.hlsl` reconstructs the world position from the scene depth along the view ray
(`framebufDepthWorld_Sampled` + `volumeViewProjInv`) and adds the froxel irradiance to its ambient;
the particle pass already binds the direct light layout at set 6. Both shaders compile with the
set-6 froxel buffer.

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

## Froxel world box units

The fill and both consumers used a fixed world box `(-4096, -2048, -4096)` size
`(8192, 4096, 8192)` in the report's **Quake units**, while the renderer scene is metric. The fill
probes were therefore kilometres apart and the sampled cells were empty; the blend weight had
almost nothing to scale. The box is now a camera-centered **32.512 m** cube (the report's 1280 u),
shared by `CmBuildTransparencyFroxels.comp.hlsl`, `GlassPaint.hlsli` and `RsParticle.frag.hlsl`
through `TransparencyFroxels.hlsli`.

`ad_swampy` blend 0/0.5/1 means: `[64.85, 53.21, 37.90]`, `[66.75, 55.00, 39.61]`,
`[65.78, 54.03, 38.67]`. The per-pixel animated noise is still 5.3 and the 0.5 point is not
monotonic, so this is not yet a clean visual A/B; the green coverage is 0.044% at this view. The
unit fix is required regardless: it puts the fill probes back in the scene.

## Tests

`qray_transparency_froxel_tests` (16 checks locally):

- 64 x 32 x 64 dimensions, 131,072 probes and the 48-byte probe layout;
- bounds round-trip and outside rejection, including the box corner;
- a constant environment evaluates to `pi * radiance`;
- halfway between two froxels blends L0 by half;
- the SH helpers produce a brighter lobe along its direction and stay non-negative.

## Next

1. The GPU image: an RGBA16F (or RGB9E5) atlas with the same 64 x 32 x 64 layout, sampled by the
   glass and particle shaders with a single world-space lookup.
2. The light-scattering update: accumulate the clustered/grid light contributions into the froxels
   once per frame, reusing the existing volumetric dispatch when possible.
3. The report's 2-band SH and 50 MB budget: the first GPU slice should measure the current 4-band
   SH first and only grow the layout after the sampling is stable.
4. A quality tier (32 x 16 x 32) for low settings, matching the volume/quality pattern.
