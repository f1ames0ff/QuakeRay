# QuakeRay Portable Toolkit — Plan

Status: draft. Revision 9. Date: 2026-10-24.

Diagram: [architecture-portable-toolkit.html](architecture-portable-toolkit.html)

## 1. Scope: Goal A — standalone toolkit

QuakeRay Portable Toolkit is a **source-level toolkit**, not an engine. The supported integration mode is **Goal A: standalone**.

- QuakeRay owns the window, Vulkan device and swapchain.
- A C++ host can declare all device requirements before creation and receive
  the SDK-owned NVRHI/Vulkan device as a borrowed shared context.
- A host engine implements an adapter that imports its map/assets into the toolkit contracts.
- The coupled Quake editor edits lights, materials and DTAL controls. The standalone
  `quakerayeditor` now previews OBJ/BSP29 scenes, picks surfaces/lights, edits material,
  light and render settings, and writes its session material/level documents. The SDK
  now exposes the source-level host contracts and sidecar semantic model; a shared
  engine-free editor core and file codec still remain future work.
- The existing `quakeray.exe` is the **reference Quake host**, not a new binary. There is no separate `qray_game` product.

Out of scope for this plan:

- **Host-owned GPU integration**: host-owned `VkDevice`, queues, target images and
  synchronization; rendering inside the host engine's viewport. This is excluded
  from the roadmap, not a deferred phase of Goal A.
- Per-engine adapters and loaders: host engines implement their own adapters.
- Linux/non-Win32, proprietary-host licensing, Unreal/Unity/Source integration.
- Geometry editing, BSP compilation, game logic, entity authoring.
- MDL v10 / skeletal animation unless it becomes a generic core capability.

Host-owned device/target/output integration would be a separate product and
licensing decision. It is not scheduled, has no reserved API identifiers, and
cannot be enabled by a host adapter alone.

## 2. Planned responsibility split

This section is target architecture, not a claim that every named interface
already exists. The transport ABI remains the public C ABI in `qray.h`; the
optional C++20 host-driver layer lowers through that same ABI.

| Area | QuakeRay modules | Host engine |
|---|---|---|
| RT rendering | `rtcore`: NVRHI/Vulkan passes, ASVGF, materials | — |
| Lighting | `lighting`: clusters, DTAL runtime, registry, publication | Provides cluster/spatial data and light sources |
| Authoring | `authoring`: sidecar model, merge, stable IDs | Maps its assets to the model |
| Editor | current `quakerayeditor` + future shared editor core | Provides picking/selection/apply hooks if embedding the editor |
| Host contracts | `IQrayHost`, `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, `QraySidecar` | Implements the host interface and supplies the renderer transport data |
| Assets | — | Map/model/texture loaders, material mapping, unit/coordinate conversion |
| Spatial | Cluster consumer | BSP/PVS or a future spatial provider (`QraySpatialProvider` placeholder) |
| GPU | Owns window, Vulkan/NVRHI device, swapchain, targets and synchronization | May declare requirements before creation; borrows the shared device and records through frame hooks |
| Game logic | — | Entity I/O, triggers, AI, animation, save/load |

Rules:

- `lighting`, `authoring` and `editor core` do not include engine headers and do not know about Quake.
- The Quake adapter lives in `hosts/quake` and is the reference implementation, not part of the core.
- If a host cannot express its data through these contracts, classify the
  limitation as host-owned preprocessing, an SDK core gap, or out-of-scope
  integration. Do not label an intentionally host-owned effect as an SDK
  defect.
- The host adapter is responsible for performance: no per-frame world rebuild, no copies of unchanged data, no hidden allocations in the frame path.

## 3. Planned target architecture

Layers:

- `rtcore` — NVRHI/Vulkan RT renderer, passes, ASVGF, materials, BLAS/TLAS ownership.
- `lighting` — cluster composition, DTAL runtime, registry, publication; no Vulkan, no engine headers.
- `authoring` — `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, sidecar, stable IDs, merge.
- `editor` — current standalone `quakerayeditor`, plus a future shared editor core.
- `hosts` — source-level adapters; Quake is the reference.
- `platform` — window, input, filesystem, GPU device.

