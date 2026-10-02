# Project A Implementation Report: Static-World DTAL Grid Groups

## 1. Identity

| Item | Value |
|---|---|
| Branch | `feature/dtal-grid-groups` |
| Base commit | `d365d033` (merge of `feature/enhanced-models`, current `origin/master` at task start) |
| Documentation commit | `340a6559` |
| Estimator commit | `522b8d48` |
| Builder commit | `3811992b` (builder, alias utility, tests, CMake test target) |
| Renderer integration commit | `e9684a0c` (group light type, member buffer, API, RHI, shaders, host adapter) |
| Cluster coverage commit | `0e916862` (union coverage, editor controls, estimator reference tests) |
| Diagnostics commit | `b038291a` (group rebuild counter) |
| Integration fixes | the fix commits on this branch (`git log feature/dtal-grid-groups`): groups build on the world draw path, unchanged collections are reused, `LIGHT_TYPE_DTAL_GROUP` is accepted by the light-array switches, and switching the group mode off and on rebuilds instead of leaving the groups inactive |

All commits are on `feature/dtal-grid-groups`; nothing was merged, force-pushed or published.

## 2. Delivered scope

### 2.1 Corrected standard-world DTAL estimator (A0)

Two confirmed baseline defects were repaired for `LIGHT_TYPE_TEXTURED_AREA`:

1. **Truncated sampling domain.** `RtRaygenDirect`/`RtQ2Indirect` multiplied the point random pair by
   `0.99` and `Random.h::sampleTriangle` did so again, so the sampled domain of a DTAL polygon was
   `[0, 0.9801]^2` while `dw` compensated the full patch area. The call sites now pass a full-domain
   pair and use `sampleLightFullDomain`; the DTAL polygon sampler consumes it unchanged. Sphere, spot,
   sun and projector sources keep their exact previous effective domain through the wrapper and the
   unchanged `sampleDisk`/`sampleTriangle` internals.
2. **Area-dependent contribution clamp.** `sampleTexturedAreaLight` computed
   `dw = safeSolidAngle(emiss * area * G)` with a hard `4π` cap. The cap is a per-patch function of
   patch area, so the same surface divided differently produced different means near the emitter.
   The corrected path computes `dw = emiss * area * cosNL / max(dist, 1)^2` through
   `getTexturedAreaLightDw`: a documented world-space minimum distance of one Quake unit as the only
   regularization, no solid-angle cap, explicit non-finite guards. The projector path is intentionally
   unchanged and is covered separately.

The independent numerical reference lives in `tests/rt_lighting_tests.c`
(`TestEstimatorSubdivision`, `TestEstimatorSamplingDomain`). It demonstrates the baseline cap bias
(near field, whole patch `3.481887` vs split patches `4.904507`, uncapped integral `4.904507` at the
same resolution) and the full-domain sampling bias (`0.9801` domain shifts the mean of a linear
function by more than `1e-4`), while the corrected formulas are subdivision invariant to `1e-9`
relative.

### 2.2 CPU builder and cache (A1)

`Quake/rt_dtal_groups.{h,c}` is a plain, engine-free builder:

- input is one admitted emitting piece (affine UV map, oriented normal, actual area, emission/style
  keys, reference mask mean, radiant power);
- convex polygons are clipped to world-aligned grid cells with a half-open upper boundary
  (`p >= min`, `p < max`), world position and UV interpolated in double precision;
- every clipped convex piece is fan-triangulated; each triangle is a member with the source affine
  map, its own UV polygon and an area computed by a double-precision cross product;
- cells are visited over the piece AABB with a checked per-piece work guard
  (`RT_DTAL_MAX_CELL_VISITS_PER_PIECE = 4096`); a piece beyond the guard keeps its exact geometry as
  one oversized parent (diagnosed, never dropped);
- groups are keyed by `(cell, signed dominant normal axis, emission key, style key)`, with a
  deterministic singleton key for the diagnostic mode and for projector pieces;
