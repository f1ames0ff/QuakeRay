# FSR upscaler luma-history format mismatch

Date: 2026-10-09. Status: fixed and verified. The rebuilt provider is vendored at
`renderer/Source/FidelityFX_SDK_1.1.4/src/PrebuiltSignedDLL/amd_fidelityfx_vk.dll` (with a
README next to it), deployment is deterministic, and the validation smoke runs that
previously reported the warning now report zero occurrences with FSR active.

## Symptom

The Vulkan validation layer reported, once per frame while the AMD FSR 3.1 upscaler was
active:

```text
Undefined-Value-StorageImage-FormatMismatch-ImageView
vkCmdDispatch(): the storage image descriptor [..., variable "rw_luma_history"] is accessed by
a OpTypeImage that has a Format operand Rgba8 (VK_FORMAT_R8G8B8A8_UNORM) which doesn't match
the VkImageView ... format (VK_FORMAT_R16G16B16A16_SFLOAT).
```

First retained in the acceptance job `job_8164f677bc164c35bd01d39100062e3c` and in direct
`tests/perf/run_menu.ps1 -Smoke -Validation` runs. It was previously hidden by the validation
layer's per-message duplicate limit because the engine's own `bloomDest` mismatch consumed that
limit first; fixing `bloomDest` (`b7bf2414`) unmasked it.

## Where the mismatch comes from

`rw_luma_history` is internal to the FSR 3.1 upscaler. The engine's `ffxDispatchDescUpscale`
call passes only color, depth, motion vectors and output (`renderer/Source/FSR.cpp`); the
provider creates its own images (`amd_fidelityfx_vk.dll` imports `vkCreateImage` and
`vkCreateImageView`) and declares the format inside its compiled shader.

In the AMD FidelityFX SDK 1.1.4 sources:

- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h` declared
  `layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D rw_luma_history;`
- `sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp` creates `LumaHistory1/2` as
  `FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`.

The `rgba8` qualifier is an FSR2 leftover: the FSR2 luma history resource is RGBA8 and its GLSL
declaration matches it, while the FSR3 upscaler resource was changed to RGBA16F and its
qualifier was not. Storing through a format-mismatched storage image is undefined for the whole
image, not only for the written texels. Every other FSR3 upscaler UAV declaration was checked
against its resource format and matches; the same-class qualifier in the FSR2 callbacks is
unreachable because the engine only selects a `'3'` provider and has no FSR2 technique.

## Fix

One line in the provider's shader source, applied to a copy of the unpacked SDK:

```diff
-layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D  rw_luma_history;
+layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f) uniform image2D  rw_luma_history;
```

## Rebuild recipe

Only AMD's own build system is used; the source tree differs from the original SDK by that one
line. Source archive `FidelityFX-SDK-v1.1.4.zip`, SHA256
`0216556BFB0E243CEC30004A2A98D38F4E3F7406CB7938E3C1B85C758E95D952`.

```text
robocopy <sdk-root>\sdk     <work>\sdk     /E
robocopy <sdk-root>\ffx-api <work>\ffx-api /E
# apply the one-line fix in <work>\sdk\include\FidelityFX\gpu\fsr3upscaler\ffx_fsr3upscaler_callbacks_glsl.h
cmake -S <work>\ffx-api -B <work>\ffx-api\build -G "Visual Studio 17 2022" -A x64 -DFFX_API_BACKEND=VK_X64
cmake --build <work>\ffx-api\build --config Release --target amd_fidelityfx_vk --parallel
```

The SDK ships its shader compiler (`sdk/tools/binary_store/FidelityFX_SC.exe`) and only needs the
Vulkan SDK for `find_package(Vulkan)`. Output: `<work>\ffx-api\bin\amd_fidelityfx_vk.dll`.

## Vended provider

- Rebuilt DLL: 9,320,960 bytes, SHA256
  `2C6E430782D66EDC99A07396A679B6846C4BA4887B48F28F3E9CA08FD048D794`, FileVersion 1.0.1.0,
  unsigned. It replaces the signed binary at the same path; the import library and the include
  directory are unchanged.
- Replaced signed binary: 9,332,432 bytes, SHA256
  `A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78`, FileVersion 1.0.1.41314
  (the size difference is the Authenticode directory). It remains recoverable from the previous
  revision of the path in git history and from the SDK archive.
- ABI: the same five exports in the same order (`ffxConfigure`, `ffxCreateContext`,
  `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`), verified with `dumpbin /exports`.
- Deployment: the root `CMakeLists.txt` now copies the provider with an always-run
  `qray_deploy_fidelityfx` target instead of a `quakeray` POST_BUILD step. Under Ninja a
  POST_BUILD command only runs when the executable relinks, so a provider-only change could
  otherwise leave existing build trees with the stale DLL.
- Provenance note: `renderer/Source/FidelityFX_SDK_1.1.4/src/PrebuiltSignedDLL/README.md`.

## Verification (2026-10-09)

Protocol per run: no other game instance; `build_win.ps1 Debug` with the deployed DLL hash
asserted immediately before launch; `tests/perf/run_menu.ps1 -Seconds 3 -Smoke -Validation`
(menu -> level -> in-game menu -> disconnect -> resize at 1280x720). The runner's own gate
passes on warnings, so each console was grepped explicitly; the in-level capture (the middle
`stats-*.dump`, where `gpu.primary_ms > 0`) proves the upscale dispatched.

| Arm | Provider DLL | mismatch warnings | `rw_luma_history` lines | VUID lines | FSR provider lines | in-level `gpu.upscale_ms` |
|---|---|---:|---:|---:|---:|---:|
| Positive control | signed original (`A1624C...`) | 10 | 10 | 0 | 2 | 0.25 ms |
| Candidate run 1 | rebuilt (`2C6E...`) | 0 | 0 | 0 | 2 | 0.25 ms |
| Candidate run 2 | rebuilt (`2C6E...`) | 0 | 0 | 0 | 2 | 0.25 ms |

- The provider line (`FSR: requested FSR 3.1, provider "3.1.4" (id=0xf5a5ca1e00c01004)`) appears
  twice per run (context creation and the resize recreate), so FSR stayed active and the zero
  warning count is not a silent fallback to TAAU.
- Evidence: `%LOCALAPPDATA%\QuakeRayMCP\acceptance_fsr_20261009\{baseline,candidate-1,candidate-2}`
  (console logs, stats dumps, runner logs, deployed-DLL hash file).
- No pixel-equality claim: removing the UNORM mismatch can legitimately change luma-history
  values above 1.0; the screenshots are a render sanity check, not a byte comparison.

## Unsigned distribution

The vendored provider is a local unsigned rebuild; AMD's driver overlay FSR detection mentioned
in `changelog.md` (v2.1.0) depends on the AMD Authenticode signature and may no longer recognize
this provider. The engine's own FSR path is unaffected. This trade-off was accepted to remove
undefined storage-image behavior from the default configuration; the signed original is one
`git revert`/`git show` away.

## When to supersede

Replace the rebuilt provider with an official AMD-signed DLL (and update the README and this
record) when that DLL produces zero `rw_luma_history` /
`Undefined-Value-StorageImage-FormatMismatch` occurrences in a
`tests/perf/run_menu.ps1 -Smoke -Validation` run.

## Alternative if the provider cannot be replaced

Use the existing TAAU fallback for the default upscale path. That removes the undefined storage
access without touching the provider, at the cost of upscaler quality; it does not fix FSR
itself.
