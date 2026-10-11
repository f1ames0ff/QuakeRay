# Upstream porting synthesis (frozen)

Status: frozen 2026-10-11. The workstream was paused before the PR #41 merge and is resumed after it.
Scope: transfer of useful solutions from upstream vkQuake into QuakeRay. The synthesis below is the
agreed basis for the P0/P1, measurement and Tier-A stages.

## Method

- Eight solver reports plus four independent verifiers; discrepancies resolved by source evidence, not
  by vote.
- The fork base is ≈1.20.3 (confirmed independently, verifier V04).
- Corrections adopted into the design before freezing:
  - `dynamicUniqueIDToSimpleIndex` is not dead code: it carries duplicate-drop semantics
    (V02/V03/V04 unanimous).
  - The "128 static entity cap" claim is wrong; the array grows (+64).
  - The AABB skip by ID is unsafe: alias entities reuse IDs with new vertices.
  - Profiler aggregation would change the published `max` semantics; the per-invocation maximum must
    be preserved (counter + max). This is a requirement on the P1 diagnostics.
  - `RT_StaticMovable` excludes sky, warp, animated and entity-owned geometry, so H08 as "reuse of the
    mechanism" does not work.
  - `SV_Gibs_OnCall` is gameplay, not tooling.
  - Commit `201a214d` does not exist; the real commits are `fdec988c` and `021a214d`.
  - Renderer-specific commits do not "plain apply".

## Classification

| ID | Item | Classification | Adoption decision |
|----|------|----------------|-------------------|
| H01 | rs_* atomic batching | CONFIRMED (mechanism) | Take: per-list accumulation for sky polys/brush surfaces (gl_sky.c:998/1252/1356, r_brush.c:359); alias sites are already per-entity. Effect to be measured. |
| H02 | mark/cull batching + relaxed OR | CONFIRMED (conditional) | Take: word-flush + worker-scaled batches; add `_Relaxed` helpers; surfvis identity must be proven. |
| H03 | slice work-stealing | CONFIRMED (structural, opaque-only) | Conditional: only if measurements show slice imbalance; alpha stays a single slice; CBX content changes, verify. |
| H04 | editor RT_TRACK | CONFIRMED (safe) | Take, but with a real generation epoch (the naive skip leaves stale `ready`). |
| H05 | profiler spinlock | REFUTED (aggregation semantics) | Do not "fix" via aggregation: preserve the per-invocation maximum (counter + max) — requirement for the P1 diagnostics. |
| H06 | entity unique ID cache | NOT ESTABLISHED | Needs a link-time hook plus invalidation; defer. |
| H07 | upload registry trimming | REFUTED (self-serving), rest N/E | Drop the AABB skip and the registry-map removal from the candidates; only Debug lookup consolidation remains. |
| H08 | frame-invariant geometry | REFUTED (as reuse) | Needs a new design coordinated with the BLAS lane; not first echelon. |
| H09 | sky skip flags | CONFIRMED | Take, but `texofs`/`used_specials` do not exist here — build them at load. |
| H10 | FTE particles | CONFIRMED (classification) | Handoff → calm-eagle: ≈8.6 MB/frame (initial caps), lock per batch, proxies under the lock. |
| H11 | alias pipeline | CONFIRMED (mechanism) | CPU fusions are possible; GPU materialization needs BLAS/cluster coordination; watch the pointer lifetime in the surface loop. |
| H12 | server | devstats CONFIRMED; gibs REFUTED | Take the devstats gate (with `developer`, as upstream); do not touch gibs. |
| H13 | emissive diagnostics | CONFIRMED + race found | Gate only AFTER fixing the writer ownership (entity tasks vs world memset without a DAG edge). |
| H14 | dead writes + no-op task | CONFIRMED | Take: `update_warp`, `lightmaps[].modified`, `R_UpdateLightmaps` (with the DAG edge rewired). |
| H15 | task queue | facts CONFIRMED, cost N/E | Take the robustness assert (we have no deadlock boundary); queue rewrite after a microbench. |
| H16 | commit catalog | 18/20 confirmed | Fix two hashes; exclude the RNG port (e6926080) — it changes simulation sequences. |

## Cross-conclusion (decisive)

No candidate can be ranked by contribution to the 22 ms until there are measurements without the
overlay and without mixing "work" against "wait on `geometryUploadMutex`". The screenshot maxima were
collected with the profiler on — a confirmed observer effect. The order is therefore unchanged:
P0/P1 diagnostics (with the H05/H13 corrections) → measurements → Tier-A candidates step by step,
each with its own deep-reasoning run.

## Proposed order

1. P0/P1 — the Medium profile in the runner plus diagnostics (S2/S3 designs with the verifier
   corrections; the missing S1/S4/S6 parts: wait for them or restart locally).
2. Measurements — heavy `ad_swampy` and fast `start`, `r_tasks 0/1`, overlay off/on → ranking.
3. Tier A (small risk): H01, H02, H04, H09, H12-devstats, H13 (with the fix), H14, H15-assert,
   Debug lookups.