The existing `qray.h` upload path is preserved. There is no second scene-transfer path and no monolithic `QrayFrame`.

Implemented source-level logical contracts in `renderer/Include/qray/host_contract.h`
(these are not a second part of the `qray.h` C ABI):

- `QraySceneAsset` — static map resources: surfaces, materials, authored lights, DTAL groups, source keys. Consumed at bind/load; core copies synchronously.
- `QrayFrameView` — camera, projection, viewport, exposure, `viewId`, `QR_VIEW_RESET_HISTORY`.
- `QrayFrameUpdates` — typed deltas (transform, texcoords, material contents, light add/modify, DTAL) and typed streams (particles, dynamic geometry).
- `IQrayHost` — `Caps`, `Attach`/`Detach`, `SurfaceKey`/`ResolveSurface`/`LightKey`, `Refresh`, `EvaluateStyles`, `ApplySidecar` with per-entry report, `LoadAuthoredLights`, optional `Pick`.
- `QraySidecar` — versioned authoring intent: materials, lights, DTAL groups. Not a serialization of runtime structs.

`QraySceneBinding` lowers scene and frame records into the existing `qr*`
calls. `host-driver-contract.md` in the SDK defines ownership, supported
updates, failure behavior and the unsupported-feature boundary.

`gpu_context.h` exposes the SDK-owned NVRHI/Vulkan context. The host passes
instance/device extensions, NVRHI features, Vulkan features, queue counts,
formats and native validation callbacks before `vkCreateDevice`. It receives
the negotiated NVRHI device and Vulkan handles after creation. The SDK remains
the sole device owner and preserves the single submission/present pipeline.

Ownership rule: **borrow for the call, copy before return.** No `qray.h` entry point retains a host pointer. Core owns GPU resources and BLAS/TLAS. The host never supplies acceleration structures.

Stable identity:

- `SceneFingerprint` — did the source/dependency set change?
- `SourceSurfaceKey` — which source surface does an edit bind to?
- `AuthoringUUID` — which authored object is this?
- runtime `uniqueID` — renderer handle; never persisted.

Clustering:

- PVS is optional; clusters are required.
- `QR_CLUSTER_MAX_REGISTERED_LIGHTS = 4095`; `QR_CLUSTER_LIGHT_MAX_SOURCES_CLUSTERS = 128` (seed clusters per light, not a per-cluster cap).
- `Q2_MAX_CLUSTERS = 8192`; per-cell fast cap 128; tail 1M; `kTopUpSources = 8`.
- No-PVS default: the SDK synthesizes all-visible PVS rows when the host omits both PVS tables, so each cluster sees the lights its sources reach; the top-up pass remains. A hard 8-ceiling exists only with `rt_cluster_sampling 0`.
- A non-BSP host can use a future `QraySpatialProvider` contract
  (`HOST_PARTITION`, `CORE_GRID`, `HOST_DENSE_MAP`: planned names, not reserved identifiers),
  or accept the measured no-PVS quality today.

Material path:

- `qrUpdateMaterialContents` updates textures only.
- Roughness/metallicity/emission are RME texture samples or per-instance defaults.
- The common editor path is texture re-synthesis / in-place update, not geometry re-upload.
- Material-handle replacement can force a static resubmit; measure it.
- A 1x1 updateable RME override is the cheapest constant-scalar path (candidate, not fact).

## 4. Baseline check — 2026-10-22

This section is a historical snapshot. Current package/API values and the
shared GPU contract are recorded in sections 4.7 and 4.8.

Branch `feat/portable-toolkit`, based on `master` (`cd88fc77`).

What is already true:

