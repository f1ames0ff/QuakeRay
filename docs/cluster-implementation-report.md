# Project B Implementation Report: Cluster Source Selection and Overflow

## 1. Identity

| Item | Value |
|---|---|
| Branch | `feature/cluster-lighting-overflow` |
| Base | the final Project A commit on `feature/dtal-grid-groups` |
| Selector | capacity, candidate partition, alias/marginal tables, tests |
| GPU | tail buffers, publication, shader branch, RHI |
| Diagnostics | overflow counters, parallel registration |
| Mapped cluster | the light report queries the folded cluster |
| Overflow report | the full fast list is reported as overflow, not as unsampled lights |
| Branch history | `git log feature/dtal-grid-groups..feature/cluster-lighting-overflow` |

All commits are on `feature/cluster-lighting-overflow`; nothing was merged, force-pushed or
published.

## 2. Delivered scope

### 2.1 Coherent acceptance and capacity (B0)

- The host registry capacity is now `QR_CLUSTER_MAX_REGISTERED_LIGHTS = 4095`, matching the
  renderer's 4096-entry light array minus the reserved directional slot. The old independent
  1024-source truncation is gone; capacity refusals keep being counted and warned about.
- `RT_ClusterLightAddMulti` serializes registration with a mutex created at the existing frame
  reset boundary, so the entity draw tasks that register lights in parallel no longer race on the
  registry, the UID hint table or the diagnostic records.
- Deduplication by UID happens before capacity is consumed, as before, now at the larger capacity.
- The light report queries the folded cluster (`RT_MapWorldCluster`) instead of a raw leaf index.

### 2.2 Complete candidate sets and ranking (B1/B2 fast side)

- Every accepted `(cluster, source)` pair is recorded in `ClusterLightLists` when the pair is
  granted or topped up, independent of whether it ends up in a fast slot. `C` is therefore
  complete before any ranking decision, and `H ∪ T = C`, `H ∩ T = ∅`, `|H| ≤ 128` hold by
  construction.
- `H` remains the existing 128-slot ranked list (distance order, nearest kept) so Project A's
  delivery is untouched. `T` is every accepted candidate not in a slot, ordered deterministically
  by the distance-shaped mass (below) with the UID as the tie-breaker.
- Sources now carry an estimated power (DTAL groups publish the builder's aggregate, world DTAL
  publishes `area * meanEmiss * luma(color)`, dlights/spots publish `luma(color) * radius²`;
  remaining light classes fall back to the nearest-source reserve). The branch and the tail weights
  shape that power by the distance term the fast selector's mass uses:
  `mass = power / max(d², radius², 1)`, where `d²` is the squared distance from the source origin to
  the cluster bounds (the DTAL group radius, floored at one unit, caps the term).
- `beta` per cluster follows the specification: empty T → 0, empty H → 1, both non-empty with zero
  estimated total mass → 0.5, otherwise `clamp(tailMass / (fastMass + tailMass), 0.1, 0.9)`.

### 2.3 Overflow distribution (B2 tail side)

- Per-cluster tail weights are `mass + floor` with `floor = 0.001 * (fastMass + tailMass + 1)`,
  so every accepted overflow source keeps positive support even at zero approximate power. The
  branch probability and the weights are built from the same masses, so the published distribution
  follows the view-shaped estimate instead of raw emitter power.
- `RT_Alias_Build` (shared with Project A) builds the tables in double precision; `RT_Alias_Marginals`
  publishes the actual marginal probability of every entry, so the shader divides by the marginal
  selection probability rather than by a sampling path probability. This corrects a real bias that
  the CPU tests caught: path probabilities do not sum to one source per alias column.
- The published GPU record is `ShQ2LightTail { lightIndex, aliasIndex, prob, marginalProb }`
  (16 bytes). The offsets buffer carries the CSR offsets plus the per-cluster beta bit-packed as
  floats after the offsets.

### 2.4 GPU publication and sampling

- `LightManager` owns two new buffers (`tailOffsets`, `tailEntries`) with the same staging,
  pending-copy and descriptor plumbing as the existing list buffers; bindings
  `BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL_OFFSETS = 10` and `..._TAIL = 11` are added to the
  generated common layout and to the RHI direct pass (8 light buffers, 7 copy items; all seven are
  scheduled, and the offsets copy spans the full `(2 * Q2_MAX_CLUSTERS + 1)` word layout so the
  packed beta region is published with the offsets).
