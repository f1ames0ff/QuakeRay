# Particle rendering — improvement plan

Planning draft, corrected after the external review and the verification round. Fixes nothing by
itself; every stage is gated by measurements taken first. Current architecture:
`docs/particle-current.md`; reference: `docs/particle-modern.md`; shortfalls:
`docs/particle-comparison.md`.

## 1. Goal and target scale

Goal: remove the particle bottleneck on the owner's scenes without losing the lighting feature, then
scale to the content that actually exists.

- **T1 committed: 100k rendered particles** on one owner-named scene, <=1 cluster evaluation and
  <=1 budgeted ray per particle, upload <=2.4 MB/frame at a 24 B record.
- **T2 stress: 262,144 particles** (the FTE compiled cap) with a loud, counted overflow policy.
- "Millions" is out of scope; shipped content expresses bursts <=1024 and per-map `particlemax`
  <=2048 as authored data, and the raster collector drops silently at 87381 3-vert particles
  (65535 FTE quads) when otherwise empty. Measure the actual counts in Stage 0 before defending T1.

## 2. Owner decision register

| ID | Decision | Recommendation |
|---|---|---|
| G0 | Confirm T1 scene and numbers; separate synthetic stress scenario | AD `start` saved viewpoint + dense-trail demo + a synthetic 100k test |
| D1 | Identity-map volume policy (start: 6125 visleafs -> 6126 identity clusters) | Bake an R16_UINT volume by painting `leaf_cluster`; keep the CPU resolve behind a flag; 64 u is lossy and must be measured |
| D2 | Resolve removal first? | Stage 0 decides; if resolve is not dominant, prioritize conversion/upload instead of the volume |
| A1 | Volume accuracy contract | Paint `leaf_cluster` per texel (never position->cell arithmetic; tfuma counterexample leaf 12278 -> cluster 1766 vs arithmetic 3118); ambiguous texels follow the CPU rule; keep 0 where the CPU returns 0 (cluster 0 has no PVS row; the sun is ray-gated) |
| A2 | Volume generation | Its own map generation bumped where the clusters rebuild; never `listGeneration` (that ticks per composition) |
| A3 | Volume memory/bounds | 64 u base (start ~195 KiB, tfuma ~2.73 MiB); pin origin/dims/sampling in the upload; 32 u optional |
| L1 | Lighting end state | Per-particle evaluation; cluster sample from the volume/table; delete the per-vertex loop |
| L2 | RT ray budget | Hard per-frame cap, spent deterministically by particle index; <=1 sun/shadow ray per particle at its center, distance-faded, temporally reused keyed on (generation, light revision, cluster) |
| L3 | TLAS membership | Engine particles stay out; they cast no shadows and add no GI; AD sprites and entity beams are a separate case, do not generalize |
| L4 | DTAL compatibility gate | Particle/smoke consumers must use `q2SampleClusterLights` (fast + tail) -> `sampleLightNee` -> divide by `lightPdf * memberPdf`; parity matrices with `rt_dtal_groups {0,1}` x `rt_cluster_sampling {0,1}` |
| D4 | Geometry/draw shape | Classic triangle `draw(3, N)`; FTE geometry varies (line sparks 2, fan/clipped 3, billboards 4); smoke 6 — a per-instance vertex buffer, not a universal quad |
| D5 | Budgets and cvars | Separate budgets for pools, uploads, beams, decals, smoke; `rt_particles_gpu`, `rt_particle_volume` documented; loud overflow |
| D6 | FTE end state | Simulation/PSET/dlights stay CPU; FTE geometry converges in the deferred stage; classic gets GPU sim first |
| D7 | `SMOKE_CLUSTER_SCAN` | Keep 16 until the GPU pass is measured; after dtal choose fast/tail/group sampling deliberately |
| D8 | Legacy-path removal | Define a parity suite (demos, maps, vendor matrix, counters) and thresholds; not just "two releases" |

## 3. Stages

### Stage 0 — measurement (mandatory gate)
- Record demos at a fixed `setpos`: `ad_particle_heavy`, `ad_particle_idle`, plus a synthetic
  particle test; commit control configs under `perf/`; keep demos/logs out of git.
- Counters: live counts per path (classic/FTE/smoke) and dropped counts; upload bytes; per-resolve
  ns + cache hit/miss; vertex-ray counter (append a trailing `raysParticle` field to `QrFrameStats`
  or bump `QR_API_VERSION`; do not grow `raysPerCategory[5]` in place); GPU particle pass timer.