- `renderer/Include/qray/qray.h` is the public C ABI, and `QR_API_VERSION` is `1.06.0000`.
- The renderer's C++ sources do not include `Quake/` headers. The remaining engine-facing layer is the `Qr*` contract in `qray.h`.
- The alias-table implementation is renderer-owned: `renderer/Source/rt_alias.*` builds as `qray_alias`, and both host and tests link it instead of compiling `shared/rt_alias.c`.
- The host-owned texture and cursor assets are out of `renderer/`: `renderer/Source/textures` is now `gfx/textures`, and `renderer/gfx` is now the root `gfx/` folder. `build_win.ps1` stages both from `gfx/`; the Debug build and asset pack still pass.
- `renderer/CMakeLists.txt` has a standalone `project(qray_renderer VERSION 0.2.0)`, finds Vulkan and NVRHI when configured from its own directory, and `cmake -S renderer -B <dir>` now configures successfully.
- The package installs `renderer.lib`, `include/qray/qray.h`, the FSR API headers/provider, and `qray_rendererConfig.cmake` plus `qray_rendererTargets.cmake`. A clean consumer that supplies NVRHI and Vulkan configures and links against `qray::renderer`.
- FSR is exposed through the `qray_ffx` interface target: the build tree uses `QR_FFX_SDK_ROOT`, while the installed tree gets `qray::ffx` from the package config. The in-tree provider remains the default; an installed AMD SDK can override it.

What still blocks calling the package complete:

- The standalone configuration still needs `QR_NVRHI_SOURCE_DIR` (defaulting to the sibling `third_party/nvrhi`) or a pre-existing `nvrhi` target. The installed package requires the consumer to provide `nvrhi` and `nvrhi_vk` targets; this is documented in the config but not yet a findable package dependency.
- The Quake-host cluster test now lives in the root `tests/qray_cluster_lists_tests.cpp`; `renderer/Tests` is engine-free.
- Shader compilation and runtime asset staging are PowerShell steps outside CMake.

Required before moving `renderer/` to its own repository:

1. Decide whether NVRHI is consumed as a source subdirectory or an installed package; document the dependency contract and remove the hard-coded sibling path from the package default.
2. Keep shader output and `gfx/` staging as host-side deployment steps, separate from the renderer library target.

## 4.1 SDK import — 2026-10-22

- `renderer/` was imported as the `QuakeRaySDK` git submodule at `cb8ddb82`;
  the spatial work at that snapshot used the `feature/universalize` branch;
  see section 4.5 and `git log` for the historical commit.
- The SDK `main` contains the package install/export path, `qray_rendererConfig.cmake`, the FSR provider and the engine-free tests.
- The Quake cluster test moved to the host tree as `tests/qray_cluster_lists_tests.cpp`.
- A clean consumer project configured and linked `qray::renderer` from an installed prefix with external NVRHI targets.
- The full CTest suite passes (14/14) after the SDK integration. `qray_spatial_twin_gpu` is the new engine-free identity/Y-up/mirrored readback test; `qray_worldlights_tests` covers no-PVS all-visible synthesis. `qray_dtal_perf` now performs a heavy warm-up before timing, and its baseline remains tied to the machine's GPU clock/power state; re-record it with `--update` after a hardware or power-plan change.

## 4.2 Readback contract — 2026-10-22

Historical snapshot; current package/API values are recorded in sections 4.7
and 4.8.

- `qray.h` is now `1.05.0000`; the package version is `1.5.0`.
- `qrGetFrameReadbackRequirements`, `qrRequestFrameReadback` and
  `qrGetFrameReadbackStatus` expose asynchronous readback of the final LDR
  backbuffer.
- The SDK copies the final composited image, including UI, into a per-frame-slot
  staging texture and completes it after that slot's previous submission has
  been reaped. The frame path does not call `vkDeviceWaitIdle`; the host owns
  `pDst` until it observes completion.
- `includeUi = QR_FALSE` and `RGBA16F` are not supported in this revision and
  return `QR_FEATURE_UNAVAILABLE`; the blocking screenshot path remains separate.
- The host-specific Q3 window callback and multi-view work are deliberately not
  part of this API change.

## 4.3 Standalone editor — current state

- Historical extraction branch: `feature/editor`; current SDK work is on
  `feature/host-contracts`.
- The optional `quakerayeditor` target builds `quakerayeditor.exe`.
- The editor owns a Win32 window and a `QrInstance`, previews Wavefront OBJ and
  Quake BSP29 scenes, resolves BSP textures/lights, and starts at
  `info_player_start`.
- CMake deploys the FSR provider, the default scene, the compiled shader
  directory and the blue-noise texture next to the executable when their source
  paths are configured.
