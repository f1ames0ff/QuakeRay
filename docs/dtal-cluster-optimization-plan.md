# DTAL and Cluster Optimization: Two-Project Roadmap

## 1. Status and authoritative documents

- Status: both projects are implemented on their branches; the delivered scope, checks, measured
  results and remaining limitations are in [the Project A report](dtal-implementation-report.md)
  and [the Project B report](cluster-implementation-report.md), and the final simplifications are
  in [the refactoring report](lighting-refactoring-report.md).
- Code baseline inspected: `dac6cdc4`; implementation started from `d365d033` (the then-current
  `origin/master`).
- Primary reproduction: fragmented static-world emissive geometry, especially lava floors and glowing walls.
- Repository documentation and artifacts must be written in English.

The work is split into two independently implementable and testable projects:

1. **[Project A: DTAL grid groups](dtal-optimization-spec.md)** reduces the number of independently registered area-light sources while preserving their actual emitting geometry, UVs, masks, and animation.
2. **[Project B: cluster source selection](cluster-lighting-spec.md)** repairs source acceptance, ranking, and sampling under cluster-list overflow. It operates on ordinary lights and opaque DTAL-group parents through one small source interface.

The previous combined implementation mandate is replaced by these two specifications. This file is the shared boundary and integration roadmap, not a third implementation project. The **[UML model](dtal-cluster-uml.md)** explains ownership and interactions without prescribing a new framework.

## 2. What must remain correct

Keep the following invariants even when simplifying implementation:

1. Every accepted emitting region remains represented at its actual position, with its actual UV mapping and emitting side.
2. Grouping does not transfer several patches' emission to one representative polygon, multiply its brightness, or enlarge only its transmitted area.
3. Parent, member, and point-selection probabilities are accounted exactly once.
4. Equivalent surface subdivisions converge to the same mean lighting under the same admission, visibility, and reach policy.
5. Geometry and member distributions are rebuilt only on DTAL regeneration or relevant invalidation, not on camera movement or animation-frame advance.
6. A source selected by a frame must refer to that frame's correct light record and compatible geometry/material data.
7. Resource replacement and slot-history updates respect outstanding GPU work.
8. Small lamps, separate emitting sides, and isolated luma regions remain present, even if their registry entries are grouped.
9. Both GLSL/HLSL and direct/indirect consumers implement the same contract.
10. Capacity rejection, incomplete coverage, invalid geometry, and intentional input filtering are observable.
11. Project B gives every accepted cluster candidate positive sampling support when it can contribute, including candidates outside the 128-entry fast list.

Invariant 11 is Project B's responsibility. Project A's mathematical preservation is evaluated with complete source selection or scenes that fit the existing cluster limits; it must not claim that grouping alone repairs the old truncation behavior.

## 3. Baseline facts and user evidence

The user already tested `rt_restir`: shadows flickered, denoising looked worse, and uneven lighting remained. Treat that as supplied evidence. It is not an acceptance reference or proof that one specific subsystem is the sole cause.

| Verified baseline | Code location | Consequence |
|---|---|---|
| World collection is event-driven | `Quake/gl_rmain.c`, `R_DrawWorldTask` | Use the existing light-only recollect boundary. |
| Static collection can emit glow repetitions and split polygons | `Quake/r_world.c`, `RT_AddEmissiveLight`, `RT_EmissiveGlowPolygons`, `RT_SplitUvPolygon` | Project A must work across BSP faces, including masked light. |
| Static world storage is capped at 8192 records | `Quake/r_world.c`, `MAX_WORLDLIGHTS_COUNT` | New patches must not be truncated by that final-record array. |
| Cluster registration is capped at 1024 | `Quake/gl_rlight.c`, `RT_ClusterLightAdd` | Project B repairs source acceptance and capacity coherence. |
| Renderer light storage has 4096 entries with a reserved regular-light offset | `renderer/Source/LightManager.h`, `LightManager.cpp` | Source budgets include reserved entries and every light class. |
| Full cluster lists evict by distance | `renderer/Source/ClusterLightLists.cpp`, `AppendSlot`, `TopUpCluster` | Power-aware ranking is new work, not an active historical feature. |
| Cluster sampling reads at most the published fast list | `Shaders/Q2LightLists.h` and `.hlsli` | Project B adds overflow support rather than merely changing ranking. |
| DTAL position and area are independent fields | `Shaders/Light.h` and `.hlsli`, `sampleTexturedAreaLight` | Increasing a representative's area does not preserve the emitting geometry. |
| Standard DTAL contribution uses an area-dependent cap | `Light.hlsli`, `safeSolidAngle`, `sampleTexturedAreaLight` | Project A includes a numerical audit and correction of confirmed subdivision bias. |
| UID-to-current-index conversion already exists | `LightManager.cpp`, `SetClusterLightLists`, `FindRegisteredLight` | Extend this publication path; a second stable-handle/GPU-remapping framework is not required. |
| RHI resource retirement already exists | `RHI/RhiFrameContext.h`, `Retire`, `WaitForIdle` | Extend existing owners and synchronization, including native-buffer lifetime. |

