# Dark smoke investigation — no regression against master (2026-10-11)

Question: on `perf/particle-cluster-cache` the engine smoke (torch/rocket plumes) looked darker than
on `origin/master`; the owner asked to "fix the torch tracing through water like master". A
deep-reasoning pass (3 solvers, 4 blind verifiers) plus a decisive runtime test closed it as a
measurement artifact, not a regression.

Method and evidence:

- Code comparison (SM1/SM3/V1/V2, master `0c04f86f` vs HEAD `5aef96de`): the smoke's raster path
  (pipeline key, premultiplied blend, layout, binds, push constants, draw routing) is
  behaviour-identical; the smoke's shading (`RsSmoke.*`, `SmokeLight.hlsli` except the added
  ray-stats set) is byte-identical. The only smoke-visible branch delta is the cluster source:
  `Quake/r_smoke.c:140` takes the 64 u cluster-volume sample on the branch
  (`RT_ParticleClusterAt`, default `rt_particle_volume 1`) where master resolved the spawn point
  exactly (`RT_ResolvePointCluster`). Cluster 0 provably receives no list lights, so a puff whose
  volume cell is "lost" (deep solid; 6.0 % of dispatcher samples on `start`) would fall to the
  0.01 floor when the sun is blocked.
- Existing artifact audit (V4): the earlier volume A/B runs (`fl-*`, `cc-*`, `cc2-*`) do not
  reproduce a stable direction — `fl-g0` shows the opposite, `cc*`/`rp*` show nulls, volume-1
  repeats across rebuilt binaries diverge, and within-arm flips (smoke animation) exceed the arm
  differences. The cross-build "master light vs branch black" pairing compared master's *pre-fire*
  frame with the branch's *post-fire* frames; matched shots (fire 4: master 0102, branch 0078/0088)
  show large dark smoke on **both** builds.
- Decisive state-controlled test (this branch, fresh spawns, `r_particles 0`,
  `r_fteparticles 0` so only `r_smoke` calls the dispatcher): one process, arms
  `rt_particle_volume 1 -> 0 -> 1`, `smoke_puff 16` immediately before each capture, fixed
  delays. Artifacts: `%LOCALAPPDATA%\Temp\opencode\vol-decisive-1` (screenshots 0106-0111).
  Result: no visible difference between the arms — the volume does not darken these plumes.
- Owner check: in vanilla `id1` the torches trace through water normally because they are traced
  models; in AD the torches are particle effects, which is why the water does not distort them
  through the scene path. This is a content/mechanism difference, not a master-vs-branch
  regression.

Conclusions:

1. No code change is warranted for master parity: the observed smoke differences were measurement
   artifacts (unmatched moments, exposure/animation drift).
2. The parked dark-smoke shader defect (direction-blind weight, single candidate, `dw=0` without a
   fallback) exists identically on master and remains parked behind the radiance-cache rework.
3. The delivered water feature composites the captured particle stand-ins at water pixels
   (`perf/stage0/water-particle-reflection-2026-10-11.md`); the particle classes outside the
   capture (engine smoke, per-channel FTE blends, line sparks) are the owner-scoped out-of-scope
   remainder. AD's particle torches therefore never distort through water like id1's models —
   only their captured particle part does, by design until the remainder is taken up.
4. The known set-7 `smokeRayStats` alias in the reflect/refract blob (documented in
   `docs/particle-plan.md`) is dormant with `rt_stats 0` and unrelated to this question.

Coverage: solvers/verifiers were read-only; the decisive test ran on the branch Debug build; the
owner's id1 check is their direct observation. No runtime measurement of the volume's "lost"
counters at smoke spawn points exists (the check cvar was off; not needed after the decisive test).