- `SetClusterLightLists` extends its existing UID-to-current-index publication to the tail entries:
  each tail UID is resolved with `FindRegisteredLight` and the resolved index is published. If any
  tail UID has no renderer record, the whole tail set is withheld for that frame (offsets and beta
  zeroed) with a diagnostic, so the frame stays on a consistent fast-only distribution instead of
  one with holes.
- `q2SampleClusterLights` (GLSL and HLSL) now:
  - computes the fast stratum masses with a positive floor (`q2FastMass`, 0.1% of the stratum
    maximum, uniform when the stratum has no positive mass);
  - draws an independent branch variate: `u_branch < beta` selects the overflow alias table,
    otherwise the existing stratified fast selector;
  - returns `p_source = beta * marginal` for overflow and `p_source = (1-beta) * p_fast` for fast;
  - reports an invalid slot (`0xFFFFFFFF`) for overflow, and `q2AccumulateLightStats` refuses the
    invalid slot, so overflow samples never write another source's dense history.
- The direct and indirect raygen call sites are unchanged: they already divide by `lightPdf`, which
  now carries the complete source probability.

### 2.5 Development switch and diagnostics

- `rt_cluster_sampling`: `0` keeps the legacy fast-only policy (tails are not published, `beta = 0`,
  the shader reduces to the old selector); `1` enables the repaired candidate/overflow policy. It
  is a benchmark-metadata setting and does not affect DTAL grouping.
- When overflow is enabled the incremental composition path is deliberately bypassed: the tail sets
  are derived from the complete candidate sets, and keeping them exact for moved/removed sources
  would require the same full walk. Frames that reuse an unchanged set still reuse the lists and
  tails. This is the "whole affected cluster initially" rule of the spec applied at frame
  granularity and is recorded as a limitation below.
- `rt_light_report` now prints per-frame candidate demand (`max`, `median`, `p95`), overflow entry
  and cluster counts, and the number of clusters whose tail did not fit the declared budget.

### 2.6 Post-review repairs

An independent review of the published state found defects that the CPU tests could not reach; the
branch history now carries the repairs:

- **All seven GPU copies are initialized.** The direct pass copied four of the eight light buffers;
  `dtalMembers`, `tailOffsets` and `tailEntries` were never scheduled. The copy list now matches the
  binding list, the offsets copy spans the full fixed layout so the packed beta values reach the
  GPU, and the pre-publication clear loop no longer writes one word past the buffer.
- **Tail publication is all-or-nothing per frame.** An unresolved tail UID, a budget refusal or an
  alias-build failure now discards the whole tail set (offsets and beta zeroed) and keeps the frame
  on the fast list with a diagnostic, instead of publishing a distribution with holes.
- **Updates are resolved before the entry budget.** `RT_ClusterLightAddMulti` looks the UID up
  before the capacity check, so a full registry still refreshes a light it already holds.
- **Renderer insertion is serialized.** `LightManager::AddLight` holds a registry mutex, so
  concurrent producers cannot interleave the find/insert/update sequence.
- **Discrete draws are 24-bit half-open.** `rnd24` replaces the 16-bit inclusive draws for the
  member column, the tail column and the fast-list branch variate in GLSL and HLSL. A 16-bit draw
  cannot address more than 65536 categories, so rare members and tail columns above that index had
  no positive support; the half-open domain also removes the `u = 1` endpoint. The CPU reference
  model already used 24-bit half-open draws.
- **Top-up candidates are recorded before retention.** Every source accepted by the supplemental
  reach is added to the candidate bitset before the nearest-eight retention selects the fast list,
  so the overflow set is built from the full accepted candidate set.

## 3. Verification

| Check | Command | Result |
|---|---|---|
| Debug build | `.\build_win.ps1 -Config Debug -BuildDir build\Debug` | pass |
| Release build | `.\build_win.ps1 -Config Release -BuildDir build\Release` | pass |
| Numerical tests | `.\build\Debug\rt_lighting_tests.exe` | `1413875 checks, 0 failures` |
| Shader properties | `python CheckShaderProperties.py --rebuild` | pass |
| Matrix reads | `python CheckMatrixReads.py` | pass |
| Whitespace | `git diff --check` | clean |

