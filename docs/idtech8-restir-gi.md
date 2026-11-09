# ReSTIR GI / Indirect (first slice)

Status: first slice. Date: 2026-11-09.

Branch: `feat/idtech8-restir-gi`, stacked on `feat/idtech8-radiance-cache`.

## Scope of this slice

The direct-light ReSTIR branch reuses light samples across frames and neighbours. This branch starts
the same treatment for the indirect bounce: a reservoir of path samples with a direction, hit
distance and outgoing radiance.

- **Record**: `RestirGiReservoir` is 48 bytes: `direction[4]` (xyz, w = distance),
  `radiance[4]` (xyz, w = padding), `weightSum`, `selectedTarget`, `sampleCount`, `pad`.
- **Estimator**: `W = weightSum / (sampleCount * selectedTarget)`.
- **Add**: one source sample with `target / pdf` and `target`; `u` picks the representative.
- **Merge**: a source reservoir contributes `target(y) * weightSum / target(x_selected)` to the
  receiving weight sum and its whole source count to `M`, the standard ReSTIR identity.
- **History cap**: `RestirGiClampHistory` scales the count and weight sum together at 64 samples,
  so the estimator is unchanged and old history fades out.
- **Validation**: non-finite weights/targets, zero targets, zero-length directions and out-of-range
  counts are rejected.

`renderer/Source/RestirGiReservoir.h` is the CPU twin; `qray_restir_gi_tests` covers it. The GPU
form will live in `renderer/Source/Shaders/RestirGi.hlsli` and share this layout and math.

## GPU path

- **Buffer**: `RhiRtIndirectPass` owns a two-slot `RestirGiReservoir` buffer at the full render
  resolution (1280 x 720 x 2 x 48 B = 88 MB for the tested size), bound as a UAV at set 9
  binding 0. The buffer is recreated on a size change and cleared once.
- **Slot**: `frameId & 1` is the current slot; the other one is the history.
- **Reprojection**: the raygen projects the current pixel with `framebufMotionDlss_Sampled`; an
  out-of-range or non-finite UV rejects the history.
- **Merge**: the current bounce sample and the reprojected history reservoir are merged with the
  same target/pdf identity as the CPU twin; the history is capped at 64 samples.
- **Visibility reuse**: when the merged representative is the history sample, the raygen traces a
  bounce towards its stored direction and compares the hit position with the stored
  `position + direction * distance`. A miss or a position outside
  `0.5 + 0.005 * distance` falls back to the current sample.
- **Shading**: the selected sample's direction, distance and radiance feed `q2ShadeIndirect`; the
  cosine pdf is used for the reused sample (the current sample keeps its sampled pdf).
- **Host switch**: `rt_restir_gi` (default 0); the uniform gate is `giReservoirParams.x`.

Limitations of this slice: the buffer is full-resolution (a half-resolution layout is the next
memory step); only temporal reuse is implemented (no spatial taps yet); the reused sample uses the
cosine pdf rather than the exact BSDF pdf; the visibility trace adds one indirect ray when the
history wins; the reservoir stores a view-dependent radiance, so reuse is approximate.

## Why the CPU twin first

The direct ReSTIR branch started the same way: the reservoir math and the history cap are the part
that is easy to get subtly wrong, and they are testable without a GPU. The shader integration is
the next commit.

## Next

1. Spatial taps from the history slot (four neighbours) with world-position validation, like the
   direct branch.
2. Half-resolution reservoir storage and a packed record once the temporal path is stable.
3. The exact BSDF pdf for a reused sample instead of the cosine approximation.
4. Feed the selected bounce into the world radiance cache so neighbours share the shading.
5. Denoiser interaction: the ReSTIR GI estimate should replace the raw indirect SH, not be blended
   twice with the SVGF history.