- Current authoring covers viewport picking, selected lights, material
  parameters, global render settings, and session writes to material/level
  documents. PAK/archive extraction beyond the current BSP path and a generic
  engine-free authoring core remain.
- The coupled `Quake/qr_editor.c` and `Quake/qr_gui.cpp` remain in the Quake
  host. The next work is the engine-free editor core plus the map/material
  adapter; the files are not copied into the SDK.
- The current panel has `Scene`, `Entities`, `Render` and `Materials` tabs.
  Remaining work is documented in `renderer/docs/editor-parity.md`; this plan
  does not present the current session writer as the final generic sidecar.

## 4.4 Current code anchors

- `renderer/Include/qray/qray.h` — existing C ABI.
- `renderer/Source/ClusterLightLists.cpp`, `renderer/Source/LightManager.cpp` — lighting runtime.
- `Quake/rt_dtal_groups.c`, `Quake/rt_dtal_lod.c`, `Quake/rt_cluster_select.c` — engine-free DTAL runtime.
- `Quake/gl_rlight.c`, `Quake/r_world.c` — Quake host / reference adapter inputs.
- `Quake/qr_editor.c`, `Quake/qr_gui.cpp` — in-engine editor (coupled).
- `renderer/Source/VulkanDevice.cpp`, `ASManager` — Vulkan/AS ownership.

## 4.5 Spatial contract — implementation 2026-10-22

This section records the 2026-10-22 API state. The meter-only follow-up below
supersedes its `unitsPerMeter` and Quake-unit compatibility details.

- SDK branch: `feature/universalize`; host branch: `feature/adapt-qray-sdk`.
  See `git log` for the current revision of each branch.
- `QrSpatialConvention` declares the host's canonical signed axes,
  right-handedness and `unitsPerMeter`. `qrSetSpatialConvention` accepts it
  once after instance creation, before the first spatial upload or drawn frame.
- The host may use its own units and a Z-up or Y-up frame in either handedness.
  The SDK maps the data into its existing right-handed, Z-up scene space and
  keeps a 1:1 meter scale. Geometry conversion is folded into the
  instance transform except for the world-light face and smoke raster vertex
  arrays; light positions/directions/areas, cluster bounds/reaches, fog,
  particles, sky fields and frame camera data are converted at their upload
  boundaries. Relative light power remains the host's calibrated value; the
  Quake adapter removes its legacy unit-area factor before the SDK encodes
  the metric emitting area.
- A left-handed frame is mirrored into canonical space and indexed triangle
  winding is reversed for geometry, polygon lights and world-light faces. The
  host supplies its native handedness view matrix. Arbitrary instance rotations
  and non-uniform scale are converted by the SDK; only a non-axis-aligned world
  convention has to be rotated by the host.
- Screen-space `SWAPCHAIN` raster geometry is exempt and never locks the
  world frame.
- QuakeRay subscribes with the identity convention and converts its existing
  Quake-unit/Z-up data to meters at the host adapter boundary.
- `rayLength` is host-controlled up to the 250,000 m safety ceiling. The
  world-depth target is `R32_SFLOAT`; the shader sky sentinel
  follows `2 * rayLength`. The former 250 m envelope is retired.
- `qrSetSmokeWindDirection` supplies wind in host scene space. If a cluster
  upload omits both PVS tables, the SDK synthesizes all-visible rows so the
  no-PVS path no longer degrades to top-up only.
- `qray_spatial_twin_gpu` renders identity, Y-up/meter and mirrored twins from
  independently authored host data and compares final LDR readback. It covers
  geometry, camera, directed/spherical lights, a world-light cluster with
  omitted PVS tables, level fog, a particle upload and the readback path.
  `qray_worldlights_tests` checks the synthesized all-visible rows. Water,
  clouds and UI remain covered by the existing host/runtime tests and
  source-level boundary checks, not by this twin.
- SDK documentation: `renderer/docs/spatial-contract.md` and
  `renderer/docs/embedding.md`.

## 4.6 Meter-only spatial API — implementation 2026-10-23

