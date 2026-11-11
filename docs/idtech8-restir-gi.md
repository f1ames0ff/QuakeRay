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
- **Reprojection**: the raygen projects the current world position with the previous frame's
  `volumeViewProj`; an out-of-range or non-finite UV rejects the history. The screen-space motion
  images (regular and DLSS) were tried in a separate set 10 and both reprojected to wrong history
  pixels for this pass, over-brightening the reused field; the motion set stays allocated but is
  not read until the convention is resolved.
- **Merge**: the current bounce sample and the reprojected history reservoir are merged with the
  same target/pdf identity as the CPU twin; the history is capped at 64 samples.
- **Spatial taps**: four neighbours of the reprojected pixel (2 pixels away) are read from the same
  previous-frame slot and merged with the same identity; the slot is not written by the current
  frame, so the taps cannot race with the current store.
- **Visibility reuse**: when the merged representative is the history sample, the raygen traces a
  bounce towards its stored direction and compares the hit position with the stored
  `position + direction * distance`. A miss or a position outside
  `0.5 + 0.005 * distance` falls back to the current sample.
- **Shading**: the selected sample's direction, distance and radiance feed `q2ShadeIndirect`; a
  reused sample evaluates `evalSpecularBouncePdf` at its direction (the first bounce is a
  Smith-GGX sample), so `target/pdf` stays consistent. The exact BSDF pdf replaced the earlier
  cosine approximation.
- **Half-resolution storage**: when `rt_gi_level` selects the half-row dispatch, the buffer keeps
  one record per traced row pair and the shader shifts the row index; the configure and render
  checks use the same row count.
- **Host switch**: `rt_restir_gi` (default 0); the uniform gate is `giReservoirParams.x`.

Limitations of this slice: reprojection uses the camera term only; the screen-space motion images
over-brightened this pass and are not read; the visibility trace adds one indirect ray when a
reused sample wins; the reservoir stores a view-dependent radiance, so reuse is approximate.

## Runtime evidence (Proton 11.0, RADV RX 9070 XT)

- Build `ci-windows-42549702028`, `e1m1`, `rt_gi_level 1`, camera reprojection + taps + source pdf:
  `rt_restir_gi 0` mean RGB `[63.5, 41.6, 35.8]`, `1` `[64.5, 40.2, 34.6]`, mean absolute channel
  difference 3.0, `NVRHI::ERROR` 0.
- Build `ci-windows-42550491186`, `rt_gi_level 0.5` (half-row dispatch): off `[66.4, 42.7, 36.7]`,
  on `[64.4, 40.3, 34.7]`, difference 3.8, 0 `NVRHI::ERROR`; the half-sized buffer and the
  `y >> 1` indexing render without a crash.
- The screen-space motion images (regular and DLSS) over-brightened the reused field
  (`[72.9, 49.5, 41.9]` and `[72.0, 47.5, 40.5]` against off `[64.3, 41.6, 35.8]`); the camera
  projection is the verified path.

## Why the CPU twin first

The direct ReSTIR branch started the same way: the reservoir math and the history cap are the part
that is easy to get subtly wrong, and they are testable without a GPU. The shader integration is
the next commit.

## Next

1. Motion-vector reprojection for dynamic objects (the framebuffer motion binding in the indirect
   pass stalled the first builds; a separate set is the next attempt).
2. Half-resolution reservoir storage and a packed record once the temporal/tap path is stable.
3. Feed the selected bounce into the world radiance cache so neighbours share the shading.
4. Denoiser interaction: the ReSTIR GI estimate should replace the raw indirect SH, not be blended
   twice with the SVGF history.
