# Cluster volume (Stage 3) — implementation and acceptance (2026-10-10)

Scope: bake a 64 u cluster volume from `leaf_cluster` and replace the CPU point-cluster
resolves of the particle paths (FTE, classic, smoke) with a table sample; keep the CPU resolve
behind `rt_particle_volume` (default 1; the dispatcher reads it, so a 1->0 flip disables the
volume immediately, while enabling after a load with it off needs the next bake). Register
D1/A1/A2/A3 and the stage note live in `docs/particle-plan.md`.
The GPU upload of the volume (`qrUploadClusterVolume` and the shader binding) is deferred to
Stage 4, where a consumer exists; Stage 3 delivers the CPU volume, the dispatcher, the
generation, the counters and the gate evidence.

## Revision and inputs

- Source: pre-change HEAD `b1878ed7` plus the Stage 3 change; build `.\build_win.ps1 Debug`,
  `quakeray.exe` SHA256 `EED7118565653E00ED42FF4728412CEC4185B40BCA6F82F6102047F07F7542E6`.
- Saves: `qr_ad_start.sav` `729D29F9…D4CA`, `qr_fuma_start.sav` `0B820D4F…98F4`,
  `qr_ad_swamp.sav` `9318AE31…FC13`; save/exe/pak sha256s are recorded in each manifest.
- Harness: `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1` (deterministic menu close
  `menu_main`+ESC, clean `toggleconsole;quit` exit, `rt_stats 3`, 15 s capture, two `r_partinfo`
  samples, window 1280x720, `r_tasks 1` unless stated).
- Medians are upper-middle (`sorted[n//2]`) window maxima; single run per arm.

## Bake report (qconsole, one per map load, generation 2 = the final table build)

| map | dims | texels | zero | bake ms | bytes |
|---|---|---:|---:|---:|---:|
| `start` (identity, 6125 clusters) | 54x45x41 | 99,630 | 81,577 | 38.0-41.5 | 194.6 KiB |
| `ad_tfuma` (grid, 7936 clusters) | 129x129x86 | 1,431,126 | 445,459 | 245.9-249.6 | 2.73 MiB |
| `ad_swampy` (grid, 7453 clusters) | 89x103x48 | 440,016 | 322,626 | 176.7-177.6 | 859.4 KiB |

