# GLSL deprecation and removal queue

The hand-off for picking this work up is `docs/handoff-glsl-deprecation.md`.

## Status

The migration is complete: the GLSL shader sources under `renderer/Source/Shaders/GLSL/`, the
GLSL-only headers and includes that sat next to the sources, and the generated
`renderer/Source/Generated/ShaderCommonGLSL.h` are gone. The runtime loads only the HLSL blobs:
the shader build compiles the root and `HLSL/` sources with dxc and the host binds the resulting
SPIR-V. The HLSL side is verified by `CheckShaderProperties.py` against the checked-in
reflection recordings under `renderer/Source/Shaders/Reflection/`, one per pair, probes
included. The legacy `--glsl` cross-language parity mode and the `ShaderPropertiesAllowList.txt`
that only it read are deleted along with the language they compared against.

## Current state

`CheckShaderProperties.py` recompiles every HLSL half and verifies the reflected property set
against its recording; the recordings are the golden half, `--record [--force]` refreshes them,
and the default run exits zero on all 76 pairs. `GenerateShaderCommon.py` no longer writes a GLSL
header, and `GenerateShaderCommonProbe.py` derives `Probes/ShaderCommon.probe.comp.hlsl` from
`ShaderCommonHLSL.hlsli` instead of the GLSL header. The `GenerateShaders.py -gencomm` chain
regenerates the headers and the probe, and the checker stays green after it.

The GLSL references that remain are historical notes in the ported shader and host comments and
in these docs; no GLSL source, tooling entry point or shader build input is left.

## Inventory (historical)

The removal deleted 69 files, approximately 450 KB, from `GLSL/`: 45 `.comp`, 8 `.frag`,
6 `.vert`, 6 `.rgen`, 2 `.rmiss`, 1 `.rahit`, 1 `.rchit`. The 33 root GLSL-only files next to
the sources (25 `.h`, 7 `.inl`, `TonemappingUtils.glsl`) and the generated
`renderer/Source/Generated/ShaderCommonGLSL.h` went with them.

## Removal queue

- [x] Stage 0 — freeze: no new GLSL files; HLSL changes no longer carry a GLSL update; the
      checker's GLSL failures were recorded, not fixed.
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
      the run. `--record [--force]` creates and refreshes the recordings. The default mode exits
      zero on all 76 pairs.
- [x] Stage 3 — delete the folder in batches, each verified by the new check: post/prepare passes
      (`CmPrepareFinal`, `CmLuminanceAvg`, `CmBloom*`, `CmPrepareHdr`, `Ef*`), then the Q2 and RT
      passes, then the shared includes (`ShaderCommon`, `Light`, `Random`, `Q2LightLists`), then the
      probe halves last. The deletion inventory included the 33 root GLSL-only files next to the
      sources (25 `.h`, 7 `.inl`, `TonemappingUtils.glsl`) and the generated
      `renderer/Source/Generated/ShaderCommonGLSL.h`. The `GenerateShaderCommonProbe.py` rework had
      to land together with the probe-half deletion: the `GenerateShaders.py -gencomm` chain
      regenerates the probe through that script, so deleting the probe half while the generator
      still read `ShaderCommonGLSL.h` recreated the file.
- [x] Stage 4 — remove the GLSL references from `build_shaders.ps1`, `GenerateShaders.py`,
      `GenerateShaderCommon.py`, `GenerateShaderCommonProbe.py`, `CheckShaderProperties.py` and the
      docs, and delete `ShaderPropertiesAllowList.txt` with the `--glsl` mode it served. The
      checker's `--glsl` flag, its parity path and the allow list are gone; the probe generator
      reads `ShaderCommonHLSL.hlsli`; `GenerateShaderCommon.py` no longer writes the GLSL header.

## Guardrails

- Do not delete GLSL before Stage 1/2 give the HLSL side its own verification; otherwise the parity
  check and the probe comparison disappear without a replacement.
- Keep the allow-list entries until the GLSL halves they explain are gone.
- The removal is a repository-wide cleanup and belongs in its own branch and pull request, not in
  the lighting work.
