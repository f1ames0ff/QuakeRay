# QuakeRay Portable Toolkit — Plan

Status: draft. Revision 5. Date: 2026-10-21.

Diagram: [architecture-portable-toolkit.html](architecture-portable-toolkit.html)

## 1. Scope: Goal A — standalone toolkit

QuakeRay Portable Toolkit is a **source-level toolkit**, not an engine. The supported integration mode is **Goal A: standalone**.

- QuakeRay owns the window, Vulkan device and swapchain.
- A host engine implements an adapter that imports its map/assets into the toolkit contracts.
- The standalone `qray_editor` renders the RT preview and edits lights, materials and DTAL groups.
- The existing `quakeray.exe` is the **reference Quake host**, not a new binary. There is no separate `qray_game` product.

Out of scope for this plan:

- **Goal B — embedded GPU**: host-owned `VkDevice`, queue, target images and synchronization; rendering inside the host engine's viewport.
- Per-engine adapters and loaders: host engines implement their own adapters.
- Linux/non-Win32, proprietary-host licensing, Unreal/Unity/Source integration.
- Geometry editing, BSP compilation, game logic, entity authoring.
- MDL v10 / skeletal animation unless it becomes a generic core capability.

Goal B is a future mode, gated by a product and licensing decision. The contracts leave room for it (`IRenderTarget`, `QrOutputMode`, `qrQueryDeviceRequirements`), but no implementation is planned here.

## 2. Responsibility split

| Area | QuakeRay modules | Host engine |
|---|---|---|
| RT rendering | `rtcore`: NVRHI/Vulkan passes, ASVGF, materials | — |
| Lighting | `lighting`: clusters, DTAL runtime, registry, publication | Provides cluster/spatial data and light sources |
| Authoring | `authoring`: sidecar model, merge, stable IDs | Maps its assets to the model |
| Editor | `editor core` + `qray_editor` | Provides picking/selection/apply hooks if embedding the editor |
| Contracts | `IQrayHost`, `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, `QraySidecar` | Implements `IQrayHost`; passes conformance tests |
| Assets | — | Map/model/texture loaders, material mapping, unit/coordinate conversion |
| Spatial | Cluster consumer | BSP/PVS or a spatial provider; `QraySpatialProvider` |
| GPU | Owns device/window in Goal A | Provides device/target/sync only in Goal B |
| Game logic | — | Entity I/O, triggers, AI, animation, save/load |

Rules:

- `lighting`, `authoring` and `editor core` do not include engine headers and do not know about Quake.
- The Quake adapter lives in `hosts/quake` and is the reference implementation, not part of the core.
- If a host cannot express its data, that is a **core capability gap** (for example, skinned meshes or lightmaps), not an adapter bug. An adapter cannot invent a renderer feature.
- The host adapter is responsible for performance: no per-frame world rebuild, no copies of unchanged data, no hidden allocations in the frame path.

## 3. Architecture

Layers:

- `rtcore` — NVRHI/Vulkan RT renderer, passes, ASVGF, materials, BLAS/TLAS ownership.
- `lighting` — cluster composition, DTAL runtime, registry, publication; no Vulkan, no engine headers.
- `authoring` — `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, sidecar, stable IDs, merge.
- `editor` — shared editor core + standalone `qray_editor`; read-only geometry.
- `hosts` — source-level adapters; Quake is the reference.
- `platform` — window, input, filesystem, GPU device.

The existing `qray.h` upload path is preserved. There is no second scene-transfer path and no monolithic `QrayFrame`.

Contracts:

- `QraySceneAsset` — static map resources: surfaces, materials, authored lights, DTAL groups, source keys. Consumed at bind/load; core copies synchronously.
- `QrayFrameView` — camera, projection, viewport, exposure, `viewId`, `QR_VIEW_RESET_HISTORY`.
- `QrayFrameUpdates` — typed deltas (transform, texcoords, material contents, light add/modify, DTAL) and typed streams (particles, dynamic geometry).
- `IQrayHost` — `Caps`, `Attach`/`Detach`, `SurfaceKey`/`ResolveSurface`/`LightKey`, `Refresh`, `EvaluateStyles`, `ApplySidecar` with per-entry report, `LoadAuthoredLights`, optional `Pick`.
- `QraySidecar` — versioned authoring intent: materials, lights, DTAL groups. Not a serialization of runtime structs.

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
- No-PVS default: 8 fast slots plus the alias tail; a hard 8-ceiling exists only with `rt_cluster_sampling 0`.
- A non-BSP host provides `QraySpatialProvider` (`HOST_PARTITION`, `CORE_GRID`, `HOST_DENSE_MAP`), or accepts the measured no-PVS quality.

Material path:

- `qrUpdateMaterialContents` updates textures only.
- Roughness/metallicity/emission are RME texture samples or per-instance defaults.
- The common editor path is texture re-synthesis / in-place update, not geometry re-upload.
- Material-handle replacement can force a static resubmit; measure it.
- A 1x1 updateable RME override is the cheapest constant-scalar path (candidate, not fact).

## 4. Baseline check — 2026-10-22

Branch `feat/portable-toolkit`, based on `master` (`cd88fc77`).

What is already true:

- `renderer/Include/qray/qray.h` is the public C ABI, and `QR_API_VERSION` is `1.03.0000`.
- The renderer's C++ sources do not include `Quake/` headers. The remaining engine-facing layer is the `Qr*` contract in `qray.h`.
- The alias-table implementation is renderer-owned: `renderer/Source/rt_alias.*` builds as `qray_alias`, and both host and tests link it instead of compiling `shared/rt_alias.c`.
- The host-owned texture and cursor assets are out of `renderer/`: `renderer/Source/textures` is now `gfx/textures`, and `renderer/gfx` is now the root `gfx/` folder. `build_win.ps1` stages both from `gfx/`; the Debug build and asset pack still pass.
- `renderer/CMakeLists.txt` has a standalone `project(qray_renderer VERSION 1.3.0)`, finds Vulkan and NVRHI when configured from its own directory, and `cmake -S renderer -B <dir>` now configures successfully.
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