- Split `RT_PROF_PARTICLES` and add sub-slots; dump `rs_particles` (r_part.c:985); note that classic
  and smoke simulation run outside the draw bracket (host.c:1008-1011).
- Ablations: `r_particles {0,2}` x `r_particle_lighting {0,1}` x `r_smoke {0,1}` x
  `r_fteparticles {0,1}`, plus `-particles`/`r_part_maxparticles` (reinitialization scenario;
  live resize does not exist, r_part_fte.c:3383-3404). Account for fallbacks: `r_fteparticles 0`
  moves effects to classic, `r_particles 0` stops FTE sim and sky weather, `r_smoke 0` changes
  classic trail substitution (r_part.c:675-685).
- Statistics: report p50/p95 per frame once true per-frame instrumentation exists; the current CSV
  holds window maxima only.
- Exit gate: a signed attribution table (resolve vs expansion vs upload vs FTE conversion vs smoke
  vs sim) with build hash, pinned config, map/content hashes, and the capture interval.

### Stage 1 — repair before redesign
- FTE conversion and scratch memset bounded by live counts (`cl_numstrisvert`/`cl_numstrisidx`), not
  capacity (r_part_fte.c:6838-6857); keep the empty-scenetri early-out.
- Overflow policy: recycle-oldest with counters and a rate-limited warning; remove the silent return
  and the Debug-only assert path (RasterizedDataCollector.cpp:221-231).
- If Stage 0 says the resolve dominates: land the point-cluster cache (uncommitted, gl_rlight.c) with
  a cvar and hit/miss counters, before the dtal merge (gl_rlight.c is dtal-owned); otherwise revert
  it. The cache is not correctness-preserving at 16 u cell boundaries and must not ship silently.
- Gate: particle slot down without visual change on `start` and `ad_tfuma`.

### Stage 2 — compact points + vertex-shader expansion (particle-owned, no set-6 changes)
- `QrParticlePoint { float position[3]; uint32_t packedColor; float size; uint32_t cluster; }`
  = 24 B -> 2.4 MB at 100k, a 10x cut vs 240 B classic; `flags` has no Stage-2 consumer; stable ID
  appends a fifth word or packs into a spare bit field in a later stage; `cluster` is required.
- Transport: a per-instance vertex buffer consumed by `draw(3, N)`; the cluster volume lives in a
  particle-owned descriptor set as a sampled image. **Set 6 stays dtal-owned** (bindings 0-8 legacy,
  9-11 DTAL members/tail); no additions to set 6.
- Classic first; keep per-vertex ray behavior for parity until Stage 4.
- Gate: <=5% fps / <=10% slot regression on both demos; upload-byte counter; caps raised loudly.

### Stage 2 — implementation notes (pending measurement)
- The new public API: `QrParticlePoint` (24 B: position, packedColor, size, cluster),
  `QrParticleUploadInfo` and `qrUploadParticles`. Classic sprites upload one point per particle
  through a per-instance buffer; the new `RsParticlePoints.vert.hlsl` expands the point into the
  triangle, replacing the 3-vertex `QrVertex` transport.
- `r_particles_points` (default `1`, archived) selects the compact-point transport for classic
  triangles; `0` restores the legacy transport for the A/B arm. `QUAD_PARTICLES` always falls back
  to legacy.
- FTE and smoke keep the legacy `QrVertex` path; the traced glass stand-ins of the classic sprites
  are kept for the new path.
- The gate above is unchanged and still awaits the Stage-0 demos (`ad_particle_heavy` /
  `ad_particle_idle`); no Stage-2 measurement is claimed yet.

### Stage 3 — cluster volume, remove CPU resolves (after dtal; accuracy contract A1-A3)
- R16_UINT volume painted from `leaf_cluster`; 64 u base; own map generation; keep 0 semantics.
- Replace every CPU resolve for classic/FTE/smoke with a volume sample plus the CPU-cluster fallback
  behind a cvar.
- Known precision: one 64 u texel on start holds clusters 2 and 4; tfuma engine cells are ~358 u.
  Mismatch counters are required; the visual tolerance is an owner decision.
