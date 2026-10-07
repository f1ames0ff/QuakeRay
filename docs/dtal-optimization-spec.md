# Project A: Static-World DTAL Grid Groups

Status: implemented on `feature/dtal-grid-groups`; see
[the Project A implementation report](dtal-implementation-report.md) for the delivered scope,
checks, measurements and remaining limitations.

## 1. Purpose and delivery boundary

Implement spatially local DTAL groups that retain every accepted real emitting patch. Reduce independent parent-source count on highly fragmented emissive BSP geometry without moving emission, discarding luma patterns, or collapsing small lamps to representative points.

This project owns geometry generation, group/member sampling, group GPU data, animation parameters, and a minimal new-source adapter to the existing cluster system. Follow the [shared roadmap and interface](dtal-cluster-optimization-plan.md).

The first independently accepted version uses the existing cluster ranking, 1024-registration limit, and 128-slot selection policy. The [cluster project](cluster-lighting-spec.md) repairs those separately. Project A must report existing losses and validate preservation with complete-domain references or scenes that fit those limits. It does not claim that fewer parents alone remove every black/noisy region.

The user-tested `rt_restir` mode is not a reference. Direct global RIS changes variance, and indirect lighting still uses cluster lists.

## 2. Correctness contract

1. Keep real position, oriented normal, UV mapping, emission support and area for every admitted patch.
2. No brightness/area compensation on a representative polygon; no convex hull emitting across gaps.
3. Parent aggregation changes sampling organization, not expected emission.
4. Member and point PDFs have positive support wherever the retained domain may emit.
5. Grid spacing, source subdivision and animation do not modify the intended material behavior.
6. Ordinary frames update current emission parameters, not immutable geometry/member tables.
7. Dark frames preserve source identity and evaluate to zero emitted radiance.
8. Failed builds do not publish partial geometry; resource replacement is safe for outstanding frames.
9. Direct/indirect and GLSL/HLSL paths use the same estimator.

Keep these properties as assertions/tests. They are not optional simplifications.

## 3. Owners and records

Use a small world builder/cache, preferably `Quake/rt_dtal_groups.c` and `.h`, integrated with `r_world.c`. `LightManager` owns uploaded group storage and parent records. The existing frame and RHI mechanisms own publication timing and retirement.

Required plain-data records:

| Record | Fields/responsibility |
|---|---|
| `DtalBuildData` | Input key, material/map identity, one group generation, patches, groups, members, diagnostics |
| `DtalPatch` | Triangle positions/edges, UV interpolation, oriented normal, actual geometric area; CPU source attribution |
| `DtalGroup` | UID, compatible emission key, grid cell/facing key, member range, bounds, source-cluster coverage, reference importance |
| `DtalMember` | Patch index, alias threshold/index, actual normalized member probability |
| Current group parameters | Current mask texture, radiance, effective styles, cone state, effective sample reach and estimated power |

Current group parameters can use the parent light record and existing per-frame upload arrays. Emission compatibility is a key and helper logic, not a mandatory independent binding manager. Deduplication is an implementation optimization, not a fourth owner.

Patches live in separate geometry buffers and do not each occupy a primary-array record in grouped mode. A parent group uses one entry. Diagnostic singleton mode uses the same patches and sampler with one member per parent.

## 4. Source collection

### 4.1 Static admission

- Use the static-world eligibility policy of `RT_CollectWorldEmissiveLights` and `RT_FaceOwnedBySubmodel`; moving inline-model faces do not enter the world group cache.
- Resolve complete canonical animation cycles and effective emission/style compatibility before grouping.
- Preserve input admission separately from new grid fragmentation.
- The old `rt_wldlights_emissive[8192]` array is not a candidate sink for this builder. Build into checked owned arrays before publishing parents.
- Report every invalid input, explicit filter and rejected area. Do not report a truncated result as complete.

### 4.2 Exact patch geometry

Triangle patches avoid the legacy eight-vertex parent limit and whole-face affine-fit requirement:

1. Triangulate convex BSP polygons deterministically with the original boundary/winding.
2. Clip geometry to grid cells, interpolating world position and UVs.
3. Triangulate clipped convex pieces and compute area in CPU double precision.
4. Orient the emitting normal consistently with the source plane and `SURF_PLANEBACK`.

Valid world triangles with constant or one-dimensional UVs remain representable by interpolation. Do not convert them to unmasked squares merely because an inverse UV fit is singular.