- `renderer/` is now the `QuakeRaySDK` git submodule at `cb8ddb82`.
- The SDK `main` contains the package install/export path, `qray_rendererConfig.cmake`, the FSR provider and the engine-free tests.
- The Quake cluster test moved to the host tree as `tests/qray_cluster_lists_tests.cpp`.
- A clean consumer project configured and linked `qray::renderer` from an installed prefix with external NVRHI targets.
- The full host CTest suite passes (10/10) after the SDK integration. `qray_dtal_perf` was initially clock-sensitive and passed on a warm GPU without changing the baseline.

## 4.2 Current code anchors

- `renderer/Include/qray/qray.h` — existing C ABI.
- `renderer/Source/ClusterLightLists.cpp`, `renderer/Source/LightManager.cpp` — lighting runtime.
- `Quake/rt_dtal_groups.c`, `Quake/rt_dtal_lod.c`, `Quake/rt_cluster_select.c` — engine-free DTAL runtime.
- `Quake/gl_rlight.c`, `Quake/r_world.c` — Quake host / reference adapter inputs.
- `Quake/qr_editor.c`, `Quake/qr_gui.cpp` — in-engine editor (coupled).
- `renderer/Source/VulkanDevice.cpp`, `ASManager` — Vulkan/AS ownership.

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

- Freeze `IQrayHost`, `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, `QraySidecar`.
- Versioning, capability flags, ownership/lifetime, borrowed views, delta/stream split.
- Mock/synthetic host and conformance tests.
- No engine headers in `lighting`, `authoring`, `editor core`.

### Phase 3 — Standalone editor extraction

- Extract a shared editor core from `qr_gui` and the authoring model.
- `qray_editor` loads a Quake map through the reference adapter.
- Same fixed-camera image as the in-engine editor within tolerance.
- No game logic; no per-engine adapter code in the editor core.

### Phase 4 — Runtime updates

- Delta-only updates, geometry removal, persistent light registry or a documented full per-frame list.
- Counters: `staticSubmits`, `materialUpdates`, `materialBytes`, `authoringActive`, allocation count.
- No hidden `Refresh` rebuilds; no `vkDeviceWaitIdle` in the normal frame path.

### Phase 5 — Second host validation

- A host-owned adapter for a second engine, starting with a data-only static map + lights + editor sidecar.
- The adapter is not part of the core; it must pass the conformance tests.
- This proves adapter thickness, not non-BSP genericity.

### Phase 6 — Future Goal B (only if chosen)

- `IRenderTarget`, `QrOutputMode`, external device/queue/target/sync.
- Host-owned adapter; BLAS/TLAS remain core-owned.
- Product and licensing decision first.

## 6. Acceptance criteria

- **Goal A:** `qray_editor` loads a Quake map, renders RT, edits one material and one light, saves the sidecar, reloads, and reports per-entry binding status; no game logic; no per-engine code in the core.
- **Conformance:** `lighting`/`authoring`/`editor core` compile without engine headers; a mock host passes the contract tests; capability flags are honored.
- **No hidden work:** with the editor off and no edits, `staticSubmits = 0`, `materialUpdates = 0`, no new allocations/locks, unchanged geometry uploads equal.
- **Material path:** a common roughness/emission edit does not trigger geometry re-upload; cost is measured.
- **No-PVS:** OLEF p99 <= 0.5% is adequate; >2% requires a visibility structure.
- **Identity:** a no-op recompile preserves bindings; a dependency-only change does not orphan surface edits; ambiguous bindings are reported.
- **Editor extraction:** fixed-camera image matches the in-engine editor within tolerance; `qr_gui` and the authoring model are reused, not duplicated.

## 7. Non-goals

- Per-engine loaders/converters in the QuakeRay repository.
- Embedded GPU rendering (Goal B) in the current plan.
- Linux/non-Win32, proprietary-host licensing, UE/Unity/Source integration.
- Geometry editing, BSP compilation, entity authoring, game logic.
- MDL v10/skeletal animation unless it becomes a generic core capability.
- A binary plugin ABI; adapters are source-level.

## 8. Risks

| Risk | Observation | Mitigation |
|---|---|---|
| Core capability gap | A host needs skinned meshes, lightmaps or water and the core lacks them | Treat as a core feature request; do not hide it in the adapter |
| No-PVS quality | OLEF >2% or cluster 0 empty | Host provides visibility; or core adds a spatial/visibility provider |
| Material cost | Texture re-synthesis >16 ms per edit | 1x1 updateable RME override; measure before promising interactivity |
| Editor extraction | 9,156-line `qr_editor.c`, 106 `Cvar_`, 49 `cl.` | In-engine prototype first; extract a shared core, do not move the file |
| Stable IDs | No map corpus; recompile/orphan behavior untested | Build a two-BSP corpus; report unresolved entries; never guess |
| Adapter leakage | Quake assumptions in the core | Include-path guard + second-host conformance tests |
| Zero-overhead claims | No HEAD baseline; missing counters | Phase 0 baseline; add counters; measure before claiming |

## 9. Open decisions

- Which second host to validate first (host-owned adapter; data-only first).
- Whether HL1-style lightmaps become an explicit core term or the product is framed as "RT relight".
- Whether the shared editor core reuses `qr_gui` panels or starts a new UI; the current plan prefers reuse.
- Whether `QrOutputMode` is reserved now or added when Goal B is chosen.
