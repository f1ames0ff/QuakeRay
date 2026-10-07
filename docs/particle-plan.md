# Particle rendering — improvement plan

Planning draft. Fixes nothing by itself; every stage is gated by measurements taken first. Current
architecture: `docs/particle-current.md`; reference architecture: `docs/particle-modern.md`;
shortfalls: `docs/particle-comparison.md`.

## 1. Goal and target scale

Goal: remove the particle bottleneck on the owner's scenes without losing the lighting feature, then
make the path scale with the content that actually exists.

Target scale (defensible, expressible by content and budgets):
- **T1 committed: 100k rendered particles** on one owner-named scene, lighting <=1 cluster
  evaluation and <=1 budgeted ray per particle, CPU resolve/geometry cost not proportional to count.
- **T2 stress: 262,144 particles** (the FTE compiled cap) with a defined overflow policy and bounded
  upload (<=2 MB/frame at 100k with a 20 B point record).
- "Millions" is explicitly out of scope; shipping content expresses per-event bursts <=1024 and
  per-map `particlemax` <=2048, and the raster collector drops silently at ~87k 3-vert particles.

## 2. Owner decision register

| ID | Decision | Recommendation |
|---|---|---|
| D0 | Confirm T1 scene and numbers | AD `start` at the saved viewpoint + one dense-trail demo |
| D1 | Identity-map volume policy (start) | Bake an R16_UINT volume from `leaf_cluster`; keep the CPU resolve behind a flag |
| D2 | Resolve removal first | Stage 0 decides: instrumented cache if resolve dominates, else volume |
| D3 | Solid/empty cells | Fill with the nearest open cluster, never 0 (cluster 0 sees sky -> spurious sun) |
| D4 | Draw shape | `draw(3, N)` with VS expansion; keep 3-vertex parity first |
| D5 | Caps and cvars | `rt_particles_gpu`, `rt_particle_volume`, `r_part_maxparticles` documented; loud overflow |
| L1 | Lighting end state | Per-particle evaluation; cluster sample from a volume/table; drop per-vertex loop |
| L2 | RT ray budget | Hard per-frame cap; <=1 sun/shadow ray per particle at its center, distance-faded, temporally reused |
| L3 | TLAS membership | Particles stay out; they cast no shadows and add no GI; state this in docs |
| D6 | FTE end state | FTE sim/PSET/dlight emission stay CPU; geometry/lighting converge later |
| D7 | `SMOKE_CLUSTER_SCAN` | Keep 16 until the GPU pass is measured with a counter |
| D8 | Legacy path removal | Frozen bugfix-only; remove after two releases at parity and green counters |

## 3. Stages

### Stage 0 — measurement (mandatory gate)
- Record two demos at a fixed `setpos`: `ad_particle_heavy` (the 45→95 spot) and `ad_particle_idle`;
  commit control configs under `perf/`, keep demos/logs out of git.
- Add counters: live counts per path (classic/FTE/smoke), dropped/overflow counts, upload bytes,
  per-resolve ns + cache hit/miss, vertex-ray count (RayStats 5→6 categories, RaygenCommon.hlsli,
  GenerateShaderCommon.py), GPU particle pass timer (nested query around the overlay particle draws,
  NvrhiFrameSkeleton).
- Split `RT_PROF_PARTICLES` into sim/resolve/fill/upload sub-slots (gl_rmain.c:1175-1187,
  glquake.h enum, gl_vidsdl.c name table) and dump `rs_particles` (r_part.c:985).
- Extend `RT_Bench_Setting` with `r_particles`, `r_particle_lighting`, `r_fteparticles`, `r_smoke`.
- Ablation matrix (runnable with cvars): `r_particles {0,2}` x `r_particle_lighting {0,1}` x
  `r_smoke {0,1}` x `r_fteparticles {0,1}`, plus `r_part_maxparticles 1024/16384/65536`.
- Exit gate: a signed attribution table (resolve vs expansion vs upload vs FTE conversion vs smoke)
  with p50/p95, build hash, and the pinned config.

### Stage 1 — repair before redesign
- FTE conversion and scratch memset bounded by live counts (`cl_numstrisvert`, `cl_numstrisidx`),
  not capacity (r_part_fte.c:6838-6857).