Use scale-aware degeneracy tolerances and measure the area they remove. Patch interiors must not overlap through grid ownership; shared boundaries alone are allowed.

### 4.3 Grid rule

- World-aligned origin `(0,0,0)`; signed cell coordinates `floor(position / N)` including negative coordinates.
- Cell edge `N = rt_dtal_spacing` in Quake units. It is neither a strict pairwise separation nor literal one-light-per-cube for incompatible materials.
- Key: `(cell, compatible emission identity, signed dominant normal axis)`; X/Y/Z resolve equal-axis ties in that order.
- Do not key by BSP face index or representative leaf: those restore subdivision dependence.
- Real patch normals are always used for shading. A facing bucket is not a group-wide front-side test.
- Large source domains are clipped into cells; assigning a huge face by its centroid alone is insufficient.
- Use a deterministic half-open boundary convention to avoid duplicating geometry on grid planes.
- Visit surface-intersecting cells, not every cell in a large diagonal face's 3D AABB. Check cell-range arithmetic, scratch bytes, visited-cell count and final output size.

Geometry remains exact even when several disconnected patches share a group. Actual sample visibility is tested against scene geometry. Grouping is not permission to emit through the space between those patches.

### 4.4 Mask and animation support

The visible source and its sampled light use the same current luma/emission channel, wrapping and intensity conventions.

The baseline glow rectangle is derived with `RT_EMIS_GLOW_THRESHOLD = 0.02` in `gl_texmgr.c`. It is an importance hint, not proof that all texels outside it are zero. A support-clipping bound must include the GPU-visible quantized data, nonzero dim texels, wrap seams and filtering footprint. If such a bound is unavailable, retain the full source geometry.

For animation, retain the union of every frame's possible support, or the full domain as a conservative fallback. Do not choose geometry from only the first lit frame.

If any frame in a cycle is a projector, retain that cycle as an ordinary projector-capable passthrough source. Its aperture must not become several independent grid projectors. Material changes affecting this classification require rebuilding before the changed interpretation is used.

### 4.5 Input controls and clearance

- `rt_dtal_minarea` is an explicit user filter applied before grid clipping to logical source pieces. Default zero is used for invariance tests. Report the nonzero setting's intentional effect and the actual piece definition used.
- `rt_dtal_maxpolys` does not truncate the corrected static-world builder's accepted mask regions. Retain its old meaning only for paths that remain legacy and explain this in UI/docs.
- Keep a tile-enumeration guard. Beyond it, retain exact source geometry with its mask instead of enumerating unbounded texture repetitions or dropping them.
- Existing center clearance tests are approximations. Cache admission before grouping; never reject a whole group because its centroid is buried.
- Check subdivision invariance with clearance off first. If center-based admission fails separate clearance tests, use a conservative domain policy or retain uncertain geometry. Do not label it exact occlusion.

## 5. Emission compatibility and ordinary frames

A compatibility key includes texture/animation-cycle identity, mask interpretation, actual color/intensity policy, effective accepted styles, and cone/projector policy. `QR_NO_MATERIAL` alone is not a texture identity for unmasked sources.

The current style-acceptance policy can depend on position and reach. Equal raw style IDs are not sufficient if effective modulation differs. Different behavior requires a different key; identical behavior should merge across faces.

At each ordinary frame:

1. Resolve the current texture frame and group emission/style parameters through existing helpers.
2. Update the parent record and estimated power, with intensity applied once.
3. Keep group membership, patch storage, member probabilities and UID fixed.

An off frame keeps its parent record. Proposal floors belong in selection probability, not in radiance. The new group record must bypass brightness-based deletion and evaluate black without forcing a faint glowing color.

Rebuild only for map changes, explicit rebuild, spacing, source admission, support/classification, or relevant compatibility changes. Radiance-only edits can reuse geometry when that remains semantically safe. Camera motion and animation-frame advance never trigger topology reconstruction.

## 6. Member and point sampling

### 6.1 Formula and interface

For selected parent `g`, member patch `i`, and point `y`:

`estimate = F_i(x,y) / (p_source(g|x) * p_member(i|g) * p_area(y|i))`.

`F_i` includes actual current emission/mask, intentional cone behavior, BRDF, geometry, visibility and reach eligibility. Uniform triangle sampling gives `p_area = 1/A_i`.

Preserve one shader convention:

