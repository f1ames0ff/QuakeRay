# GLSL deprecation and removal queue

The hand-off for picking this work up is `docs/handoff-glsl-deprecation.md`.

## Status

The GLSL shader sources under `renderer/Source/Shaders/GLSL/` are deprecated. The runtime loads
only the HLSL blobs: the shader build compiles the root and `HLSL/` sources and the host binds the
resulting SPIR-V. The HLSL side is verified by `CheckShaderProperties.py` against the checked-in
reflection recordings under `renderer/Source/Shaders/Reflection/`, one per pair. GLSL is still in
the tree for one purpose: the legacy `--glsl` cross-language parity mode of the same checker,
which compiles each GLSL twin and its HLSL replacement and compares the reflected properties,
plus the probe pairs (`GLSL/<name>.probe.comp` against `Probes/<name>.probe.comp.hlsl`).

No new GLSL sources are to be added. When an HLSL shader changes, its GLSL twin is not updated
anymore; the failing twins listed below are intentionally not repaired.

## Current state (origin/master 9f086ec4 and every branch merged with it)

The post-effects / bloom / HDR migration updated the HLSL halves without updating the GLSL twins.
`CheckShaderProperties.py` therefore fails identically on `origin/master` and on merged branches:

- `GLSL/CmPrepareFinal.comp` — compile errors (`tmReinhard` is no longer a field of the tonemapping
  block; `mix` overload resolution) against the rewritten HLSL that now uses `LocalExposure.hlsli`,
  `NearDof.hlsli` and `PostEffects.hlsli`.
- `GLSL/ShaderCommonFunc.probe.comp` — the same removed `tmReinhard` field.
- `GLSL/CmLuminanceAvg.comp` — four property mismatches (descriptor set 1 binding 0, storage class,
  block layout, builtin `GlobalInvocationId`) against the rewritten HLSL reduce.

This is the expected state while GLSL is deprecated; the parity check is now the transitional
`--glsl` mode of the checker, and the default mode verifies the HLSL side against the recordings.

## Inventory

- 69 files, approximately 450 KB: 45 `.comp`, 8 `.frag`, 6 `.vert`, 6 `.rgen`, 2 `.rmiss`,
  1 `.rahit`, 1 `.rchit`.
- The GLSL halves are never executed by the engine; the probe files are not loaded by the host.

## Removal queue

- [ ] Stage 0 — freeze: no new GLSL files; HLSL changes no longer carry a GLSL update; the checker's
      GLSL failures are recorded, not fixed.
- [x] Stage 1 — make the check HLSL-only: the `--skip-glsl` mode landed first (default off) and
      Stage 2 has replaced it: the default mode now verifies each HLSL half against its recording
      and the transitional flag is gone. Verified: `--skip-glsl` exited zero on this branch, the old
      default failed exactly as recorded above. The mode also surfaced a stale HLSL probe —
      `Probes/ShaderCommonFunc.probe.comp.hlsl` still touched the removed `tmReinhard` — now aligned
      with the current `ShTonemapping` block. Property verification for the HLSL side lives in the
      recordings under `Reflection/` (see Stage 2).
- [x] Stage 2 — the golden half is a recorded HLSL reflection: `CheckShaderProperties.py` records
      the reflected property set of every pair, probes included, under `Reflection/<pair path>.txt`
      (`Reflection/HLSL/CmPrepareFinal.comp.txt` for `HLSL/CmPrepareFinal.comp.hlsl`,
      `Reflection/Probes/ShaderCommon.probe.comp.txt` for the probes), and the default mode
      recompiles each HLSL half and verifies it against its recording with an empty allow list. A
      missing or malformed recording, a mismatch and a recording without a current pair all fail
      the run. `--record [--force]` creates and refreshes the recordings; `--glsl` keeps the legacy
      cross-language parity comparison, with `ShaderPropertiesAllowList.txt` applying to that mode
      only, until Stage 4 deletes both. The default mode exits zero on all 76 pairs, and `--glsl`
      still fails exactly the three stale twins listed above.
- [ ] Stage 3 — delete the folder in batches, each verified by the new check: post/prepare passes
      (`CmPrepareFinal`, `CmLuminanceAvg`, `CmBloom*`, `CmPrepareHdr`, `Ef*`), then the Q2 and RT
      passes, then the shared includes (`ShaderCommon`, `Light`, `Random`, `Q2LightLists`), then the
      probe halves last. The deletion inventory must include the 33 root GLSL-only files next to the
      sources (25 `.h`, 7 `.inl`, `TonemappingUtils.glsl`) and the generated
      `renderer/Source/Generated/ShaderCommonGLSL.h`. The `GenerateShaderCommonProbe.py` rework must
      land together with the probe-half deletion: `GenerateShaderCommon.py -gencomm` regenerates
      `GLSL/ShaderCommon.probe.comp` through that script, so deleting the probe half while the
      generator still writes it recreates the file.
- [ ] Stage 4 — remove the GLSL references from `build_shaders.ps1`, `GenerateShaderCommon.py`,
      `GenerateShaderCommonProbe.py`, the docs and the `ShaderPropertiesAllowList.txt` entries that
      exist only for GLSL/HLSL spelling differences.

## Guardrails

- Do not delete GLSL before Stage 1/2 give the HLSL side its own verification; otherwise the parity
  check and the probe comparison disappear without a replacement.
- Keep the allow-list entries until the GLSL halves they explain are gone.
- The removal is a repository-wide cleanup and belongs in its own branch and pull request, not in
  the lighting work.
