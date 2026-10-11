# Branch hand-off: `perf/particle-cluster-cache` (2026-10-11)

Audience: agents working on `perf/rhi-dyn-blas-clean`, `perf/rhi-dyn-blas-reuse`,
`refactor/dtal-cluster-dedup` and similar branches. This document says what this branch contains,
how to merge it, what conflicts to expect, how to resolve them, and what to verify afterwards.

## 1. What the branch is

`perf/particle-cluster-cache` is 64 commits over `origin/master` (`0c04f86f`; 60 non-merge plus
4 merges), carrying the particle plan stages and the cluster publication line:

- **Stage 2** — classic particles move to 24 B `QrParticlePoint` instances
  (`r_particles_points`, `RsParticlePoints.vert`), the point capture path in the collector and the
  point draw list in the raster overlay; FTE keeps the legacy vertex transport.
- **Stage 3** — the cluster light list path (`ClusterLightLists.*`, `LightManager.*`): CPU
  resolves replaced by list composition, incremental updates, the deterministic order, the sliced
  publication (`clust upload`/`clust publish` rows) and the per-slot light-upload capture.
- **Stand-in gate** — `rt_particle_proxy_gate` (auto closes the proxy capture when the last
  completed frame traced no reflect/refract ray and submitted no `PT_GLASS` instance; the
  always-on ray mark in the raygen; the collector early-outs; the device policy).
- **Water composite** — the raygen mask branch accepts `MEDIA_TYPE_WATER`, so the traced stand-in
  layer composites at water pixels (and the raster discard fires there), not only at glass panes.
- **`r_tasks` default flip** — `1` (owner decision 2026-10-11); `r_tasks 0` deprecated, the cvar
  and its fork slated for removal once the serial path is retired.
- Records and contracts: `perf/stage0/*-2026-10-10.md` and `*-2026-10-11.md` (cluster publication,
  volume, point gate, glass gate, water reflection, dark-smoke investigation), `docs/particle-plan.md`,
  `docs/multithreading.md`, `ARCHITECTURE.md` rows, `changelog.md` Unreleased.

Key recent commits: `505e122c` `5cfe95fe` `c8a6a945` (cluster batches 1-3), `742c44e0` `c2313664`
`b70350c8` `955f3a77` (review fixes/pointers), `952bd1c7` `de7c133e` (measured falsifications),
`7bbcc9c1` `946ed997` (glass-gate v1/v2), `08d11d30` (water composite), plus the r_tasks and docs
commits at the tip.

## 2. Merge mechanics

```powershell
git fetch origin perf/particle-cluster-cache
git merge origin/perf/particle-cluster-cache
```

Per target branch (computed against `origin/master` on 2026-10-11):

| Target | Files both sides changed vs `origin/master` | Merge reality |
|---|---|---|
| `perf/rhi-dyn-blas-clean` | `ARCHITECTURE.md`, `PERFORMANCE.md` only | near clean; docs combine |
| `perf/rhi-dyn-blas-reuse` | + `renderer/Source/RHI/NvrhiFrameSkeleton.cpp` | the actual conflict is `ARCHITECTURE.md`; the skeleton file auto-merges |
| `refactor/dtal-cluster-dedup` | 14 files (cluster/light area) | the list is what **master** changed after the DTAL fork point; the DTAL ref itself only changed `tests/perf/run_stress.ps1` — the real conflict is that script |
| `feature/voxel-smoke` | `renderer/Include/qray/qray.h`, `RHI/NvrhiFrameSkeleton.cpp`, `RHI/RhiRasterOverlayPass.{h,cpp}` | real conflicts; keep this branch's particle-point staging and gate plumbing and merge the smoke work around it |
| `perf/alias-pose-dag` | `Quake/gl_rmain.c` | likely a conflict in the task-graph area |
| `feature/water-caustics` | 12 files incl. modify/delete of GLSL sources removed since | older branch; re-derive after any refresh |

Computed 2026-10-11 with `git diff --name-only` plus a `git merge-tree` simulation; recompute after
any target branch is rebased, refreshed or re-extended.

### 2a. `perf/rhi-dyn-blas-clean` (dynamic BLAS shape/reuse)

Code overlap: none. The target rewrites `RhiAccelStructs.{h,cpp}` / `DynamicBlasShape.h` and the
tests; this branch does not touch those files. Interact only through runtime behaviour:

- The particle proxy BLAS (`RhiAccelStructs::BuildParticleProxies`) is built and rebuilt per frame
  under the auto gate; after merging, re-verify that the dynamic-BLAS shape-fit/reuse policy does
  not change that path (build the merged binary and run the water stand below).
