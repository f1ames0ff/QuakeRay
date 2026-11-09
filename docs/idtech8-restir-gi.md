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

## Why the CPU twin first

The direct ReSTIR branch started the same way: the reservoir math and the history cap are the part
that is easy to get subtly wrong, and they are testable without a GPU. The shader integration is
the next commit.

## Next

1. GPU reservoir buffer at the indirect render resolution, two history slots plus a current slot;
   a new binding on the indirect pass (set 9, not the shared set 6).
2. Temporal reprojection with the G-buffer motion vectors and normal/depth validation; spatial
   taps from the history slot with the same world-position validation as the direct branch.
3. Target: `bsdf * radiance / pdf` at the receiving pixel; visibility reuse for the selected
   direction when the history sample is not the current one.
4. Feed the selected bounce into `RtQ2Indirect.rgen`'s SH path and let the world radiance cache
   shade neighbours instead of retracing.
5. Denoiser interaction: the ReSTIR GI estimate should replace the raw indirect SH, not be blended
   twice with the SVGF history.
