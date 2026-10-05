# Hand-off: stale GLSL twins after the HLSL-only migration

## Purpose

This hands off the `CheckShaderProperties.py` failure state that the lighting branch inherits from
`origin/master`. The work is a repository-wide shader-cleanup task, not a lighting task; it should be
picked up separately from the DTAL/cluster work. The accompanying policy and removal queue live in
`docs/glsl-deprecation.md`.

> Completed: the removal queue in `docs/glsl-deprecation.md` was executed. The GLSL sources, the
> generated `ShaderCommonGLSL.h`, the checker's `--glsl` mode and the allow list are gone, and the
> recordings under `Reflection/` verify the HLSL side. This document is kept as the record of the
> state the work started from.

## Summary

`CheckShaderProperties.py` reports 6 property mismatches on any tree merged with current
`origin/master`. A clean `origin/master` worktree produces the exact same 9 error lines. The cause
is the unfinished master migration to HLSL-only shaders: the HLSL halves were rewritten and the
GLSL twins were left behind. Runtime is unaffected because the engine loads only the HLSL blobs;
GLSL is the golden half of the parity check only.

## Evidence

- Merged lighting branch (`refactor/dtal-cluster-dedup`, merge commit `894400fc`):
  `cd renderer/Source/Shaders && python CheckShaderProperties.py` exits non-zero with 6 mismatches.
- Clean `origin/master` (`9f086ec4`) in a throwaway worktree: the same command produces an
  identical failure list (verified with `Compare-Object`). The throwaway worktree was removed.
- The failure lines:
  - `GLSL/CmPrepareFinal.comp:154` — `'tmReinhard' : no such field in structure`; `'mix' : no
    matching overloaded function found`; assignment type mismatch. The HLSL successor was rewritten
    around `LocalExposure.hlsli`, `NearDof.hlsli` and `PostEffects.hlsli`.
  - `GLSL/ShaderCommonFunc.probe.comp:89-90` — the same removed `tmReinhard` field and the
    resulting type error.
  - `GLSL/CmLuminanceAvg.comp` — four property mismatches against the rewritten HLSL reduce:
    `descriptor set 1 binding 0`, `descriptor set 1 binding 0: storage class`,
    `block set 1 binding 0`, `builtin GlobalInvocationId`.
- New HLSL-only passes (`CmBloomDownsample/Upsample`, `CmPrepareHdr`, `EfFilmGrain`,
  `EfGameplayFeedback`, `EfSharpen`, `EfVignette`) have no GLSL twins; the checker skips them, which
  is the intended direction.

## Root cause

`origin/master` is mid-migration from paired GLSL/HLSL shaders to HLSL-only. The migration updates
the HLSL sources, the shared `.hlsli` includes and the tonemapping block layout, but no longer
updates the GLSL twins. `CheckShaderProperties.py` still compiles the GLSL twins as the golden half,
so the stale files fail. Compile errors cannot be suppressed by
`ShaderPropertiesAllowList.txt`: `checkPair()` returns a hard failure when `glslc` fails
(`CheckShaderProperties.py:764`); the allow list only covers property differences on modules that
compile.

## What the lighting branch does and does not do

- Does not repair the stale GLSL twins and does not touch the checker semantics; that would be
  scope creep in the lighting PR.
- Records the policy and the removal queue in `docs/glsl-deprecation.md` (commit `b758b36c`).
- Notes the red gate as a pre-existing master state, identical before and after the merge. The
  lighting work itself compiled, `rt_lighting_tests.exe` reports `1413875 checks, 0 failures`, and
  `CheckMatrixReads.py` passes.

## Recommended follow-up

Execute the stages in `docs/glsl-deprecation.md`, in a branch of its own:

1. Freeze: no new GLSL sources; HLSL changes stop carrying GLSL updates.
2. Add a `--skip-glsl` / warning mode to `CheckShaderProperties.py` (default off), then flip the
   default once the remaining twins are frozen. Keep HLSL-side property verification through the
   generated `ShaderCommon` tables and the allow list.
3. Decide the probes' new golden half (`Probes/*.hlsl` currently pair with `GLSL/*`), then delete
   `GLSL/` in batches: post/prepare and `Ef*` passes first, then Q2/RT passes, then the shared
   includes, then the probe halves.
4. Clean the references in `build_shaders.ps1` (4), `GenerateShaderCommon.py` (24),
   `GenerateShaderCommonProbe.py` (39), `CheckShaderProperties.py` (53), the allow-list entries that
   exist only for GLSL/HLSL spelling differences, and the docs.

## Acceptance for closing the hand-off

- The chosen checker mode exits zero on `origin/master` and on branches merged with it.
- HLSL reflection is still verified without the GLSL half (generated tables and/or recorded SPIR-V).
- The probe comparison has a defined replacement or is retired deliberately.
- `GLSL/` is deleted with its references in the tooling and docs, or the deprecation is extended
  explicitly if removal is deferred again.

## Touchpoints

- `renderer/Source/Shaders/CheckShaderProperties.py`, `ShaderPropertiesAllowList.txt`
- `renderer/Source/Shaders/GLSL/` (69 files, approximately 450 KB)
- `renderer/Source/Shaders/Probes/` (probe pairs)
- `build_shaders.ps1`, `renderer/Source/Generated/GenerateShaderCommon.py`,
  `renderer/Source/Shaders/GenerateShaderCommonProbe.py`
- `docs/glsl-deprecation.md` (policy and queue)
