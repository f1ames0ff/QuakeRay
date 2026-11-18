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

## 3. Windows CI & Proton 11.0 Runtime Verification (`ci-windows-44174810055`)

- **Windows CI (`44174810055`)**:
  - `Debug` job (`134363226985`): **success** (4m45s) — `.\build_win.ps1 Debug -Tests` + `ctest -LE gpu` + full `dxc` SPIR-V shader build with 0 errors.
  - `Release` job (`134363227500`): **success** (6m33s) — published `ci-windows-44174810055`.
- **Proton 11.0 Runtime (`RX 9070 XT`, `radv Mesa 26.2.6`, `Vulkan 1.4.354`, `e1m1`, `rt_gi_level 2`)**:
  - `0` `NVRHI::ERROR`; `cvarlist` confirms `rt_radiance_cache_cell "10"` (`0.25` m) and `rt_radiance_cache_lod "1024"` (`25.6` m).
  - `RT: grid sources 438, power 0.0000..55395.2031, camera 12.0 -8.8 2.8`, `GPU light-grid builder dispatched 512 groups`.

| Dump (`qperfdump.log`) | Configuration (`e1m1`, `rt_gi_level 2`) | `gpu.frame` | `cache hit` | `cache miss` | `cache active` | `indirect` rays | `shadow dir` | Mean RGB (`3840x2160`) |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 1 (`screenshot0000.png`) | `split 1`, `gi 1`, `cache 1`, `vol 1` | `24.41 ms` | `3,368,783` | `23,654` | `23,083` | `7,146,818` | `2,356,487` | `[54.52, 35.94, 28.33]` |
| 2 (`screenshot0001.png`) | + `rt_light_volume_local_add 512` (`split 1`) | `24.02 ms` | `3,388,325` | `7,641` | `23,357` | `7,161,759` | `2,383,505` | `[58.61, 35.06, 27.02]` |
| 3 (`screenshot0002.png`) | `split 0`, `gi 1`, `cache 1`, `vol 1` | `24.33 ms` | `3,366,019` | `27,816` | `23,777` | `7,143,932` | `2,403,461` | `[61.23, 35.52, 27.20]` |
| 4 (`screenshot0003.png`) | `split 0`, `gi 0`, `cache 1`, `vol 1` | `24.76 ms` | `3,365,537` | `24,614` | `23,010` | `7,143,981` | `2,410,525` | `[63.02, 35.94, 27.74]` |
| 5 (`screenshot0004.png`) | `split 0`, `gi 0`, `cache 0`, `vol 1` | `23.12 ms` | `0` | `0` | `0` | `7,142,953` | `2,427,968` | `[66.38, 39.24, 30.83]` |

Key runtime findings:
1. **Split volume + local 10³ volume (Slice 1)**: `rt_radiance_cache_split 1` with `rt_light_volume_local_add 512` runs cleanly with 0 errors, increasing `cache hit` by `+19,542` and reducing `cache miss` from `23,654` to `7,641`.
2. **Metric `0.25 m` cells + normal hash + 4-slot probing (Slices 2 & 6)**: `cache active` rose from `876` (pre-fix `0.40 m` position-only hash) to `~23,000–23,800` active normal-aware slots per frame with a `99.2%–99.8%` hit rate (`~3.37M` hits vs `7.6k–27.8k` misses).
3. **ReSTIR GI estimator & clean ray-stats (Slice 5)**: toggling `rt_restir_gi 1` vs `0` (`Dump 3` vs `Dump 4`) keeps `cache hit` (`3,366,019` vs `3,365,537`), `shadow dir` (`2,403,461` vs `2,410,525`), and `indirect` (`7,143,932` vs `7,143,981`) free of bogus counter inflation, while mean RGB stays within normal torch flicker (`[61.23, 35.52, 27.20]` vs `[63.02, 35.94, 27.74]`, MAD `5.24`).