- member tables are compacted per group, UIDs come from a checked 60-bit namespace over the complete
  key with deterministic collision resolution, and alias tables are built in double precision by
  `Quake/rt_alias.c` (`RT_Alias_Build`), which also serves Project B;
- member weights are `area * (0.05 + 0.95 * clamp(maskMean))`, so every retained patch keeps a
  positive selection floor;
- budgets are explicit: `RT_DTAL_MAX_GROUPS = 65536`, `RT_DTAL_MAX_MEMBERS = 262144`,
  `RT_DTAL_MAX_INPUTS = 262144`; a burst fails the build as a whole instead of publishing a partial
  generation.

`tests/rt_lighting_tests.c` (registered with CTest as `rt_lighting_tests`) checks area conservation,
negative cells, grid-plane ownership, deterministic rebuilds, singleton/grouped area parity, weighted
alias PMFs (including 1:1e12 dynamic range and 2048-entry tables), and the estimator reference.
Current result: `1408687 checks, 0 failures`.

### 2.3 Renderer integration (A2)

- New light type `LIGHT_TYPE_DTAL_GROUP = 6` with parent fields encoded in the existing
  `ShLightEncoded`, and a new immutable storage buffer `dtalMembers`
  (`ShDtalMember`, 144 B: per-patch `A/B/C`, normal, area, vertex count, UV polygon, alias primary
  probability, alias path probability, alias index).
- `QrDtalGroupUploadBatch` / `qrUploadDtalGroups` are wired through `qray.cpp`,
  `VulkanDevice`, `Scene` and `LightManager`; `LightManager` owns the member buffer, its per-frame
  staging, pending-copy flags and the descriptor at `BINDING_LIGHT_SOURCES_DTAL_MEMBERS = 9`.
  `RhiRtDirectPass` wraps, binds and copies the new buffer next to the existing five.
- Direct and indirect raygen (GLSL and HLSL) draw an independent member variate and compensate
  `p_source * p_member` exactly once; member samples are attributed to the selected parent fast slot
  for the existing dense statistics.
- Group parent color/material/cone/animation parameters are refreshed per frame from the canonical
  source texture and the cached style acceptance; geometry and member tables are untouched by
  ordinary frames. A frame whose animation changes the mask/projector classification is evaluated
  with the last compatible classification and requests a world recollect instead of silently using
  the new interpretation.
- The engine's world draw path (`R_DrawWorld`) collects the static emissive set directly, not
  through `RT_RecollectWorldEmissiveLights`. Both paths now run the same wrapper, which begins the
  builder input, collects, and rebuilds; the rebuild compares a hash of the collected inputs plus
  the grid policy with the installed generation, so the per-static-submit collection does not
  rebuild topology when nothing changed (the reuse counter is reported). This was a follow-up fix:
  the first delivered integration only wrapped `RT_RecollectWorldEmissiveLights` and therefore
  never activated the builder on the normal map-load path.
- `LightManager::AddDtalGroups` bypasses the brightness-based deletion threshold, so a dark/off
  group keeps its identity and evaluates to zero emission.
- Static-world admission still applies the existing `RT_FaceOwnedBySubmodel`, material, style and
  clearance policy before the piece reaches the builder; inline models and moving surfaces do not
  enter the cache.

### 2.4 Source-domain adapter

`RT_ClusterLightAddMulti` extends cluster registration with an optional mapped-cluster list, a
source bounds radius and the union coverage of a group. The host resolves member centers to mapped
clusters at build time (up to 16 distinct clusters per group; beyond that the build fails explicitly
and the per-piece lights stay). `ClusterLightLists::GrantSource` walks the PVS of every registered
cluster, deduplicates a source already holding a slot in a cluster, and widens the reach gate by the
source bounds radius. Group bounds are used for reach eligibility, not just the centroid.

### 2.5 Controls and diagnostics

- `rt_dtal_groups`: `0` one light per admitted piece (legacy path with the corrected sampler),
  `1` grid groups, `2` diagnostic singleton groups over the same patches and sampler.
