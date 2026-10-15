# DTAL distance and visibility cluster LOD — specification

Status: specification for the layer after the merged DTAL projects (Project A grid groups, Project B
cluster source selection; both in `master` via PR #51). Branch `perf/dtal-distance-clusters`.
Owner idea, 2026-10-14; revised the same day with the owner's decisions: the grouping is
**precomputed** at the DTAL build instead of re-derived per tick, the merge is an accepted measured
approximation rather than an exact-energy guarantee, and the representative reuses the existing
buried/clearance and normal-offset placement guards. The defaults below are proposals to tune by
capture; section 15 lists the decisions that need the owner's word.

## 1. Purpose

The grouped DTAL parents are registered as individual cluster sources today. On maps where grid
clipping splits medium and large emitters across cells, the grouped parent count even *increases*
over the per-piece path (measured: `e1m7` 66 to 109/121, `e4m1` 269 to 358, Project A report), and
every one of those sources keeps a slot, a fast-list entry and a sampling branch no matter how far
away, off-screen or occluded it is.

This layer merges grouped parents into **LOD clusters** when distance and visibility make the merge
imperceptible:

- near the player and directly visible: a parent keeps its own representation (as today);
- beyond a start distance: compatible parents merge into one representative light per cluster;
- the cluster's world size grows with distance so its **screen footprint stays approximately
  constant** — the merge error is bounded in screen space, not in world space;
- parents that are off-frustum or not visible through the current cluster rows merge more
  aggressively;
- the total emitted power, colour, style and pass-through semantics are preserved.

The grouping itself is **precomputed** at the DTAL build (section 5a); the frame only selects levels
and applies the diff.

Naming: "LOD cluster" here is a group of DTAL parents. It is not a light cluster (the PVS-keyed
receiver lists) and not a Project A grid group.

## 2. What already exists (the layer's base)

- **Pieces to groups** (Project A): `Quake/rt_dtal_groups.{h,c}`, keyed by `(cell, signed dominant
  normal axis, emission key, style key)`, grid edge `rt_dtal_spacing` (8), cap `rt_dtal_maxgroups`
  (2048), static per world generation, immutable member tables; parent parameters (colour, mask,
  animation) refresh per frame from the canonical source texture.
- **Groups to cluster lists**: `RT_ClusterLightAddMulti` (union coverage up to
  `QR_CLUSTER_LIGHT_MAX_SOURCES_CLUSTERS` (128) clusters, bounds
  radius, power) and `ClusterLightLists` composition. The incremental composition is the only mode
  the renderer selects (`rt_cluster_incremental` is `CVAR_ROM`, a saved `0` is ignored).
- **Multithreading**: the render frame always runs as the task graph (`r_tasks` and its serial fork
  were retired on 2026-10-14); `r_parallelmark 1` is the engine default.
- **Budgets**: builder 65536 parents / 262144 members; renderer member buffer
  `QR_DTAL_MAX_UPLOAD_MEMBERS` = 131072 records (144 B each); light storage 4096 records; cluster
  registration 1024 sources; fast list 128 entries.
- **Placement guards**: buried pieces are dropped before grouping by `RT_LightBuried` under
  `rt_dtal_clearance` (thread-local buried cache, reset at map load), and a registered light's
  origin is moved along its surface normal (`RT_RegisterWorldModelLight`'s `nudge`). The
  representative reuses the same guards (section 7).

## 3. Goal and non-goals

Goal:

1. Reduce the number of registered DTAL sources and their fast-list presence where the player cannot
   tell the difference, bounded by a configurable angular size.
2. Make the grouped path affordable on the maps where grouping currently costs frames (it removes
   the parent-count increase's cost) without changing near-field quality.
3. Reuse the machinery landed for cluster rebuild, list updates, incremental composition and the
   task frame; add no second light framework.
4. Precompute the hierarchy (band lattices, membership, representative geometry) at the DTAL build,
   so the frame carries only the level selection and the registration diff.

Non-goals:

- No change to piece-to-group grouping (Project A) or to list source acceptance/overflow (Project B).
- No new light-sampling framework in the first stage: a representative is a DTAL group parent with
  one synthesized member, so the existing member-sampling contract is kept unchanged.
- No shader changes in stage 1.
- No exact reproduction of the merged members' light distribution: the merge is a measured
  approximation (owner-accepted 2026-10-14). Guards, walks and captures protect against obvious
  over-brightening, dark bands and lost key lights; they do not promise per-member fidelity.

## 4. Terms

| Term | Meaning |
|---|---|
| Piece | One admitted emitting polygon fragment (Project A input). |
| Group | Project A parent: pieces of one grid cell/normal/style key. |
| Node | Precomputed merge candidate at one level: a band cell plus the merge key, with its member groups and aggregate. |
| Level | A band index `k`; a node lattice of one band size. |
| LOD cluster | A published node: the representative over its assigned member groups. |
| Band `k` | Distance range `[d0 * 2^k, d0 * 2^(k+1))`. |
| Cell | World-anchored grid cell of the band's size. |
| Visibility class | Visible / off-frustum / not-in-visible-rows (section 8). |

## 5. Bands and screen-space-constant cells

- `d0` = `rt_dtal_lod_start`: merging starts beyond this distance; nearer parents stay individual.
- The screen target is `theta0` = `rt_dtal_lod_screen` (degrees). The cell size of band `k` is
  `s_k = 2 * tan(theta0/2) * d0 * 2^k`; a cell at the band's inner edge subtends `theta0`, and at the
  band's outer edge about `theta0/2` — constant up to the factor of two between consecutive bands.
- A group at distance `d` uses band `k = floor(log2(d / d0))`; its cell is the world-anchored lattice
  cell of size `s_k`. World anchoring (not camera-relative) keeps cluster identities stable while
  the camera moves; the screen-area goal is statistical, not exact.
- `rt_dtal_lod_maxbands` caps `k` so clusters do not grow without bound; beyond the cap every
  eligible group merges at the cap's cell size.

### 5a. Precomputed hierarchy

At the DTAL build (map load and every regeneration, in the same safe preparation boundary as the
group builder), each group is inserted once into every level's world-anchored lattice:

- a node is keyed by `(level k, cell, dominant axis, emission key, style key, pass-through class)`;
- a node holds its member groups, the aggregate over them (power, power-weighted centroid and
  normal, summed area, conservative bounds radius) and a stable UID;
- a node with fewer than two members is kept for identity stability but is never publishable (no
  singleton clusters);
- the per-node cluster coverage is built lazily on first publication and cached by the integration.

The precompute cost is `O(maxBands * groups)` at the build; the runtime never re-derives cells,
membership or representative geometry.

## 6. Merge compatibility

A cluster groups parents that share a key of:

- band `k` and cell;
- signed dominant normal axis (the Project A vocabulary) — so the representative's average normal
  is only formed over same-hemisphere normals and cannot degenerate;
- emission key and style key — a light style is a time function; cross-style merging would need a
  combined function and is refused;
- pass-through class (glass/water) and the projector exclusion (projectors stay singletons, as in
  Project A).

Parents that are alone in their cell stay individual (no singleton clusters). The key is evaluated
at build time; a change to it, or to the band controls, forces the hierarchy rebuild.

## 7. Representative synthesis

For a cluster of parents `i` (each with finite power estimate `P_i`, origin `o_i`, area `A_i`,
normal `n_i`):

- power `P = sum(P_i)`;
- origin `c = sum(P_i * o_i) / P` (power-weighted centroid);
- normal `n = normalize(sum(P_i * n_i))` (same-axis subset by construction);
- area `A = sum(A_i)`; bounds radius `R` is the conservative bound of the members' bounds
  (the AABB half-diagonal rule of Project A applied to the member spheres);
- radiance/mask: power-weighted average of the parents' *current* per-frame radiance;
- representation: one synthesized member (quad of area `A` at `c`, oriented by `n`, affine UV from
  the combined bounds) inside a `LIGHT_TYPE_DTAL_GROUP` parent with one member. The member sampler
  sees a singleton member (`p_member` = 1) and `p_source` keeps its existing contract. The
  representative is an approximation of the members' combined emission (owner-accepted); the
  acceptance checks are in section 13.
- placement reuses the existing guards: the members have already passed `RT_LightBuried` /
  `rt_dtal_clearance`, and the representative's registration origin is moved along its normal as
  `RT_RegisterWorldModelLight` does. A representative that still tests buried, or whose normal or
  area degenerates, is refused — the node stays unpublished and the refusal is diagnosed.

The representative's per-frame parameters (colour, mask, animation) are refreshed exactly as group
parents refresh from their canonical texture, so animated and lightstyle content stays live without
rebuilding membership. The aggregate geometry is refreshed when the node's assigned membership
changes (section 10), not every frame.

## 8. Visibility classes

- **Visible**: at least one of the group's registered clusters appears in the camera's current
  visible cluster rows (the rows the list builder already walks).
- **Off-frustum**: the group's bounds are outside the view frustum.
- **Not-in-visible-rows**: the group contributes only through PVS reach beyond the current rows.

Off-frustum and not-in-visible groups use band shift `rt_dtal_lod_offscreen` (default +1: one band
coarser, i.e. 4x cell area). The class is hysteresis- and dwell-damped (section 9).

Stage 1 note: the visibility test is frustum plus the existing cluster-row visibility. It is
conservative — a parent behind a wall that is still in the camera's PVS rows stays "visible" and
keeps its quality. A per-source sampled occlusion test (a few amortized rays) is a stage-2 option
if captures show the far-side case matters.

## 9. Stability (anti pop-in)

- Hysteresis `h` = `rt_dtal_lod_hysteresis` (default 0.15) on every band edge: a parent merges once
  `d > edge * (1 + h)` and splits once `d < edge * (1 - h)`.
- Dwell `rt_dtal_lod_dwell` (default 0.25 s): a visibility-class change applies only after the class
  has been stable for that long.
- Unmerge is asymmetric: a merge may wait for the hysteresis edge, but a group that returns inside
  the exact range is restored to its own representation with priority — a group whose distance is
  below the unmerge edge, or whose visibility class left the off-frustum set, is processed first
  each tick (section 10). A teleport, a map change or a hierarchy rebuild resets the selection to
  individual rather than letting a stale merge persist.
- Cluster identity: a deterministic UID derived from `(level, cell, dominant axis, emission, style,
  pass class)` so membership churn updates parameters without resetting temporal history (shadow
  history, RIS, denoiser) and a UID is never recycled for a different cell. The history is kept only
  while the representative stays within tolerance of its previous state and reset otherwise.
- Stage 1 transitions swap representation atomically, and the published replacement set represents
  every group exactly once; the perceptual bound is the screen target (sources are about `theta0` in
  size at an edge). If captures show visible pop, stage 2 options are a short cross-fade or a
  distance offset tuned with the same captures.

## 10. Cadence, batching and asynchronous application

The runtime never re-groups; the grouping is the precomputed hierarchy (section 5a). What runs per
tick is the level selection and the diff it causes:

- Cadence: the selection pass runs on `rt_dtal_lod_interval` (default 50 ms, about 20 Hz) from the
  frame setup stage. Between ticks the published set is stable, so at most one membership update
  becomes visible per interval; hysteresis and dwell (section 9) clamp the tick-to-tick changes
  further.
- Budget: a tick consumes a bounded budget of groups — near-distance groups, pending changes and
  the asymmetric unmerge set first, then a resumable cursor over the rest — so a single tick cannot
  stall a frame regardless of the group count; a group not yet visited keeps its last selection. A
  full pass over all groups takes at most `ceil(groups / budget)` intervals; groups not yet visited
  keep their last selection until their turn.
- Activation cost: when a node's assigned member set crosses the publishable threshold, or its
  members' position or power changed, the node's aggregate is recomputed from its assigned members
  and its registration is updated through the existing diff; the recompute is bounded by the tick's
  changes, not by the whole group set. A tick whose selection did not change costs the scan alone.
- Publication: the replacement set is built as a whole and swapped at a frame boundary, so the
  frame sees a consistent set — every group is represented exactly once: by its node or by itself,
  never both and never neither.
- Asynchronous application: the pass runs as a task on the task frame and its results are
  double-buffered and swapped at a frame boundary. The ownership, lifetime and lock rules of
  [docs/multithreading.md](multithreading.md) apply to the pass as to any existing light producer,
  including its adaptation checklist.

## 11. Integration

- The hierarchy is built with the group build in the existing safe preparation boundary
  (`RT_DtalGroups_*`, map load and regeneration), into owned arrays, validated before replacement. A
  map change or a rebuild resets the selection to individual and drops the hierarchy. The selection
  and application run in the light setup stage of the frame, `O(visited groups)` per tick with
  per-group state, deterministic ordering, parallelizable under the task frame.
- The per-frame diff of the registered source set goes through `RT_ClusterLightAddMulti` and the
  incremental composition: only the clusters whose grants changed are touched, which is exactly the
  path the incremental rebuild landed for moving lights.
- Representatives are dynamic records with the existing frame-slot and pending-copy lifetime;
  parents merged for the frame are not registered (their GPU members stay resident for unmerge).
- Budgets: published nodes <= groups <= `rt_dtal_maxgroups`; the hierarchy stores at most
  `maxBands * groups` nodes and `groups` member slots; member storage grows by at most one member
  per published node (144 B), far under `QR_DTAL_MAX_UPLOAD_MEMBERS`.
- Diagnostics: `rt_light_report` gains published nodes per band and class, transitions per interval,
  the source-count reduction against `rt_dtal_lod 0`, and the hierarchy build time and memory.

## 12. Controls (proposed)

| Cvar | Proposed default | Meaning |
|---|---|---|
| `rt_dtal_lod` | `0` during development; release default is an owner decision | master switch |
| `rt_dtal_lod_start` | `512` (about the owner's 10 m example; tune) | merge start distance, world units |
| `rt_dtal_lod_screen` | `2.0` (tune) | target angular size of one cluster cell seen from the camera, degrees (the cell subtends about `theta` at the band's near edge and `theta/2` at its far edge) |
| `rt_dtal_lod_maxbands` | `4` (tune) | number of doubling distance bands (band `k` spans `[d0*2^k, d0*2^(k+1))`; the cell size doubles per band and caps at the last one) |
| `rt_dtal_lod_offscreen` | `1` (tune) | band shift for off-frustum / not-visible rows |
| `rt_dtal_lod_hysteresis` | `0.15` | edge hysteresis fraction |
| `rt_dtal_lod_dwell` | `0.25` | minimum class dwell, seconds |
| `rt_dtal_lod_interval` | `0.05` | selection cadence, seconds |

`rt_dtal_lod`, `rt_dtal_lod_offscreen`, `rt_dtal_lod_hysteresis`, `rt_dtal_lod_dwell` and
`rt_dtal_lod_interval` are runtime controls. `rt_dtal_lod_start`, `rt_dtal_lod_screen` and
`rt_dtal_lod_maxbands` shape the hierarchy and force the DTAL rebuild (groups and hierarchy) through
the existing invalidation boundaries; the selection itself is per-frame and needs no rebuild.

## 13. Tests and acceptance

Unit tests (extend `tests/rt_lighting_tests.c`):

- hierarchy build: level/cell math, the `maxbands` clamp, node membership, deterministic keys/UIDs
  and reset behaviour;
- singleton nodes: a node with fewer than two members is never published;
- aggregates: power, centroid, normal, area and the conservative bounds are invariant across
  activation, deactivation and membership churn;
- selection state machine: hysteresis and dwell; a smooth camera approach produces no oscillation;
  the asymmetric unmerge restores a returning group with priority;
- budget and cursor: bounded work per tick, the full-pass bound, and exactly-once representation
  (a group is never both an individual and a node member);
- degenerate normal/area and the buried placement fallback refuse the representative.

Integration:

- Debug build, CTest, Release build per the standing PR gate;
- hierarchy build time and memory at map load, and the selection cost per tick on the AD hub, swamp
  and Fuma;
- matched captures on the AD hub, swamp and Fuma, `rt_dtal_lod 0` against `1`, on the shipped
  defaults (`rt_dtal_groups 1`, `rt_cluster_sampling 1`): source counts, cluster fast-list pressure
  (the 128-entry reports), CPU and GPU frame time;
- quality: settled-camera screenshot diffs within the same-build noise floor; a camera walk across
  `d0` and across band edges for pop-in and flicker; a denoiser stability check (no new temporal
  ghosting); checks for lost key lights and for visible over-brightening or dark bands at merge
  boundaries;
- the performance target: with the LOD on, the grouped defaults reach or beat the legacy-path
  milestone numbers on swamp and Fuma, removing the measured groups-on regression.

## 14. Risks and limitations

- The merge is an accepted approximation: a representative changes the lighting distribution and
  the shadow placement of its members. The acceptance is the guard set and the capture matrix of
  section 13, not exact energy preservation.
- Average-normal fidelity is an approximation; bounded by `theta0` and the axis compatibility rule.
- Occlusion detection is stage-1 conservative (frustum + visible rows); the purely behind-a-wall
  case keeps quality until a stage-2 sampled test is justified.
- Temporal history: a stable UID does not guarantee compatible history when membership, position or
  normal change materially; the node's history is kept only inside a tolerance of its previous
  state and reset otherwise (measured, not assumed).
- Mixed selections inside one cell near a band edge are resolved by the node rule (a node is
  published only for its assigned members) and clamped by hysteresis and dwell; the churn is
  measured, not assumed.
- The precompute stores at most `maxBands * groups` nodes; memory and build time at map load are
  measured.
- Animated/lightstyle parents merge only within their style/emission key, so animation never
  averages across incompatible radiance functions.
- The representative quad over a folded cluster is a flat approximation; the merge key's axis rule
  and the distance gate keep the visible error bounded. If screenshots show the folded case, the
  representative can grow to a small grid of members (a stage-2 option).

## 15. Decisions

Taken by the owner on 2026-10-14:

1. Precomputed hierarchy: the grouping is built at the DTAL build and cached; the frame only selects
   levels and applies the diff (the owner's cache idea).
2. Approximation accepted: the merge is a performance trade, verified by the guard set and the
   captures rather than exact-energy claims.
3. Representative placement reuses the existing buried/clearance test and the normal-offset
   registration; a representative that cannot be placed is refused.
4. Acceptance scenes: AD hub, swamp, Fuma, plus `e1m7` for the parent-count case.

Open:

- the proposed defaults of `rt_dtal_lod_start` / `rt_dtal_lod_screen` / `rt_dtal_lod_offscreen` as
  starting points for capture tuning;
- release enablement: after acceptance, default on, or opt-in.
