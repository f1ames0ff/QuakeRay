# DTAL distance and visibility cluster LOD — specification

Status: specification for the layer after the merged DTAL projects (Project A grid groups, Project B
cluster source selection; both in `master` via PR #51). Branch `perf/dtal-distance-clusters`.
Owner idea, 2026-10-14. The defaults below are proposals to tune by capture; section 15 lists the
decisions that need the owner's word.

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

Naming: "LOD cluster" here is a group of DTAL parents. It is not a light cluster (the PVS-keyed
receiver lists) and not a Project A grid group.

## 2. What already exists (the layer's base)

- **Pieces to groups** (Project A): `Quake/rt_dtal_groups.{h,c}`, keyed by `(cell, signed dominant
  normal axis, emission key, style key)`, grid edge `rt_dtal_spacing` (8), cap `rt_dtal_maxgroups`
  (2048), static per world generation, immutable member tables; parent parameters (colour, mask,
  animation) refresh per frame from the canonical source texture.
- **Groups to cluster lists**: `RT_ClusterLightAddMulti` (union coverage up to 16 clusters, bounds
  radius, power) and `ClusterLightLists` composition. The incremental composition is the only mode
  the renderer selects (`rt_cluster_incremental` is `CVAR_ROM`, a saved `0` is ignored).
- **Multithreading**: `r_tasks 1` and `r_parallelmark 1` are the engine defaults.
- **Budgets**: builder 65536 parents / 262144 members; renderer member buffer
  `QR_DTAL_MAX_UPLOAD_MEMBERS` = 131072 records (144 B each); light storage 4096 records; cluster
  registration 1024 sources; fast list 128 entries.

## 3. Goal and non-goals

Goal:

1. Reduce the number of registered DTAL sources and their fast-list presence where the player cannot
   tell the difference, bounded by a configurable angular size.
2. Make the grouped path affordable on the maps where grouping currently costs frames (it removes
   the parent-count increase's cost) without changing near-field quality.
3. Reuse the machinery landed for cluster rebuild, list updates, incremental composition and the
   task graph; add no second light framework.

Non-goals:

- No change to piece-to-group grouping (Project A) or to list source acceptance/overflow (Project B).
- No new light-sampling framework in the first stage: a representative is a DTAL group parent with
  one synthesized member, so the existing member-sampling contract is kept unchanged.
- No shader changes in stage 1.

## 4. Terms

| Term | Meaning |
|---|---|
| Piece | One admitted emitting polygon fragment (Project A input). |
| Group | Project A parent: pieces of one grid cell/normal/style key. |
| LOD cluster | New representative over one or more groups in one band cell. |
| Band `k` | Distance range `[d0 * 2^k, d0 * 2^(k+1))`. |
| Cell | World-anchored grid cell of the band's size. |
| Visibility class | Visible / off-frustum / not-in-visible-rows (section 8). |

## 5. Bands and screen-space-constant cells

- `d0` = `rt_dtal_lod_start`: merging starts beyond this distance; nearer parents stay individual.
- The screen target is `theta0` = `rt_dtal_lod_screen` (degrees). The cell size of band `k` is
  `s_k = 2 * tan(theta0/2) * d0 * 2^k`; a cell at the band's inner edge subtends `theta0`, at the
  outer edge up to `2 * theta0` — approximately constant by construction.
- A group at distance `d` uses band `k = floor(log2(d / d0))`; its cell is the world-anchored lattice
  cell of size `s_k`. World anchoring (not camera-relative) keeps cluster identities stable while
  the camera moves; the screen-area goal is statistical, not exact.
- `rt_dtal_lod_maxbands` caps `k` so clusters do not grow without bound; beyond the cap every
  eligible group merges at the cap's cell size.

## 6. Merge compatibility

A cluster groups parents that share a key of:

- band `k` and cell;
- signed dominant normal axis (the Project A vocabulary) — so the representative's average normal
  is only formed over same-hemisphere normals and cannot degenerate;
- emission key and style key — a light style is a time function; cross-style merging would need a
  combined function and is refused;
- pass-through class (glass/water) and the projector exclusion (projectors stay singletons, as in
  Project A).

Parents that are alone in their cell stay individual (no singleton clusters).

## 7. Representative synthesis

For a cluster of parents `i` (each with finite power estimate `P_i`, origin `o_i`, area `A_i`,
normal `n_i`):

- power `P = sum(P_i)`;
- origin `c = sum(P_i * o_i) / P` (power-weighted centroid);
- normal `n = normalize(sum(P_i * n_i))` (same-axis subset by construction);
- area `A = sum(A_i)`; bounds radius `R` is the AABB half-diagonal of the members' vertices
  (conservative, same rule as Project A bounds);
- radiance/mask: power-weighted average of the parents' *current* per-frame radiance;
- representation: one synthesized member (quad of area `A` at `c`, oriented by `n`, affine UV from
  the combined bounds) inside a `LIGHT_TYPE_DTAL_GROUP` parent with one member. The member sampler
  sees a singleton member (`p_member` = 1) and `p_source` keeps its existing contract; total power
  is preserved because the emission is the weighted average radiance over the summed area.
- If the synthesized quad is degenerate (`n` near zero, `A` below epsilon), the cell keeps its
  parents individual, and the refusal is diagnosed.

The representative's parameters (colour, mask, animation) are refreshed per frame from the member
parents exactly as group parents refresh from their canonical texture, so animated and lightstyle
content stays live without rebuilding membership.

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
- Cluster identity: a deterministic UID derived from `(band, cell, dominant axis, emission, style)`
  so membership churn updates parameters without resetting temporal history (shadow history, RIS,
  denoiser) and a UID is never recycled for a different cell.
- Stage 1 transitions swap representation in one frame; the perceptual bound is the screen target
  (sources are about `theta0` in size at an edge). If captures show visible pop, stage 2 options are
  a short cross-fade or a distance offset tuned with the same captures.

## 10. Cadence, batching and asynchronous application

The aggregation does not run every frame and does not publish every frame:

- Cadence: the classification and cluster-assignment pass runs on `rt_dtal_lod_interval`
  (default 50 ms, about 20 Hz) from the frame setup stage. Between ticks the published cluster set
  is stable, so at most one membership update becomes visible per interval; hysteresis and dwell
  (section 9) clamp the tick-to-tick changes further.
- Batching: a tick consumes a bounded budget of groups (new and changed groups first, then a
  resumable cursor over the rest), so a single tick cannot stall a frame regardless of the group
  count; groups not yet visited keep their last classification until their turn.
- Smoothing: a camera turn, a teleport or a map change marks the affected groups for
  reclassification, and the dwell plus the bounded budget spread that change over several ticks
  instead of one frame.
- Asynchronous application: under `r_tasks 1` the pass runs as a task with its results
  double-buffered and swapped at a frame boundary; under `r_tasks 0` it runs inline on the tick.
  The ownership, lifetime and lock rules of [docs/multithreading.md](multithreading.md) apply to the
  pass as to any existing light producer, including its adaptation checklist.
- A tick touches the cluster lists only through the registration diff of the groups it changed; a
  tick whose classification did not change costs the classification alone.

## 11. Integration

- Classification and aggregation run in the light setup stage of the frame (the world/light task
  path), `O(groups)` with per-group state, deterministic ordering, parallelizable under `r_tasks`.
- The per-frame diff of the registered source set goes through `RT_ClusterLightAddMulti` and the
  incremental composition: only the clusters whose grants changed are touched, which is exactly the
  path the incremental rebuild landed for moving lights.
- Representatives are dynamic records with the existing frame-slot and pending-copy lifetime; parents
  merged for the frame are not registered (their GPU members stay resident for unmerge).
- Budgets: clusters <= groups <= `rt_dtal_maxgroups`; member storage grows by at most one member per
  cluster (144 B), far under `QR_DTAL_MAX_UPLOAD_MEMBERS`.
- Diagnostics: `rt_light_report` gains cluster counts per band and class, transitions per frame, and
  the source-count reduction against `rt_dtal_lod 0`.

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
| `rt_dtal_lod_interval` | `0.05` | aggregation cadence, seconds |

All archived and invalidating through the existing `rt_require_static_submit`/collect boundaries;
the aggregation itself is per-frame and needs no rebuild.

## 13. Tests and acceptance

Unit tests (extend `tests/rt_lighting_tests.c`):

- band/cell math and deterministic keys/UIDs;
- energy preservation: power, centroid, normal, area are invariant across merge and split;
- hysteresis and dwell state machines: a smooth camera approach produces no oscillation;
- degenerate normal/area fallbacks refuse merging.

Integration:

- Debug build, CTest, Release build per the standing PR gate;
- matched captures on the AD hub, swamp and Fuma, `rt_dtal_lod 0` against `1`, on the shipped
  defaults (`rt_dtal_groups 1`, `rt_cluster_sampling 1`): source counts, cluster fast-list pressure
  (the 128-entry reports), CPU and GPU frame time;
- quality: settled-camera screenshot diffs within the same-build noise floor; a camera walk across
  `d0` and across band edges for pop-in and flicker; a denoiser stability check (no new temporal
  ghosting);
- the performance target: with the LOD on, the grouped defaults reach or beat the legacy-path
  milestone numbers on swamp and Fuma, removing the measured groups-on regression.

## 14. Risks and limitations

- Average-normal fidelity is an approximation; bounded by `theta0` and the axis compatibility rule.
- Occlusion detection is stage-1 conservative (frustum + visible rows); the purely behind-a-wall
  case keeps quality until a stage-2 sampled test is justified.
- Camera-movement list churn is bounded by hysteresis, dwell and the incremental composition, and is
  measured, not assumed.
- Animated/lightstyle parents merge only within their style/emission key, so animation never
  averages across incompatible radiance functions.
- The representative quad over a folded cluster is a flat approximation; the merge key's axis rule
  and the distance gate keep the visible error bounded. If screenshots show the folded case, the
  representative can grow to a small grid of members (a stage-2 option).

## 15. Decisions to confirm with the owner

1. Representative model: synthetic averaged area light (proposed; matches "lights as one light by
   the average normal") or merged member sampling (higher fidelity, heavier).
2. Proposed defaults of `rt_dtal_lod_start` / `rt_dtal_lod_screen` / `rt_dtal_lod_offscreen` —
   accepted as starting points for capture tuning.
3. Release enablement: after acceptance, default on (owner decision), or opt-in.
4. Acceptance scenes: AD hub, swamp, Fuma (plus `e1m7` for the parent-count case)?
