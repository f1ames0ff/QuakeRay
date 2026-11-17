# id Tech 8 Correctness Audit & Fix Handoff (2026-11-17)

Status: implemented on branch `fix/idtech8-correctness` in worktree `.worktrees/idtech8-fixes` (host base `14cd1788`, `renderer` submodule `6dfe04a` -> `b80703c`). Changes are committed locally in `renderer` and staged/committed on the host branch; nothing has been pushed to remote.

## 1. Delivered Fixes (Slices 1-6)

Public engine ABI in `renderer/Include/qray/qray.h` was preserved unchanged (`RadianceCacheEntry == 32 B`, `RestirGiReservoir == 48 B`, `RestirReservoir == 16 B`, `LightVolumeProbe == 48 B`).

| Slice | `renderer` commit | Scope & key fixes |
|---|---|---|
| 1 | `60b9044` | **Split Light-Volume Cull & Hit-Buffer Indexing**: `Source/Shaders/HLSL/RtLightVolume.rgen.hlsl` split branch now traces with `traceVolumeRay` (`RAY_FLAG_NONE`) instead of culled `traceIndirectRay`; both `RtLightVolume.rgen.hlsl` and `RtLightVolumeShade.rgen.hlsl` index `lightVolumeHits` by local `invocation` instead of `probeIndex`. |
| 2 | `965f027` + host edits | **Metric Unit Unification (`QR_QUAKE_UNIT_IN_METERS = 0.025f`)**: unified light grid (`kBaseCell = 2.0f`, `kSnap = 16.0f`, `kInfluenceMargin = 0.8f`, `kMinDistanceScaleSq = 6.25e-4`), light volumes (`kBaseBounds = 32.0f`, `kSnap = 16.0f`, `Q2_LIGHT_VOLUME_BASE_BOUNDS = 32.0`, `Q2_LIGHT_VOLUME_SNAP = 16.0`), transparency froxels (`Q2_TRANSPARENCY_FROXEL_BOX_SIZE = 32.0`, fill step `1.6` m), radiance cache (`rt_radiance_cache_cell` default `"10"` -> `0.25f` m fallback, `25.6f` m LOD fallback, host `QR_QuakeDrawFrame` & `SpatialFrame.cpp` `frame.Length` scaling, `max(lodDistance, 0.25)`), and `QuakePointToMeters` conversion for `UploadClusterLightSources` view origin. |
| 3 | `832573a` | **Glass Pass Ordering & Particle World Reconstruction**: `RhiRtDirectPass::EnsureLightSet` now accepts `pUniformBuffer` (`worldUniformBuffer.Get()` from `NvrhiFrameSkeleton.cpp`) and dispatches `PrepareUniformSet`, `BuildLightGrid`, and `DispatchTransparencyFroxels` before `ReflRefr` runs (removing duplicate dispatches inside `RhiRtDirectPass::Render`). `RsParticle.frag.hlsl` reconstructs particle world position from camera `invProjection` + `inViewDepth` + `invView` instead of reading opaque wall depth. |
| 4 | `2dba65a` | **Direct ReSTIR Target & Duplicate Luminance Removal**: added `q2RestirTarget` (`max(mass, proxy * 1e-3) * max(getLuminance(encoded.color), 0.0) * scale`, `0.0` for directional lights) across all 4 sites (grid candidate, uniform initial, empty-cell fallback, `q2RestirMergeHistory`) in `Source/Shaders/RestirSampling.hlsli`, removing double luminance multiplication from `q2SampleGridLights::outMass`. Added unit tests in `Tests/RestirReservoirTests.cpp`. |
| 5 | `05870d0` | **ReSTIR GI Estimator `W`, Full Validation & Counter Cleanup**: added `RestirGiTarget` / `q2GiTarget` (`luminance * max(cosTheta, 0.0)`), zeroed `radiance.w` in `RestirGiAddSample` / `q2GiAddSample`, weighted `current` sample by `currentTarget * max(oneOverSourcePdf, 0.0)`, shaded with `shadingWeight = q2GiWeight(reservoir)` (replacing broken `1.0` weights and `radiance.w` reads), gated reuse on `frameId > 0`, enforced `q2GiUsable && q2GiHistoryValid && q2GiSurfaceMatches` on both history and all 4 spatial taps (without skipping spatial taps when center history is invalid), reset `skyBsdfWeight = 1.0` on visible geometry reuse, and deleted the 5 bogus `rayStatsAdd` calls + duplicate `RAY_STATS_CATEGORY_INDIRECT` increment after `traceBounce` in `RtQ2Indirect.rgen.hlsl`. |
| 6 | `b80703c` | **Radiance Cache Normal Hash, 4-Slot Linear Probing & Clean Outgoing Radiometry**: added 9-bit quantized normal (`QuantizeNormal` / `q2RadianceQuantizeNormal`, 7 bins/axis, `+Z` fallback) into the hash key, 4-slot linear probing (`kProbeSteps = 4u` / `Q2_RADIANCE_CACHE_PROBE_STEPS = 4u`) with matching-key preference, expired/empty reuse resetting `samples = 1`, and oldest-slot eviction. Extracted shared `q2InterpolateLightVolumeSh` and added `q2EvaluateLightVolumeIrradiance` in `LightVolumeGpu.hlsli`. Updated `LightVolumeShade.hlsli` and `RtQ2Indirect.rgen.hlsl` to pass `normalGeom`, multiply second-bounce returns by `bounceBrdf = evalBRDFLambertian(1.0) * max(dot(surf.normal, bounceDir), 0.0)`, use `q2EvaluateLightVolumeIrradiance` on second-bounce misses, remove second-bounce `q2RadianceCachePut`, and remove final-gather `q2RadianceCachePut(surf.position, diffuse + specular)`. |

