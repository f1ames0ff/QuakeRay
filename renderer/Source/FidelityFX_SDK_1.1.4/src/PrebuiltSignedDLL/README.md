# amd_fidelityfx_vk.dll — FSR 3.1 provider

This folder ships the FidelityFX API provider the renderer links against
(`amd_fidelityfx_vk.lib`). The folder name is historical: the DLL here is a **local,
unsigned rebuild** of AMD's FidelityFX SDK 1.1.4 provider with a single shader fix,
replacing the AMD-signed binary of the same provider version.

## Why the rebuild

Vulkan validation reported, once per frame while AMD FSR 3.1 upscaling was active:

```text
Undefined-Value-StorageImage-FormatMismatch-ImageView
vkCmdDispatch(): the storage image descriptor [..., variable "rw_luma_history"] is accessed
by a OpTypeImage that has a Format operand Rgba8 (VK_FORMAT_R8G8B8A8_UNORM) which doesn't
match the VkImageView ... format (VK_FORMAT_R16G16B16A16_SFLOAT).
```

The FSR 3.1 upscaler creates its internal `LumaHistory1/2` resources as
`FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`, while its GLSL still declared `rgba8` for the
`rw_luma_history` storage image (an FSR2 leftover). Storing through a mismatched
storage-image format is undefined behavior for the whole image.

## Build provenance

- Source: AMD FidelityFX SDK v1.1.4 (GPUOpen), archive `FidelityFX-SDK-v1.1.4.zip`,
  SHA256 `0216556BFB0E243CEC30004A2A98D38F4E3F7406CB7938E3C1B85C758E95D952`.
- Patch (the only source change):
  `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`, one line
  on the luma-history binding:

  ```diff
  -layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D rw_luma_history;
  +layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f) uniform image2D rw_luma_history;
  ```

- Build: the SDK's own CMake (`-DFFX_API_BACKEND=VK_X64`, Release, Visual Studio 2022)
  with the SDK's bundled shader compiler; the full recipe is in
  [docs/fsr-luma-history-format-fix.md](../../../../docs/fsr-luma-history-format-fix.md).
- Result: 9,320,960 bytes, SHA256
  `2C6E430782D66EDC99A07396A679B6846C4BA4887B48F28F3E9CA08FD048D794`,
  FileVersion 1.0.1.0.
- Replaced signed binary: 9,332,432 bytes, SHA256
  `A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78`,
  FileVersion 1.0.1.41314 (size difference equals the Authenticode directory).
- ABI unchanged: the same five exports in the same order — `ffxConfigure`,
  `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`. The vendored
  `amd_fidelityfx_vk.lib` is unchanged.

## Unsigned distribution

The rebuilt file is not Authenticode-signed. The AMD-signed binary remains recoverable
from the previous revision of this path in git history and from the AMD SDK archive.
AMD's driver overlay FSR detection described in `changelog.md` (v2.1.0) depends on the
AMD signature and may no longer recognize this provider; the engine's own FSR path is
unaffected.

## When to replace this file

Replace it with an official AMD-signed DLL (and update this note) when that DLL produces
zero `rw_luma_history` / `Undefined-Value-StorageImage-FormatMismatch` occurrences in a
`tests/perf/run_menu.ps1 -Smoke -Validation` run. The FidelityFX SDK's MIT-style license
notice is shipped in `ffx-api/include/ffx_api/ffx_api.h`.