- The public API is `1.06.0000`; `QrSpatialConvention` version 2 retains its
  layout but replaces `unitsPerMeter` with a zero-required reserved field.
  Every host now supplies lengths and positions in meters, areas in square
  meters, and inverse lengths per meter.
- The renderer keeps its private right-handed, Z-up frame at a 1:1 meter
  scale. Internal spatial constants are expressed directly in meters, areas
  in square meters and inverse lengths per meter.
- `Quake/qr_spatial_adapter.*` is the Quake host boundary. It converts geometry,
  lights, clusters, fog, particles, rasterized world data, and frame parameters
  to meters, compensates for the legacy relative-light intensity fix, and
  leaves `SWAPCHAIN` data in pixels. Array scratch is thread-local and valid
  until each synchronous SDK upload returns.
- The spatial CPU contract tests cover meter scaling and rejected legacy
  conventions; the GPU twin uses meter-valued identity, Y-up and mirrored
  scenes. The optional standalone editor remains a separate consumer and must
  be adapted before its Quake-unit scene data is used with API 1.06.

## 4.7 Renderer host contract and API docs — implementation 2026-10-24

- SDK package version `0.2.0`; public API `1.06.0000`. The installed target now
  exports `QR_STATIC` and `QR_USE_SURFACE_WIN32`, plus `NOMINMAX` for C++
  consumers, instead of requiring a second host to reproduce renderer-local
  CMake switches.
- `renderer/docs/host-contract.md` defines the adapter boundary and maps BSP /
  lightmap, vertex-animated, multi-stage shader, material/skeletal and
  Source-era renderer data through the current ABI. Unsupported multi-pass,
  stencil, blend-equation and render-to-texture effects have explicit host
  fallbacks rather than implied support.
- `renderer/docs/api.md` now records state lifetime, RGBA8 material/cubemap
  inputs, SWAPCHAIN pixel geometry, raster ordering, blend limits and the
  source-level versioning rule. `renderer/docs/sky-and-atmosphere.md` covers
  the sky/cloud/atmosphere payload and validation sequence. Focused guides now
  also cover sun/god rays, lights/dlights/DTAL/NEE, and indirect
  GI/ReSTIR/denoiser; `host-feature-matrix.md` is the feature-to-ABI entry
  point. `renderer/docs/README.md` is the single index and recommended reading
  order for every SDK document. All are installed with the package.
- Raster ordering is explicitly host-owned: the SDK preserves upload order,
  never infers camera depth, and does not sort within a batch. The upload lock
  makes concurrent calls safe but does not define their relative order, so a
  host with overlapping blended producers must serialize or prepare the order
  before upload. The fixed `DEFAULT` world/particle/point/smoke category order
  is documented separately; interleaving those categories does not survive.
  Conflicting `SMOKE|PARTICLE` states, particle/smoke state on non-`DEFAULT`
  uploads and depth state on `SWAPCHAIN` are rejected; the particle-proxy gate
  shares the same upload lock.
- Lit particle raster and traced particle proxies now use the same source alpha
  product, `drawColor.a * vertexColor.a * textureAlpha`; premultiplied RGB is
  not multiplied by alpha a second time. `qrUploadParticles` now shares the
  geometry upload mutex, closing the prior collector race.
- The SDK ships its required NVRHI binding-layout patch, checks the source
  limit during standalone configure, and the package consumer asserts
  `c_MaxBindingLayouts >= 13`. The FidelityFX imported target now follows
  `CMAKE_INSTALL_INCLUDEDIR`/`LIBDIR`, including relocatable custom layouts.
- `renderer/Tests/PackageConsumer` is a minimal installed-package probe. A
  clean consumer built and linked through `qray::renderer` without defining the
  SDK's public compile switches itself.