The dims/texels reproduce the independently verified rule (non-solid leaf union over
`i < model->numleafs`, `ceil(ext/64)`); the 64 u rule and the volume build are new code (the grid
branch's own scaling remains ~358 u on tfuma). Painting uses the fresh (uncached) CPU rule
`RT_ResolvePointClusterUncached` per texel centre. The volume is built on the main thread in
`RT_UploadWorldLights` after `RT_BuildWorldClusters`; the lazy mapping path never bakes. The
`rt_worldlights_stats` toggle re-runs the upload and therefore re-bakes (a diagnostic-path cost,
up to ~250 ms on tfuma).

## Volume on/off

| Arm | fps | frame | parts | resolve | convert | upload | fte | rays | live |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| start vol=0 | 61.7 | 13.02 | 6.78 | 3.35 | 5.87 | 2.04 | 5845 | 10673 | 5329-6179 |
| start vol=1 | 69.5 | 11.27 | 3.64 | **0.30** | 2.68 | 2.02 | 5949 | 10707 | 5453-6015 |
| start vol=1, `r_tasks 0` | 55.1 | 21.75 | 2.66 | 0.24 | 1.85 | 1.23 | 6158 | 11348 | 6202-5780 |
| tfuma vol=0 | 44.3 | 15.93 | 2.71 | 2.61 | 2.22 | 1.10 | 3079 | 12164 | 3150-3073 |
| tfuma vol=1 | 45.2 | 15.60 | 1.99 | 1.86 | 1.53 | 1.12 | 3149 | 12212 | 3142-3022 |
| swampy vol=0 | 29.5 | 23.73 | 0.19 | 2.76 | 0.15 | 0.15 | 1 | 6 | 2-0 |
| swampy vol=1 | 29.1 | 23.90 | 0.18 | 2.80 | 0.15 | 0.15 | 1 | 3 | 2-1 |

Floors measured on the same build with `r_particles 0`: start `0.26`, tfuma `1.81`, swampy
`2.98`. The particle-attributable resolve collapses onto the floor: start `3.35 -> 0.30` (floor
0.26), tfuma `2.61 -> 1.86` (floor 1.81), swampy unchanged within the arm spread. This is the
per-map floor reformulation of the Stage 3 gate: the global `cpu.particles_resolve_ms` can never
reach zero while alias/brush/DTAL callers resolve. `fte convert` drops 54% on start and 31% on
tfuma; population is unchanged within noise; `rays_particle` moves slightly because cluster 0
keeps the sun ray but no lights. `r_tasks 0/1` counters are consistent.

Demo (`ad_particle_heavy`, same runner and build, paired arms, gate 2048 in both):

| Arm | frames | fps | resolve | convert | frame |
|---|---:|---:|---:|---:|---:|
| volume 0 | 2804 | 40.4 | 2.44 | 3.65 | 24.37 |
| volume 1 | 3114 | 44.9 | 0.24 | 1.39 | 21.92 |

`interrupted=0` in both; no desync.

## Accuracy (check mode: every 64th dispatcher call also runs the fresh CPU rule)

| Arm | samples | mismatch | lost (vol 0, CPU != 0) | extra (vol != 0, CPU 0) |
|---|---:|---:|---:|---:|
| start vol=1 check | 400,375 | 172,807 (43.2%) | 23,881 (6.0%) | 6,975 (1.7%) |
| tfuma vol=1 check | 135,315 | 34,347 (25.4%) | 2,163 (1.6%) | 18 (0.0%) |

These rates sit between the offline uniform-box and leaf-weighted envelopes (the retained W4
simulation under `%LOCALAPPDATA%\Temp\opencode\w4-verify\`: box-uniform ~6.8%/~3.5%, first-open
conditioning 24.5%/~3.5%, leaf-weighted 73%/58%) because the runtime check samples actual
particle vertices, which cluster near surfaces; the counters are the contract. Both volume cvars
are entries of the `CVAR_DEF_LIST` and therefore archived (`gl_vidsdl.c:371`); the check arms pin
`rt_particle_volume_check 64` in their fixtures so a persisted value cannot add sampling
silently. One semantic deviation from "the same rule at texel centres": points outside the
non-solid leaf-union box clamp to the edge texel instead of returning the CPU value (mostly 0 in
void); that class is part of `extra`. The owner visual tolerance for the 64 u quantisation remains
an owner decision; `rt_particle_volume 0` falls back to the cached CPU resolve.

## Gate revalidation on this build

| Arm | fps | frame | fte | cull | fade | live |
|---|---:|---:|---:|---:|---:|---|
| start gate=0 | 63.1 | 12.28 | 7433 | 0 | 0 | 7201-7137 |
| start gate=1024 | 69.6 | 13.15 | 2365 | 405 | 455 | 2171-2684 |
| start gate=2048 | 63.9 | 11.99 | 6141 | 22 | 351 | 5676-6286 |
| start gate=2048, r_tasks 0 | 54.8 | 22.29 | 6238 | 22 | 325 | 6011-5574 |
| tfuma gate=0 | 42.6 | 16.96 | 4018 | 0 | 0 | 3901-3914 |
| tfuma gate=2048 | 43.5 | 16.52 | 3108 | 1338 | 580 | 3181-3035 |
| swampy gate=2048 | 28.2 | 25.25 | 1 | 0 | 0 | 2-0 |

Dose response: start -17% at 2048 and -68% at 1024 (run-to-run population varies a few points),
tfuma -23%, swampy unchanged with zero counters. The resolve column reflects the volume (on by
default in this build).

## Validity and limitations

- Every save arm exited through `F10 toggleconsole;quit` (no menu, no force kill) and reports two
  differing live totals, so the world was not paused; the demo arms exit via `rt_bench ... quit`
  and the runner's deterministic menu close (the demo arms have no manifests of their own; their
  exe identity rests on the on-disk binary and the settings witness). Only the `gate-*` and `s3-*`
  runs belong to the menu-hazard harness generation and are invalid as evidence; `s3b-*`/`gate2-*`
  used the fixed harness and are superseded by this matrix's rebuild, not by the menu issue.
  Revision/exe/save/pak hashes are pinned per manifest.
- Single run per arm; medians are the `sorted[n//2]` element over the retained rows (a few cells
  sit up to one order statistic away; the raw dumps stay per run for exact reproduction) and the
  window values are maxima, so they overstate per-frame values; the bake is a one-shot load cost
  outside the capture windows; the published verifier counters (32-bit atomics read as 64-bit
  fields) would wrap only after ~45 h (start rate) / ~135 h (tfuma) of continuous check mode, and
  the internal 64-step sampling counter wraps after ~42 min without affecting any dumped value.
- The GPU upload and shader-side sampling are Stage 4; the classic swap is scene-reachable but
  small on the owner saves (6-12 live classic on tfuma, 22-25 on swampy; smoke 0 everywhere), so
  classic/smoke parity rides other content (`demo1`-style classic capture, a rocket-trail scene
  for smoke). The owner visual check for the 64 u quantisation is pending.

## Changed files (this change)

`Quake/r_world.c` (volume build, generation, sampler/dispatcher, verify counters),
`Quake/gl_rlight.c` (`RT_ResolvePointClusterUncached`), `Quake/glquake.h` (declarations, report
fields), `Quake/gl_vidsdl.c` (cvars, counters, CSV, witness), `Quake/r_part_fte.c`,
`Quake/r_part.c`, `Quake/r_smoke.c` (consumer swap), `perf/particle_attribution.ps1`,
`perf/stage0/run_points_demo_ab.ps1` (menu fix), `perf/README.md` (column contract),
`docs/particle-plan.md`, `docs/particle-current.md`, and this evidence document.
