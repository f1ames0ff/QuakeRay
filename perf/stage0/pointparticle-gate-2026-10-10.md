# Pointparticle distance gate — implementation and acceptance (2026-10-10)

Scope: client-side distance gate for ssqc `pointparticles` (`svcdp_pointparticles` /
`svcdp_pointparticles_1` -> `CL_ParseParticles` -> `PScript_RunParticleEffectState`) that stops the
map-wide AD ambient glow population (QC `spawn_pemitter` emitters calling `pointparticles`) from
spawning far from the camera. Owner decision (2026-10-10): gate ON by default, radius 2048
(`r_part_emit_distance`, `0` = off), deterministic wall semantics (upstream parity), scope = the
point branch only. Register row E1 and the stage note live in `docs/particle-plan.md`; the column
contract is in `perf/README.md`.

## Revision and inputs

- Build: `.\build_win.ps1 Debug` (exit 0), `quakeray.exe` SHA256
  `E778314A447668E6A16B89B40470B55D2D776B997C39F35F69A48AE74BC69737`
  (source: pre-change HEAD `42bd04cd` plus the change under test).
- Saves: `qr_ad_start.sav` `729D29F9…D4CA` (`start`), `qr_fuma_start.sav` `0B820D4F…98F4`
  (`ad_tfuma`), `qr_ad_swamp.sav` `9318AE31…FC13` (`ad_swampy`, particle-free regression scene).
- Harness: `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1` (loads the save, `rt_stats 3` +
  `rt_stats_dump_start/end` + `rt_bench` around a 15 s capture, two `r_partinfo` samples, window
  1280x720, fixture `qr_audit_max.cfg` + `rt_upscale_fsr31 3` + `r_tasks 1` + arm overrides).
  Demo regression via `perf/stage0/run_points_demo_ab.ps1 -Demos ad_particle_heavy -Arms points_on
  -ExtraCvars 'r_part_emit_distance 2048'`.
- Metrics are window-maxima medians from `stats-*.dump` (declared limitation); `fps` is the GPU-side
  counter; `cull`/`fade` are the run totals of the new counters (last dump row; under `rt_bench` the
  counters accumulate for the whole run and reset at bench start, outside a bench they reset per
  published window).

## Arms and results

| Arm | fps | frame | parts | resolve | convert | upload | fte | verts | rays | cull | fade | live |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| start pre-change (other build) | 52.7 | 15.97 | 8.46 | 4.14 | 7.07 | 2.43 | 7540 | 30160 | 14246 | — | — | 6693–6976 |
| start gate=0 | 55.5 | 14.42 | 7.99 | 3.80 | 6.71 | 2.34 | 7222 | 28888 | 13925 | 0 | 0 | 7140–6940 |
| start gate=1024 | 67.5 | 11.78 | 2.87 | 1.79 | 2.47 | 0.80 | 2319 | 9276 | 3983 | 382 | 454 | 2132–2348 |
| start gate=2048 | 57.7 | 14.62 | 7.03 | 3.50 | 5.99 | 2.00 | 6369 | 25476 | 11759 | 20 | 352 | 5804–6039 |
| start gate=2048, r_tasks 0 | 44.6 | 25.72 | 5.63 | 3.16 | 4.89 | 1.24 | 6083 | 24332 | 10993 | 26 | 339 | 5605–6276 |
| tfuma pre-change (other build) | 40.3 | 18.05 | 3.46 | 3.09 | 2.75 | 1.30 | 3952 | 15808 | 13701 | — | — | 3988–3902 |
| tfuma gate=0 | 41.2 | 17.51 | 3.28 | 2.91 | 2.59 | 1.21 | 3918 | 15672 | 13659 | 0 | 0 | 3805–3807 |
| tfuma gate=2048 | 41.9 | 17.34 | 2.61 | 2.70 | 2.01 | 0.98 | 3105 | 12423 | 12248 | 1344 | 581 | 3004–3139 |
| swampy pre-change (other build) | 26.1 | 28.75 | 0.25 | 3.42 | 0.19 | 0.18 | 1 | 23 | 6 | — | — | 2–1 |
| swampy gate=2048 | 27.3 | 26.53 | 0.21 | 3.13 | 0.16 | 0.15 | 1 | 23 | 6 | 0 | 0 | 2–1 |

Per-type witness (start, first `r_partinfo` sample, top families): gate=0 — LFLAME 1044/1008,
PORTALFRONT 840/744/656/656/592/592, PORTALSIDE 672/576; gate=2048 — LFLAME 825/815, PORTALFRONT
813/802/655/655/617/617, PORTALSIDE 496/466; gate=1024 — PORTALSIDE 312/291/251, LFLAME 246/234.
Running-effect types: 64 -> 64 -> 44; total live 7140/6940 -> 5804/6039 -> 2132/2348.

