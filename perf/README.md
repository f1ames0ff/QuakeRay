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

That stores `benchmark.baseline.json` next to the log. Later runs compare the last block's `fps` and every `cpu.slot` average:

```powershell
perf\compare_benchmark.ps1 -Benchmark ...\ad\benchmark.log
```

A metric that is worse by more than the tolerance (default 20 per cent; `-Tolerance 0.1` to tighten) fails the run with exit code 1. `-Index 0` selects an earlier block when a log holds several runs.

Recommended scenario for the DTAL-heavy case: Arcane Dimensions start with the torches and dynamic lights in view, 3840x2160, FSR 3.1 ultra performance (`rt_upscale_fsr31 5`), `vid_vsync 0`, the same route and settings on every run. Keep `rt_stats 0` while the benchmark runs so the profiler panels do not add work, and compare runs made on the same machine and driver.
