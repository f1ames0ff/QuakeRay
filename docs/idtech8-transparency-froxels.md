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
