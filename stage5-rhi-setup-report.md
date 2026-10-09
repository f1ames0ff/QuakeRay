# Stage 5: RHI setup decomposition report

Date: 2026-10-09. Branch `diag/rhi-setup-stage5`, base `93b890eb` (engine 0.31.3, master + host split + runner menu fix). Operator-approved temporary instrumentation; no public profiler schema, CSV or CMake changes.

Superseded note (2026-10-09): this is the original decomposition snapshot. The residual claim in it (0.02–0.40 ms) was corrected on the reuse branch after the exact `bench_active` window binding (≈0.002 ms). The accepted candidate analysis, the merged-tip builds/CTest and the same-binary `r_tasks 0/1` re-validation live on `perf/rhi-dyn-blas-reuse` and `perf/rhi-dyn-blas-clean`; this branch is kept as historical evidence (post-merge build and CTest 8/8 were also run on its tip).

## Method

- Temporary per-frame `steady_clock` sub-timers inside the RHI setup bracket (`renderer/Source/RHI/NvrhiFrameSkeleton.cpp`) and inside `RhiAccelStructs::BuildTopLevel` / `AppendDynamicSlot` (`renderer/Source/RHI/RhiAccelStructs.cpp`). Samples accumulate in preallocated buffers without allocations or logging inside the measured regions; the CSV (`rhi_setup_diag_<stamp>.csv`, unique per run) is written at process exit and its name is echoed on stdout (`RHI_SETUP_DIAG file=…`).
- Scenarios and flow for every capture: `qr_gpu_heavy` (corrected save, sha256 `07CE5BB1…`) and `qr_fuma_start` (`27D2992C…`), Balanced, 8 s warmup + 6 s capture, screenshots, machine guard, one game instance. Capture window = the benchmark's own frames (last N gameplay rows of the diagnostic CSV); mean and p95 per bucket; the residual is computed frame-wise, never by subtracting percentiles.
- Runner fixes this branch carries so captures are valid on this engine build: dismiss the main menu after the level start (Escape after the `demo(s) in loop` marker), restart the map and reload the save before the warmup (the operator's workaround for the intermittent level-load black screen), and re-acquire the foreground with a 3 s tolerance instead of aborting on a transient focus loss. Captures: `92df74af`, `c464abd9`, `498275a0`.

## Identity and instrumentation overhead

Executables (from each run's `manifest.json`); `id1/qray.pkz` is repacked by every build and recorded per run in `assets.json`; the executable is the only code difference between builds.

| build | exe sha256 (16) | pkz sha256 (16) | Bog outer mean/p95 (fps) | Fuma outer mean/p95 (fps) |
| --- | --- | --- | --- | --- |
| base (uninstrumented) | D72A15211A1C7F9E | 93493050C1894907 | 4.197/4.52 (25.1), 4.157/4.47 (25.2) | 3.139/3.68 (37.2), 3.340/3.78 (37.0) |
| round 1 | 0FA71E21F5C0DEC1 | 6DC155FBF5B14511 | 4.485/4.46 (24.4), 4.347/5.36 (23.8) | 3.119/3.55 (37.3), 3.209/3.83 (36.5) |
| round 2 | E502BD6B781F19CD | A6D7EE01D00F159D | 4.165/4.58 (24.7), 4.265/4.75 (24.3) | 3.417/4.09 (35.1), 3.218/3.66 (36.6) |
| round 3 | 5A3432EB16315A5A | F8EACF90B1C3E1CE | 4.149/4.50 (25.0), 4.137/4.42 (25.0) | 3.319/3.98 (35.7), 3.141/3.59 (37.4) |
| round 4 | 759DF74EDA9EB250 | 963B470D90FFAF23 | 4.202/4.52 (24.9) | 3.125/3.54 (36.8) |

`cpu.draw.RHI_setup_ms` (outer slot) means stay inside the run-to-run band across all builds (Fuma 3.12–3.42, Bog 4.12–4.49 ms): no measurable instrumentation overhead. The outer slot exceeds the instrumented inner span by 0.02–0.40 ms (the `BeginGpuPass`/`EndGpuPass` bracket incl. profiler and GPU-timer-query recording); that bracket cost is outside the sub-buckets and is reported separately.

## Decomposition (bucket means, ms)

Round-1 level — within the setup span:

| bucket | Bog-1 | Bog-2 | Fuma-1 | Fuma-2 |
| --- | --- | --- | --- | --- |
| resources/descriptors | 0.012 | 0.017 | 0.008 | 0.008 |
| AS (BuildStatic+BuildTopLevel) | 4.050 | 4.468 | 3.067 | 3.084 |
| uniform patch | 0.004 | 0.005 | 0.004 | 0.004 |
| preprocessing | 0.017 | 0.021 | 0.016 | 0.016 |
| residual | 0.000 | 0.000 | 0.000 | 0.000 |

Round-2 level — inside the AS bucket:

| bucket | Bog-1 | Bog-2 | Fuma-1 | Fuma-2 |
| --- | --- | --- | --- | --- |
| as_static | 0.001 | 0.001 | 0.001 | 0.001 |
| as_toplevel | 4.257 | 4.206 | 3.342 | 3.148 |
| tl_static (instances) | 0.015 | 0.014 | 0.016 | 0.012 |
| **tl_dynamic (dynamic slot)** | **4.138** | **4.090** | **2.975** | **2.810** |
| tl_particles | 0.020 | 0.019 | 0.264 | 0.248 |
| tl_vertex_copies | 0.065 | 0.064 | 0.067 | 0.059 |
| tl_rest | 0.018 | 0.018 | 0.019 | 0.017 |

Round-3 level — inside `AppendDynamicSlot`:

| bucket | Bog-1 | Bog-2 | Fuma-1 | Fuma-2 |
| --- | --- | --- | --- | --- |
| prefix copies | 0.012 | 0.012 | 0.011 | 0.010 |
| descriptor+shape construction | 1.337 | 1.338 | 0.776 | 0.748 |
| build path (lookup+create+move+record) | 2.611 | 2.584 | 2.071 | 1.966 |
| instance synthesis | 0.003 | 0.004 | 0.003 | 0.003 |
| handle recreations per frame | 0.553 | 0.449 | 1.326 | 1.245 |

Round-4 level — inside the build path (single runs):

| bucket | Bog | Fuma |
| --- | --- | --- |
| handle create + retire | 0.153 | **0.745** |
| build recording (`buildBottomLevelAccelStruct`) | **2.533** | **1.181** |
| remainder of the path | 0.043 | 0.026 |

## Dominant mechanism

`AppendDynamicSlot` is 91–96 % of the outer setup slot (3.95–4.19 ms of 4.12–4.49 on Bogbottom; 2.72–2.87 of 3.10–3.42 on Fuma). Its steady-state per-frame cost is the dynamic BLAS maintenance:

1. recording the dynamic builds (2.53 ms Bog / 1.18 ms Fuma) — scales with the dynamic geometry records per frame (~2336 / ~1285);
2. building the per-geometry descriptors and the shape vector (1.34 / 0.74 ms), including two heap-allocated per-filter vectors every frame;
3. recreating the BLAS handles on shape churn (0.15 / 0.75 ms; 0.5 / 1.3 recreations per frame).

## Recommended candidate (bounded reuse, no layout change)

1. Keep the existing BLAS handle while each frame's build fits the create-time allocation and recreate only when the list grows; validate the allocation semantics against the pinned NVRHI before use. Upper bound of the saving: the create share (≈0.75 ms Fuma, ≈0.15 ms Bog).
2. Reuse the per-filter `GeometryDesc`/`shape` buffers across frames instead of allocating them per frame (part of the 1.34 / 0.74 ms descriptor bucket).

The build-recording floor (the largest share) is the animated-geometry cost unless the per-geometry records are restructured; that exceeds stage 5's reuse scope. If the paired candidate runs cannot separate the candidate from the noise band, the plan falls back to stage 6 (brush styles).

Next: implement the candidate on a `perf/` branch from the same base with identical instrumentation on control and candidate, paired Bogbottom + Fuma runs, and decide per evidence.