4. Tier B/C — per the measurements and with lane coordination (H03, H06, H08, H11).

## Progress since the synthesis (this branch)

P0/P1, done:

- Medium acceptance profile: `tests/perf/qr_medium.cfg`, `run_stress.ps1 -RtProfile audit|medium`,
  and the `qr_ad_swamp` save in the allow-list.
- Per-thread profiler accumulation that preserves the published per-invocation maxima (the H05
  correction is honored); removed the per-invocation `RT_Prof_End` spinlock.
- Upload-lock wait/hold probe with per-producer lanes (`rt_upload_trace 1`;
  `upload.lock`/`upload.lane` rows in the bench block), which separates work from wait.
- Per-type upload lanes (brush/sprite/alias, plus `alias surf` for the enhanced md5/md3 path) and the
  alias enhanced-path draw counter.
- The alias pose probe (draws/static/repeat): on swamp only 116-136 of about 750k draws are static
  poses and 6-8 repeat one, so an unchanged-pose alias cache is a dead end on these scenes.
- World-sky batching (one upload per chain instead of per surface; sky lane calls 257 to 7 on swamp).
- Skip-covered-packing (C2): `brush pack` 8.96 to 0.64 ms on swamp (-93%) and 5.68 to 1.09 ms on Fuma
  (-81%) together with `rt_brush_persistent`.
- Per-surface md5/md3 vertex slices (`firstvertex_vbo`, rebased `rtindices`).

Measurements, done:

- Fuma and swamp at the audit profile with `r_tasks 1` and `r_tasks 0` (two runs each): task-mode
  worker sums are 3-12x inflated by the upload-lock convoy; the red sky/alpha rows are queue artifacts;
  alias uploads are one per visible entity and re-lerp every frame.
- Heavy swamp and fast Fuma at 4K Medium (FSR Balanced): GPU-bound, frames 24.24/19.74 ms against the
  16.7 ms budget; the 4K Medium GPU breakdown is in `docs/fuma-cpu-optimization-plan.md`.
- Persistence A/B with and without C2 on both maps.

Measurements, missing:

- Overlay-off captures (the observer-effect check) and the fast `start` map capture.
- The same-message acceptance ledger with a focused window.

Corrections still open:

- H13 writer ownership landed with the Tier A slice (atomic `rt_emis_stats` fields plus a spinlock
  around the `rt_emis_watch` table); the earlier "no DAG edge" note is superseded by the verified fact
  that the `draw_world_task -> entities/alpha` edges already existed.
- The `rt_brush_persistent` enablement decision (profile vs default, mover visual check) is pending.

Focus caveat (measured): `main_sdl.c` sleeps 16 ms per frame while the game window has no input focus,
so every unfocused capture shows `interval = profiled frame + about 16-18 ms`. CPU slots and GPU pass
timers stay valid; the acceptance ledger must run with a focused window (the runner's focus gate exists
for exactly this reason).

### Tier A port (first slice, landed 2026-10-11)

- H02 was refuted as a live mechanism during verification: the SIMD mark/cull/chain block is compiled
  out in `r_world.c` (preprocessor, PDB and frame-CSV evidence), so it moved to a separate
  parallel-marking design task instead of the port list.
- Landed: H15 (task-queue sizing invariant `MAX_EXECUTABLE_TASKS = MAX_PENDING_TASKS * MAX_WORKERS`
  with the upstream-style compile-time asserts); H14 (removed the dead `update_warp` writes and the
  `lightmaps[].modified` field, the no-op `R_UpdateLightmaps` task and its DAG edges;
  `r_gpulightmapupdate` stays for the lightstyle interpolation); H01 (per-call batching of
  `rs_brushpolys` in `R_DrawBrushModel`; the sky sites are single-task and were left unchanged);
  H12 (devstats edict-walk gate); H04 (prefix clearing in `RT_TRACK_BeginFrame` instead of the 28 KB
  memset); H09 entity side (load-time per-model `used_specials` for the sky-entity skip; the world
  texture list/`texofs` was left out); H13 ownership fix (the emissive diagnostic counters are atomic;
  the gating itself was dropped as unmeasurable and UX-changing); the Debug duplicate-find
  consolidation in `Scene::Upload`/`GeomInfoManager` (the release-path duplicate policy in
  `VulkanDevice` is untouched).
- Infrastructure: `run_stress.ps1` validates the clouds quality per profile (so a `-RtProfile medium`
  run no longer throws), and the bench settings block records `r_tasks` and `r_parallelmark`.
- Build and CTest green; the per-item acceptance captures belong to the following measurement stage.

## References

- `docs/fuma-cpu-optimization-plan.md` — the live plan and changelog with all measured numbers.
- Commits: 79382c2a (per-thread profiler accumulation), 19d1ca2b (upload-lock wait/hold probe),
  57f7c465 (world-sky batching), 30957b1b (C2, type lanes, per-surface slices), 0b98fa78 (Medium
  profile and the swamp save in the runner).
