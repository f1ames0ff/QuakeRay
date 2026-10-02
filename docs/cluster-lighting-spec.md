# Project B: Cluster Source Selection and Overflow

Status: implemented on `feature/cluster-lighting-overflow`; see
[the Project B implementation report](cluster-implementation-report.md) for the delivered scope,
checks, measurements and remaining limitations.

## 1. Purpose and independence

Repair cluster-source acceptance and source selection when local candidate counts exceed the fast-list capacity. Improve the usefulness of the fast list while keeping every accepted candidate sampleable through an overflow distribution.

Implement against ordinary lights first. DTAL groups are opaque parent sources exposed through the [shared source interface](dtal-cluster-optimization-plan.md); their member geometry and mask sampling belong to [Project A](dtal-optimization-spec.md).

B does not construct grids, clip emissive geometry, edit member probabilities, or change DTAL radiance to compensate for a rejected source. It can be implemented before or after A and gets its own report and commits.

## 2. Core invariants

For each receiving cluster `c`, form the accepted candidate set `C_c` under explicit visibility/reach policy, then partition:

`H_c union T_c = C_c`, `H_c intersection T_c = empty`, `size(H_c) <= 128`.

- `H_c` is the fast set; `T_c` contains every other accepted source.
- Ranking affects efficiency, not source existence.
- Source probabilities include every categorical stage exactly once.
- An uploaded accepted source has a valid renderer record and published index.
- Full and incremental composition preserve equivalent candidate support.
- Slot statistics never describe another source after replacement.
- Publication and resource replacement remain compatible with existing frames in flight.
- Every intentional policy rejection, unresolved domain and capacity failure is distinguishable from fast-list overflow.

## 3. Existing owners and baseline

Use existing `RT_ClusterLightAdd`/frame registration, `LightManager`, `ClusterLightLists`, and RHI publication/lifetime mechanisms.

Relevant baseline issues:

1. Host registration accepts at most 1024 sources even when renderer storage can accept more.
2. Renderer storage is 4096 entries including reserved positions; world DTAL collection has its own 8192 cap.
3. Filled fast lists and top-up retention use distance, not power.
4. Only listed lights are selectable by the cluster shader; shader weighting cannot recover an absent source.
5. UID-to-current-index conversion and order-change detection already exist in `LightManager::SetClusterLightLists`.
6. Some visibility/denial diagnostics exist, but source evictions and demand before truncation need clearer reporting.

Do not add a new handle registry, frame-map buffer, publication manager or resource-retirement service solely for B.

## 4. Coherent source acceptance

### 4.1 Capacity and deduplication

- Deduplicate UID updates before checking new-entry capacity.
- Make host registration cover the parents actually accepted by the renderer; remove independent 1024 truncation.
- Account for reserved array positions and ordinary, dynamic, projector and group parents together.
- Use checked configurable/growing storage where required, updating registry/hash sizing, previous-index buffers and RHI native wrap sizes together.
- A declared resource-budget failure must be explicit before incomplete state is published. Do not discard a tail or choose sources by producer arrival order while claiming full support.
- Patches within a DTAL group do not consume parent capacity.

If increasing parent capacity requires API additions, follow existing API-version policy and rebuild host/renderer together.

### 4.2 Producers and concurrency

Existing entity draw tasks can run in parallel. Audit shared host/renderer source insertion. Use task-owned staging and deterministic merge after producers complete, or measured correct synchronization through existing owners. An unsynchronized vector or per-frame shared counter is not made safe by enlarging it.

The accepted set used for candidate publication must match the frame's actual parent records. An absent record is a coherence problem, not a silently ignored alias outcome.

## 5. Candidate policy and source-domain adapter

### 5.1 Build complete candidates before ranking

Inputs are generic parent metadata: UID, origin/bounds, effective reach policy, estimated power and source-cluster coverage.