- Docs: `ARCHITECTURE.md` — keep this branch's particles row (gate/water wording) **and** the
  target's dynamic-BLAS rows; `PERFORMANCE.md` — keep both sections.

### 2b. `perf/rhi-dyn-blas-reuse`

`NvrhiFrameSkeleton.cpp`: this branch adds the particle-point staging wrap, the ray-stats set for
the smoke pipeline and the gate hand-off; the target's changes are elsewhere in the file — keep
both when resolving; run the particle stands below.

### 2c. `refactor/dtal-cluster-dedup`

The shared-file list above is **not** a conflict set: those 14 files are what `origin/master`
changed after the DTAL branch's merge base (`8ee2b72a`), while the DTAL ref's own delta since then
is `tests/perf/run_stress.ps1` only — its cluster work is already in `origin/master`. Merging this
branch into the current DTAL ref therefore conflicts in `tests/perf/run_stress.ps1` alone; resolve
by combining the DTAL runner scenarios with the current runner and rerunning the DTAL evidence.

If the DTAL branch is later rebased or re-extended onto the current master, recompute the overlap
first. If it then re-touches the cluster/light files, keep this branch's structure — Prepare /
16 publish slices / Commit, the `cluster_publish_task`/`cluster_commit_task` nodes, the per-slot
light-upload capture and flush (`qrBeginDeferredLightUploads`/`qrFlushDeferredLightUploads`), the
deterministic registration order, the device-first skip and the `QR_CLUSTER_PUB_*` diagnostics —
and merge the DTAL semantics into the corresponding stage rather than replacing the structure.
Keep the L4 DTAL parity matrices (`docs/particle-plan.md`) as an acceptance obligation: a green
publication assert run alone is not parity.

## 3. Post-merge verification protocol (Debug)

1. `.\build_win.ps1 Debug` (exit 0; shaders rebuild and deploy).
2. Harness stands (one game instance only):
   - `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1 -Save qr_ad_start` (gate closes; expect
     ~zero proxies, `particles upload` ≈0.25 ms, `fte convert` ≈1.0 ms) and
     `… -Extra @('rt_particle_proxy_gate 0')` (capture on: ≈11.9k proxies, upload ≈2.1 ms).
   - `-Save qr_ad_swamp` (water; expect a closed→open gate transition, rays_refl_refr ≈46k, the
     capture on).
   - `%LOCALAPPDATA%\Temp\opencode\run_water_shot.ps1` (the owner's `qr_start_water.sav`; the flame
     reflection appears with `rt_glass_particles 1`, disappears with `0`).
   - `perf/stage0/run_points_demo_ab.ps1 -Demos ad_particle_heavy -Arms points_on
     -ExtraCvars 'r_tasks 1; rt_particle_proxy_gate 1'` (4K demo; `gate=closed` lines, slots near
     0.2/1.0 ms).
3. Check the merged behaviours that this branch owns: the cluster publication columns
   (`clust upload/publish`, skips/copies), the `cpu.wait_ms` > 0 task-mode signature with the new
   `r_tasks` default, the proxy BLAS present on the water view (TLAS instance count 8 with the
   gate open vs 7 closed), no validation errors, and the DTAL parity matrices.
4. Compare against the records' baselines (`perf/stage0/*-2026-10-10.md` / `*-2026-10-11.md`);
   import cross-build numbers as context only. Preserve the run fixture and the config backup in
   the evidence tree — the default-flip verification (`rtasks-check-1`) lost its fixture, so its
   "unpinned" premise stays re-auditable only through the recorded generator and the behavioral
   signature.

## 4. Contracts to preserve

- **Gate**: water and glass are always traced. Auto closes when `rt_glass_particles` is off, or when
  the last built instance set has no `PT_GLASS` instance **and** the last completed frame traced no
  reflect/refract ray; the ray mark is always-on (plain store with the debug counters off). Closed
  state is self-consistent: no proxies → no BLAS/TLAS instance → `glassParticles` 0 → no
  query/discard/composite.
- **Water/glass layer**: the mask (`framebufQ2GlassFilter.a`, written for `MEDIA_TYPE_GLASS` and
  `MEDIA_TYPE_WATER`) gates both the composite (`CmCheckerboard`) and the raster discard
  (`RsParticle.frag`, `RsWorld.frag`); `.z` carries the surface view depth.
- **Capture exceptions** (by design, documented): engine smoke (`SMOKE` state), per-channel FTE
  blends (`BM_BLENDCOLOUR`/`BM_SUBTRACT`/`BM_INVMODC`), line sparks (`BEF_LINES`) never enter the
  stand-ins; mirrors have no water/glass mask writer.