- Gate: parity on `start` (identity, 6126 clusters) and `ad_tfuma` (grid, 18806 visleafs -> 7936
  clusters); resolve cost ~0.

### Stage 4 — per-particle lighting, DTAL gate, ray budget
- One cluster/light evaluation per particle; <=1 budgeted ray per particle under a hard per-frame
  cap; delete the per-vertex loop (RsParticle.vert.hlsl:34).
- DTAL gate (L4): the smoke decoder currently misreads groups and drops them; implement the direct-
  pass pattern and prove parity with `rt_dtal_groups`/`rt_cluster_sampling` matrices.
- Gate: bounded ray counter; visual parity; `rt_bench` baselines unchanged or better.

### Stage 5 — GPU simulation (classic only, optional until measured)
- Ping-pong state, spawn ring, append/compaction, indirect draw; bench-freeze mode for determinism
  (fixed seed, fixed step, culling off); CPU remains the authority for dlights and PSET effects.
- Gate: demo/bench determinism, 1M stress bounded with loud overflow, FTE untouched.

### Stage 6 — deferred convergence and quality (named future work)
- FTE geometry/lighting convergence, smoke on the volume, culling, sorting/transparency, soft
  particles, particles in the TLAS, froxel ambient; each enters only when a measurement names it.
- Voxel smoke: the `feature/voxel-smoke` branch overlaps r_smoke.c, gl_vidsdl.c, render.h, qray.h,
  VulkanDevice.*, NvrhiFrameSkeleton.*, RhiRasterOverlayPass.*; serialize rebases with Stage 0
  counters and the smoke work.

## 4. Interfaces (sketch, corrected names)

```cpp
struct QrParticlePoint { float position[3]; uint32_t packedColor; float size; uint32_t cluster; };
struct QrParticleUploadInfo { const QrParticlePoint *pPoints; uint32_t count;
                              QrMaterial material; uint32_t pipelineState; QrFloat4D smokeLook; };
QrResult qrUploadParticles(QrInstance, const QrParticleUploadInfo *);
struct QrClusterVolumeUploadInfo { QrFloat3D worldMins; float invCellSize; QrExtent3D dims;
                                   const uint16_t *pCells; uint32_t generation; };
QrResult qrUploadClusterVolume(QrInstance, const QrClusterVolumeUploadInfo *);
```

`qray.h` appends; it never renumbers existing bindings, struct fields or enum values. If
`QrFrameStats` must grow, append a trailing field or bump `QR_API_VERSION` together with
`RayStats.h`, `ShaderCommonC.h`, `GenerateShaderCommon.py` and `VulkanDevice.cpp`.

## 5. Branch order and ownership

1. `bugfix/present-wait-removal` (commit 50910a89, Swapchain.cpp only) to master.
2. `refactor/dtal-cluster-dedup` to master (owns gl_rlight.c, ClusterLightLists.*, LightManager.*,
   r_world.c cluster building and the set-6 layout).
3. Particle work rebased: Stage 0 touches host.c, gl_vidsdl.c, glquake.h, RayStats, generated
   bindings, qray.h and the dumps; Stage 1 touches gl_rlight.c (coordinate with dtal); Stage 2
   touches qray.h/qray.cpp, VulkanDevice.*, the collector and r_part*; Stage 3+ follow dtal
   interfaces; set-6 changes are serialized and currently avoided.
4. Docs and changelog per stage; update `perf/README.md` with the canonical demos.

## 6. Regression guard and evidence

- `perf/compare_benchmark.ps1` today: default tolerance 20%, compares block averages, and silently
  skips metrics missing from the baseline. Strengthen before Stage 2: explicit metric list, p95
  support, fail on missing columns, and a pinned settings witness in `benchmark.log`.
- Evidence manifest per stage: commit, pinned config, map/content hashes, GPU/driver, internal
  resolution, demo hash, capture interval. No stage is accepted on owner observation alone.

## 7. Risks

- The "40 ms" figure is a window maximum; the plan must not promise numbers Stage 0 has not
  confirmed.
- Volume precision on identity maps and near solid/leaf boundaries; mismatch counters and an owner
  tolerance decision are required.
- DTAL consumer correctness (empty samples for groups on the current particle path) is a hard gate.
- Dual paths drift; bound with counters, parity demos and removal criteria.
- Driver hazards: push-constant 128 B floor, R16_UINT sampling, per-slot ring sync, AMD TDR history.
