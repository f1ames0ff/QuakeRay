# Merging master into a renamed/rewritten renderer branch

This guide is for agents working on branches that build on the renaming detach (`vkpt/` -> `renderer/`,
`namespace qray`, `Rg*`/`rg*`/`RG_*` -> `Qr*`/`qr*`/`QR_*`, the public header `<qray/qray.h>`, the CMake target
`renderer`). Master is written in the OLD names: **every hunk that arrives from master must be translated**, not
copied. The worked example is the merge commit `5cf4ca0c` ("Merge origin/master into feature/detach-p0-p1"); its
diff is the reference for what a good resolution looks like.

## Before merging

1. `git fetch origin` and inspect what arrives: `git log --oneline HEAD..origin/master`,
   `git diff --stat HEAD origin/master -- renderer Quake CMakeLists.txt`.
2. Assume master will reintroduce old paths (`vkpt/...`) and old tokens. New files master added under `vkpt/`
   surface as `CONFLICT (file location): ... added in origin/master inside a directory that was renamed in HEAD` -
   keep them, move them under `renderer/`, and translate their names.
3. Do not merge with a dirty working tree beyond the known `third_party/nvrhi` patch.

## The merge

`git merge origin/master -m "Merge origin/master into <branch>"`

Expect three conflict classes:

- **content (`UU`)**: the files both sides touch constantly - `Quake/gl_rmain.c`, `Quake/r_alias.c`,
  `Quake/r_sprite.c`, `Quake/r_world.c`, `readme.md`, `renderer/Source/RasterizedDataCollector.cpp`,
  `renderer/Source/VertexCollector.cpp`, and whatever the current master work touches;
- **modify/delete (`DU`)**: master edited a path this branch renamed or deleted - `vkpt/Include/vkpt/vkpt.h`,
  `vkpt/Source/LightManager.cpp`, `vkpt/Source/VulkanDevice.cpp`, `vkpt/Source/Generated/GenerateShaderCommon.py`,
  and modules the branch deleted (`Rasterizer.*`, the legacy frame modules);
- **file location (`UA`)**: new master files placed under the old directory.

## Resolution rules

- **Never `--ours`/`--theirs` wholesale.** Resolve hunk by hunk and keep both intents: master's new logic must
  survive, expressed in the branch's names and rewritten code.
- **Token translation table** (apply to every merged line):
  `Rg<Name>` -> `Qr<Name>`, `rg<name>` -> `qr<name>`, `RG_<NAME>` -> `QR_<NAME>`, `RGAPI`/`RGCONV`/`RG_STATIC` ->
  `QRAPI`/`QRCONV`/`QR_STATIC`, `RG_RTGL_VERSION_API` -> `QR_API_VERSION` (the string value stays),
  `namespace vkpt`/`vkpt::` -> `qray`, `vkpt/` paths -> `renderer/`, `<vkpt/vkpt.h>` -> `<qray/qray.h>`.
  Watch for false positives: `RGBA`, `Rgb*`, unrelated `rg*` tokens.
- **For a `DU`**: read master's change (`git diff <merge-base> origin/master -- <old path>`, or
  `git show origin/master:<old path>` against its parent) and port it into the new-path file, translating names.
  Keep the branch's rewritten structure; do not resurrect the old file.
- **For a module the branch deleted and re-homed** (e.g. the legacy `Rasterizer`'s draw loops now live in
  `RhiRasterOverlayPass`/`RhiUiPass`, its collector holder in `VulkanDevice`): re-express master's change in the
  new home - for example master's per-draw scissor became `ToLegacyScissor()` in the RHI passes.
- **New master files** (`GlobalLightSampling.*`, `qr_gui.*`, `rt_dtal_debug.*`, vendored `third_party/imgui`):
  keep as written, translate any renderer tokens inside; wire them into CMake in the same commit. Shaders must use
  the `QR_*` macros or they will not compile against the regenerated headers.
- **Generated headers** (`renderer/Source/Generated/*`): regenerate through `GenerateShaderCommon.py`; never
  hand-merge output. Apply master's generator changes to the rewritten generator, keeping its `QR_*` emission and
  `namespace qray`, then run it and confirm the outputs are the expected ones.
- **`readme.md`**: keep the branch's condensed structure; fold master's new facts as short bullets.
  **`changelog.md`**: merge as text; old names in historical prose are fine.
- **No comments may be added** while resolving (see the global rule). Do not re-add `#endif // ...` notes.

## Verification checklist (all mandatory before committing the merge)

1. `cl -Zs` sweep over the compile database (215+ TUs); every C/C++ TU must pass. The `Windows/QuakeRay.rc` entry
   "fails" under `-Zs` by design (it is an `rc.exe` command) - ignore it. For TUs master added, take the flags of
   a sibling TU if the database is stale.
2. `.\build_shaders.ps1` plus `CheckShaderProperties.py` and `CheckMatrixReads.py` - all green.
3. `git diff --check` - clean except vendored `third_party/imgui` trailing whitespace (leave it).
4. Old-token grep in code and paths: `vkpt`, `\bRg[A-Z]`, `\brg[A-Z]`, `\bRG_[A-Z]`, `RTGL` - zero outside
   historical prose (`changelog.md`) and the few "access RTGL data" wording comments in
   `renderer/Source/Shaders/ShaderCommon*Func.*`.
5. Commit the merge, then let the owner build and run: the gate must cover whatever the merge brought (in the
   `5cf4ca0c` case: the GUI editor, the DTAL limits and `rt_dtal_debug`, the scissor/ImGui clipping, the restir
   uniforms).

## Pitfalls seen in practice

- The dev-config file is `qray.txt` now; an old `vkpt.txt` stops being read silently.
- The legacy-frame golden files are gone; do not restore them to satisfy an importer's hunk.
- `third_party/nvrhi` stays build-patched and shows dirty; never stage it.
- Branches cut from master directly will miss the legacy-removal changes and make "dead code" live again - stack
  on the branch that carries them, or merge that branch first.
