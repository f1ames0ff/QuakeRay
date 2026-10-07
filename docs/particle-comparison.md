# Particle rendering — comparison and shortcomings

Planning draft, corrected after the external review. Current-side facts are anchored in
`docs/particle-current.md`; modern-side facts and sources in `docs/particle-modern.md`. Decision IDs
refer to `docs/particle-plan.md`.

## 1. Capability matrix

| Capability | Current | Modern reference | Severity |
|---|---|---|---|
| Simulation | CPU linked list, single-threaded | GPU compute, ping-pong state, emitter records | Architecture limiter |
| Light-cluster resolve | CPU BSP walk per classic particle and per FTE vertex (`r_part.c:939`, `r_part_fte.c:6896`; smoke at spawn `r_smoke.c:140`) | World-space grid/volume lookup on GPU | High |
| Geometry build | CPU expands per particle, path-dependent: classic 3 verts/240 B, FTE billboards 4 verts/320 B + 24 B indices, smoke 6 verts/480 B per puff | Compute or VS expansion; zero CPU vertices | High |
| Upload / conversion | Full re-upload via mapped staging; FTE copies its grown capacity when anything is live (`r_part_fte.c:6838-6857`); one device copy per frame | Persistent GPU buffers; counts only | High |
| Lighting evaluation | Per vertex: <=16 cluster evals + ray queries (`RsParticle.vert.hlsl:34`) | Per particle (1 cluster sample, budgeted rays) | High (3-4x redundant) |
| RT participation | Not in TLAS; no occlusion/reflection/GI; ambient from a downsampled buffer (`RsParticle.frag.hlsl:37-44`) | Effects TLAS any-hit (Q2RTX) or a separate approximation (Remix) | Medium (document honestly) |
| Sorting / transparency | Unordered alpha blend; FTE per-batch blend order; smoke qsort | Optional GPU sort (raster alpha only); path tracers blend in the any-hit | Low now; next ceiling |
| Culling | None for engine particles; weather surface path has distance rejection (`r_part_fte.c:3704-3706`) | Emitter/frustum/distance cull, budget management | Medium at high counts |
| Overflow | Classic drops silently; collector asserts (Debug) / drops batch (Release) | Append/compaction with counters and loud budgets | Medium: invisible failure |
| Live counters | `rs_particles` computed but never dumped; no FTE/smoke/overflow counters | Counters everywhere, GPU pass timers | High (blocks attribution) |
| Scale expressed | classic 16384 (raisable via `-particles`); FTE 65536 default / 262144 compiled; raster cap 87381 3-vert particles (65535 FTE quads) when the collector is otherwise empty | 100k-1M with GPU sim and budgets | "Millions" not expressible by shipped content |
| DTAL consumers | Particle/smoke lighting calls `sampleLight`, which on the dtal branch returns an empty sample for `LIGHT_TYPE_DTAL_GROUP`; the smoke decoder misreads groups; tail lists unread | n/a (this engine's branch) | High: correctness gate for Stage 2/3 |

Per-path bytes (corrected): classic 3 x 80 = 240 B; FTE billboard 4 x 80 = 320 B + 6 x uint32 = 24 B
indices; smoke 6 x 80 = 480 B per puff. "240 B" is classic-only.

## 2. Ranked shortcomings

1. **Per-particle/per-vertex CPU resolves and vertex building on the main thread** — proven
   architecture limits; the magnitude is currently measured only as window maxima (5.35-6.87 ms
   means of maxima, p95 ~7.7-7.9 ms) plus owner observations, so Stage 0 must attribute.
2. **Capacity-proportional FTE work** — the conversion is skipped when nothing is live, but when it
   runs it copies the grown capacity, not the live counts.
3. **Per-vertex lighting redundancy** — 3x/4x the needed scans and rays; multi-vertex FTE geometry
   (line sparks 2, fan/clipped 3, billboards 4) makes a single "4 verts" assumption wrong.
4. **No counters and silent overflow** — failures are invisible; the raster cap drops batches.
5. **DTAL consumer incompatibility** — the correct pattern is `q2SampleClusterLights` (fast + tail)
   -> `sampleLightNee` -> divide by `lightPdf * memberPdf` (as in `RtRaygenDirect.rgen.hlsl:93-132`);
   `sampleLight` alone is wrong on dtal.
6. **No threading** — the whole path is on the main thread (use_tasks forced false).
7. **Transparency/overdraw** — the next ceiling once CPU costs are removed.
8. **No RT participation** — particles neither occlude nor contribute GI (document honestly).

## 3. Must-not-regress list

- Demo playback and MP protocol: `CL_ParseParticles` cl_parse.c:1621-1665, `svcdp_*` 2047-2061,
  `svc_particle` 1899; demos replay through the same parser (cl_demo.c:376-383).
- PSET scripts and `effectinfo` content (common.c:3087-3130; effect fields cliptype/bounce/emit/ramp
  from r_part_fte.c:1456-3280 must survive any new representation).
- `r_particle_lighting` as the feature switch; `r_particles`, `r_fteparticles`, `r_smoke` gates.
- Classic beams as model/temp entities (cl_tent.c:231-247,344-414) and FTE beam segments
  (r_part_fte.c:5801-5857) — both paths, different topology.
- Decals clipped (entity-relative) and unclipped (r_part_fte.c:5859-6014,6194-6306).
- Network/model trails and `emitstate` (cl_main.c:924-1004; r_part_fte.c:6596-6604).
- Weather both paths: network box (cl_tent.c:281-307) and surface/sky with rejection
  (r_part_fte.c:3665-3724,6986-7000).
- FTE spawn-side dlights and sounds (r_part_fte.c:3888-3934) stay CPU-authoritative.
- AD sprite edicts (part_generate.qc:224-230,780-804; r_sprite.c:245-291) and interaction with the
  `feature/voxel-smoke` branch (overlap: r_smoke.c, gl_vidsdl.c, render.h, qray.h, VulkanDevice.*,
  NvrhiFrameSkeleton.*, RhiRasterOverlayPass.*).
- Reflection-pinned shader files and the benchmark guard; strengthen the guard (default 20%,
  block averages, silently skipped metrics today).

## 4. Decision links

- Target scale/budgets: D0, D5, G0 in `docs/particle-plan.md`.
- Cost attribution gate: Stage 0.
- Resolve removal (cache vs volume): D2, Stage 3.
- Volume accuracy contract: A1-A3, Stage 3.
- Lighting/DTAL compatibility: L1-L4.
- FTE end state: D6.