Host changes in `.worktrees/idtech8-fixes`:
- `Quake/qr_spatial_adapter.cpp`: metric origin for cluster light uploads + metric cache cell/LOD conversion (`QR_QUAKE_UNIT_IN_METERS`).
- `Quake/gl_vidsdl.c`: `rt_radiance_cache_cell` default updated from `"16"` to `"10"` (`0.25` m).
- `docs/idtech8-{phase0,restir,volumes,radiance-cache,restir-gi,transparency-froxels,tuning}.md` updated to unified metric constants and post-fix status; `docs/idtech8-handoff.md` added.

## 2. Linux Verification Evidence (2026-11-17)

Compiled and ran in `/home/user/repos/vkquake-rt/.worktrees/idtech8-fixes` with `g++ -std=c++20 -DQR_STATIC -D__cdecl= -Wall -Wextra -Wno-unused-parameter -Werror` (and `gcc` for C tests):

| Check | Result | Notes |
|---|---|---|
| `qray_light_grid_tests` | **21 checks, 0 failures** | Metric 2.0 m / 16.0 m / 0.8 m grid bounds, snap, capacity, determinism |
| `qray_light_volume_tests` | **85 checks, 0 failures** | Metric 32.0 m / 16.0 m cascades, local volumes, SH4 evaluation & schedule |
| `qray_radiance_cache_tests` | **62 checks, 0 failures** | Layout, hash, 0.25 m / 25.6 m LOD, normal separation, 4-slot probing/expiry/eviction |
| `qray_restir_reservoir_tests` | **36 checks, 0 failures** | RIS weight, merges, clamping, luminance-scale `W == 1/pdf`, consistent merge |
| `qray_restir_gi_tests` | **56 checks, 0 failures** | Layout, pack/unpack, age/receiver validation, `RestirGiTarget`, `W == 1/pdf`, RIS merge |
| `qray_transparency_froxel_tests` | **16 checks, 0 failures** | Froxel 32.0 m box, SH4 interpolation and evaluation |
| `qray_spatial_contract_tests` | **passed** | Compiled with `-include cstddef`; verifies `QrSpatialConvention` layout |
| `prof_window_tests` | **passed** | Profiler window lifecycle & concurrent publication |
| `rt_light_admit_tests` | **25 checks, 0 failures** | Host light admission rules |
| `Quake/qr_spatial_adapter.cpp` + `renderer/Source/SpatialFrame.cpp` | **exit 0 (`-fsyntax-only`)** | Clean C++20 host/renderer spatial boundary syntax check |
| `CheckMatrixReads.py` | **exit 0** | Every checked HLSL matrix read matches golden |

