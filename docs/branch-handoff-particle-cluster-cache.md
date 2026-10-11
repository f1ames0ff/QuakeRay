# Branch hand-off: `perf/particle-cluster-cache` (2026-10-11)

Audience: agents working on `perf/rhi-dyn-blas-clean`, `perf/rhi-dyn-blas-reuse`,
`refactor/dtal-cluster-dedup` and similar branches. This document says what this branch contains,
how to merge it, what conflicts to expect, how to resolve them, and what to verify afterwards.

## 1. What the branch is

`perf/particle-cluster-cache` is 61 commits over `origin/master` (`0c04f86f`), carrying the
particle plan stages and the cluster publication line:

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

| Target | Files both sides changed | Conflict expectation |
|---|---|---|
| `perf/rhi-dyn-blas-clean` | `ARCHITECTURE.md`, `PERFORMANCE.md` only | near clean; docs combine |
| `perf/rhi-dyn-blas-reuse` | + `renderer/Source/RHI/NvrhiFrameSkeleton.cpp` | one real file |
| `refactor/dtal-cluster-dedup` | 14 files (cluster/light area) | expect real conflicts |

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

### 2c. `refactor/dtal-cluster-dedup` (expect conflicts)

Shared, conflicting files: `Quake/gl_rlight.c`, `Quake/gl_rmain.c`, `Quake/gl_vidsdl.c`,
`Quake/r_world.c`, `Quake/gl_heap.c`, `Quake/glquake.h`, `Quake/host.c`,
`renderer/Source/LightManager.cpp`, `renderer/Source/VulkanDevice.{h,cpp}` and the docs.
Resolution rules, in order of precedence for the merged behaviour:

1. Keep this branch's publication structure — Prepare / 16 publish slices / Commit, the new tasks
   (`cluster_publish_task`, `cluster_commit_task`), the per-slot light-upload capture and flush
   (`qrBeginDeferredLightUploads`/`qrFlushDeferredLightUploads`), the deterministic registration
   order, the device-first skip and the `QR_CLUSTER_PUB_*` diagnostics. The DTAL branch's dedup
   work sits inside the light-registration and decode paths — merge those semantics into the
   corresponding stage of this structure rather than replacing the structure.
2. Keep the L4 DTAL parity matrices (`docs/particle-plan.md`) as an acceptance obligation: run
   them on the merged binary; a green publication assert run alone is not parity.
3. `docs/multithreading.md` — keep the r_tasks default/deprecation text from this branch; merge
   any new DAG edges from the DTAL branch.
4. Where the same function conflicts irreconcilably, prefer the version matching the merged
   task graph and file a follow-up for the other side — do not silently drop the DTAL work.

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
   import cross-build numbers as context only.

## 4. Contracts to preserve

- **Gate**: water and glass are always traced. Auto closes only when the last completed frame
  traced no reflect/refract ray **and** submitted no `PT_GLASS` instance; the ray mark is
  always-on (plain store with the debug counters off). Closed state is self-consistent: no
  proxies → no BLAS/TLAS instance → `glassParticles` 0 → no query/discard/composite.
- **Water/glass layer**: the mask (`framebufQ2GlassFilter.a`, written for `MEDIA_TYPE_GLASS` and
  `MEDIA_TYPE_WATER`) gates both the composite (`CmCheckerboard`) and the raster discard
  (`RsParticle.frag`, `RsWorld.frag`); `.z` carries the surface view depth.
- **Capture exceptions** (by design, documented): engine smoke (`SMOKE` state), per-channel FTE
  blends (`BM_BLENDCOLOUR`/`BM_SUBTRACT`/`BM_INVMODC`), line sparks (`BEF_LINES`) never enter the
  stand-ins; mirrors have no water/glass mask writer.
- **Cluster publication**: deterministic order, sliced resolve, per-slot capture; the
  `QR_CLUSTER_PUB_*` mask and the skip/copy counters are the diagnostics.
- **`r_tasks`**: default `1`; `0` deprecated; the cvar and the serial fork are slated for removal
  — do not build new features on the `r_tasks 0` path.

## 5. Known limitations and open items

- The owner scoped "all particles reflect" (smoke/per-channel/lines stand-ins) out of scope; the
  remaining exclusions above stand until that item is taken up.
- The gate's water/mirror entry window is 2-3 frames (stats-ring lag); invisible today, revisit
  with any water/mirror display work.
- The set-7 `smokeRayStats` alias in the reflect/refract blob (cubemap table vs stats buffer) is a
  known dormant issue (`docs/particle-plan.md`); fix by moving the smoke stats set.
- `perf/stage0/dark-smoke-investigation-2026-10-11.md` records why the darker-smoke reports were
  measurement artifacts, not a regression (no code change made).
- Evidence trees from this work live under `%LOCALAPPDATA%\Temp\opencode` (`gg*`, `wg-*`, `cl-b*`,
  `fl*`/`cc*` as imported history) and in `build/Debug/ad/benchmark.log`.
