# QuakeRay engine

QuakeRay is a ray tracing engine for Quake 1 with Q2RTX-style partial path tracing, built on NVRHI and running on Vulkan.

## Features

* NVRHI renderer (Vulkan)
* Path-traced lighting, ReSTIR-DI
* DTAL: dynamic textured area lights
* Alpha-transparent textures are traced through, not only cut out
* Spot lights: a dynamic light can shine in a cone (`dlightspot` until the editor places them)
* ASVGF denoiser
* Ray-traced GI, NEE
* FSR 3.1 upscaling
* Dynamic HDR tone mapping
* Procedural sky, god rays
* Materials, ray-traced water
* Shader smoke (rockets, fire)
* Per-cluster light lists (legacy)
* Animated light styles
* Post-processing: chromatic, LUT

## Roadmap

* DirectX 12
* FSR 4, radiance cache
* DLSS
* UE5-style post effects
* Full path tracing
* Hybrid rasterization/RT
* Light and material editor
* Arcane Dimensions support
* Quake Remastered support
* More shader effects

## Definitions

* **ASVGF** - the renderer's temporal + a-trous denoiser, filtering each lighting channel apart
* **TAL** - Q2RTX's name for a light cut out of a surface: a face polygon sampling its emission mask
* **DTAL** - QuakeRay's TAL: any emissive surface is a light, moving models included

## Changelog

See [changelog.md](changelog.md).

## Build

The project is built with CMake and Ninja using the MSVC compiler from Visual Studio Build Tools.

### Windows

Prerequisites:

* [Git for Windows](https://github.com/git-for-windows/git/releases)
* [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with the "Desktop development with C++" workload
* [CMake](https://cmake.org/download/) 3.20 or newer
* [Ninja](https://ninja-build.org/)
* [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (with `glslc` and `dxc`; the shaders are compiled from `renderer/Source/Shaders` - the HLSL twins with `dxc`, the remaining GLSL stages with `glslc`)
* GPU with ray tracing support

Steps:

1. Clone the repository:

   ```
   git clone --recursive https://github.com/sdas234f23f/QuakeRay_new.git
   ```

2. (Re)build the SPIR-V shaders - optional: `build_win.ps1` already builds and deploys them (see step 3), so you only need this when iterating on `renderer/Source/Shaders` on their own:

   ```
   .\build_shaders.ps1
   ```

   This compiles `renderer/Source/Shaders` (the HLSL twins with `dxc`, the remaining GLSL stages with `glslc`) and deploys the SPIR-V into `build\Debug\id1\shaders` (`-DestDir <dir>` to deploy somewhere else). Pass `-Rebuild` to ignore the shader cache and recompile everything, and `-GenCommon` when the generated shader-common headers changed.

3. Configure and build:

   ```
   .\build_win.ps1 Debug
   ```

   Debug builds go to `build\Debug` (the default build dir for the given configuration). Pass an explicit directory as a second argument only if you know you want a different one.

   (or with plain CMake: `cmake -B build\Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` + `cmake --build build\Debug`; use `-DCMAKE_BUILD_TYPE=Release` and `build\Release` for a release build).

   The build then deploys the ray-traced game data into `build\<Config>\id1`: the material definitions (`renderer/Source/materials.yaml` → `id1/materials/materials.yaml`), `renderer/Source/textures`, `renderer/Source/progs` and `renderer/Source/mdl_skins`, the blue noise table and the water normal map, and the SPIR-V shaders into `id1/shaders`.

4. Run the game:

   ```
   build\Debug\quakeray.exe
   ```

   `SDL2.dll` and all codec DLLs are copied next to `quakeray.exe` automatically during the build. The renderer is compiled into the executable - no external renderer DLL is needed. The `.spv` shaders and the blue noise texture are loaded from the game data (`id1/shaders/`, `id1/BlueNoise_LDR_RGBA_128.ktx2`).

5. (Optional) Package a release - needs a Release build (`.\build_win.ps1 Release`):

   ```
   .\bundle_release.ps1
   ```

   Writes `dist\QuakeRay-<version>-win64.zip`: the Release `quakeray.exe`, the runtime DLLs, the `id1` runtime assets (`materials`, `mdl_skins`, `progs`, `shaders`, `textures` and the blue noise / water normal KTX2 tables) and `readme.md`, `changelog.md` and `LICENSE.txt`. The version in the archive name is read from `ENGINE_VERSION` / `ENGINE_VER_PATCH` (`Quake\quakedef.h`) unless `-Version` passes one in; debug artifacts are never included, and the original game data is not bundled.

## Ray tracing settings

Everything is exposed as console variables; run `cvarlist rt_` in the console for the full list. The ones that change the look most are:

* `rt_brightness 1.0` - overall brightness of the ray-traced image
* `rt_exposure_bias -2.8` - exposure in EV, a power-of-two factor applied inside the tone curve
* `rt_contrast 0.6` - mixes the fixed tone curve with the auto-exposure adapted one (`0` keeps the fixed curve, `1` is the adapted curve alone)
* `rt_sun 1` with `rt_sun_pitch 140` / `rt_sun_yaw 120` - the sun's intensity and direction: `0` turns it off, and the indirect sun and god rays scale with it
* `rt_sun_color 255 255 255` - colour of the sun and its disc, independent of the sky, as `<r> <g> <b>` in `0-255`; commas and a bare query work, and it is archived
* `rt_sun_edit 0` - mode: while it is `1` the sun follows the crosshair and writes `rt_sun_pitch` / `rt_sun_yaw`; the fire button leaves it without shooting, and it never survives a restart
* `rt_godrays_intensity 1` with `rt_godrays 1` - strength of the volumetric sun shafts and their on/off switch: `2` doubles them, `0` removes them and the shadow map they are marched through
* `rt_sky 1`, `rt_sky_brightness 1.0`, `rt_physical_sky 1` - sky intensity and sky model
* `rt_sky_color 32 0 64` - colour of the sky, tinting it and the ambient light it casts, as `<r> <g> <b>` in `0-255`; commas, quotes and a bare query work, and it is archived
* `rt_sky_clouds_color 0 0 0` - colour the clouds are composited over the sky with, as `<r> <g> <b>` in `0-255`; commas and a bare query work
* `rt_sky_ambient_lod 4` - mip level the ambient sky light is read from; lower is more directional, `10` is a flat wash
* `rt_sky_nee 1` - sample the sky as an explicit light; `0` restores the pre-NEE result
* `rt_gi_level 1` - indirect lighting: `0` off, `0.5` half-resolution, `1` one indirect bounce, `2` adds the diffuse second bounce; the menu cycles the same levels
* `rt_sun_bounce_range 2000` - how far the sun reaches into an indirect bounce, in Quake units; `0` turns indirect sunlight off, and smaller is cheaper and dimmer
* `rt_sun_bounce_scale 1.0` - multiplier on the sun's contribution to an indirect bounce; `1.0` is the physical value
* `rt_nee_samples 1` - next-event light samples per pixel in the direct pass: `1` or `2`, where `2` trades more shadow rays for a quieter image
* `rt_indir2bounces 0` - legacy switch for the second diffuse bounce, kept for old configs; `rt_gi_level` now selects it
* `rt_denoiser 1` - ASVGF reconstruction of the lighting channels (`0` composites the raw ReSTIR output)
* `rt_no_textures 0` - `1` swaps the diffuse albedo for a fixed value, i.e. "no textures"
* `rt_emis_light_intensity 1.0` - how much light the emissive (luma-masked) surfaces emit
* `rt_dtal_minarea 0` / `rt_dtal_maxpolys 64` - the size floor (world units², `0` off) and the per-surface cap (`0` = no cuts) of the DTAL splits; `rt_dtal_rebuild` re-runs the collection
* `rt_dtal_clearance 1` - a DTAL polygon facing solid geometry within this many units is not created (`0` off)
* `rt_dtal_debug 0` - `1` draws the DTAL wireframes, `2` their normals as arrows
* `rt_light_color 255 255 255` - tint on every light source, as `<r> <g> <b>` in `0-255`; commas, quotes and a bare query work, and it is archived
* `rt_globallight 255 255 255` - colour a light starts from before its own colour and the tint above, as `<r> <g> <b>` in `0-255`; same forms, and `rt_globallight_mult` is the separate intensity
* `rt_light_styles 1` with `rt_light_styles_reach 48` - animated light entities flicker on their own fixture; the reach in Quake units keeps the flicker there, and `-1` removes the limit
* `rt_cluster_incremental 1` with `rt_cluster_dlights 1` - per-cluster light lists: `rt_cluster_incremental` is read-only (it rebuilds only the changed lights' slots), and `rt_cluster_dlights 0` keeps the moving emitters out of the lists while still traced and lit
* `rt_turb_warp 1` - amplitude of the classic texture warp on lava and teleport surfaces (`0` freezes them; water and slime use the RT water waves instead)
* `rt_teleport_portals 0` - off, so teleport surfaces render as ordinary surfaces; `1` re-enables the mirrored destination
* `rt_stats <level>` - the on-screen readout: `1` ray counters, `2` adds GPU pass timings, `3` adds the CPU profile; `0` hides it, a bare `rt_stats` prints the panels, archived
* `rt_stats_dump` - writes the current frame's readout to `qperfdump.log`, one `section name value` line per number; appended, on a separate thread
* `rt_bench <demoname> [quit]` - plays a demo at its own speed with the frame profiler summed over it and appends the result to `benchmark.log`; `quit` closes the game after the run
* `rt_debugflags 0` - diagnostic views (raw direct/indirect/specular, gradients, ...)
* `rt_viewm_scale 0.32` - the weapon is drawn `0.32` times smaller and closer to the eye by the same factor, unchanged on screen but out of the walls; `1` restores the classic weapon

## Game data

Quake 1 game files (`id1/`) are required (registered or shareware). HD texture packs can be used through `.pkz` archives or `.mat` material definitions, and the ray-traced material overrides are deployed into the build's game dir by `build_win.ps1` (`id1/materials/materials.yaml` plus the `id1/textures`, `id1/progs` and `id1/mdl_skins` folders) - nothing has to be packed by hand.

