# Particle rendering — comparison and shortcomings

Planning draft. Current-side facts are anchored in `docs/particle-current.md`; modern-side facts
and sources in `docs/particle-modern.md`. Decision IDs refer to `docs/particle-plan.md`.

## 1. Capability matrix

| Capability | Current | Modern reference | Shortfall severity |
|---|---|---|---|
| Simulation | CPU linked list, single-threaded | GPU compute, ping-pong state, emitter records | Architecture (limits scale), not the measured hot spot |
| Light-cluster resolve | CPU BSP walk per classic particle and per FTE vertex (`r_part.c:939`, `r_part_fte.c:6896`) | World-space grid/volume lookup on GPU (id Tech froxel SH, UE translucency volume) | High: main-thread cost proportional to particles |
| Geometry build | CPU expands 3 (classic) / 4 (FTE) vertices per particle every frame | Compute or VS expansion; zero CPU vertices | High: 240 B/particle write + copies |
| Upload | Full re-upload via mapped staging every frame; FTE converts full grown capacity (r_part_fte.c:6843-6857) | Persistent GPU buffers; counts only | High (FTE conversion is capacity-proportional) |
| Lighting evaluation | Per vertex: <=16 cluster evals + <=2 ray queries (`RsParticle.vert.hlsl:34`) | Per particle (1 cluster sample, budgeted rays) | High (3-4x redundancy) |
| RT participation | Not in TLAS; no occlusion/reflection/GI; ambient from downsampled buffer (`RsParticle.frag.hlsl:37-44`) | Effects TLAS any-hit (Q2RTX) or separate approximation (Remix) | Medium (document honestly) |
| Sorting / transparency | Unordered alpha blend; FTE per-batch blend order | Optional GPU sort (raster alpha only); path tracers use ordering in the any-hit | Low now; next ceiling after CPU is fixed |
| Culling | None (whole pools drawn) | Emitter/frustum/distance cull, budget management | Medium at high counts |
| Overflow | Classic drops silently; collector asserts (Debug) / drops batch (Release) | Append/compaction with counters; loud budgets | Medium: invisible failure |
| Live counters | `rs_particles` computed but never dumped; no FTE/smoke counts | Counters everywhere, GPU pass timers | High (blocks attribution) |
| Scale expressed | classic 16384; FTE 65536 default / 262144 compiled; raster cap ~87k 3-vert particles | 100k-1M with GPU sim, budgeted | "Millions" not expressible by shipped content |

## 2. Ranked shortcomings

1. **Per-particle/per-vertex CPU resolves and vertex building on the main thread** — proven
   architecture limits; magnitude currently unmeasured per-path (dumps give a 4.7-7.4 ms mean
   particle slot, one 39.1 ms window maximum, and an owner A/B `r_particles 0` 45→95 fps in one
   moment). Requires Stage 0 instrumentation before any fix is chosen.
2. **Capacity-proportional FTE work** — once any FTE particle exists the buffers never shrink
   (r_part_fte.c:6831) and every frame memsets/converts 100k verts + 150k idx worth of scratch.
3. **Per-vertex lighting redundancy** — 3x/4x the cluster scans and ray queries needed.
4. **No counters and silent overflow** — failures are invisible; the raster cap drops at ~87k
   particles with only a Debug assert.
5. **No threading** — the whole path is on the main thread (use_tasks forced false).
6. **Transparency/overdraw** — becomes the limiting factor once CPU costs are removed.
7. **No RT participation** — particles neither occlude nor contribute GI; reflections miss them.

## 3. Must-not-regress list

- Demo playback and MP protocol messages (`svc_particle`, `svcdp_pointparticles`,
  `svcdp_trailparticles`, cl_parse.c:1899,2047-2061,2711-2740).
- PSET scripts and `effectinfo` content (AD weather, explosions, trails).
- `r_particle_lighting` as the feature switch; `r_particles`, `r_fteparticles`, `r_smoke` gates.
- Classic smoke option and the smoke spawn-time cluster behavior (r_smoke.c:140).
- Reflection-pinned shader files (`Reflection/HLSL/RsParticle*.txt`) and the benchmark guard
  (`perf/compare_benchmark.ps1`).
- AD/rerelease content paths and the shipped `particles/fte_weather.cfg`.

## 4. Decision links

- Target scale and budget: D0, D5 in `docs/particle-plan.md`.
- Cost attribution gate: Stage 0.
- Resolve removal policy (cache vs volume): D2, Stage 3.
- Lighting end state and ray budget: L1-L3.
- Identity/grid volume policy: D1, D3.
- FTE end state and CPU ownership: D6.
