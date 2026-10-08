# Stage 0 particle attribution

Stage 0 of `docs/particle-plan.md`: attribute the particle bucket before any fix is designed. The
harness pins one scenario (Arcane Dimensions `start`, `setpos 357 -212 322 0 158 0`) and compares a
baseline run against ablation arms through the `stats-*.dump` recordings that
`rt_stats_dump_start`/`rt_stats_dump_end` write. `perf\particle_attribution.ps1` summarizes the
dumps: mean, p95 and the delta against the baseline arm for every frozen column.

## Files

| file | contents |
| --- | --- |
| `particle_heavy.cfg` | the pinned scenario settings and viewpoint; also the reset point used before every arm |
| `particle_idle.cfg` | the same pins for the idle scenario (same viewpoint, no firing) |
| `arm_baseline.cfg` | the baseline particle state: every system on |
| `arm_no_classic.cfg` | `r_particles 0` |
| `arm_flat.cfg` | `r_particle_lighting 0` |
| `arm_no_smoke.cfg` | `r_smoke 0` |
| `arm_no_fte.cfg` | `r_fteparticles 0` |
| `arm_partcache_off.cfg` | `rt_particle_resolve_cache 0` |
| `arm_points_off.cfg` | `r_particles_points 0` |

## Workflow

1. Build Debug and copy the configs into the game directory, where the engine's `exec` looks for
   them: `Copy-Item perf\stage0\*.cfg build\Debug\ad\`.
2. Launch the game with Arcane Dimensions and pin the machine-dependent settings by hand, once per
   session: the same resolution for every run (the reference runs use 3840x2160), the same upscaler
   (reference: `rt_upscale_fsr31 5`), the same demo, route and driver. The control configs pin
   `vid_vsync 0`, `host_maxfps 0`, `rt_stats 3` and `rt_stats_interval 0.2`.
3. Load demo and benchmark: `exec particle_heavy`, then `playdemo ad_particle_heavy`, or
   `rt_bench ad_particle_heavy` to also append the frame-level profile to `benchmark.log`.
   `particle_idle` stands at the same viewpoint without firing. Record both demos once with
   `record` and keep them out of git, as the plan asks.
4. Dump stats for an arm: before every arm run `exec particle_heavy` (the reset), then
   `exec arm_<name>`, then `rt_stats_dump_start`. The recording stops by itself after 30 s
   (`rt_stats_dump_end` stops it sooner) and writes `build\Debug\ad\stats-<datetime>.dump`. Name or
   move each dump after its arm. The `cpu.*` columns are only measured while the CPU profile panel
   is on, which is why `rt_stats 3` is pinned in the control configs.
5. Run the attribution script:

   ```powershell
   perf\particle_attribution.ps1 -Baseline baseline -Arms @{
       baseline      = 'build\Debug\ad\stats-heavy-baseline.dump'
       no_classic    = 'build\Debug\ad\stats-heavy-no_classic.dump'
       flat          = 'build\Debug\ad\stats-heavy-flat.dump'
       no_smoke      = 'build\Debug\ad\stats-heavy-no_smoke.dump'
       no_fte        = 'build\Debug\ad\stats-heavy-no_fte.dump'
       partcache_off = 'build\Debug\ad\stats-heavy-partcache_off.dump'
       points_off    = 'build\Debug\ad\stats-heavy-points_off.dump'
   } -Output perf\stage0\attribution.md
   ```

   `-Csv` writes the long format instead; `-Output` writes the report to a file. A column that a
   dump does not carry is warned about and shown as `-`, never skipped silently. The `cpu.*` values
   are per-window maxima, so the script's mean and p95 are statistics of those maxima, not of
   per-frame times.
6. Repeat step 4 for every arm and rerun the script. Compare arms recorded in one session on one
   machine and driver.

Stage 2 adds the transport switch: a run that measures the compact-point path holds
`r_particles_points 1` (its default), and `arm_points_off` is the only arm that sets it to 0 for
the legacy `QrVertex` transport. Name the cvar in the control configs so every run pins it, and
because it is archived, set it back to 1 (`exec arm_baseline` does not name it today) before a run
that follows `points_off`.

## Arms and expected signature

| arm | settings | expected signature against baseline |
| --- | --- | --- |
| baseline | `r_particles 2`, `r_particle_lighting 1`, `r_smoke 1`, `r_fteparticles 1`, `rt_particle_resolve_cache 1` | the reference row |
| no_classic | `r_particles 0` | the classic pool and trails stop, and so do the FTE sim and sky weather (`r_part_fte.c:6983`); `particles_classic` -> 0; `cpu.particles_sim_ms`, `cpu.particles_resolve_ms`, `cpu.particles_fill_ms`, `cpu.particles_upload_ms` and `gpu.particles_ms` drop |
| flat | `r_particle_lighting 0` | the lit shader pair is replaced by the old flat colour; the per-vertex cluster scan and rays stop, so `rays_particle` and `gpu.particles_ms` drop while the counts stay |
| no_smoke | `r_smoke 0` | smoke puffs stop; rockets and grenades fall back to classic trails (`r_part.c:675-685`), so `particles_classic` may rise while `particles_smoke` -> 0 |
| no_fte | `r_fteparticles 0` | the FTE sim stops and its effects fall back to classic; `particles_fte`, `particles_vertices` and `fte_convert_bytes` -> 0, `cpu.fte_convert_ms` -> 0, `particles_classic` may rise |
| partcache_off | `rt_particle_resolve_cache 0` | once the cache cvar exists: the point-cluster cache is off, `cpu.particles_resolve_ms` rises and fps falls; the counts stay unchanged |
| points_off | `r_particles_points 0` | the legacy `QrVertex` transport instead of the 24 B point: `particle_upload_bytes` and `raster_upload_bytes` rise (3 `QrVertex` per sprite, ~240 B), `cpu.particles_fill_ms` rises, the live counts stay; the measured fps/slot delta against baseline is what the Stage-2 gate reads |

## Manual smoke check (instrumentation)

One run in a normally loaded map proves the particle paths feed the new columns: load a demo or a
save the usual way, `exec verify_particles.cfg`, and open the newest `stats-<datetime>.dump`. The
config spawns smoke puffs, records the readout for ~4 s and writes the dump; it does not quit the
game. Expected in at least part of the rows: `particles_smoke` > 0, `cpu.particles_resolve_ms` and
`gpu.particles_ms` > 0, `particles_cache_hits + particles_cache_misses` > 0, and `rays_particle` > 0
while `r_particle_lighting 1`.

## Automated Stage-2 point A/B

`run_points_ab.ps1` drives one engine session end to end: it loads a save, dismisses the menu, waits
for the world spawn, then fires the rocket launcher for a few seconds twice - first with
`r_particles_points 1`, then with `0` - recording an `rt_bench` block and a stats dump per arm:

```powershell
powershell -File perf\stage0\run_points_ab.ps1 -Save start1 -FireSeconds 5
```

It writes `stage2-points-on.dump` / `stage2-points-off.dump` into the game directory, prints the new
benchmark blocks (frame and particle slots) and runs `particle_attribution.ps1` on the two dumps.
Requirements: the save rests where firing keeps particles in view, the player owns the rocket
launcher (the script selects it with `impulse 7`), and no other engine instance is running. Keep
the engine window uncovered while the run is in flight - an unfocused window sleeps 16 ms per frame.

## Landed instrumentation

- The Stage-0 slots and columns are in this tree (commits `c53c52d8`, `e89abc1c`): the dump carries
  every column the attribution script lists, `rt_particle_resolve_cache` is registered, and the
  control configs and arms can be run as written.
- Stage 2 adds `r_particles_points` (default `1`, archived) and no new columns; `arm_points_off`
  switches the classic traffic back to the legacy transport. A build predating the Stage-2 work
  prints `Unknown command` for it and runs the rest of the `exec`, so the arm is not a hard failure
  on an older binary.
- `particle_heavy.cfg` and `particle_idle.cfg` pin `r_particles_points 1`; the reset before every
  arm stays `exec particle_heavy`.