- Reuse renderer-owned authoritative PVS composition.
- Ordinary sources can use the existing single-cluster domain; groups supply a conservative mapped-cluster range through A's adapter.
- Union group visibility and use bounds-aware reach; do not inspect member aliases or infer emission from a group-center normal.
- Source and receiver cluster mapping must use the actual folded-map policy. Fix diagnostics that query a raw leaf as though it were an already mapped cluster.
- Top-up admission is a documented supplemental policy. Its old nearest-eight retention cap must not delete accepted overflow candidates.
- A standard-world DTAL's sample reach cannot be shortened to a smaller top-up search radius while its evaluator still uses the longer reach.

Define admission and report its rejections before fast-list selection. A's pointwise standard-world reach rule remains A's evaluator contract; B performs a conservative broad phase for it. Ordinary lights retain explicitly documented existing source-domain semantics unless a separate policy change is justified and tested.

### 5.2 Initially simple updates

- Cache static source-domain eligibility.
- On dynamic movement/addition/removal, identify affected receiver clusters and rebuild their candidate/distribution data as a whole.
- Reuse unaffected clusters and world geometry.
- Map/visibility/reach-policy changes invalidate the affected eligibility domain.
- Recompute cached proposals on a defined bounded schedule or actual source changes. Stale weights can be used only with their actual stored probabilities and preserved support.

Do not start with a per-entry incremental alias repair algorithm. Whole affected-cluster reconstruction is the default until profiling establishes a need for finer updates.

## 6. Fast-set ranking

Use a finite size-aware expected-contribution estimate such as:

`score = estimatedPower / max(distanceToSourceBoundsSquared, sourceScaleSquared, distanceFloorSquared)`.

Requirements:

- Derive power for each source encoding correctly: sphere/triangle and textured-area input colors have different area conventions.
- Group power comes from its aggregate metadata, not one member's color/area.
- UID resolves equal-score ties deterministically.
- Preserve a small nearest-source reserve; evaluate 16 nearest plus contribution-ranked remaining slots initially.
- Avoid false zero from average-normal or center-cone proxies.
- Add bounded hysteresis only if slot churn is measured; a provisional 20% exchange threshold is not a correctness constant.

The nearest reserve and hysteresis are compact ranking policies inside `ClusterLightLists`, not independent managers. Whatever ranking is selected, sources outside H remain in T.

## 7. Source-selection mathematics

### 7.1 Fast/overflow branch

When both sets exist, choose overflow with `beta` and fast with `1-beta`. Start measurements with a power-ratio estimate clamped to `[0.1,0.9]`.

- Empty T: `beta=0`.
- Empty H: `beta=1`.
- Both nonempty with zero estimated total power: `beta=0.5` and supported fallback proposals.
- Both empty: return a zero sample.

For disjoint sets:

`p_source(i) = (1-beta) * p_fast(i | c,x)` for `i in H`.

`p_source(i) = beta * p_tail(i | c)` for `i in T`.

Use an independent random dimension for the branch, or derive the correctly remapped conditional variate. Do not use a restricted branch interval as an unmodified within-branch random number.

### 7.2 Fast selector

Reuse the existing stratified selector with actual probabilities and adequate support. If it draws one of S strata uniformly, then source i by mass within that stratum:

`p_fast(i) = (1/S) * m_i / sum(stratum masses)`.

Keep the stratum factor for short final strata. Live sources that can contribute need positive pixel-selection support even when approximate geometry/mask/history weights are zero. A CPU ranking floor alone is insufficient.

An empty stratum gives a null zero-valued outcome, not an unaccounted retry. Live-source probabilities plus null outcomes normalize; do not renormalize away holes only in the tests.

### 7.3 Overflow selector

Keep a compact CPU range of `(UID, probability, alias data)` and publish an equivalent GPU range with current parent indices. Use the same tested alias-building utility as member distributions when A is available; the utility itself has no dependency on DTAL geometry.

Use positive fallback probabilities for every accepted overflow source and enough random-bit resolution. Validate the actual quantized distribution. Normalized table values, alias thresholds and selected indices must come from the same publication.

There is no dense per-overflow-source copy of the existing fast-slot history buffer. Keep memory proportional to actual candidate relationships and report it.

### 7.4 Interface to shading

Return parent array index, complete `p_source`, and valid fast slot or invalid overflow slot. The light sampler returns its conditional member probability separately:

`shade factor = 1/(p_source * p_member)`.

Ordinary sources have `p_member=1`. B does not alter member/point sampling or transfer rejected light power into another source.

Null branches, black samples and occlusion remain legitimate zero outcomes without retry. A missing record for an accepted published UID is an error to repair/report.

## 8. Publication without another identity system

Extend `LightManager::SetClusterLightLists` or a directly adjacent publication function:

1. Cluster CPU data keeps stable UIDs and deterministic distribution-entry order.
2. Resolve each referenced UID using `FindRegisteredLight(frameIndex, uid, ...)` after the frame's source set is accepted.
3. Publish current indices in fast and overflow GPU entries.
4. Reuse only when list/distribution generation and accepted UID/index order still match.
5. If parent order changes, refresh affected index publications; leave PMFs and aliases reusable when their UID entry order is unchanged.

This avoids an additional `FrameSourceMap` GPU indirection and stable-handle lifecycle. It can cost an O(published entries) refresh when array order changes. Measure that cost and prefer batching/reusing UID resolution before introducing a more complex remap design.

Source removal changes candidate membership before publication. No alias entry may accidentally point at a different UID because an old frame index was retained.

Use existing frame slots, copy flags, descriptors and RHI retirement. Native buffer storage and wrappers must share correct last-use lifetime. A resource-budget or staging failure must not publish mismatched offsets/indices/PMFs or an incomplete tail.

## 9. Visibility statistics and temporal stability

- Accumulate existing dense statistics only for a valid selected fast slot.
- Overflow samples do not write slot zero or another source's slot.
- When a slot's UID or conditional sampled domain changes, invalidate its old history with safe ordered commands or the existing history-generation mechanism.
- Avoid clearing unaffected histories or all denoiser history every frame.
- A change only in branch probability does not change the selected fast source's conditional visibility distribution.
- Older in-flight frames must not contaminate a new slot attribution. Validate resource/ring ordering, not just CPU bookkeeping.
- Extend gradient/replay random dimensions coherently; distribution changes require invalidating affected replay assumptions.

If the existing history structures cannot safely express the required change, add the smallest local version check. That is different from imposing a universal multi-generation framework.

## 10. Resources and memory

Use compact ranges/CSR and reuse capacities. Required data are per-cluster headers, existing fast entries and compact overflow entries. Candidate/distribution storage is O(actual cluster-source relationships), not merely O(number of lights).

Update generated layouts, descriptors, buffer capacities, native wrapping, copies and barriers across direct/indirect consumers together. Do not simply multiply `Q2_LIGHT_LIST_MAX_PER_CELL`; its dense statistics buffer scales with slots, sides and history frames.

Declare checked byte/work budgets. On budget exhaustion, report failure or retain only a complete semantically compatible old state. Do not pretend truncation is an optimization or automatically increase A's grid spacing.

## 11. Implementation steps

### B0. Diagnostics and capacity

- [ ] Capture ordinary-source baseline and existing losses independently of A.
      (The host diagnostics exist; no GPU capture was possible here.)
- [x] Add registration rejection reasons, evictions, candidate demand and occupancy reporting.
- [x] Fix mapped-camera-cluster diagnostics.
- [x] Deduplicate before capacity checks and make source acceptance coherent/race-free.
- [x] Update real renderer/native-buffer capacities and reserved-entry accounting together.

### B1. Candidate sets

- [x] Form complete accepted C before ranking.
- [x] Support ordinary domains and A's optional opaque group metadata.
- [x] Define conservative top-up/visibility/reach eligibility.
- [x] Rebuild affected clusters as a whole initially and verify full/incremental support parity.
      (CPU support tests; the overflow policy bypasses the incremental path.)

### B2. Fast/overflow sampling

- [x] Select deterministic useful H and preserve all remaining sources in T.
- [x] Build supported tail aliases and branch probabilities.
- [x] Extend the existing UID-to-frame-index publication for both sets.
- [x] Return complete source probability and valid/invalid statistics-slot attribution in GLSL/HLSL direct/indirect paths.
- [x] Preserve history/ring safety during movement, removal and rank exchange.

