# id Tech 8 Transparency Froxel Irradiance Volume (first slice)

Status: first slice. Date: 2026-11-09.

Branch: `feat/idtech8-transparency-froxels`, stacked on `feat/idtech8-restir-gi`.

## Scope of this slice

The report shares one froxel irradiance volume across every transparency (particles, glass, decals)
and stores it during the light-scattering froxel dispatch. This slice adds the ABI and the CPU
sampling oracle; the GPU image and its update pass are the next commits.

- **Grid**: 64 x 32 x 64 froxels = 131,072 probes, one world-space box (`origin` + `size` per
  axis), independent of the camera snap.
- **Probe**: 4-coefficient RGB SH (L0, L1x, L1y, L1z), 48 bytes; the volume is 6.3 MB at this
  resolution.
- **Lookup**: trilinear blend of the eight surrounding froxels and the Lambertian SH convolution
  (`0.886227` for L0, `1.023328` for L1).
- **SH helpers**: `AccumulateRadiance`, `NormalizeSh` (4*pi / sampleCount) and
  `EvaluateIrradiance`, the same math as the probe volumes.

`renderer/Source/TransparencyFroxelVolume.h/.cpp` is the oracle; `qray_transparency_froxel_tests`
covers it.

## GPU image

`RhiRtDirectPass` owns the 6.3 MB probe buffer at set 6 binding 20, allocated once and cleared on
creation. `Shaders/TransparencyFroxels.hlsli` carries the GPU layout, the SH accumulation helpers
and the trilinear `q2SampleTransparencyFroxel`; the light-scattering fill pass and the glass /
particle consumers are the next commits. Until the fill exists the buffer stays zero and no
shader reads it.

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