- **Cluster publication**: deterministic order, sliced resolve, per-slot capture; the
  `QR_CLUSTER_PUB_*` mask and the skip/copy counters are the diagnostics.
- **Default flip obligations**: the M5 scene/transition matrix in `docs/multithreading.md`
  continues against the default-on task path; a material regression there reopens the flip.
- **`r_tasks`**: default `1`; `0` deprecated; the cvar and the serial fork are slated for removal
  — do not build new features on the `r_tasks 0` path. Removing the fork itself additionally
  requires deciding the callers kept serial on purpose (console/menu/loading, the tris/bbox debug
  views) and single-worker hosts, so it is a scoped follow-up rather than a delete.

## 5. Known limitations and open items

- The owner scoped "all particles reflect" (smoke/per-channel/lines stand-ins) out of scope; the
  remaining exclusions above stand until that item is taken up.
- The gate's water/mirror entry window is 2-3 frames (stats-ring lag). Observable at water since
  the water composite landed (a newly visible water surface uses the raster copies for the first
  frames; the traced layer arrives with the gate); mirrors have no mask writer and stay raster-only.
- The water mask also feeds `rt_glass_denoise`: water pixels now read as pane-like (mask value and
  motion vectors); the denoiser-on check on a water scene is still pending (the owner's config has
  the denoiser off).
- The set-7 `smokeRayStats` alias in the reflect/refract blob (cubemap table vs stats buffer) is a
  known dormant issue (`docs/particle-plan.md`); fix by moving the smoke stats set.
- `perf/stage0/dark-smoke-investigation-2026-10-11.md` records why the darker-smoke reports were
  measurement artifacts, not a regression (no code change made).
- Evidence trees from this work live under `%LOCALAPPDATA%\Temp\opencode` (`gg*`, `wg-*`, `cl-b*`,
  `fl*`/`cc*` as imported history) and in `build/Debug/ad/benchmark.log`.

## 6. Remaining plan items (not delivered by this branch)

From `docs/particle-plan.md`, the owner queue (2026-10-10/11) and the final-review follow-ups;
everything not listed here is delivered by this branch or by master.

- **Compact FTE transport** — the queue's next item and Stage 2's remaining half: FTE particles
  still use the legacy 80 B `QrVertex` transport. The designed replacement is a 28 B vertex record
  with its own upload path (no shader changes, A/B behind a planned `r_particles_fte_compact`).
- **Distance culling** — the queue's third item; not scoped in detail yet.
- **Stage 4b — DTAL parity (L4 hard gate)** — the smoke decoder still misreads light groups and
  drops them; the direct-pass pattern and the `rt_dtal_groups` x `rt_cluster_sampling` parity
  matrices are unimplemented, parked behind the radiance-cache rework (owner, 2026-10-10).
- **Stage 4a/4c/4d backlog** — the GPU cluster volume, the L1 scale semantics on the synthetic 100k
  scene, and the L2 per-frame ray budget/temporal reuse (needs stable particle IDs and a
  GPU-visible light revision).
- **Radiance-cache rework (future cycle)** — particles sample a world-space cached lighting instead
  of per-particle evaluation (froxel/SHaRC direction; FSR Radiance Cache is unavailable on Vulkan);
  it inherits the cross-check findings (direction-ignoring weight, `dw=0` single candidate,
  missing alpha any-hit).
- **Stage 6 named future work** — particles in the TLAS, froxel ambient, culling,
  sorting/transparency, soft particles, FTE convergence, smoke on the volume; coordinate with
  `feature/voxel-smoke`.
- **Stage 5 (GPU simulation, classic)** — optional until measured; not started by design.
- **Cluster-volume accuracy** — the owner visual-tolerance decision for the 64 u volume (mismatch
  ~43 %, 6 % "lost" on `start`) is open; the one-line zero-fallback (volume 0 -> exact resolve)
  and the bake-time fill for zero cells are ready candidates.
- **Water/gate follow-ups** — the 2-3 frame entry window on a newly visible water surface, and the
  pending water x `rt_glass_denoise` check (the water mask reads as pane-like).
- **Out-of-scope remainder (owner, 2026-10-11)** — stand-ins for the engine smoke, the per-channel
  FTE blends and the line sparks; plus the mirror water/glass mask writer.
- **Small items** — per-frame p50/p95 instrumentation (the dump still holds window maxima only); a
  strict fail-on-missing-columns mode for `perf/compare_benchmark.ps1` (p95 support exists); the M5
  scene/transition matrix that continues against the default-on task path.
