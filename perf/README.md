# Performance tests

Two layers guard the frame cost:

- a headless GPU microbenchmark for the textured-area-light (DTAL/TAL) sampling loop, run with `ctest` next to the renderer's existing GPU regression tests;
- an in-game `rt_bench` comparison for the whole frame, driven by `compare_benchmark.ps1`.

Both compare against a stored baseline with a relative tolerance and fail when a run is slower, so a change that costs frame time is visible before it ships.

## Headless GPU microbenchmark

Build with the tests enabled (the project default is Debug):

```powershell
.\build_win.ps1 Debug -Tests
```

Run the GPU tests:

```powershell
ctest --test-dir build\Debug -R qray_dtal_perf --output-on-failure
```

The first run writes the baseline to `build\Debug\renderer\Tests\DtalPerf.baseline.txt` and passes. Later runs compare each case (`small`, `medium`, `large`, `huge`; more lights and samples per thread per case) with a 20 per cent tolerance. Accept intentionally changed numbers with:

```powershell
build\Debug\renderer\Tests\qray_dtal_perf.exe build\Debug\renderer\Tests\DtalPerf.comp.spv build\Debug\renderer\Tests\DtalPerf.baseline.txt --update
```

`--tolerance 0.1` tightens the comparison. The probe calls the renderer's own `sampleTexturedAreaLight` from `Light.hlsli`; the bindless texture fetch is stubbed, so a change to the texture table alone is not covered by this test.

The existing clouds regression runs the same way (`-R qray_clouds_gpu`). GPU tests need a Vulkan device, so the GitHub workflow, which only builds, does not run them.

## In-game benchmark comparison

Record or pick a demo of the worst spot and run it with the engine's benchmark command. With the benchmark log in hand (`<game>\benchmark.log`, written by `rt_bench <demo>`):

```powershell
perf\compare_benchmark.ps1 -Benchmark C:\Users\f1am3d\repos\vkquake-rt\build\Debug\ad\benchmark.log -Update
```

That stores `benchmark.baseline.json` next to the log. Later runs compare the last block's `fps`, every `cpu.slot` average and the `cpu.cluster` counters:

```powershell
perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log
```

A metric that is worse by more than the tolerance (default 20 per cent; `-Tolerance 0.1` to tighten) fails the run with exit code 1. `fps` and `cluster:hits` fail when they drop; `cluster:misses` and the millisecond slots fail when they rise. `-Index 0` selects an earlier block when a log holds several runs.

Recommended scenario for the DTAL-heavy case: Arcane Dimensions start with the torches and dynamic lights in view, 3840x2160, FSR 3.1 ultra performance (`rt_upscale_fsr31 5`), `vid_vsync 0`, the same route and settings on every run. Keep `rt_stats 0` while the benchmark runs so the profiler panels do not add work, and compare runs made on the same machine and driver.

### Strict coverage

The comparison never drops a metric silently. A metric that is in the log but missing from the baseline (or whose baseline is zero or negative) is still listed in the table and printed as a warning; `-Strict` turns any coverage gap, in either direction, into a failure:

```powershell
perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log -Strict
```

`-RequireMetrics` pins the columns a run must contain; a missing one fails even without `-Strict`:

```powershell
perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log -RequireMetrics 'slot:frame','slot:particles sim','fps'
```

The names are the ones the guard prints (`fps`, `slot:<label>`, `main:<label>`, `cluster:hits`, `cluster:misses`) plus the p95 names below. Both switches are opt-in: without them the old comparison runs, but skipped metrics are now reported instead of ignored.

### p95 from per-sample dumps

`benchmark.log` holds run averages and maxima. Per-sample data comes from the stats recorder: `rt_stats_dump_start` begins capturing the readout, and `rt_stats_dump_end` (or the 30 s / 700 sample cap) writes `stats-<datetime>.dump` in the game directory. Pass it with `-Samples` and the guard computes the nearest-rank p95 of every `fps`, `cpu.*_ms` and `gpu.*_ms` column and compares it as `p95:<column>`, for example `p95:cpu.frame_ms`:

```powershell
perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log -Samples ...\ad\stats-20261007-120000.dump -Strict
```

Record the baseline with the same flags (`-Update -Samples ...`): a plain `-Update` stores only what that run measured and drops the stored p95. The dump must come from the same demo, route, machine and driver as the log, because the guard treats the two captures as one baseline. A `p95_ms=` attribute that `benchmark.log` carries per slot is compared as `<metric>:p95` once the engine writes it (pending instrumentation, below).

### Particle Stage 0 workflow

Stage 0 of `docs/particle-plan.md` is the measurement gate for the particle work, and these runs are its evidence. The planned control demos are `ad_particle_heavy` (dense particle trail) and `ad_particle_idle` (the same viewpoint at rest), recorded on Arcane Dimensions `start` from one saved `setpos`/`setangle`. Record them once, keep demos and logs out of git, and hold the settings fixed on every run (`vid_vsync 0`, `rt_stats 0`, the same resolution, FSR/DLSS and render scale as the stored baseline). Stage 2 adds the transport switch `r_particles_points` (default `1`, archived): a compact-point run holds it on, and `perf\stage0\arm_points_off.cfg` holds it at `0` for the legacy `QrVertex` transport, so pin it on every run like the settings above:

1. `rt_bench ad_particle_heavy quit` plays the demo and appends a block to `<game>\benchmark.log` (for example `build\Debug\ad\benchmark.log`).
2. `rt_stats_dump_start`, replay the same demo window, `rt_stats_dump_end` writes `stats-<datetime>.dump`.
3. Store the control baseline once: `perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log -Samples ...\ad\stats-....dump -Update`.
4. After every change, repeat steps 1-2 and compare: `perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log -Samples ...\ad\stats-....dump -Strict -RequireMetrics 'slot:frame','slot:particles sim','p95:cpu.particles_sim_ms'`.
5. The Stage-0 attribution table (resolve vs expansion vs upload vs FTE conversion vs smoke vs sim) comes from the companion `perf\particle_attribution.ps1` script, which compares the dumps of the ablation arms against a named baseline arm; see its help for arguments.

The `ad_particle_*` demos are still pending: until they are recorded, `-RequireMetrics` on their slot and p95 names fails by design. The columns themselves have landed (the table below).

### Particle statistics columns

The instrumentation has landed these columns; the status tracks the guard's contract, so a column is only pinned once the baseline has been re-recorded. The guard picks up `cpu.*_ms`/`gpu.*_ms` dump columns automatically as p95 metrics with `-Samples`, and a new `cpu.slot` line becomes a `slot:<label>` metric automatically, so the switches start covering a column as soon as the data appears.

| Column | Home | Meaning | Status |
|---|---|---|---|
| `cpu.particles_sim_ms` | dump, `benchmark.log` slot | classic particle simulation | landed |
| `cpu.particles_resolve_ms` | dump, `benchmark.log` slot | cluster/light resolve per particle | landed |
| `cpu.particles_fill_ms` | dump, `benchmark.log` slot | CPU fill of particle geometry | landed |
| `cpu.particles_upload_ms` | dump, `benchmark.log` slot | particle upload to the GPU | landed |
| `cpu.fte_convert_ms` | dump, `benchmark.log` slot | FTE representation conversion | landed |
| `particles_classic` | dump | live classic particles | landed |
| `particles_fte` | dump | live FTE particles | landed |
| `particles_vertices` | dump | vertices emitted by the particle paths | landed |
| `particles_smoke` | dump | live smoke particles | landed |
| `particles_dropped` | dump | particles dropped by the overflow policy | landed |
| `particles_emit_culled` | dump | pointparticles messages dropped beyond `r_part_emit_distance` | landed |
| `particles_emit_faded` | dump | pointparticles messages distance-faded inside the radius window | landed |
| `particles_volume_samples` | dump | cluster-volume verifier samples (`rt_particle_volume_check`) | landed |
| `particles_volume_mismatch` | dump | of those, volume cluster != CPU cluster | landed |
| `particles_volume_lost` | dump | volume 0 where the CPU cluster is non-zero | landed |
| `particles_volume_extra` | dump | volume non-zero where the CPU cluster is 0 | landed |
| `fte_convert_bytes` | dump | bytes written by FTE conversion | landed |
| `particle_upload_bytes` | dump | particle bytes uploaded per frame | landed |
| `gpu.particles_ms` | dump | GPU particle pass timer | landed |
| `rays_particle` | dump (`QrFrameStats`) | particle-ray counter | landed |
| `particles_cache_hits` | dump | point-cluster resolve cache hits per window | landed |
| `particles_cache_misses` | dump | point-cluster resolve cache misses (full walks) per window | landed |
| `particles_cache_avg_ns` | dump | mean resolve cost per profiled call | landed |
| `raster_upload_bytes` | dump (`QrFrameStats`) | bytes accepted by the raster collector | landed |
| `raster_upload_dropped_batches` | dump (`QrFrameStats`) | batches dropped by the raster collector | landed |
| `clust_pub_mask` | dump, `cpu.cluster` block line | reason bits of the last cluster light-list publication attempt (see `QR_CLUSTER_PUB_*` in `qray.h`); `skip device`/`skip slot` bits mean the publication was avoided, `copy` means the lists were rewritten | landed |
| `clust_pub_skips` | dump, `cpu.cluster` block line | frames of the window whose cluster light-list publication reused the device's lists | landed |
| `clust_pub_copies` | dump, `cpu.cluster` block line | frames of the window whose cluster light-list publication rewrote the lists | landed |

The counter columns (`particles_*`, `*_bytes`, `rays_particle`, `raster_upload_*`, the cache counters) are recorded but not compared yet: the guard's dump reader computes p95 only for the timing columns, so the counters need their own compare rule (direction and zero baseline) before they can be pinned. The timing columns above need no parser change.

Stage 2 changes the particle transport without adding columns: the compact-point path against the legacy `QrVertex` transport is measured by the existing `cpu.particles_fill_ms`, `cpu.particles_upload_ms`, `particle_upload_bytes` and `raster_upload_bytes` columns.
