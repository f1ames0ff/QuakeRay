# Hand-off: incremental updates of materials and lights (no full world rebuild)

Target branch: `bugfix` (separate agent). Reference branch: `feature/qr-editor`.
Scope: item 1 of the deferred list — "full world rebuild on every edit is the source of the
freeze and of the light re-initialisation; a light/TAL-only update path is needed".

## Problem

Every editor flush (up to 4 times per second while a widget is dragged) rebuilds the whole
static scene: the CPU re-collects all world surfaces and emissive lights, the GPU idles,
every static BLAS is destroyed and rebuilt, and the light list is reset and re-uploaded.
A memory leak in that path was already fixed (`vkpt: the scratch buffer reuses a chunk of
exactly the requested size`, feature/qr-editor), so it no longer grows VRAM, but the cost
per edit is unchanged: a hitch and a visible "all lights reinitialised" flash.

## Current path (all line numbers on feature/qr-editor; verify on bugfix)

1. `Quake/qr_editor.c` — `QRE_FlushDirty` (~475-497): throttles while an ImGui item is
   active (0.25 s), then `TexMgr_ReloadImagesForMaterial` per dirty material and
   `Atomic_StoreUInt32 (&rt_require_static_submit, true)`.
   `QRE_ReapplyTouched` (~499-514) does the same for Cancel/Exit.
2. `Quake/gl_texmgr.c` — `TexMgr_ApplyMaterialFromMat` (~1677-1701): synthesizes
   albedo/RME/normal, creates a **new** `RgMaterial`, destroys the old one under
   `rtspecial_mutex`, then `glt->rtmaterial = newMaterial`.
3. `Quake/gl_rmain.c` — `R_DrawWorldTask` (897-923): when the flag is set, one frame does
   `rgBeginStaticGeometries` → `R_DrawWorld` (re-collects everything, including
   `RT_AddEmissiveLight` for the TALs) → `rgSubmitStaticGeometries`.
4. `vkpt/Source/Scene.cpp` — `StartNewStatic` (282-295): `lightManager->Reset()` (this is
   the light flash), clears `staticUniqueIDToSimpleIndex` and `movableGeomIndices`.
5. `vkpt/Source/ASManager.cpp` — `BeginStaticGeometry` (656-663): `collectorStatic->Reset()`,
   `geomInfoMgr->ResetWithStatic()`.
6. `vkpt/Source/ASManager.cpp` — `SubmitStaticGeometry` (665-718): `vkDeviceWaitIdle` →
   `staticBlas->Destroy()` for every static group → `CopyFromStaging` of all static
   vertices/indices → `SetupBLAS` per group → `asBuilder->BuildBottomLevel` (scratch) →
   `geomInfoMgr->CopyFromStaging`.

## Why the full rebuild exists (do not "just skip it")