### B3. Acceptance and tuning

- [x] Validate mean lighting under oversubscription and parent reorder.
      (Deterministic CPU reference for 0/1/128/129/512/2048 candidates and shuffled order.)
- [ ] Benchmark ordinary lights with A absent/off.
- [ ] Tune ranking/branch/update policies from measured noise and cost.
      (`beta` bounds, the 0.1% fast-mass floor and the tail weight floor are provisional defaults.)
- [x] Leave B's report and integrate A through the shared boundary without editing member geometry.

## 12. File map

| Files | B's work |
|---|---|
| `Quake/gl_rlight.c`, relevant `gl_rmain.c` producer integration | Coherent registration, metadata and diagnostics |
| `qray.h`, `qray.cpp`, `VulkanDevice.*`, `Scene.*`, `LightManager.*` | Source/table API, capacity, UID publication and buffers |
| `ClusterLightLists.*`, `WorldLights.*` if needed | Complete candidates, ranking, fast/tail and affected-cluster updates |
| Generator/common resource declarations, `Q2LightLists`, `Q2ClusterLights` twins | Sampling headers, overflow entries and probabilities |
| Direct/indirect raygens and probes | Probability composition, slot attribution and temporal consistency |
| RHI light-buffer consumers and existing lifetime code | Descriptor/native sizes, copies, barriers and replacement |

New test targets and compact alias helper are registered explicitly. A's builder, geometry and luma-support algorithms are not B's work.

## 13. Required tests

- 0, 1, 128, 129, 512 and 2048 ordinary accepted candidates near a receiver; compare against exhaustive reference selection.
- Fast-only, tail-only, both sets, holes and unequal strata, zero approximate powers, large PMF dynamic range and ties.
- More than 1024 registered parents within the declared renderer resource budget, duplicate updates and rejection at the actual capacity.
- Dynamic source insertion/removal/reorder with cached aliases; ensure every publication selects the expected UID.
- Movement between leaves/rooms, folded map mapping, source reach greater than top-up reach, and conservative bounds.
- Fast-slot replacement with older GPU work still in flight, gradient/replay changes and map reset.
- All ordinary source types; use synthetic generic bounds/multi-cluster metadata before real A groups are available.

Assertions:

- Every accepted candidate is in exactly H or T.
- PMFs plus null outcomes normalize and match the actual implementation.
- All published indices resolve to their intended UIDs.
- No unintended capacity/list loss inside the supported budget.
- Updating one affected cluster does not rebuild unrelated DTAL geometry or unaffected static eligibility.

Raw mean/reference acceptance follows the shared criteria: predeclared receivers, linear output, direct/indirect separation and `abs(testMean-referenceMean)+3*combinedStandardError` within the declared tolerance. A noisy result is inconclusive. `rt_restir` and denoised screenshots are not mathematical references.

## 14. Reports and handoff

Provide a development comparison control such as `rt_cluster_sampling`: `0` uses the existing fast-only distance policy; `1` uses the repaired candidate/ranking/overflow selector. This switch changes source-selection policy, not A's grouping setting. Capacity-coherence fixes and diagnostics do not need to be undone by the switch. Keep the old executable/captures when a comparison specifically requires the old registration behavior, and identify which baseline is used in every benchmark.

Record the enabled selector, branch policy, update schedule and any ranking tuning in benchmark metadata. Enable the repaired policy by default only after independent tests pass. Do not make the DTAL spacing slider select or tune a cluster policy implicitly.

Expose:

- Attempted/deduplicated/accepted/rejected parents with causes.
- Demand before partition: median/p95/max and cluster IDs.
- Fast occupancy versus overflow size/power.
- Fast eviction versus candidate-domain rejection.
- Sample branch proportions and full/incremental rebuild/proposal timings.
- Publication/remapping cost, GPU/CPU bytes and dense-history allocation.
- Raw noise and denoised stability on identical camera paths.

Choose defaults from measurements without claiming that ranking alone restores omitted light. Follow shared build commands and leave a standalone implementation report. UML D07 describes B; D04 is integration through existing owners, not a new frame service.
