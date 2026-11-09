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
- **Reprojection**: the raygen reads the engine's DLSS motion image in set 10 and computes
  `previousUV = currentUV + motion`. `HitInfo.hlsli` stores `motion = previousUV - currentUV`, so
  this is the same convention the verified direct-ReSTIR path uses. A non-finite sample falls back
  to `viewPrev`/`projectionPrev`, the main camera pair that also builds the motion vectors; an
  out-of-range or non-finite UV rejects the history. The earlier camera path used
  `volumeViewProj_Prev`, the volumetric fog projection, which lands on a different pixel (see the
  isolation below).
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

Limitations of this slice: the visibility trace adds one indirect ray when a reused sample wins;
the reservoir stores a view-dependent radiance, so reuse is approximate; the history has no
surface-normal validation.

## Motion-vector isolation (e1m1, build `ci-windows-42593822566` unless noted)

The first slice rejected the motion images because both over-brightened the field. The isolation
showed the old camera path was the outlier, not the motion vectors:

| configuration | mean RGB |
|---|---|
| GI off | `[63.7, 41.8, 36.0]` |
| GI on, `previousUV = currentUV - motion` (minus) | `[73.6, 49.0, 41.5]` |
| GI on, `previousUV = currentUV + motion` (plus) | `[74.0, 48.9, 41.4]` |
| GI on, same-pixel control `previousUV = currentUV` | `[74.8, 50.3, 42.1]` |
| GI on, old `volumeViewProj_Prev` camera path | `[64.5, 40.2, 34.6]` |
| GI on, main-matrix camera path, `w > 1e-6` guard | `[64.1, 40.3, 34.7]` |
| GI on, final combined branch (motion primary) | `[73.5, 49.2, 41.4]` |

Both signs agree within noise (the spawn motion is small), and the same-pixel control matches the
motion path: the reprojection source, not the sign, caused the difference. The luminance
histograms keep the same p99/max tails, so the lift is a broad convergence change, not fireflies.

The camera path had a second defect behind the first: `Matrix::MakeProjectionMatrix` writes
`w = -z_view`, so a point in front of the camera has a **negative** clip w. The camera reprojection
guarded on `w > 1e-6`, which rejected every pixel; both the `volumeViewProj_Prev` and the
main-matrix camera paths were effectively no-history paths (their means stay near GI-off). The
fallback now uses `abs(w) > 1e-6`; the shipping path is the motion primary, with the corrected
camera projection for a non-finite motion sample.

AD 1.50, `ad_swampy`, build `ci-windows-42593822566`, `rt_truelight 0`: GI on
`[36.8, 28.9, 17.7]` vs off `[34.1, 26.8, 16.3]` (+8% on the mean), 0 `NVRHI::ERROR`.

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