The numerical tests cover the required oversubscription cases with a deterministic simulation that
mirrors the published GPU algorithm:

- 0, 1, 128, 129, 512 and 2048 accepted candidates; the estimator `value / p_source` reproduces the
  exhaustive reference `Σ value` within `4σ` (and the test fails as inconclusive when `4σ` exceeds
  5% of the reference);
- alias PMFs from the published floats match the normalized weights for uniform, zero, negative,
  extreme-dynamic-range and 2048-entry tables;
- shuffling the candidate array does not change which UIDs are in H or T;
- H/T bounds, disjointness, coverage and beta range invariants are asserted per case.

The pinned `e4m1` lamp gate (scripted captures, exposure frozen, post and upscaling disabled, light
styles frozen) measures the tail-on/off wall gap that motivated the branch shaping: 7.8% before the
shaping and 2.1% after it with light statistics on, 4.7% -> 0.4% with them off, with cluster 336
`beta` falling from 0.356 to 0.100 and `rt_cluster_assert` clean. The in-game `rt_bench` comparison
on the Arcane Dimensions `ad_tfuma` route (scripted demo, 4K, vsync off, FSR 3.1 ultra performance,
statistics off) measures the frame cost of the policy. That route is CPU bound: the GPU frame is
about 11 ms while the focused CPU frame is about 36 ms, and a run whose window loses focus sleeps
16 ms in every frame, which is exactly the difference between the first run pair that looked like a
large regression. With the window focused and the wall-to-CPU gap under 2 ms in every counted run,
the route runs at 27.5 fps with the fast list only and 26.5/26.4 fps with the overflow tail; the
difference is the measured cluster work, about 1.0 ms of `clust lists` plus 0.37 ms of `clust tail`
per frame across the route's about 140 full recompositions. `rt_cluster_assert 1` over the whole
route reports no mismatches. The `rt_cluster_sampling` default stays off until that default is
decided.

## 4. Memory and capacity

- Tail entries budget: `Q2_LIGHT_LIST_TAIL_CAPACITY = 1048576` entries × 16 B = 16 MiB device-local
  plus the staging copies.
- Tail offsets and beta: `(2 * 8192 + 1) * 4 B` = 64 KiB.
- The dense per-slot statistics buffer is unchanged: overflow has no per-source copy of it.
- A tail that would exceed the budget, or an alias table that fails to build, discards the whole
  tail set for that frame with a diagnostic (`tailBudgetExceeded`); the frame then samples the fast
  list only. No frame is published with partial overflow support.

## 5. Deviations, limitations and unresolved items

1. **H ranking remains distance-based.** The overflow tail is ranked and weighted by the
   distance-shaped mass and `beta` is derived from it, but the 128 fast slots keep the historical
   nearest-first order. A mass-aware fast ranking with a nearest reserve is a tuning step that needs
   cost measurements this environment could not produce.
2. **No incremental overflow updates.** With overflow enabled, a changed light set recomposes the
   lists; the incremental path still serves the legacy policy. The spec allows this as the initial
   simple update and asks for profiling before finer repair.
3. **Fast-list retention is unchanged.** A cluster still keeps at most 128 sources in H and evicts
   by distance; the overflow set is what makes every accepted candidate sampleable. The supplemental
   top-up pass still retains its nearest eight sources in the fast list, but every reach-accepted
   source is recorded as an overflow candidate before that retention, so the overflow set covers the
   full accepted domain rather than the retention survivors.
4. **Runtime oversubscription evidence covers the pinned scenes.** The lamp gate captures nine
   oversubscribed clusters; the `ad_tfuma` route exercises the policy over a moving player with
   about 140 full recompositions and reports no `rt_cluster_assert` mismatches. The evidence is
   still scene-limited (two maps), not a general guarantee.
5. **Publication cost.** Fast and tail index publication resolves every referenced UID each time the
   lists change; the spec accepts this O(published entries) refresh until measured otherwise.
6. **Power metadata coverage.** Some light classes (alias/sprite entity lights registered through
   the generic emitter path) still register with zero estimated power; they keep positive overflow
   support through the weight floor and the nearest reserve, but their ranking is not power-aware.