- `LightSample.dw = emitterGeometryFactor / p_area`.
- The caller shades with `1/(p_source * p_member)`.
- Ordinary lights and singleton parents have `p_member = 1`.

Parent selection is opaque to A. When B is absent, `p_source` comes from the existing selector. When B is present it includes its branch probability. Global RIS retains its existing path-specific normalization and adds the member compensation once; it is not reinterpreted as a new exact marginal PDF.

Use independent random dimensions for parent, member and point draws. A black/occluded/out-of-domain sample returns zero without retry.

### 6.2 Member distribution

- Uniform-radiance/unmasked groups: `p_i = A_i / sum(A)`.
- Masked groups: reference estimate proportional to `A_i * estimatedMeanMask_i`, mixed with a positive support-preserving fallback over every retained patch.
- Estimates can include the whole animation cycle. Never remove a region because the reference frame is dark.
- Build alias tables in double precision, publish the actual normalized GPU probabilities, and test the quantized distribution.
- Use enough categorical random bits that large groups and unequal areas remain reachable; do not blindly reuse 16-bit draws for every table size.

One small reusable alias-table helper is sufficient. Per-sample member selection must not scan every patch.

### 6.3 Point sampling and variance

Start with full-domain uniform-area points within retained patches and actual shader luma lookup. Keep support clipping conservative.

If sparse-mask noise fails the measured quality gate, add bounded within-patch importance cells with explicit known area and actual point density. This is a measured follow-up inside A, not a mandatory general importance-sampling framework.

`getTalCdfUv` by itself does not return the full density for clipped/repeated domains. It must not be connected to a uniform-area compensation unchanged.

### 6.4 Estimator repair

Before comparing aggregation, check the baseline `safeSolidAngle(area * G)` contribution clamp. Its area dependence can make a surface's mean vary with subdivision.

- Demonstrate confirmed bias with a numerical reference, including near-field receivers.
- Correct the standard DTAL path to compensate the actual sample density with finite-value guards.
- Any near-zero-distance regularization must be a documented world-space geometric rule, not a patch/group-area or PDF-dependent clamp.
- Do not replace the old cap with a hidden final-radiance/firefly clamp in reference output.
- Remove the truncated sampling domain from the new path: direct/indirect call sites and `Random.h`/`.hlsli::sampleTriangle` multiply random inputs by `0.99` in the baseline. Provide a full-domain DTAL sampler without changing unrelated source types incidentally.

Grouped and singleton comparisons use the same corrected estimator. Existing sphere/sun/projector behavior gets separate tests if any shared helper change affects it.

## 7. Minimal adapter to existing source selection

### 7.1 Group registration

Register one parent UID per group and publish its bounds, effective reach, power estimate, and union of possible mapped source clusters. Extend source-type encoding and group importance proxies in both shader languages.

- The existing composer admits a group through the union of coverage, deduplicating the UID for each receiver.
- Use bounds for group reach eligibility; do not rely on a centroid inside solid geometry.
- Keep distance ranking and existing list/capacity behavior for independent A delivery.
- New group importance evaluation must not assign zero from an average normal, center cone, or zero approximate mask mean when a real member can contribute.
- Extend existing light-type checks for valid fast-slot visibility statistics to the group type. Attribute a member sample to its selected parent slot; do not create dense statistics for every patch.
- This adapter is minimal new-source support, not Project B's complete candidate/overflow machinery.

Ambiguous coverage requires a conservative set or an explicit build failure. Publishing an accepted group as unresolved and silently erasing every member is invalid.

### 7.2 Group-independent finite reach

For corrected standard world DTAL, a point contributes only when `length(x-y) <= R`, using `RT_ClusterLightReachStatic`'s conversion and zero-setting fallback. Apply this identically in grouped/singleton and direct/indirect mode; an out-of-reach draw contributes zero without retry.

Candidate bounds are a conservative broad phase. Do not shorten group source reach to a smaller `topUpReach` while its evaluator uses `R`. The adapter must preserve that domain for groups; policy/ranking changes for other sources belong to B.

This is an explicit change from center-to-cluster-only legacy reach. Record it separately from grouping in comparisons. Reach changes update parameters/coverage, not member geometry.

### 7.3 Existing limits

Report input and parent counts, registry rejects, full lists and source grants on A's reproduction. A can pass independent correctness tests only with complete-domain diagnostic selection or a declared scene that fits existing registration/list limits.

