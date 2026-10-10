# Cluster volume (Stage 3) — implementation and acceptance (2026-10-10)

Scope: bake a 64 u cluster volume from `leaf_cluster` and replace the CPU point-cluster
resolves of the particle paths (FTE, classic, smoke) with a table sample; keep the CPU resolve
behind `rt_particle_volume` (default 1). Register D1/A1/A2/A3 and the stage note live in
`docs/particle-plan.md`. The GPU upload of the volume (`qrUploadClusterVolume` and the shader
binding) is deferred to Stage 4, where a consumer exists; Stage 3 delivers the CPU volume, the
dispatcher, the generation, the counters and the gate evidence.

All runs below were produced after the harness fix for the menu/pause hazard (deterministic
`menu_main`+ESC close, clean `toggleconsole;quit` exit, live-total sampling); every run in this
document reports `exit path: F10 toggleconsole;quit` and differing live totals.

## Revision and inputs

- Source: pre-change HEAD `b1878ed7` plus the Stage 3 change (11 files; see the commit).
- Build: `.\build_win.ps1 Debug`, `quakeray.exe` SHA256 `7E2AAB3D…E04`; the fixture and manifest
  sha256s are recorded in each run directory.
- Saves: `qr_ad_start.sav` `729D29F9…D4CA`, `qr_fuma_start.sav` `0B820D4F…98F4`,
  `qr_ad_swamp.sav` `9318AE31…FC13`.
- Harness: `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1` (menu fix, `rt_stats 3`, 15 s
  capture, two `r_partinfo` samples, window 1280x720, `r_tasks 1` unless stated).

## Bake report (qconsole, one per map load, generation 2 = the final build of the load)

| map | dims | texels | zero | bake ms | bytes |
|---|---|---:|---:|---:|---:|
| `start` (identity, 6125 clusters) | 54x45x41 | 99,630 | 81,577 | 38.2-39.8 | 194.6 KiB |
| `ad_tfuma` (grid, 7936 clusters) | 129x129x86 | 1,431,126 | 445,459 | 248.3-264.1 | 2.73 MiB |
| `ad_swampy` (grid, 7453 clusters) | 89x103x48 | 440,016 | 322,626 | 177.0-177.9 | 859.4 KiB |

The dims and texel counts reproduce the independently verified rule (non-solid leaf union over
`i < model->numleafs`, `ceil(ext/64)`); the 64 u rule and the volume build are new code (the grid
branch's own scaling remains ~358 u on tfuma). Painting uses the fresh (uncached) CPU rule
`RT_ResolvePointClusterUncached` per texel centre. The volume is built on the main thread in
`RT_UploadWorldLights` after `RT_BuildWorldClusters`; the lazy mapping path never bakes.

## Volume on/off (window-maxima medians; fps is the GPU-side counter)

| Arm | fps | frame | parts | resolve | convert | upload | fte | rays | live |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| start vol=0 | 63.4 | 12.58 | 6.72 | 3.19 | 5.84 | 2.11 | 6113 | 11341 | 5527-6161 |
| start vol=1 | 67.9 | 11.81 | 3.55 | **0.31** | 2.68 | 2.07 | 5970 | 10732 | 5716-6149 |
| start vol=1, `r_tasks 0` | 59.1 | 20.99 | 2.68 | 0.23 | 1.86 | 1.24 | 6127 | 11249 | 5744-6176 |
| tfuma vol=0 | 44.7 | 16.05 | 2.71 | 2.53 | 2.24 | 1.12 | 3121 | 12319 | 3135-3249 |
| tfuma vol=1 | 45.0 | 15.97 | 2.01 | 1.89 | 1.51 | 1.08 | 3120 | 12226 | 3036-3141 |
| swampy vol=1 | 29.2 | 23.83 | 0.17 | 2.82 | 0.14 | 0.14 | 1 | 6 | 2-1 |

- The particle-attributable resolve collapses onto the non-particle floor: start `3.19 -> 0.31`
  (floor measured with `r_particles 0`: 0.26-0.33), tfuma `2.53 -> 1.89` (floor 1.80-2.05), swamp
  unchanged (floor 3.2-3.4, no particles). This is exactly the per-map floor reformulation of the
  Stage 3 gate adopted after the W-verification: the global `cpu.particles_resolve_ms` can never
  reach zero while alias/brush/DTAL callers resolve.
- `fte convert` drops 54% on start and 33% on tfuma; the population (`particles_fte`) is unchanged
  within noise (the volume changes clusters, not spawns); `rays_particle` moves slightly because
  cluster 0 keeps the sun ray but no lights.
- `r_tasks 0` and `r_tasks 1` counters are consistent; the serial arm is slower by frame pacing.

Demo regression (`ad_particle_heavy`, gate 2048 + volume default): 3200 frames, 69.43 s,
`interrupted=0`, fps 46.1, `particles resolve` **0.22** (was 2.45 before the volume), `fte convert`
1.35 (was 3.64). No desync.

## Accuracy (check mode: every 64th dispatcher call also runs the fresh CPU rule)

| Arm | samples | mismatch | lost (vol 0, CPU != 0) | extra (vol != 0, CPU 0) |
|---|---:|---:|---:|---:|
| start vol=1 check | 394,952 | 169,501 (42.9%) | 20,282 (5.1%) | 7,591 (1.9%) |
| tfuma vol=1 check | 130,908 | 33,216 (25.4%) | 1,852 (1.4%) | 9 (0.0%) |

These rates are higher than the offline uniform-box envelope (start 64 u ~24.7% on open-leaf
first descents, tfuma ~2.5%) because the runtime check samples actual particle vertices, which
cluster near surfaces and spawn offsets; the counters are the contract. The owner visual
tolerance for the 64 u quantisation (dark/bright shifts near leaf boundaries) remains an owner
decision; `rt_particle_volume 0` is the rollback and the CPU resolve stays exact behind it.

## Validity and limitations

- Every run: clean exit path, live totals differ between the two samples (world not paused), save
  sha256 pinned; the previous harness generation could leave the menu/quit dialog open, so all
  runs of that generation (tags `gate-*`, `s3-*`) are marked invalid and are not used here; the
  gate conclusions are re-validated on this build (see the gate record's revalidation section).
- Single run per arm; window-maxima medians overstate per-frame values; the volume bake is a
  one-shot load cost (not included in the capture windows); the GPU upload and the shader-side
  sampling are Stage 4; classic and smoke replacements are code-reachable but not scene-reachable
  on the owner saves (classic 0 live, smoke 0), their parity must ride other content
  (e.g. the `demo1` capture with ~1090 classic sprites, a rocket-trail scene for smoke).
- `rt_particle_volume` is archived, default 1; `rt_particle_volume_check` is non-archived, default
  0. The mismatch counters reset with the other particle counters (bench start and idle windows).

## Changed files (this change)

`Quake/r_world.c` (volume build, generation, sampler/dispatcher, verify counters),
`Quake/gl_rlight.c` (`RT_ResolvePointClusterUncached`), `Quake/glquake.h` (declarations, report
fields), `Quake/gl_vidsdl.c` (cvars, counters, CSV, witness), `Quake/r_part_fte.c`,
`Quake/r_part.c`, `Quake/r_smoke.c` (consumer swap), `perf/particle_attribution.ps1`,
`perf/stage0/run_points_demo_ab.ps1` (menu fix), `docs/particle-plan.md`,
`docs/particle-current.md`, and this evidence document.
