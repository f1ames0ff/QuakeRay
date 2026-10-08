# FSR upscaler luma-history format mismatch

Date: 2026-10-09. Status: root cause found and the provider rebuilt from AMD's own source; the
rebuilt DLL is not vendored or runtime-verified yet. Parked out of the MCP server work on
`docs/mcp-plan-review`.

## Symptom

The Vulkan validation layer reports, once per frame while the AMD FSR 3.1 upscaler is active:

```text
Undefined-Value-StorageImage-FormatMismatch-ImageView
vkCmdDispatch(): the storage image descriptor [..., variable "rw_luma_history"] is accessed by
a OpTypeImage that has a Format operand Rgba8 (VK_FORMAT_R8G8B8A8_UNORM) which doesn't match
the VkImageView ... format (VK_FORMAT_R16G16B16A16_SFLOAT).
```

Observed in the acceptance job `job_8164f677bc164c35bd01d39100062e3c` and in direct
`tests/perf/run_menu.ps1 -Smoke -Validation` runs. It was previously hidden by the validation
layer's per-message duplicate limit because the engine's own `bloomDest` mismatch consumed that
limit first; fixing `bloomDest` (`b7bf2414`) unmasked it.

## Where the mismatch comes from

`rw_luma_history` is internal to the FSR 3.1 upscaler. The engine's `ffxDispatchDescUpscale`
call passes only color, depth, motion vectors and output (`renderer/Source/FSR.cpp`); the
provider creates its own images (`amd_fidelityfx_vk.dll` imports `vkCreateImage` and
`vkCreateImageView`) and declares the format inside its compiled shader.

In the AMD FidelityFX SDK 1.1.4 sources the vendored DLL was built from:

- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h` declares
  `layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D rw_luma_history;`
- `sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp` creates `LumaHistory1/2` as
  `FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`.

The `rgba8` qualifier is an FSR2 leftover: the FSR2 luma history resource is RGBA8 and its GLSL
declaration matches it, while the FSR3 upscaler resource was changed to RGBA16F and its
qualifier was not. Storing through a format-mismatched storage image is undefined for the whole
image, not only for the written texels. Every other FSR3 upscaler UAV declaration was checked
against its resource format and matches.

## Fix

One line in the provider's shader source:

```diff
-layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D  rw_luma_history;
+layout (set = 0, binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f) uniform image2D  rw_luma_history;
```

## Rebuild recipe

Only AMD's own build system is used, no other changes:

```text
robocopy <sdk-root>\sdk     <work>\sdk     /E
robocopy <sdk-root>\ffx-api <work>\ffx-api /E
# apply the one-line fix in <work>\sdk\include\FidelityFX\gpu\fsr3upscaler\ffx_fsr3upscaler_callbacks_glsl.h
cmake -S <work>\ffx-api -B <work>\ffx-api\build -G "Visual Studio 17 2022" -A x64 -DFFX_API_BACKEND=VK_X64
cmake --build <work>\ffx-api\build --config Release --target amd_fidelityfx_vk --parallel
```

The SDK ships its shader compiler (`sdk/tools/binary_store/FidelityFX_SC.exe`) and only needs the
Vulkan SDK for `find_package(Vulkan)`. Output: `<work>\ffx-api\bin\amd_fidelityfx_vk.dll`.

## Build performed

- Source: the unpacked AMD `FidelityFX-SDK-v1.1.4` shipped by the operator (in Downloads), copied
  to a work directory, patched, built Release.
- Rebuilt DLL SHA256:
  `2C6E430782D66EDC99A07396A679B6846C4BA4887B48F28F3E9CA08FD048D794` (9,320,960 bytes, unsigned).
- Vendored signed DLL (unchanged):
  `renderer/Source/FidelityFX_SDK_1.1.4/src/PrebuiltSignedDLL/amd_fidelityfx_vk.dll`,
  SHA256 `A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78` (9,332,432 bytes,
  version 1.0.1.41314, "VK AMD FidelityFX Library").
- Nothing in this repository has been replaced. The rebuilt DLL currently exists only in the
  local build directory.

## Next steps

1. Deploy the rebuilt DLL to `build/Debug` and run
   `tests/perf/run_menu.ps1 -Smoke -Validation`; require zero
   `Undefined-Value-StorageImage-FormatMismatch` warnings while the FSR upscaler stays active,
   with the level still rendering.
2. Compare a level frame before/after to confirm the upscaler output is unchanged or better.
3. Decide provenance: the vendored file is the AMD-signed binary; the fix here is a local
   unsigned rebuild from AMD's source. Either keep the rebuild as an engine-local patch DLL or
   wait for an AMD release that carries the same fix.
4. If accepted, replace the vendored DLL (the import library and export ABI stay unchanged) and
   update the acceptance record.

## Alternative if the provider cannot be replaced

Use the existing TAAU fallback for the default upscale path. That removes the undefined storage
access without touching the provider, at the cost of upscaler quality; it does not fix FSR
itself.
