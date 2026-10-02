# Project B Implementation Report: Cluster Source Selection and Overflow

## 1. Identity

| Item | Value |
|---|---|
| Branch | `feature/cluster-lighting-overflow` |
| Base commit | `b1d9bb4f` (final commit of `feature/dtal-grid-groups`, including the world-draw and light-type fixes) |
| Selector commit | `b5915c40` (capacity, candidate partition, alias/marginal tables, tests) |
| GPU commit | `baf0e498` (tail buffers, publication, shader branch, RHI) |
| Diagnostics commit | `11bac83e` (overflow counters, parallel registration) |
| Mapped-cluster commit | `2478edc6` (the light report queries the folded cluster) |
| Report commit | `c0358e59` (this document, rebased) |
| Overflow warning commit | `7d0d3a4d` (the full fast list is reported as overflow, not as unsampled lights) |

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
  by estimated power with the UID as the tie-breaker.
- Sources now carry an estimated power (DTAL groups publish the builder's aggregate, world DTAL
  publishes `area * meanEmiss * luma(color)`, dlights/spots publish `luma(color) * radius²`;
  remaining light classes fall back to the nearest-source reserve).
- `beta` per cluster follows the specification: empty T → 0, empty H → 1, both non-empty with zero
  estimated total power → 0.5, otherwise `clamp(tailPower / (fastPower + tailPower), 0.1, 0.9)`.

### 2.3 Overflow distribution (B2 tail side)

- Per-cluster tail weights are `power + floor` with `floor = 0.001 * (fastPower + tailPower + 1)`,
  so every accepted overflow source keeps positive support even at zero approximate power.
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
  generated common layout and to the RHI direct pass (8 light buffers, 7 copy items).
- `SetClusterLightLists` extends its existing UID-to-current-index publication to the tail entries:
  each tail UID is resolved with `FindRegisteredLight` and the resolved index is published. An
  unresolved UID gets `LIGHT_INDEX_NONE` and marginal zero, so it can only produce a null sample,
  never another light.
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

## 3. Verification

| Check | Command | Result |
|---|---|---|
| Debug build | `.\build_win.ps1 -Config Debug -BuildDir build\Debug` | pass |
| Release build | `.\build_win.ps1 -Config Release -BuildDir build\Release` | pass |
| Numerical tests | `.\build\Debug\rt_lighting_tests.exe` | `1413860 checks, 0 failures` |
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

Unavailable in this environment: scripted GPU captures on an oversubscribed map. No runtime
oversubscription benchmark, noise measurement or frame-timing comparison is claimed, and the
`rt_cluster_sampling = 1` default is therefore left off until those measurements exist.

## 4. Memory and capacity

- Tail entries budget: `Q2_LIGHT_LIST_TAIL_CAPACITY = 1048576` entries × 16 B = 16 MiB device-local
  plus the staging copies.
- Tail offsets and beta: `(2 * 8192 + 1) * 4 B` = 64 KiB.
- The dense per-slot statistics buffer is unchanged: overflow has no per-source copy of it.
- Tail clusters whose cumulative entries would exceed the budget are skipped with a diagnostic
  (`tailBudgetExceeded`); the rest of the clusters keep their complete overflow support.

## 5. Deviations, limitations and unresolved items

1. **H ranking remains distance-based.** The repaired policy ranks the overflow tail by power and
   derives beta from power, but the 128 fast slots keep the historical nearest-first order. A
   power-aware fast ranking with a nearest reserve is a tuning step that needs the noise/cost
   measurements this environment could not produce.
2. **No incremental overflow updates.** With overflow enabled, a changed light set recomposes the
   lists; the incremental path still serves the legacy policy. The spec allows this as the initial
   simple update and asks for profiling before finer repair.
3. **Fast-list retention is unchanged.** A cluster still keeps at most 128 sources in H and evicts
   by distance; the overflow set is what makes every accepted candidate sampleable. Global
   `TopUpCluster` still keeps its old nearest-eight retention policy.
4. **No runtime oversubscription evidence.** The 128-slot limit warning observed on `e4m1` during
   the Project A manual test is the exact situation this policy repairs, but the repaired selector
   has not been captured on a GPU here.
5. **Publication cost.** Fast and tail index publication resolves every referenced UID each time the
   lists change; the spec accepts this O(published entries) refresh until measured otherwise.
6. **Power metadata coverage.** Some light classes (alias/sprite entity lights registered through
   the generic emitter path) still register with zero estimated power; they keep positive overflow
   support through the weight floor and the nearest reserve, but their ranking is not power-aware.