- Overflow policy: recycle-oldest + counted, rate-limited warning; remove the silent return.
- If Stage 0 shows the resolve dominates: land the already-drafted point-cluster cache (gl_rlight.c,
  512x4/16u) as a stopgap with a cvar and hit/miss counters — coordinate with the dtal branch, which
  owns `gl_rlight.c`; otherwise revert it.
- Gate: particle slot down without visual change on `start` and `ad_tfuma`.

### Stage 2 — compact points + vertex-shader expansion (independent of dtal)
- New `QrParticlePoint` (~20-32 B) and `qrUploadParticles`; `draw(3, N)`; corner expansion in
  `RsParticle.vert`; classic first, smoke later.
- Removes CPU vertex expansion and cuts upload ~12x; unlocks counts above the 87k raster drop.
- Keep per-vertex ray behavior for parity in this stage; lighting moves in Stage 3.
- Gate: <=5% fps / <=10% slot regression on both demos; upload-byte counter bounded.

### Stage 3 — cluster volume, remove CPU resolves (after dtal)
- Build R16_UINT volume from `leaf_cluster` (identity 7309 clusters on start; grid 7936 on tfuma),
  64 u texels (start ~199 KiB, tfuma ~2.7 MiB), nearest-open fill, clamped sampling; one generation
  counter shared with the lists.
- `q2ClusterAt(p)` in the particle shaders; delete every CPU resolve for classic/FTE/smoke.
- Grid maps may use the same volume or shader arithmetic; one source of truth (`leaf_cluster`).
- Gate: parity on both maps; particle resolve cost ~0; no ray-budget increase.

### Stage 4 — per-particle lighting budget
- One cluster/light evaluation per particle in a compute pre-pass (or in the point record), and
  <=1 distance-faded sun/shadow ray per particle under a hard per-frame cap, temporally reused.
- Delete the per-vertex loop (RsParticle.vert.hlsl:34); keep `SMOKE_CLUSTER_SCAN` at 16 unless the
  counter says otherwise.
- Gate: bounded ray counter; visual parity; `rt_bench` baselines unchanged or better.

### Stage 5 — GPU simulation (classic only, optional until measured)
- Ping-pong state, spawn ring, append/compaction, indirect draw; bench-freeze mode for determinism
  (fixed seed, fixed step, culling off); CPU remains the authority for dlights and PSET effects.
- Gate: demo/bench determinism, 1M stress bounded with loud overflow, FTE untouched.

## 4. Interfaces (sketch)

```cpp
struct QrParticlePoint { float position[3]; uint32_t packedColor; float size; uint32_t flags; };
struct QrParticleUploadInfo { const QrParticlePoint *pPoints; uint32_t count;
                              QrMaterialHandle material; uint32_t pipelineState; QrFloat4 smokeLook; };
QrResult qrUploadParticles(QrInstance, const QrParticleUploadInfo *);
struct QrClusterVolumeUploadInfo { QrFloat3D worldMins; float invCellSize; QrExtent3D dims;
                                   const uint16_t *pCells; uint32_t generation; };
QrResult qrUploadClusterVolume(QrInstance, const QrClusterVolumeUploadInfo *);
```

Bindings: particle point buffer and cluster volume join the existing set-6 layout as trailing
entries; generated bindings regenerated on the dtal side first. `qray.h` appends, never renumbers.

## 5. Branch and ownership order

1. `bugfix/present-wait-removal` (commit 50910a89, Swapchain.cpp only) to master.
2. `refactor/dtal-cluster-dedup` to master (owns `ClusterLightLists.*`, `LightManager.*`,
   `gl_rlight.c`, `r_world.c` cluster building, set-6 layout).
3. This particle work rebased: Stage 0/1/2 touch `r_part*`, `r_smoke.c`, `gl_rmain.c`,
   `RhiRasterOverlayPass.*`, `RsParticle.*` only; Stage 3 follows dtal's interfaces.
4. Docs and changelog per stage; `perf/README.md` scenario updated with the canonical demos.

## 6. Risks

- The "40 ms" figure is a window maximum; the plan's value depends on Stage 0 producing a
  reproducible attribution — no stage should promise a number that Stage 0 has not confirmed.
- Dual paths (legacy vs new) drift; bound them with counters, parity demos and a removal criterion.
- Volume precision near solid/leaf boundaries; mitigated by nearest-open fill and 64 u texels.
- Driver hazards: push-constant 128 B floor, R16_UINT sampling on all vendors, per-slot ring sync.
- dtal and particle work share set-6 shaders and generated bindings; serialize those changes.