Line numbers can drift; locate by symbol. Recheck these facts when implementation starts on another commit.

## 4. Ownership and the small shared interface

### 4.1 Three owners

| Owner | Responsibility |
|---|---|
| World DTAL builder/cache | Accepted source geometry, grid groups, member tables, immutable CPU build result |
| Existing `LightManager` | Group GPU storage, current parent light records, UID resolution, copies/descriptors/lifetime integration |
| Existing `ClusterLightLists` | Receiver candidate sets, source ranking, fast/overflow distributions and affected-cluster updates |

Emission binding, group, member, and source-domain records are ordinary data structures inside these owners. They do not each require a manager, scheduler, event bus, or independent publication service.

### 4.2 Source domain passed to clustering

A parent source exposes:

- Stable `uniqueID`.
- Origin and spatial bounds.
- Effective source reach and a flag identifying the reach policy.
- A sorted, deduplicated range of mapped source clusters, or the existing one-cluster form for ordinary sources.
- A finite estimated power and optional conservative directional information.

The cluster system sees those fields. It does not inspect a group's patches, UVs, luma pixels, or member aliases.

Project A supplies the minimal adapter for this extended domain to the existing composer: group coverage is the union of its members' possible source domains, and broad-phase reach uses bounds. This adapter is part of accepting a new source type; it does not introduce Project B's ranking, global-capacity overhaul, or overflow distribution.

The host can traverse BSP geometry and publish mapped source-cluster IDs plus the existing mapping revision. The renderer unions its authoritative PVS rows. Do not register one group through only its representative leaf, nor submit several contradictory registrations of the same UID as a substitute for coverage.

### 4.3 Sampling boundary

The source selector returns:

- Current parent-array index.
- `p_source`, or the existing path-specific RIS normalization.
- Fast-slot attribution when applicable; an explicit invalid slot for overflow selection.

The light sampler returns:

- Actual sampled position, normal/geometry information, color/mask, and `LightSample.dw`.
- Conditional `p_member`, which is one for ordinary lights and singleton comparison parents.

For ordinary cluster sampling:

`shade factor = 1 / (p_source * p_member)`.

`dw` contains point-density compensation exactly once. Project A owns member/point sampling. Project B owns source selection, including fast/overflow branch probability. The interface is the same when only one project is enabled.

## 5. Simplicity rules

Use existing machinery first:

- Extend the current UID registry and UID-to-index list publication for overflow entries; CPU distributions keep UIDs, GPU publications contain current array indices.
- Resolve array-order changes by refreshing the published indices when required. Measure that cost before adding another GPU indirection layer.
- Rebuild world data synchronously in the existing safe preparation boundary, into temporary owned arrays. Validate before replacement.
- Use a simple requested/processed revision or equivalent serialized invalidation rule. Do not lose a request arriving during a build.
- Reuse existing material revision, group-data generation, list generation, and frame-slot bookkeeping. Add a new counter only for a demonstrated independent lifetime.
- Reuse `RhiFrameContext` and current resource owners. CPU render-task completion is not GPU completion.
- Recompute an affected cluster's distributions as a whole initially. Keep unaffected clusters and immutable world-member tables cached.
- Use one small alias-table builder for member and overflow distributions.
- Add within-patch mask-importance tables only if sparse-mask measurements justify them; their PDFs must be derived correctly.

The frame must still consume compatible data, but a new `LightingFrameBundle` class, `FrameSourceMap` GPU buffer, nine-version tuple, multi-snapshot service, and background build queue are not prescribed. A synchronous implementation with existing frame slots is the initial design.

If rebuilding causes a measured unacceptable hitch, asynchronous generation can become a separate follow-up with its own lifetime design. Do not build that infrastructure in anticipation.

