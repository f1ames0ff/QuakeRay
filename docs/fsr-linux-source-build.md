# FSR 3.1 from source on Linux and Windows

Status: 2026-11-03. The renderer links a locally built FidelityFX FSR 3.1
Vulkan provider on both platforms. No checked-in `amd_fidelityfx_vk` binary is
used. The Linux build and an FSR-active in-game run are verified below; the
Windows build path uses AMD's SDK CMake and is wired into `build_win.ps1`.

## Source layout

`renderer/Source/FidelityFX_SDK_1.1.4` contains the FidelityFX SDK 1.1.4
`src/sdk` and `src/ffx-api` sources, the local patches, and two build front
ends:

```text
linux/build_provider.sh       glslangValidator permutation generator + .so
windows/build_provider.ps1    SDK CMake + bundled FidelityFX_SC.exe + .dll/.lib
```

The Linux tool in `linux/shader_compiler` is the SDK's GLSL compiler with the
small portability fixes needed on Linux; it is not a second FSR
implementation. It compiles the same 10 FSR3 upscaler passes and 4 variants
(64 permutations per pass) that the Windows `FidelityFX_SC.exe` compiles, then
the provider links the SDK's own backend, upscaler, and ffx-api sources.

## Patches

The vendored tree and `patches/` carry six changes:

| Patch | Why |
| --- | --- |
| `0001-fsr3-luma-history-rgba16f` | `rw_luma_history` is an `R16G16B16A16_FLOAT` resource; the SDK declared the GLSL storage image as `rgba8`, which is an undefined format mismatch. |
| `0002-fsr3-context-size-linux-wchar` | The private FSR3 context embeds `wchar_t` resource names. Linux `wchar_t` is 32-bit, so the private structure (about 818 KiB) does not fit the SDK's 128 Ki-`uint32_t` opaque context. The context is four times larger; Windows fits either way. |
| `0003-vk-scratch-alignment` | GCC at `-O2` emits an aligned SSE store (`movaps`) into the Vulkan scratch buffer's `EffectContext` array. The SDK laid the array out at an 8-byte offset, so the store faulted. Every scratch array now starts at a 16-byte boundary. |
| `0004-ffx-api-entry-linux` | `FFX_API_ENTRY` is empty on non-Windows compilers instead of `__declspec(dllexport)`. |
| `0005-ffx-provider-msvc-literal` | `0xF5A5'CA1Eui64` is MSVC-only; use `0xF5A5CA1Eull`. |
| `0006-ffx-api-include-case` | Two `ffx-api` headers used `<FidelityFx/...>`; Linux is case-sensitive, and the tracked symlink workaround broke the Windows source copy. The headers now use `<FidelityFX/...>`. |

The alignment patch was verified by rebuilding the provider at `-O2 -g`: the
previous build segfaulted in `CreateBackendContextVK`, the patched build runs
and dispatches FSR. All six patches were also applied in sequence to pristine
upstream 1.1.4 files with `git apply --check`; every patch applied cleanly,
which exercises the external-SDK path used by the Windows build script.

## Linux build

From the repository root:

```bash
GLSLANG_VALIDATOR=/path/to/glslangValidator \
VULKAN_HEADERS_DIR=/path/to/vulkan/headers \
./build_linux.sh
```

`build_linux.sh` runs `renderer/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh`
into `build/linux-debug/ffx/libamd_fidelityfx_vk.so`, then configures the engine
with `-DQR_FFX_VK_SO=<that path>`. The `.so` carries the
`libamd_fidelityfx_vk.so` SONAME and is found through the executable's
`RUNPATH`.

The script also applies the pinned
`third_party/nvrhi-max-binding-layouts.patch` for the build and reverses it on
exit, exactly like `build_win.ps1`. A fresh checkout therefore builds a binary
with the RT pass enabled instead of silently falling back to "the frame cannot
be drawn".

The provider exports the five FFX API entry points:

```text
ffxConfigure  ffxCreateContext  ffxDestroyContext  ffxDispatch  ffxQuery
```

## Linux verification

Run (one game instance, 1280x720 windowed, FSR quality):

```bash
./build/linux-debug/quakeray -basedir "$PWD/build/linux-debug" -game id1 \
    -condebug -width 1280 -height 720 -window +exec qr_live.cfg
```

`qr_live.cfg` sets `rt_upscale_fsr31 2`, `rt_stats 3`, binds `F10` to
`rt_stats_dump` and `F12` to `screenshot`, then loads `e1m1`. The log shows:

```text
FSR: requested FSR 3.1, provider "3.1.4" (id=0xf5a5ca1e00c01004)
FSR: upscale dispatch 854x480 -> 1280x720 (jitter -0.250, 0.167, reset=1)
```

