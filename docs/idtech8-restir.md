# id Tech 8 ReSTIR — direct-light reservoirs

Status: first implementation. Date: 2026-11-07.

Branch: `feat/idtech8-restir`, built from `feat/idtech8-grid-v2` (grid port PR #69).

## What is implemented

`rt_restir 1` switches the direct raygen from one independent grid/cluster draw per NEE sample to
a temporal/spatial reservoir:

- One weighted reservoir per packed pixel and frame slot. The record is 16 B:
  `lightIndex`, `sampleCount`, `weightSum`, `selectedTarget`.
- Two slots in one structured buffer, selected by `frameId & 1`. The dispatch reads the other
  slot (the previous frame) and writes its own slot, so there is no intra-frame race.
- Initial candidates come from the world-space grid cell when `rt_light_grid` covers the point
  (`target / gridPdf` per candidate). Outside the grid, or when the cell produces no usable
  sample, they come from the global light array (`target * lightCount`).
- Temporal reuse merges the previous frame's reservoir at the motion-vector-reprojected pixel.
  `weight = target(y) * weightSum / selectedTarget`; the merged reservoir carries the source
  sample count, and `W = weightSum / (sampleCount * selectedTarget)` is the shading multiplier.
- Spatial reuse merges four history taps two pixels away from the reprojected pixel. Both reuse
  paths validate the previous G-buffer world position and normal; a moving object or a geometric
  edge is rejected instead of smeared.
- The history is capped at 64 source samples. The clamp scales `weightSum` and `sampleCount`
  together, so `W` and the estimator are unchanged while old history fades out.
- `restirParams.z` (the engine's frame-history-reset flag) drops all history for that frame.
- The reservoir buffer is zero-filled whenever it is created or resized; `sampleCount == 0` is the
  shader's no-history marker. When `rt_restir` is off, set 6 binds a two-element stand-in and no
  render-sized allocation exists.

The direct pass traces one shadow ray per pixel in ReSTIR mode (the sun ray is unchanged), instead
of `rt_nee_samples` independent rays.

## Files

- `renderer/Source/RestirReservoir.h` — CPU twin of the merge, the valid-sample predicate and the
  clamp; `renderer/Tests/RestirReservoirTests.cpp` covers the RIS identity, draw probabilities,
  rejected merges and clamp invariance.
- `renderer/Source/Shaders/Restir.hlsli` — the GPU record and merge.
- `renderer/Source/Shaders/RestirSampling.hlsli` — candidate generation, reprojection, spatial taps,
  history validation and the slot store.
- `renderer/Source/Shaders/HLSL/RtRaygenDirect.rgen.hlsl` — selects the ReSTIR path.
- `renderer/Source/RHI/RhiRtDirectPass.{h,cpp}` — the reservoir ring, set-6 binding 17, size/enable
  configuration, zero fill and light-set invalidation.
- `renderer/Source/RHI/NvrhiFrameSkeleton.cpp` — calls `ConfigureRestir` before the reflect/refract
  pass borrows set 6.
- `renderer/Include/qray/qray.h` — the updated `rt_restir` / `rt_restir_candidates` contract.

## Deliberate approximations

- The target is the existing `globalLightTarget`, not the full BRDF integrand. The single-frame
  weight is exactly the legacy RIS pdf, so `rt_restir` with no valid history reproduces the old
  estimate. Temporal/spatial merges inherit the target mismatch as bias, as in practical ReSTIR.
- History-length capping is the only bias control. There is no pairwise MIS weight, no spatial
  pass resolution step and no reservoir pair (two lights per reservoir).
- Motion vectors are the primary reprojection source; a failed/out-of-range reprojection uses the
  current pixel for spatial taps but skips temporal reuse.
- Checkerboard packing: reservoirs are indexed in the packed image, and the previous regular pixel
  is mapped back with `getCheckerboardPix`. Every packed texel is traced each frame, so the history
  slot covers the full image.
- The reservoir stores the frame-local light-array index. The engine's light registry keeps the
  order stable for map lights, but a frame that inserts or removes dynamic lights can reshuffle
  indices; a reused sample then shades whatever now occupies that slot for one frame. A stable
  light UID in the encoded light record is the follow-up that removes this assumption.

## Validation

- CPU `qray_restir_reservoir_tests`: 32 checks, 0 failures (local g++ build).
- CPU `qray_light_grid_tests`: 16 checks, 0 failures.
- Windows CI builds Debug and Release and runs the non-GPU tests; shader compilation is part of the
  build (`build_shaders.ps1`).
- `RtRaygenDirect.rgen.hlsl` was compiled locally with the Vulkan SDK dxc 1.9.2609 (`lib_6_3`,
  SPIR-V, all four extensions the generator passes) under Wine; it produced SPIR-V with no
  diagnostics.
- `CheckShaderProperties.py` reflection recordings were not regenerated on this Linux host (no
  dxc). Run `python CheckShaderProperties.py --record --force` where the Vulkan SDK dxc is
  available before enforcing the checker on this branch.
- Runtime quality/A-B measurement on the AD maps with `rt_light_grid 1 rt_restir 1` is still open.

## Next

- ReSTIR GI for the indirect pass (the report's ReSTIR GI / one-sample-per-pixel path) is not part
  of this branch; only direct NEE uses the reservoir.
- Cascaded/local irradiance and radiance volumes (`feat/idtech8-volumes`): one cascade and one local
  volume per frame, probe NEE through the typed grid.
- Then tune reservoir history length, spatial tap count/radius, grid capacity/reach/cell size and
  measure the AD stress scenes.