Demo regression: `ad_particle_heavy` with the gate at 2048 — 2747 frames, 69.42 s, fps 39.6,
`interrupted=0`, no parse/desync errors; the run reports particles resolve 2.45 / fte convert 3.64,
inside the same-config historical band for this demo (2.24-4.56 / 3.43-6.00; the broader band
including `r_particles 0`/`r_fteparticles 0` arms is 0.22-4.56 / 0-6.00), with no same-session A/B arm.

## Findings

1. Off path is inert: `r_part_emit_distance 0` produces zero `cull`/`fade` and live/fps within the
   run-to-run spread (start 55.5 vs 52.7 fps across builds; tfuma 41.2 vs 40.3). The gate adds no
   message reads and consumes no RNG; with `radius <= 0` or `!r_vieworg_valid` it short-circuits
   before any gate statement, so the off path is a no-op beyond three loads. Firing the gate
   deliberately removes downstream spawn draws, so particle evolution is not bit-identical to an
   ungated run (expected, recorded).
2. The counters work and are dose-monotone: cull 0/20/382 and fade 0/352/454 on start for
   0/2048/1024; tfuma 0/1344 cull at 2048. Both `r_tasks 1` and `r_tasks 0` produce consistent
   totals (20/352 vs 26/339 on start-2048).
3. Population and transport scale with the cut: start 2048 removes ~12% of live particles
   (7222 -> 6369) and ~11% of `fte convert` (6.71 -> 5.99); start 1024 removes ~68% (-> 2319,
   convert 2.47); tfuma 2048 removes ~21% (3918 -> 3105, convert 2.59 -> 2.01, upload 1.21 -> 0.98).
4. The approved default 2048 is a mild gate on `start` at this viewpoint: only 20 messages in 15 s
   are beyond 2048; most of the win at 2048 comes from scaling counts of messages in
   [1024, 2048). 1024 cuts far more (owner-facing dose-response recorded; the cvar allows tuning
   without a code change).
5. Regressions clean: `ad_swampy` (essentially particle-free: 1-2 live, 23 vertices) shows zero
   counters and no resolvable change (the frame/fps deltas are cross-build and inside the spread).
   The demo `ad_particle_heavy` with the gate at 2048 shows no desync indicator (`interrupted=0`,
   2747 frames / 69.42 s inside the historical 1256-3628 frame spread); it is a single arm, so an
   in-session A/B was not performed (recorded as a limitation).
6. Known behavior: recipes whose per-message effective count stays at or below 1 (a literal
   `count 1` and the `countabsolute` parts of a recipe) reduce only at the hard wall at R;
   multi-count recipes (e.g. AD LFLAME `count 12/4/4`) fade inside [R/2, R). The wall drop removes
   the whole message, including any dlight or sound that message would have spawned (owner-approved
   scope: point particles only). The `fade` counter counts scaled messages, not removed particles
   (the kernel spawns `ceil(pcount)`, so a scaled message may still spawn the same count).

## Falsifiers checked

- `cull == 0` at any arm on both particle scenes: not observed (non-zero wherever distance mass
  exists), and zero exactly on the off path and on the particle-free scene.
- Counters not published / header mismatch: dumps parse by name; both new columns present in the
  CSV of every gate run (`perf/compare_benchmark.ps1` skips unknown counter columns by design;
  `particle_attribution.ps1` gained both names).
- Regression at `r_tasks 0`: same qualitative counters at a longer serial frame.
- Demo desync / frame-count change: none (2747 frames, interrupted 0).

## Limitations

- Window-maxima medians overstate per-frame values. Same-conditions repeats are scarce: the only
  true repeat pair in the artifact set (tfuma) differs by ~49% in fps relative to the lower value
  (27.1 vs 40.3), and demo runs of the same demo span 18-54 fps, so the gate=0 vs gate=2048 `start`
  fps delta (+2.2) is not resolved at this radius; the live, vertex and transport reductions are the
  decisive evidence.
- Pre-change rows are a different binary; they are context only (cross-build comparisons are
  forbidden for acceptance). Within-build comparisons above are gate=0 vs 1024 vs 2048. The save
  captures are not frame-identical across arms, so "consistent" counter totals are qualitative.
- Visual confirmation at the boundary (wall pop for low-count recipes) is an owner check at the
  fixed save viewpoints; not performed in automation.
- The distance distribution is inferred from counters at two radii, not measured per particle.
- The demo regression is a single arm; the idle-mode counter lifecycle was not captured (code
  emits the same reset path as `particles_dropped`); NaN/negative-count/invalid-efnum paths are
  code-inspected only; `r_tasks` is not part of the bench settings witness (the t0 arm is witnessed
  by its generated fixture); `r_part_emit_distance` is not archived, so tuning does not persist
  across sessions.

## Changed files (this change)

`Quake/cl_parse.c` (gate), `Quake/r_part_fte.c` (cvar), `Quake/gl_rmain.c` +
`Quake/cl_main.c` + `Quake/glquake.h` (`r_vieworg_valid` latch), `Quake/gl_vidsdl.c` (counters,
CSV, witness), `perf/particle_attribution.ps1`, `perf/README.md`, `docs/particle-plan.md`, and this
evidence document itself.
