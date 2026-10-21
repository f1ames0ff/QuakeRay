# DTAL distance and visibility LOD — implementation report

Status: the implementation landed on `perf/dtal-distance-clusters` (core `bd4f8445`, engine wiring
`668b249f`, representative registration `b9976718`); the acceptance runs are the remaining step and
are recorded in section 7 as they complete. The specification is
[dtal-distance-cluster-spec.md](dtal-distance-cluster-spec.md); the roadmap is
[dtal-cluster-optimization-plan.md](dtal-cluster-optimization-plan.md).

## 1. Scope

The layer merges grouped DTAL parents into distance/visibility LOD nodes where the merge is
imperceptible: a precomputed hierarchy (built once per DTAL build) plus a per-tick level selection
with hysteresis, dwell, a bounded budget and an atomic exactly-once replacement. Proxies and
projectors stay singletons; the merge is the owner-accepted approximation, guarded by captures and
by the refusal rules below.

## 2. Core (`Quake/rt_dtal_lod.{h,c}`)

- **Hierarchy build.** Every group enters one node per band level of a world-anchored lattice; a
  node is keyed by `(level, cell, signed dominant axis, emission key, style key, pass class)`,
  stores its member list, the aggregate over them (power, power-weighted centroid and normal, summed
  area, conservative bounds radius) and a deterministic UID derived from the key. Inputs flagged
  `RT_DTAL_LOD_INPUT_SINGLETON` never enter a node. Cost `O(maxBands * groups)` at the build.
- **Selection.** Per group: distance band with edge hysteresis (`d > edge*(1+h)` merges,
  `d < edge*(1-h)` splits), a dwelled visibility class, the offscreen band shift applied to the
  chosen distance band, a bounded budget with a resumable cursor, and priority for returning and
  pending groups. `RT_Dtal_Lod_Select` returns the number of published-representation transitions.
- **Publication.** A node publishes with at least two assigned members and a non-degenerate
  aggregate (`power > 1e-9`, `area > 1e-8`, `|sum P*n| > 1e-6 * P`); every group resolves to its
  node or to itself, exactly once. Degenerate refusals are counted; shaping-cvar drift is rejected.
- **Accessors.** `Nodes()`/`Members()` expose the compact published set, `GroupNode()` the per-group
  representation, `Stats()` the counts and per-band distribution.

## 3. Engine integration (`Quake/r_world.c`, `Quake/gl_vidsdl.c`)

- **Controls.** `rt_dtal_lod` (0), `rt_dtal_lod_start` (512), `rt_dtal_lod_screen` (2), 
  `rt_dtal_lod_maxbands` (4), `rt_dtal_lod_offscreen` (1), `rt_dtal_lod_hysteresis` (0.15),
  `rt_dtal_lod_dwell` (0.25), `rt_dtal_lod_interval` (0.05); the shaping trio rebuilds the hierarchy
  through `RT_DtalLodRequestRebuild`, the rest are runtime.
- **Build hook.** A successful `RT_DtalGroups_Rebuild` marks the LOD dirty; the next tick builds the
  hierarchy from the groups and sources (proxy-fed and projector groups flagged singleton).
- **Tick.** Runs in the light stage after `RT_DtalGroups_UpdateFrame`, on the interval, with the
  frustum classification (`R_CullBox`) and a budget of 256 groups.
- **Effective representation.** Per published node: the union of the members' cluster coverage
  (refused, members stay individual, when the union overflows `QR_CLUSTER_LIGHT_MAX_SOURCES_CLUSTERS`)
  and the buried test (`RT_LightBuried` under `rt_dtal_clearance`).
- **Upload.** The frame builds a combined batch: one node parent with a single synthesized quad
  member (basis from the node normal, side `sqrt(area)`, UV corners) plus the individual groups with
  their real members; merged parents are omitted and their GPU members are re-uploaded on unmerge.
  Node colour and mean emission are the power-weighted averages of the members' current per-frame
  values through `RT_KeepEmissiveLightColor`, so a node never drops out of the cluster lists.
- **Registration.** A published node registers its UID at the origin nudged along the average normal
  (`nudge = 2.0f`, the `RT_RegisterWorldModelLight` rule) with the union coverage (or the
  `RT_ClusterLightAddPower` fallback); merged groups are skipped.
- **Diagnostics.** `rt_light_report` gains a `dtal lod` line: groups, merged, nodes, individual,
  transitions, degenerate refusals, uploaded node/group records and the coverage/buried refusals.

## 4. Ownership and lifetime

The tick, the effective set and the combined upload arrays are written and read inside the viewmodel
task's light stage; the light report reads the snapshot after the frame's join, like the cluster
statistics. The thread-local buried cache, the `RT_DtalReserve` growth and the cvar reads follow the
existing producers' rules; the selection reads only frame-ordered state (`rt_dtal_build`, the
sources, `r_refdef`, `realtime`, the frustum). [docs/multithreading.md](multithreading.md) applies.

## 5. Tests

`tests/rt_lighting_tests.c` extends with eight cases: hierarchy math and the `maxbands` clamp,
singleton refusal (separate cells and the input flag), determinism and reset, hysteresis and the
asymmetric unmerge, dwell with the offscreen shift, budget/cursor with exactly-once publication,
degenerate refusals and the configuration guard. Result: `1413989 checks, 0 failures`; CTest 8/8.

## 6. Builds

Debug build clean at `/W4` on the branch. The Release build is part of the acceptance step.

## 7. Acceptance

Matched captures `rt_dtal_lod 0` against `1` on the AD hub, swamp and Fuma, with the shipped grouped
defaults, recording source counts, the fast-list pressure reports, CPU/GPU frame time and settled
screenshots; `e1m7` covers the parent-count case. The runs follow
[tests/perf/run_stress.ps1](../tests/perf/run_stress.ps1) and are recorded here when complete: the
runner's foreground-focus gate and the shared machine guard schedule them with the owner.

## 8. Limitations

- Stage-1 visibility is the view frustum only; the visible-rows class and the sampled occlusion test
  remain stage-2 options per the specification.
- A coverage-union overflow refuses the node rather than truncating its cluster coverage.
- The representative is a flat quad over members that may fold; the axis rule and the screen target
  bound the error, and the spec's stage-2 option of a small member grid stays open.
- The temporal history is not explicitly invalidated when a node's membership changes; the stable
  UID keeps the identity and the stage-1 expectation is measured in the acceptance captures.
- The release default stays `0` until the owner decides after acceptance.
