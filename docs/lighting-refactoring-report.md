# Lighting Refactoring Report: DTAL and Cluster Deduplication

## 1. Identity

| Item | Value |
|---|---|
| Branch | `refactor/dtal-cluster-dedup` |
| Base | the final Project B commit on `feature/cluster-lighting-overflow` |
| Commit 1 | one UID resolution path and one cone encoder |
| Commit 2 | single alias-table API, removed unused selector entry points |
| Commit 3 | shared alias draw helper for GLSL and HLSL |
| Branch history | `git log feature/cluster-lighting-overflow..refactor/dtal-cluster-dedup` |
| Combined diff | 15 files, 103 insertions, 196 deletions |

The branch inherits both accepted projects and changes no behaviour that either report
records as verified.

## 2. Baseline recorded before the first change

- `.\build_win.ps1 -Config Debug -BuildDir build\Debug` — pass.
- `.\build\Debug\rt_lighting_tests.exe` — `1413860 checks, 0 failures`.
- `python CheckShaderProperties.py --rebuild`, `python CheckMatrixReads.py` — pass.
- `git diff --check` — clean.

All four are re-run after every commit in this branch; the final results are in section 5.

## 3. Concrete duplication found and removed

### 3.1 UID-to-current-index resolution lived twice in the publisher

`LightManager::SetClusterLightLists` resolved a stable UID to the frame's current light-array
index with two copied probe loops: one for the fast list and one for the overflow tail, each with
its own cache handling. They are now one local `resolveUid` lambda over a single result cache;
both lists call it. The fast hole marker (`kLightUidHole`) and the unresolved case still produce
`LIGHT_INDEX_NONE`, exactly as before. Net effect in that commit: 39 insertions, 99 deletions.

### 3.2 The cone encoding was written twice in the light encoders

The textured-area encoder and the DTAL group encoder each carried their own angle validation,
`0.999` inner clamp and `4π`-safe outer clamp. Both now call one `EncodeCone` helper. The helper
was extracted verbatim from the textured-area branch; the group encoder already used it, so the
only semantic change is that the textured-area encoder shares the same code path.

### 3.3 The alias-table API carried an output nobody used

`RT_Alias_Build` wrote a `secondary` array (`1 - primary`) that every production caller
immediately overwrote with the real marginal probabilities computed by `RT_Alias_Marginals`. The
parameter and its writes were removed; tests now derive the alias branch probability from the
published primary, which is what the shader actually uses. `RT_ClusterSelect_FastProbability` was
never called outside its own translation unit and was removed; the CPU tests and the shader mirror
use `RT_ClusterSelect_FastSelect`, which is the one implementation of the fast draw.

### 3.4 The composer recorded a candidate distance it never read

`ClusterLightLists::Candidate` stored `dist2` with every recorded pair, but the overflow build
ranks by power and never looked at it. The field and its recording parameter were removed, which
shrinks the candidate sets from 8 to 4 bytes per pair.

### 3.5 The alias draw was copied between the member and tail samplers

`Light.h` and `Q2LightLists.h` each computed the alias column, the within-column fraction and the
threshold comparison. The arithmetic now lives in one small helper per language
(`AliasTable.h` / `AliasTable.hlsli`: `aliasColumn` and `aliasChoice`), included by both headers.
The GLSL and HLSL helpers are the two language halves of the same three lines, which is the
correct form of sharing for this codebase: the probe pair still checks them against each other.
No resource, binding or interface changed.

## 4. What was deliberately not changed

- Sampling formulas and distributions: member weights, tail weights, `beta` bounds, the 0.1% fast
  floor and the `4π`/distance regularizations are untouched.
- Emission areas, reach policy, admission/filter policy, quality defaults and cvars.
- Fast/overflow set composition and the `rt_cluster_sampling` switch semantics.
- Animation parameters and lightstyles handling.
- The GLSL/HLSL pairs were not collapsed into one language; only their duplicate arithmetic was
  moved into a shared per-language helper.

## 5. Verification after the refactoring

| Check | Command | Result |
|---|---|---|
| Debug build | `.\build_win.ps1 -Config Debug -BuildDir build\Debug` | pass |
| Release build | `.\build_win.ps1 -Config Release -BuildDir build\Release` | pass |
| Numerical tests | `.\build\Debug\rt_lighting_tests.exe` | `1413860 checks, 0 failures` |
| Shader build | `.\build_shaders.ps1 -Rebuild -DestDir build\Debug\id1\shaders` | 49 shaders deployed |
| Shader properties | `python CheckShaderProperties.py --rebuild` | pass |
| Matrix reads | `python CheckMatrixReads.py` | pass |
| Whitespace | `git diff --check` | clean |

Every commit in this branch was built and tested before the next one started, so a failure can be
attributed to a single commit.

## 6. Performance and memory

No GPU runtime measurements are available in this environment, so this report does not claim a
performance change. The structural effect is smaller code and data:

- the candidate pair lost a third of its size (8 B → 4 B) for the overflow composition;
- the publisher performs one cache implementation instead of two, with the same number of UID
  lookups;
- the alias API lost an array write per table build.

## 7. Documentation state

- `docs/dtal-optimization-spec.md` and `docs/cluster-lighting-spec.md` carry the actual checklist
  state and point at the implementation reports.
- `docs/dtal-cluster-optimization-plan.md` and `docs/dtal-cluster-uml.md` now describe the
  implementation as delivered, including the simplifications that were kept: three owners, no
  second handle registry, no GPU frame map, no publication service, no background build queue and
  no per-structure managers.

## 8. Remaining limitations

- The fast 128-slot ranking remains distance-based; power ranking is used for the overflow tail and
  the branch only.
- The overflow policy bypasses the incremental composition path; a changed light set recomposes.
- No runtime oversubscription benchmark, noise measurement or timing comparison was possible.
- The candidate demand/tail diagnostics are printed at report time but are not exposed in the
  Light Editor yet.
