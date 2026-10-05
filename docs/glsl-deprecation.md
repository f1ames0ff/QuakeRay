# GLSL deprecation and removal queue

The hand-off for picking this work up is `docs/handoff-glsl-deprecation.md`.

## Status

The GLSL shader sources under `renderer/Source/Shaders/GLSL/` are deprecated. The runtime loads
only the HLSL blobs: the shader build compiles the root and `HLSL/` sources and the host binds the
resulting SPIR-V. GLSL is still in the tree for one purpose: the cross-language parity check of
`CheckShaderProperties.py`, which compiles each GLSL twin and its HLSL replacement and compares the
reflected properties, plus the probe pairs (`GLSL/<name>.probe.comp` against
`Probes/<name>.probe.comp.hlsl`).

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

This is the expected state while GLSL is deprecated; the parity check is now a transitional tool.

## Inventory

- 69 files, approximately 450 KB: 45 `.comp`, 8 `.frag`, 6 `.vert`, 6 `.rgen`, 2 `.rmiss`,
  1 `.rahit`, 1 `.rchit`.
- The GLSL halves are never executed by the engine; the probe files are not loaded by the host.

## Removal queue

- [ ] Stage 0 — freeze: no new GLSL files; HLSL changes no longer carry a GLSL update; the checker's
      GLSL failures are recorded, not fixed.
- [~] Stage 1 — make the check HLSL-only: the `--skip-glsl` mode is in `CheckShaderProperties.py`
      (default off): it compiles, disassembles and reflects each HLSL half alone and skips the GLSL
      twins entirely. Verified: `--skip-glsl` exits zero on this branch, the default mode still
      fails exactly as recorded above. The mode also surfaced a stale HLSL probe —
      `Probes/ShaderCommonFunc.probe.comp.hlsl` still touched the removed `tmReinhard` — now aligned
      with the current `ShTonemapping` block. Remaining: flip the default once the owner freezes the
      twins. Keep property verification for the HLSL side through the generated `ShaderCommon`
      tables and the allow list.
- [ ] Stage 2 — retire the probe parity: decide the probes' new golden half (HLSL reflection or
      recorded SPIR-V), because `Probes/*.hlsl` currently pair with `GLSL/*`.
- [ ] Stage 3 — delete the folder in batches, each verified by the new check: post/prepare passes
      (`CmPrepareFinal`, `CmLuminanceAvg`, `CmBloom*`, `CmPrepareHdr`, `Ef*`), then the Q2 and RT
      passes, then the shared includes (`ShaderCommon`, `Light`, `Random`, `Q2LightLists`), then the
      probe halves last.
- [ ] Stage 4 — remove GLSL references from `build_shaders.ps1`, `GenerateShaderCommon.py`, the docs
      and the `ShaderPropertiesAllowList.txt` entries that exist only for GLSL/HLSL spelling
      differences.

## Guardrails

- Do not delete GLSL before Stage 1/2 give the HLSL side its own verification; otherwise the parity
  check and the probe comparison disappear without a replacement.
- Keep the allow-list entries until the GLSL halves they explain are gone.
- The removal is a repository-wide cleanup and belongs in its own branch and pull request, not in
  the lighting work.