The frame profiler confirms the upscale pass executes on the GPU and CPU:

```text
gpu.frame   ms                9.05
gpu.pass    upscale           0.21
gpu.rays    total             1943745
world       clusters          1149
cpu.draw    upscale           avg_ms=0.220 max_ms=0.290
```

No `the TAAU path has to cover the frame` warning appears. The RHI skeleton
only skips `RenderTaaU` when `RhiFsrPass::Render` returns true, i.e. when
`FSR::Apply` returned `FB_IMAGE_INDEX_UPSCALED_PONG`; the dispatch log above is
emitted on that success path. The captured frame is
`build/linux-debug/id1/screenshot0000.png` (1280x720, 54,208 unique colours), taken
after the first successful dispatch.

All FSR resolution modes dispatch through the same provider:

| `rt_upscale_fsr31` | Mode | Dispatch |
| --- | --- | --- |
| 1 | Native AA | 1280x720 -> 1280x720 |
| 2 | Quality | 854x480 -> 1280x720 |
| 3 | Balanced | 752x423 -> 1280x720 |
| 4 | Performance | 640x360 -> 1280x720 |
| 5 | Ultra performance | 426x240 -> 1280x720 |

`rt_upscale_fsr31 6` is the custom mode and uses `rt_renderscale` (native at
the default 1.0). None of these runs produced a TAAU fallback warning.

A 45-second FSR quality run produced no segmentation faults, assertions,
validation messages, or TAAU fallback warnings.

A live window resize from 1280x720 to 960x540 recreated the context and
dispatched again without a restart:

```text
FSR: requested FSR 3.1, provider "3.1.4" (id=0xf5a5ca1e00c01004)
FSR: upscale dispatch 854x480 -> 1280x720 (jitter 0.438, -0.241, reset=1)
FSR: requested FSR 3.1, provider "3.1.4" (id=0xf5a5ca1e00c01004)
FSR: upscale dispatch 640x360 -> 960x540 (jitter 0.250, -0.389, reset=0)
```

A fresh build in `build/linux-clean` (`QR_BUILD_DIR=build/linux-clean
./build_linux.sh`) rebuilt the provider and permutation headers from scratch,
passed CTest 6/6, and dispatched 854x480 -> 1280x720 with the same provider line.
`QR_RUN_TESTS=1 ./build_linux.sh` runs the same CPU suite after the build; the
GPU regression suite remains opt-in with `-DQR_BUILD_TESTS=ON` because it needs
`dxc` and a Vulkan device.

## Windows build

`build_win.ps1` builds the provider before configuring the engine:

```powershell
renderer\Source\FidelityFX_SDK_1.1.4\windows\build_provider.ps1 `
    -OutDir build\Debug\ffx -Config Release
```

The script copies the vendored `sdk` and `ffx-api` to the output directory,
applies any missing patches, runs the SDK's own CMake with
`-DFFX_API_BACKEND=VK_X64` and `FidelityFX_SC.exe`, then places
`amd_fidelityfx_vk.dll` and `amd_fidelityfx_vk.lib` in the output directory.
`build_win.ps1` passes those paths to the renderer as
`QR_FFX_VK_LIBRARY`/`QR_FFX_VK_RUNTIME_DLL`; the deployment, editor, and GPU
test targets copy that source-built DLL.

The vendored tree was diffed against the upstream 1.1.4 checkout: every file
under `sdk/include`, `sdk/src`, `sdk/libs`, and `ffx-api` is present. The only
omissions are the interactive `.bat` launchers, the DX12-only binary store
contents, and the shader-compiler source tree (the VK build uses the SDK's
prebuilt `FidelityFX_SC.exe` and `glslangValidator.exe` tools).

`.github/workflows/build-windows.yml` now runs the Windows source build with
`-ForceFFXProvider` and checks that the produced `amd_fidelityfx_vk.dll`
exports all five FFX API entry points. That workflow is the automated Windows
gate once the renderer and host commits are pushed.

## Limitations

- The Linux provider is FSR 3.1 upscaling only. Frame generation is stubbed
  with an `FFX_ERROR_INVALID_ARGUMENT` entry point because the renderer does
  not use it; the FFX API still exposes the five required symbols.
- The Linux build needs a `glslangValidator` and Vulkan headers; these are
  found through `GLSLANG_VALIDATOR`/`VULKAN_HEADERS_DIR` (or the Vulkan SDK).
- The Windows provider build was not executable in this Linux worktree. Its
  inputs and SDK CMake invocation follow AMD's 1.1.4 build recipe; the
  remaining gate is a Windows build of `build_win.ps1 Debug` and a run.