## 3. Environment & Verification Boundaries (`AGENTS.md`)

- **QuakeRay MCP**: earlier in the audit session, `uv sync --locked --project tools/quakeray_mcp --python 3.10` and `quakeray_mcp --selfcheck` passed (`exit 0`). After container restart `/home/user/.local` was absent from the sandbox mount (`uv` unavailable), so code verification on this Linux environment used bounded file inspection, C++/C unit tests, host+renderer `-fsyntax-only` compilation, and `CheckMatrixReads.py`.
- **Windows / GPU / Shader Reflection Coverage Gap**: this Linux container has no MSVC/Windows SDK toolchain, Vulkan SDK `dxc` SPIR-V compilation, or GPU runtime (`.\build_win.ps1 Debug`, `CheckShaderProperties.py --record`, and live `quakeray.exe` runtime runs are **not** executed here). Wait for any running `quakeray` / `qray_*` instance before launching Windows validation (`AGENTS.md` one-instance rule).

## 4. Next Steps on Windows (Required Before Merging)

1. Checkout/update `.worktrees/idtech8-fixes` (or cherry-pick/merge branch `fix/idtech8-correctness` + submodule `b80703c`) and build **Debug**:
   ```powershell
   git submodule update --init --recursive
   .\build_win.ps1 Debug
   ```
   Verify zero `dxc` shader compilation errors across `RtLightVolume.rgen.hlsl`, `RtLightVolumeShade.rgen.hlsl`, `CmBuildLightGrid.comp.hlsl`, `CmBuildTransparencyFroxels.comp.hlsl`, `RsParticle.frag.hlsl`, `RtRaygenDirect.rgen.hlsl`, and `RtQ2Indirect.rgen.hlsl`.
2. Regenerate shader reflection recordings if enforced:
   ```powershell
   python renderer/Source/Shaders/CheckShaderProperties.py --record --force
   ```
3. Run CTest in the Windows Debug build directory and verify all tests pass.
4. Before running runtime checks, enforce the one-instance rule (`Get-Process quakeray, qray_* -ErrorAction SilentlyContinue`), then re-baseline on Debug runtime:
   - **Split volume & local volumes**: `e1m1` with `rt_light_volume 1 rt_radiance_cache 1 rt_radiance_cache_split 1` and `rt_light_volume_local_add 512` (verify 0 `NVRHI::ERROR` and hit buffer/probe hits across cascades and local volumes).
   - **Transparency froxels & glass**: `map idtech8_test_glass` with `rt_transparency_froxel 1` and `rt_transparency_froxel_blend 0 / 0.5 / 1` plus a particle scene (`RsParticle` depth reconstruction check).
   - **Direct ReSTIR on AD**: `map ad_swampy` with `rt_truelight 0`, `rt_light_grid 1`, `rt_restir 0` vs `1` (`q2RestirTarget` gap check).
   - **ReSTIR GI & Radiance Cache**: `e1m1` (`rt_gi_level 1` and `2`) toggling `rt_restir_gi 0/1` and `rt_radiance_cache 0/1`, inspecting `rt_stats_dump` (`cache hit/miss/active`, `indirect`, `shadow` counts) to verify unbiased GI convergence and elimination of bogus ReSTIR-GI stats inflation.