- Verification: `build_win.ps1 Debug` passed with shader compilation and asset
  staging; the full CTest run passed 17/17, including raster and traced-proxy
  particle-alpha gates and `qray_dtal_perf`. All 81 HLSL/probe pairs pass the
  full shader-property/reflection checker. The default and custom
  `inc`/`lib64` installed-package consumers build, link and run. The guarded
  `tests/perf/run_menu.ps1 -Smoke -Validation` run passed menu, base-map load,
  in-game menu, return to UI-only rendering and resize with
  `ValidationEnabled=True`, `ValidationErrorCount=0` and process exit code 0.
  The runner writes the same result tuple to
  `build/Debug/menu-candidate-bc6a6b7c.stdout.result.json`.
  The tested host binary SHA-256 is
  `7407A302A351EAED4572DABD842AAF7073B5332A840351F87A1931CADA5DED85`;
  the SDK checkout is `fccd3ea` plus the documented working-tree changes.
- `build/Debug/id1/screenshot0052.png` records the loaded traced world and
  `screenshot0054.png` records the post-return UI-only frame. These are smoke
  evidence, not a visual-equivalence or performance acceptance result.

## 4.8 Shared GPU context — implementation 2026-10-24

- `renderer/Include/qray/gpu_context.h` adds a versioned C++20 contract for
  SDK-owned shared GPU access. `QrayGpuRequirements` is passed before
  `vkCreateDevice`; `QrayGpuContext` returns the borrowed NVRHI device, native
  Vulkan handles, queue roles/counts and negotiated capabilities.
- The integration rules are in
  `renderer/docs/gpu-context.md`: one `QrInstance` owns one device; a host must
  not create a second device or allocator path for participating resources and
  must release host resources before destroying the instance.
- Requirements cover Vulkan API version, frames in flight, instance/device
  extensions, NVRHI features, formats/sample counts, queue counts, preferred
  physical device, native device validation and an optional Vulkan feature
  chain callback. The SDK merges them with its own RT/NVRHI requirements and
  selects a physical device that satisfies both.
- The host can create NVRHI resources on the returned device. It cannot destroy
  the device or outlive `qrDestroyInstance`; host resources must be released
  first.
- `qrSetGpuFrameHooks` exposes two record-only phases:
  `BeforeRenderer` and `BeforePresent`. Both receive the SDK's already-open
  NVRHI command list. The SDK owns submission, synchronization and present.
- The renderer constructor is now failure-safe: partial Vulkan/NVRHI objects
  are destroyed before instance creation error propagation.
- Verification: `qray_gpu_context_tests` covers requirements and contract
  metadata; `qray_spatial_twin_gpu` passes create-time extension/format/NVRHI
  requirements, retrieves the shared context, creates a host NVRHI buffer,
  records host work in both frame phases and renders the full RT twin scene.
- QuakeRay is adapted through `Quake/qr_gpu_context.cpp`: the reference host
  uses the requirements API, validates the returned shared context and creates
  a probe NVRHI buffer during initialization. A valid `qr_ad_swamp` capture
  completed with rendered RT frames after the adaptation:
  `build/Debug/audit-sdk-gpu-context-docs-final-20261024-231244-aac95f`.
  The capture contains 121 frames, `Shared GPU context: SDK-owned
  NVRHI/Vulkan device validated` in its console log, executable SHA-256
  `3276D9C16651976C563F0F291636C30B533BC2CAADD9FBE442C6C01A2F72F0CC`.
  Its 36.2 FPS Debug figure is smoke evidence, not a performance acceptance
  result.

## 5. Phases

### Phase 0 — Baseline and scope

- Freeze HEAD, executable/pak hashes, preset and asset set.
- Capture a baseline at HEAD (correctness in Debug; timing only with explicit permission).
- Confirm Goal A in writing.
- No renderer behavior change.

### Phase 1 — In-engine authoring slice

- One material override (roughness/emission) and one authored light.
- Sidecar save/reload; per-entry identity report.
- No static resubmit for the common material path; measure texture re-synthesis and upload cost.
- No-PVS OLEF test on a synthetic scene (8 fast + tail; cluster 0; p99 OLEF).
- Keep the slice small: `rt_material`/`rt_lights`/`qray.h` plus a test hook, not a `qr_editor.c` rewrite.

### Phase 2 — Contract spec and conformance

- **Implemented foundation:** source-level `IQrayHost`, `QraySceneAsset`,
  `QrayFrameView`, `QrayFrameUpdates`, `QraySidecar`, versioned headers,
  capability flags, ownership/lifetime and file codec boundary.
