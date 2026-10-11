# Fixed-scene performance tests

The cluster work (candidates, fast lists, overflow tails, incremental maintenance) is CPU work,
while the tail publication and the shader branch are GPU work. A fix can only be told from a
passing measurement when the scene and the load shape are fixed, so every measurement point is a
save and every save is run in two load modes:

| Mode | Shape | Settings |
|---|---|---|
| `gpumax` | GPU bound, CPU with headroom | 3840x2160, no upscaling, denoiser on, `rt_gi_level 2`, godrays extreme, clouds high, bloom, exposure, sharpen, vignette, filmgrain |
| `cpumax` | CPU bound, GPU nearly idle | 1280x720 window, no upscaling, denoiser off, `rt_gi_level 0`, no godrays, no clouds, no post |

Each mode config unlocks the video mode and restarts it (`vid_unlock`, then
`vid_width`/`vid_height`, then `vid_restart`) so the resolution in the table is the one measured: a
mode set from a config is otherwise ignored, because the startup video mode stays locked while the
config files run. Every block records the mode that was really in force in its `vid=` field.

Everything that shapes the cluster system is pinned in `qr_perf_common.cfg` and is identical in
both modes: `rt_light_reach_static 25`, `rt_light_reach_dynamic 10`, `rt_cluster_incremental 1`,
`rt_cluster_assert 0`, `rt_stats 0`, `developer 0`, `vid_vsync 0`, `host_maxfps 0`, and both
particle systems are off (`r_particles 0`, `r_fteparticles 0`) so the known particle-side engine
issue cannot distort the cluster measurements. The benchmark records the settings it ran under in
its block header, so a report says which load shape it is.

## The measurement points

A point is a save of a live scene, made with the engine this repository builds:

```
build\Debug\quakeray.exe -basedir build\Debug -game ad
```

Stand where the test should run, let the scene settle for a second or two and save it with a name
of its own, for example:

```
save qrt_start_bigroom
save qrt_ad_combat
```

One save per point; the runner runs the same save in both modes. Useful points:

- a room with many particles and moving lights (drives the per-change cluster work);
- a view with heavy geometry and reflections (drives the GPU);
- the worst place the game was ever reported slow in.

Saves live in `build\Debug\ad\<name>.sav`. A save made in another worktree has to be copied there,
and the mod version and the map must be the ones this build ships. Pick scenes whose action is the
steady state of the place (a fight, a machine, a waterfall), not the few seconds of a scripted
intro: an intro is a frame-rate-dependent transient, and a save taken inside one measures the
intro rather than the place.

## Running a point

```
powershell -File tests\perf\run_place.ps1 -Save qrt_start_bigroom -Mode both
```

The runner copies the mode config into the game directory, starts the engine, loads the save once,
dismisses the menu and then measures two arms, each from a reload of the same save so both start
from the same snapshot:

1. `rt_cluster_sampling 1`, the save reloaded, `-Warmup` seconds (default 6) and `-Seconds`
   seconds of measurement;
2. `rt_cluster_sampling 0` over the same reload, warmup and time.

Each arm is summed by the benchmark (`rt_bench start` / `rt_bench stop`, appended to
`benchmark.log`) and printed on exit. `-Tag` also copies the two raw blocks into the game
directory. `analyze_bench.py` prints the last blocks as a table:

```
python tests\perf\analyze_bench.py build\Debug\ad\benchmark.log 4
```

## Reading the numbers

- The primary metric is the `cpu.slot` averages in the `rt_bench` block: `frame`, `clusters`,
  `clust lists`, `clust topup`, `clust mark`, `clust grid`, `clust tail`, `clust fill`,
  `clust upload`, `clust publish`. Wall fps
  (`fps=` in the block header) is frames over wall seconds; an unfocused window sleeps 16 ms per
  frame in this engine, so wall fps is only comparable between runs that keep focus. The runner
  leaves the window in place, but do not cover it or work on the machine during a run.
- Compare blocks that share `demo` (the save or map name), `vid`, `rt_light_reach_static` and the
  `rt_cluster_sampling` flag. The two modes are compared within themselves: a CPU fix shows in
  `cpumax`, and `gpumax` tells whether the same fix leaves the GPU bound frame alone.
- `cpumax` exists to make the CPU the frame's limiter: the cluster cost moves the frame there, and
  in `gpumax` the same cost hides under the GPU frame, which is what the mode is for.
- `rt_cluster_assert 1` must never appear in a performance run: the validator walks every cluster,
  slot and tail entry per frame and costs about as much as the whole cluster pass on a large map.

## Files

| File | Purpose |
|---|---|
| `qr_perf_common.cfg` | cluster and measurement settings shared by both modes |
| `qr_perf_gpumax.cfg` | GPU-bound overrides |
| `qr_perf_cpumax.cfg` | CPU-bound overrides |
| `run_place.ps1` | runs one save in one or both modes and prints the two blocks |
| `analyze_bench.py` | prints the last `benchmark.log` blocks as a table |