Material **texture indices are baked into the static geometry at upload**. Destroying a
material notifies static geometry with empty textures and nothing re-notifies it, which is
why the first cut of the editor blanked surfaces; `rt_require_static_submit` was introduced
to re-upload the world so it picks up the new handles (see the changelog bullet "An edit no
longer blanks the surface it edits"). Any incremental scheme must keep the material index
stable, or re-notify the world for that material only.

## Existing API that makes an incremental path possible

- `rgUpdateMaterialContents` — `vkpt/Source/vkpt.cpp:289`, `VulkanDevice::UpdateMaterial`.
  Updates the textures of an existing material; the material handle stays valid.
- Updateable materials — `RG_MATERIAL_CREATE_UPDATEABLE_BIT` (rg API) and
  `TextureManager::CreateMaterial` (`vkpt/Source/TextureManager.cpp:493-498`, the
  `observer` path forces updateable), `TextureManager::UpdateMaterial` (~590-600).
  A prior in-place attempt with `rgUpdateMaterialContents` produced garbage and was rolled
  back on feature/qr-editor; the most likely reason is that the material was not created
  with the updateable flag, so the update path was not valid for it. Verify this first.
- BLAS update in place — `ASManager::UpdateBLAS` (`ASManager.cpp:480-511`) with
  `update = true`, i.e. `vkCmdBuildAccelerationStructuresKHR` in update mode instead of a
  destroy + full build. `UpdateBLAS` is exercised by the dynamic path already.
- Texcoord-only update — `Scene::UpdateTexCoords` (`Scene.cpp:255-265`) →
  `ASManager::UpdateStaticTexCoords`.
- World lights — `rgUploadWorldLights` (`vkpt.cpp:267`), `RgWorldLightsUploadInfo`; the
  emissive TALs themselves are `RgTexturedAreaLightUploadInfo` + `rgUploadTexturedAreaLight(s)`
  (see `RT_AddEmissiveLight`, `Quake/r_world.c` ~2382, 2446).
- Profiling already in place: `RT_PROF_WORLD` around `R_DrawWorldTask`.

## Proposed plan

1. **Measure first.** Time `R_DrawWorldTask` per edit and split it: material synthesis,
   AS rebuild, light/TAL re-upload. Confirm the dominant term before touching anything.
2. **Classify edits in the editor.** Light-only fields — `is_light`, `light_color`,
   `light_brightness`, `light_styles`, `light_upoffset`, and the emissive mask parameters
   (`color_emissive*`, `texture_emissive`, `emissive_factor`, `blend`) — change light/TAL
   data, not the texture set. Handle-changing fields are the texture paths themselves
   (`texture_base/normals/gloss`), where a new material (or at least new textures) really is
   required. Keep the classification next to `QRE_ParamSet`/`QRE_MarkDirty` so a single
   place decides which flush is requested.
3. **Make editor materials updateable.** Create editor-synthesized materials with
   `RG_MATERIAL_CREATE_UPDATEABLE_BIT`; on a repeat edit of the same material, call
   `rgUpdateMaterialContents` and keep `glt->rtmaterial` unchanged (no world re-notify, no
   AS rebuild, no light reset). Fall back to create/destroy + `rt_require_static_submit`
   when the texture set changes, when the update fails, or on the first edit of a material
   that was loaded without the flag. Watch `RebuildTalCdf`: it is driven from material
   creation/update and must run for the new RME data.
4. **Light-only flush.** If step 3 keeps the world stable, the remaining reason for a static
   submit is the light list. Add a second flag ("lights dirty") beside
   `rt_require_static_submit`; `R_DrawWorldTask` handles it by re-uploading lights/TAL only
   (`rgUploadWorldLights` / textured area lights), leaving `SubmitStaticGeometry` to the
   handle-changing case.
5. Only if steps 3-4 still cost too much, consider update-mode BLAS rebuilds as the middle
   ground (no destroy, no `vkDeviceWaitIdle`, same index).

## Risks / checks

- Material index stability: any path that creates a material must not let the world hold a
  stale index; the deferred destroy in `TextureManager::DestroyMaterial` is per frame slot.
- `rtspecial_mutex` guards create/destroy of special textures — keep the update path under
  the same lock discipline.
- Updateable materials must have consistent size/mip layout between updates; a change of
  resolution (`texture_base` swap) is a handle-changing edit and must take the slow path.
- The light flash may also come from `LightManager::Reset` being needed for correctness
  (unique id bookkeeping); check what it actually invalidates before skipping it.
- Editor session semantics: `Apply`, `Cancel` (`QRE_ReapplyTouched`) and `Exit` must still
  end with the world consistent, whichever path the edits took.

## Acceptance criteria

- Dragging `feather`/`threshold`/`light_brightness` on AD `#lava_tf2` (and on a brush light)
  produces no visible flash of all lights, no multi-frame hitch, and no `vkDeviceWaitIdle`
  stall in a frame capture; VRAM stays flat in a Debug run (the leak fix holds).
- A texture-path change still updates the surface (slow path), and `Cancel` restores the
  loaded material exactly.
- No surface ends up textureless after any sequence of edits (the original regression that
  `rt_require_static_submit` was added for).

## Do not touch

- `vkpt/Source/ScratchBuffer.cpp` strict-comparison fix and the chunk reservation
  (feature/qr-editor) — separate bug, already fixed; if both branches touch it, merge
  carefully.
- The Vulkan error diagnostics in `vkpt/Source/Common.*` and `MemoryAllocator.cpp`
  (they name the failing call, its size and debug name) — useful for this work too.