If a singleton test exceeds those limits, use the CPU/numerical reference or a bounded fixture. Do not quietly discard sources, auto-grow spacing, or introduce B's capacity rewrite to make the comparison pass.

## 8. Build, publication and lifetime

Keep rebuild work at the existing safe world preparation boundary. Cvar/editor callbacks only mark invalidation; coalesce slider changes.

Initial implementation:

1. Synchronize CPU material/input mutation using existing frame ordering or a narrowly scoped input capture.
2. Capture settings/material identity and the pending request revision.
3. Build into temporary owned arrays and validate geometry, PMFs, counts and budgets.
4. Check that relevant inputs still match before installing.
5. Replace the complete group-data generation and update `LightManager`'s existing resource/copy/descriptor plumbing.
6. Keep later invalidations pending; do not reset a request that the build did not process.

No background service or separately managed frame bundle is required. Frame slots still must reference compatible complete geometry and current parameters. Retain native storage and NVRHI wrappers until their actual last GPU use, using existing owners and `RhiFrameContext` integration; a CPU task wait alone is insufficient.

An old build can temporarily remain only if compatible with current source/material support. Same-map identity alone does not justify combining old clipped geometry with a newly emitting mask. Failure must be explicit; retries must be bounded.

Group IDs come from a checked group namespace and deterministic complete key, not a small unchecked Morton hash. Unchanged groups retain identity. Singleton test parents have a distinct namespace. Conditional member-domain changes invalidate affected visibility/replay history through existing mechanisms rather than requiring a new universal generation framework.

## 9. GPU and API integration

Add one group-parent light type and immutable patch/member buffers. Keep current group emission parameters in parent records or a small per-frame array owned by `LightManager`.

Wire upload through `qray.h`, `qray.cpp`, `VulkanDevice`, `Scene`, and existing `LightManager` ownership. Copy or own API arrays; no borrowed builder pointers may survive publication.

The generated patch layout can use aligned position/edges, UV deltas, normal, and actual area. Define it in `GenerateShaderCommon.py` and verify C/GLSL/HLSL offsets and strides.

Update native wrapping sizes, descriptors, copy flags and barriers in `RhiRtDirectPass` and all light-buffer consumers, including indirect shaders. Do not create separate GPU buffers merely for conceptual UML boxes.

Keep UID-to-current-index resolution in existing publication. A does not add another stable handle registry or GPU index-map buffer.

## 10. Implementation steps

### A0. Reference and inventory

- [ ] Record baseline assets, settings, cameras, hardware and available source/list diagnostics.
      (Settings, defaults and diagnostics are recorded in
      `dtal-implementation-report.md`; no GPU runtime capture was possible here.)
- [x] Inventory affected light switches, shader consumers and resources.
- [x] Register numerical geometry/sampling tests explicitly; the inspected project has no configured general CTest suite.
- [x] Check and repair confirmed standard-DTAL subdivision/domain bias with grouping off.

### A1. CPU grouping

- [x] Implement static source admission, projector-cycle handling and conservative support.
- [x] Implement grid-local exact patches and deterministic compatible groups.
- [x] Build aliases, bounds, UIDs and coverage; enforce checked work/memory budgets.
- [x] Validate area conservation, PMFs, cache invalidation and failure behavior.
      (CPU tests; the runtime invalidation path is implemented but not exercised on a GPU.)

### A2. Renderer integration

- [x] Add group parent and buffers through existing owners/API.
- [x] Implement conditional member/point sampling in direct/indirect GLSL/HLSL.
- [x] Add the minimal group-domain adapter and supported group importance proxy.
- [x] Preserve exact dark emission and current animation parameters without geometry rebuild.
- [ ] Verify resource replacement with frames in flight.

### A3. Controls, measurement and acceptance

- [x] Add global Light Editor controls and light-only coalesced rebuild requests.
- [ ] Add group/member debug views and counts/time/memory/upload diagnostics.
      (Parent/member/budget/coverage counts and the member memory budget are reported;
      no per-group/member debug view was added and no GPU timing was measured.)
- [ ] Compare corrected singleton/grouped modes and measure sparse masks.
- [ ] Test A with the old cluster policy within its declared limits.
- [x] Report completed scope, settings/defaults, unavailable checks and remaining cluster limitations.