- `rt_dtal_spacing`: validated grid edge, default `128` world units.
- Both are ordinary invalidating cvars (coalesced through the existing `rt_require_static_submit`
  boundary) and have Light Editor controls and benchmark-metadata entries.
- `rt_light_report` prints admitted pieces, parents, members, oversized pieces, budget refusals and
  coverage refusals.

## 3. Verification

| Check | Command | Result |
|---|---|---|
| Debug build | `.\build_win.ps1 -Config Debug -BuildDir build\Debug` | pass |
| Release build | `.\build_win.ps1 -Config Release -BuildDir build\Release` | pass |
| Shader build | `.\build_shaders.ps1 -DestDir build\Debug\id1\shaders` | 49 shaders deployed |
| Shader properties | `python CheckShaderProperties.py --rebuild` | pass (after the GLSL `sample` identifier fix) |
| Matrix reads | `python CheckMatrixReads.py` | pass |
| Numerical tests | `.\build\Debug\rt_lighting_tests.exe` | `1408687 checks, 0 failures` |
| Whitespace | `git diff --check` | clean |

Unavailable in this environment: scripted GPU captures, so image artifacts, raw noise, denoised
stability, frame timings, GPU memory and FPS measurements are **not** claimed. The numeric acceptance
above is CPU-only and structural/analytic; no runtime check was marked passed without running it.

### 3.1 Manual visual test (performed)

The build was run interactively on `e1m1`, `e1m7` and `e4m1` (registered Quake data, AMD RX 9070 XT,
Vulkan 1.4.349, Debug build). The first run found no visual regression when toggling
`rt_dtal_groups`, but it could not have: the reported group counters stayed at
`0 admitted pieces -> 0 parents ... 0 builds; inactive` while 269 static world lights were baked,
because `R_DrawWorld` collected the world directly and never entered the group wrapper. That run was
therefore a legacy-path test only, and it is not evidence for the grouped path.

After the integration fixes the grouped path was exercised on the stock maps:

| Scene | Grid | Admitted pieces | Parents | Member patches | Builds |
|---|---|---|---|---|---|
| `e1m7`, stable | 256 | 66 | 52 first build, 121 after material settle | 668 → 4674 | 3 → 9, then constant |
| `e1m7`, stable | 128 | 66 | 109 | 960 | 3, constant |
| `e4m1` | 256 | 269 | 358 (329 in an earlier state) | 5641 | 10, constant |

- The report line shows `active`, the group UIDs (type 4) are registered in the cluster lists and
  receive slots, and no assertion or drop was observed.
- After the initial material/animation settling the build counter stops growing and the reuse
  counter grows, so ordinary frames do not rebuild topology.
- Switching the mode off and on again (`rt_dtal_groups 0` then `1`) returns to `active`; the fix for
  that case is on this branch.

**Measured limitation:** on these two stock maps the parent count *increases* (66 → 109/121,
269 → 358). Grid clipping splits medium and large emitters across cells faster than compatible
pieces merge inside a cell. The reduction target is met only where many compatible pieces share
cells (heavily fragmented emissive meshes); it is not demonstrated on `e1m7`/`e4m1`, and
`rt_dtal_groups` therefore stays off by default. Screenshot pairs were taken with the denoiser off
and without a guaranteed identical camera, so they are a smoke check only (bright pair means 28.09
vs 27.04, dark pair 4.59 vs 3.99); they are not the required numerical reference and the grouped
mean-preservation reference remains the CPU test.
The on-screen checks to perform are listed in the Project B test protocol; the counters to read are
`dtal groups ... -> ... builds, ... reused collections; active`.

On `e4m1` the existing fast-list limit fired as expected (`RT: 22 clusters reached the 128 light
limit`) with the legacy selector — this is the cluster limitation Project A explicitly does not
repair and Project B owns.