## 6. Independent delivery and order

Recommended order for this task:

1. Implement and accept Project A with the existing cluster policy, using bounded scenes and an independent complete-domain reference.
2. Implement and accept Project B with ordinary sources and the unchanged Project A source adapter.
3. Run combined integration tests and tune release settings.

Project B may also be implemented first against ordinary sources. Neither project's numerical tests or core algorithms require the other project's completion. The minimal new-source adapter is the integration seam, not a request to implement both projects together.

Use separate commits or pull requests and separate benchmark reports. Do not combine renderer-wide capacity or fast/tail changes into a DTAL grouping commit.

| Case | Purpose | Acceptance interpretation |
|---|---|---|
| A off, B off | Legacy cluster policy with a documented corrected/unmodified DTAL estimator | Historical comparison, not unquestioned ground truth |
| A on, B off | DTAL parent-count reduction and member sampling | Proves A within declared existing source/list limits; old cluster losses remain visible |
| A off, B on | Cluster correctness with ordinary or singleton sources | Proves B independently of grouping |
| A on, B on | Complete end-to-end behavior | Verifies independent probabilities, coverage, history, and actual performance |

Within Project A, compare grouped and singleton parents over exactly the same admitted patches and corrected estimator. If a singleton reproduction exceeds the old renderer limits, use the numerical/diagnostic reference rather than silently truncating it or pulling capacity work into A.

The cluster development switch selects legacy fast-only versus repaired source selection independently of `rt_dtal_groups`. It need not restore known registration bugs when disabled. Preserve the original executable for historical comparisons and distinguish source-selection policy from source-capacity behavior in results.

## 7. Integration acceptance

After independent acceptance, verify:

1. `p_source * p_member * p_area` describes the actual combined selection path, including null outcomes and fast/overflow branch probability.
2. Direct and indirect lighting agree on geometry, emission, reach and source-index publication.
3. Dynamic parent insertion/removal does not cause an overflow entry to point to a different UID.
4. Multiple leaves, thin walls, long emitters, and folded-map clusters preserve source coverage.
5. Animation and lightstyles update radiance without rebuilding member distributions.
6. Fast-slot replacement does not apply old visibility history to a new source; in-flight resources remain valid.
7. Reference tests remain invariant under BSP subdivision and group spacing, apart from explicit measured variance and input-filter policy.
8. Source/list pressure, raw noise, denoised stability, CPU/GPU time, rebuild time, and memory are reported for all four combinations.

The existing pointwise standard-world DTAL reach proposal replaces center-only reach for the new representation and must be declared in A's report. B consumes that reach domain conservatively; it must not redefine the DTAL evaluator's cutoff.

## 8. Shared tooling and handoff

The current build/check entry points are:

```powershell
.\build_win.ps1 -Config Debug -BuildDir build\Debug
.\build_win.ps1 -Config Release -BuildDir build\Release
.\build_shaders.ps1 -GenCommon -Rebuild -DestDir build\Debug\id1\shaders
git diff --check
```

Run from `renderer/Source/Shaders`:

```powershell
python CheckShaderProperties.py --rebuild
python CheckMatrixReads.py
```

Edit `GenerateShaderCommon.py` and regenerate layouts/bindings; do not hand-edit generated outputs. Update matching GLSL/HLSL consumers and RHI buffer sizes/descriptors. The inspected CMake files do not configure a general numerical test suite: register the required test targets explicitly and document their real commands.

Each project leaves its own English implementation report: actual scope, interface changes, formulas, build/test results, captures, timing/memory data, limitations and selected defaults. Independent multi-agent review is not required. No report may claim rendering or numerical correctness from UML syntax checks alone.

## 9. UML views

The [UML companion](dtal-cluster-uml.md) divides the existing views by ownership:

| Diagram | Subject | Owner/project |
|---|---|---|
| D01 | Two-project boundary and existing integration points | Shared roadmap |
| D02 | DTAL group data and storage | A |
| D03 | Synchronous regeneration | A |
| D04 | Existing frame preparation and UID publication | Shared integration |
| D05 | Small group-data lifetime model | A and existing RHI |
| D06 | Sampling boundary and conditional member selection | A, with B supplying `p_source` |
| D07 | Candidate preservation and fast/overflow selection | B |

These are views of three owners, not seven new runtime subsystems.