Post-review state: an independent review found that the collected batch accumulated across
collections, cached groups lost their `active` state, group bounds could under-report member
vertices and an incomplete collection could publish a partial generation. The branch history now
carries the repairs (documented in `dtal-implementation-report.md` §3.2), including the new numeric
tests that pin the batch reset and the conservative bounds. The member-sampling alias bias that this
branch still carries is corrected on `feature/cluster-lighting-overflow`; the standalone Project A
branch is not a numerical reference for member sampling.

## 11. File map

| Files | A's work |
|---|---|
| `Quake/rt_dtal_groups.c`, `.h` (new), CMake | Plain builder/cache and tests |
| `Quake/r_world.c`, `glquake.h` | Static collector, current emission helpers, source-domain adapter |
| `Quake/gl_texmgr.c`, `.h` if needed | Conservative support and estimate metadata |
| `Quake/gl_vidsdl.c`, `qr_editor.c`, `rt_dtal_debug.*` | Settings, coalescing, diagnostic views |
| `Quake/gl_rlight.c` only for group adapter | Group bounds/coverage registration and reporting; capacity/ranking overhaul is B |
| `qray.h`, `qray.cpp`, `VulkanDevice.*`, `Scene.*`, `LightManager.*` | New source type, buffers, parameters and existing publication |
| `ClusterLightLists.*` only for group-domain adapter | Bounds/multi-cluster eligibility; no fast/tail redesign |
| Generator, common resource declarations, GLSL/HLSL `Light`, `Random`, affected raygens/probes | Correct sampler, new type, probability factors and layout |
| RHI light consumers, existing resource lifetime code | Actual binding/copy sizes and safe replacement |

Shared files are not permission to bundle B's unrelated changes into A.

## 12. Tests and acceptance

Required fixtures:

- Same rectangle as 1, 16, 64 and irregularly sized pieces; identical geometry/UV/material.
- Receivers far away, near the surface, at edges, behind the source, and at a reach boundary cutting through a group.
- Uniform, sparse, EXIT-like, below-0.02 and wrap-seam masks, plus moving animated support and dark/lit frames.
- Small neighboring lamps, opposite sides, parallel floors, thin walls, long emitters and multiple/folded source clusters.
- Negative coordinates, grid-plane ownership, diagonal large faces, degenerate UV and very unequal patch areas.
- Dirty edits during rebuild, map reload, support-changing material edit and old GPU work still using replaced buffers.

Structural acceptance:

- Admitted area equals patch-area sum within a declared scale-aware tolerance; start investigation at `1e-5` relative plus an absolute floor.
- No duplicated patch interiors, invalid indices or inaccessible nonzero-support patches.
- Alias PMFs match the actual quantized draw and normalize.
- Grouped/singleton mode shares the same admitted geometry and reach policy.
- Repeated identical builds preserve keys/member order/UIDs.
- Animation/camera-only sequences produce zero topology rebuilds and zero static geometry reuploads.

Reference acceptance uses linear output before denoising/exposure/tonemapping. Compare direct and indirect separately. Predeclare receivers; require `abs(testMean-referenceMean) + 3*combinedStandardError <= max(1% of reference, documented dark-region tolerance)`. Uncertainty too large to decide is inconclusive, not a pass. Use exhaustive/independent integration, not `rt_restir` or a compile-only shader probe.

Measure parent-count reduction, source/list losses under the unchanged policy, raw noise/denoised stability, rebuild/steady-frame CPU/GPU costs, scratch/persistent bytes and uploaded bytes for spacing 64/128/256. Set the parent-reduction target from actual baseline evidence; do not promise an FPS multiplier.

## 13. Controls and handoff

| Control | Meaning | Development starting value |
|---|---|---|
| `rt_dtal_groups` | Singleton/group aggregation over the same corrected patch sampler | Off until bounded comparison tests pass |
| `rt_dtal_spacing` | Validated grid cell edge in world units | 128, provisional |
| `rt_dtal_rebuild` | Explicit regenerate command, extended | Existing command |
| `rt_dtal_debug` | Keep current modes; add group/member views | Off |

Validate nonfinite/zero/negative/extreme spacing before division or integer conversion. Final release defaults require measurements. Do not adapt spacing automatically to cluster pressure.

Follow shared build commands and leave A's implementation report with actual formulas, source-domain semantics, file changes, checks, captures, metrics and the still-existing cluster limits. Review UML D02/D03/D05/D06 as views of this small implementation, not as instructions to create extra services.