Scripted screenshot capture (a config with `map`, frame `wait`s and `screenshot`) proved unreliable in
this environment: before the map finishes loading the frame rate is high, so a fixed `wait` count
elapses before the map is ready, and the map-loaded frame prints only after the capture sequence.
No automated image pair was obtained, and none is claimed. During that investigation an unrelated
pre-existing crash was found and is **not** a regression of this work: `viewpos` dereferences
`cl.entities[cl.viewentity]` (`Quake/cl_main.c:1243`) and faults with `0xC0000005` when called before
the client has a valid view entity; the report records it because the diagnostic was part of the
failed capture script.

## 4. Measurements that were obtained

- Debug and Release builds succeed from a clean CMake configure; the shader pipeline builds both
  languages from the unchanged sources plus the new `ShDtalMember` layout.
- Member record cost: `sizeof(ShDtalMember) = 144 B` (verified by a compile-time assert against the
  public upload record). Renderer budget `QR_DTAL_MAX_UPLOAD_MEMBERS = 131072` corresponds to
  18.0 MiB device-local plus 36.0 MiB staging at two frames in flight.
- Builder caps: 65536 parents and 262144 members; the fixture test reduces one 256×256 emissive
  rectangle at spacing 64 to 16 parents / 32 triangle members with exact area conservation.
- Alias tables: up to 2048 entries tested; induced PMF matches the normalized weights within `1e-4`
  with a 200000-draw empirical check at 5σ.

## 5. Deviations, limitations and unresolved items

1. **No runtime GPU validation.** The renderer changes compile and the shader contract is verified by
   the probe/checker tooling, but no frame was rendered on a GPU here. Resource replacement with
   frames in flight uses the existing pending-copy/retirement mechanisms and was not exercised.
2. **Conservative support bound.** The builder keeps the admitted geometry and UVs as the collector
   produced them; where the existing collector substituted a square for a face whose inverse UV fit
   is singular, the builder keeps that square. It never converts masked geometry to an unmasked
   square on its own. An affine map that collapses to zero world area is dropped with
   `droppedDegenerate`/`droppedArea` diagnostics rather than silently kept.
3. **Oversized pieces.** A piece whose AABB cell count exceeds the enumeration guard becomes one
   exact oversized parent instead of being clipped across its true cell set. This is reported
   (`oversizedPieces`) and keeps geometry/masks exact, at the cost of locality for pathological
   diagonal faces.
4. **Coverage sampling.** Group cluster coverage samples at most 64 member centers (stride) and
   refuses the build beyond 16 distinct clusters. This is conservative for the tested shapes but is
   not an exhaustive per-member domain bound.
5. **Projector cycles.** A piece whose canonical material is a projector is kept as its own
   singleton parent (identity mixed into the key), never merged into a grid group, matching the
   specification's passthrough rule.
6. **Estimator correction scope.** Only the standard non-projector DTAL sampler changed. Sphere,
   spot, sun and projector formulas are untouched; the projector path keeps the old `safeSolidAngle`
   compensation and should get its own audit before any change.
7. **Existing cluster limits remain visible.** Registration is still capped at `RT_CLUSTER_MAX_LIGHTS
   = 1024` and the fast list at 128 entries; Project A reduces parent demand but does not repair
   those limits. They are Project B's scope (next branch).
8. **Allocation ownership.** The host keeps builder-owned plain arrays and the renderer copies the
   member bytes into its own staging at upload; no borrowed builder pointer survives publication.
9. **Old `rt_wldlights_emissive` array.** It is still filled (up to its 8192 records) for fallback,
   diagnostics and comparisons, but the builder is fed before that cap, so grid grouping is not
   truncated by it.

## 6. Handoff to Project B

The source interface Project B consumes is stable:

- `QrClusterLightSource` now carries `radius`, `clusterCount` and `pClusters` alongside the existing
  UID/origin/reach fields.
- `ClusterLightLists` composes from the union of the supplied clusters and deduplicates a source
  that is already placed in a cluster.
- The fast list, registration cap and distance ranking are unchanged, as required for Project A's
  independent delivery; their repair is `feature/cluster-lighting-overflow`.