- **Implemented lowering:** `QraySceneBinding` uses the existing `qray.h`
  upload/draw calls and never introduces a second scene path.
- **Implemented conformance:** fake-transport call-order and validation tests,
  installed-package linkage and the existing Quake/GPU regression routes.
- **Remaining authoring work:** shared engine-free editor core, generic file
  codec and migration of the existing YAML writers.

### Phase 3 — Standalone editor extraction

- Extract a shared editor core from `qr_gui` and the authoring model.
- `quakerayeditor` loads a Quake map through its current BSP preview path.
- Same fixed-camera image as the in-engine editor within tolerance.
- No game logic; no per-engine adapter code in the editor core.

### Phase 4 — Runtime updates

- Delta-only updates, geometry removal, persistent light registry or a documented full per-frame list.
- Counters: `staticSubmits`, `materialUpdates`, `materialBytes`, `authoringActive`, allocation count.
- No hidden `Refresh` rebuilds; no `vkDeviceWaitIdle` in the normal frame path.

### Phase 5 — Direct reference-host validation

- Use the existing QuakeRay adapter and task graph as the required real-host validation.
- Run the contract through the same meter boundary and production renderer path.
- A second real host is optional evidence for broader adapter portability, not a
  release prerequisite for SDK contract version 1.0.

## 6. Acceptance criteria

- **Goal A:** `quakerayeditor` loads a Quake map, renders RT, edits material and
  light state, saves and reloads its documents, and reports per-entry binding
  status; no game logic; no per-engine code in the core.
- **Conformance:** `lighting`/`authoring`/`editor core` compile without engine headers; a mock host passes the contract tests; capability flags are honored.
- **No hidden work:** with the editor off and no edits, `staticSubmits = 0`, `materialUpdates = 0`, no new allocations/locks, unchanged geometry uploads equal.
- **Material path:** a common roughness/emission edit does not trigger geometry re-upload; cost is measured.
- **No-PVS:** OLEF p99 <= 0.5% is adequate; >2% requires a visibility structure.
- **Identity:** a no-op recompile preserves bindings; a dependency-only change does not orphan surface edits; ambiguous bindings are reported.
- **Editor extraction:** fixed-camera image matches the in-engine editor within tolerance; `qr_gui` and the authoring model are reused, not duplicated.

## 7. Non-goals

- Per-engine loaders/converters in the QuakeRay repository.
- Host-owned Vulkan device, queue, target and synchronization integration.
- Linux/non-Win32, proprietary-host licensing, UE/Unity/Source integration.
- Geometry editing, BSP compilation, entity authoring, game logic.
- MDL v10/skeletal animation unless it becomes a generic core capability.
- A binary plugin ABI; adapters are source-level.

## 8. Risks

| Risk | Observation | Mitigation |
|---|---|---|
| Scope classification | Host-owned deformation, stencil/RTT, arbitrary blend equations or multi-view is mistaken for an SDK gap | State the division explicitly; require an equivalent upload or a host-owned composition path |
| No-PVS quality | OLEF >2% or cluster 0 empty | Omitted PVS tables synthesize all-visible rows; a host with occlusion data can still provide PVS; a spatial provider remains a separate quality option |
| Material cost | Texture re-synthesis >16 ms per edit | 1x1 updateable RME override; measure before promising interactivity |
| Editor extraction | 9,156-line `qr_editor.c`, 106 `Cvar_`, 49 `cl.` | In-engine prototype first; extract a shared core, do not move the file |
| Stable IDs | No map corpus; recompile/orphan behavior untested | Build a two-BSP corpus; report unresolved entries; never guess |
| Adapter leakage | Quake assumptions in the core | Include-path guard, package consumer and the QuakeRay reference integration |
| Zero-overhead claims | No HEAD baseline; missing counters | Phase 0 baseline; add counters; measure before claiming |

## 9. Open decisions

- Whether a second real host should be added later to broaden observed adapter portability.
- Whether HL1-style lightmaps become an explicit core term or the product is framed as "RT relight".
- Whether the shared editor core reuses `qr_gui` panels or starts a new UI; the current plan prefers reuse.
