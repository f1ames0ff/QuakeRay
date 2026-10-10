# Stage 5: RHI setup decomposition report

Date: 2026-10-09. Branch `diag/rhi-setup-stage5`, base `93b890eb` (engine 0.31.3, master + host split + runner menu fix). Operator-approved temporary instrumentation; no public profiler schema, CSV or CMake changes.

Provenance note (2026-10-09): the decomposition below was captured on `diag/rhi-setup-stage5` at base `93b890eb` (engine 0.31.3). The candidate and acceptance sections ran on `perf/rhi-dyn-blas-reuse` (diagnostics) and `perf/rhi-dyn-blas-clean` (functional), which are now merged with `origin/master` `0c04f86f` (engine 0.40.0, render task graph). The "Post-merge task-graph adaptation" section below records the merged-tip builds, tests and same-binary `r_tasks 0/1` captures.

## Method

- Temporary per-frame `steady_clock` sub-timers inside the RHI setup bracket (`renderer/Source/RHI/NvrhiFrameSkeleton.cpp`) and inside `RhiAccelStructs::BuildTopLevel` / `AppendDynamicSlot` (`renderer/Source/RHI/RhiAccelStructs.cpp`). Samples accumulate in preallocated buffers without allocations or logging inside the measured regions; the CSV (`rhi_setup_diag_<stamp>.csv`, unique per run) is written at process exit and its name is echoed on stdout (`RHI_SETUP_DIAG file=…`).
- Scenarios and flow for every capture: `qr_gpu_heavy` (corrected save, sha256 `07CE5BB1…`) and `qr_fuma_start` (`27D2992C…`), Balanced, 8 s warmup + 6 s capture, screenshots, machine guard, one game instance. Capture window = the benchmark's own frames (the legacy rounds selected the last N gameplay rows of the diagnostic CSV; the candidate section below replaces that with the exact `bench_active` binding and re-measures the outer−inner relation); mean and p95 per bucket; the residual is computed frame-wise, never by subtracting percentiles.
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

`cpu.draw.RHI_setup_ms` (outer slot) means stay inside the run-to-run band across all builds (Fuma 3.12–3.42, Bog 4.12–4.49 ms): no measurable instrumentation overhead. The outer slot exceeds the instrumented inner span by ≈0.002 ms (0.0018–0.0022 ms across the legacy captures, measured with the exact window binding; the wider 0.02–0.40 ms range computed earlier was an artifact of the superseded last-N window selection). The `BeginGpuPass`/`EndGpuPass` bracket contributes only that residual and is reported separately.

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

`AppendDynamicSlot` is 96.3–96.5 % of the outer setup slot on Bogbottom and 87.2–88.3 % on Fuma (rounds 2–4 re-measured with the exact window binding: 4.00–4.05 ms of 4.15–4.20 on Bogbottom, 2.74–3.02 of 3.13–3.42 on Fuma). Its steady-state per-frame cost is the dynamic BLAS maintenance:

1. recording the dynamic builds (2.53 ms Bog / 1.18 ms Fuma) — scales with the dynamic geometry records per frame (~2336 / ~1285);
2. building the per-geometry descriptors and the shape vector (1.34 / 0.74 ms), including two heap-allocated per-filter vectors every frame;
3. recreating the BLAS handles on shape churn (0.15 / 0.75 ms; 0.5 / 1.3 recreations per frame).

## Recommended candidate (bounded reuse, no layout change)

1. Keep the existing BLAS handle while each frame's build fits the create-time allocation and recreate only when the list grows; validate the allocation semantics against the pinned NVRHI before use. Upper bound of the saving: the create share (≈0.75 ms Fuma, ≈0.15 ms Bog).
2. Reuse the per-filter `GeometryDesc`/`shape` buffers across frames instead of allocating them per frame (part of the 1.34 / 0.74 ms descriptor bucket).

The build-recording share is the largest bucket, but it is not decomposed further: inside it sit the engine-side animated geometry, NVRHI's own conversion (five temporary vectors per build, transform uploads, a build-size query), scratch management and the driver-facing build recording. How much of the 2.5 / 1.2 ms is irreducible animated-geometry cost is not measured; restructuring the per-geometry records would exceed stage 5's reuse scope. If the paired candidate runs cannot separate the candidate from the noise band, the plan falls back to stage 6 (brush styles).

Next: implement the candidate on a `perf/` branch from the same base with identical instrumentation on control and candidate, paired Bogbottom + Fuma runs, and decide per evidence.

## Stage 5 candidates: descriptor-buffer reuse and BLAS capacity reuse (2026-10-09)

Branch `perf/rhi-dyn-blas-reuse`, rebased onto `origin/refactor/dtal-cluster-dedup` @ `600a9278`. Method per the agreed protocol: temporary diagnostics, no public schema/CMake/threading changes. The binding fix `99eb7fb6` adds `steady_ms` and `bench_active` to the diagnostic CSV; the analysis selects the exact `rt_bench` window (the contiguous `bench_active` run) and accepts it only with count == samples == frames, `dropped=0`, `ui_only=0` and per-frame agreement of `total_ms` with the engine's `cpu.draw.RHI_setup_ms` (|Δ| ≈ 0.002 ms mean, ≤ 0.043 ms max across all runs). The runner flow fix `c4557972` (dismiss the menu after each load) sits below both arms: the single dismissal sent immediately after the QR_RELOAD marker was swallowed by the loading screen and the main menu stayed open for the whole capture (two rejected Bogbottom captures with `key_game=0` in every frame; the screenshot shows the main menu).

All runs: one frozen asset pack (`E6A6AFD5…`), 8 s warmup + 6 s capture, Balanced, one save per invocation, machine guard enforced by the runner (no separate per-run guard record is kept). The candidate-1 series alternates arms within pairs; the candidate-2b series was run in blocks after an aborted batch, and its final adjacent pair still separates the arms (control 3.078 ms, run after the candidate block's 2.657 ms). Executable hashes: binding-only control `34EEB9D6…` (`99eb7fb6`), descriptor reuse `93DA90F5…` (`b58abacf`), strict capacity reuse `42274498…` (`4c8121f6`), relaxed capacity reuse `E472945D…` (`5b89b25a`). For the phase-B and candidate-2b comparisons the control is the candidate-1 executable `93DA90F5…`; `34EEB9D6…` is the control of the candidate-1 comparison only. The rebased arm commits differ from the pre-rebase ones only in `tests/perf/run_stress.ps1` (`git diff` verified), so the compiled sources are identical.

### Candidate 1 — descriptor storage reuse (`b58abacf`) — accepted

The per-entry descriptor vector is refilled in place and the shape scratch persists instead of two per-filter allocations per frame. 2+2 per scenario, alternating order:

| metric, ms | Bog control | Bog cand. | Δ | Fuma control | Fuma cand. | Δ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| outer RHI setup | 4.218 | 4.110 | **−0.107** | 3.179 | 3.131 | **−0.048** |
| `dyn_descs_ms` | 1.389 | 1.301 | −0.088 | 0.754 | 0.717 | −0.037 |
| `dyn_build_ms` | 2.633 | 2.636 | +0.002 | 2.027 | 2.012 | −0.015 |

Both per-pair differences are negative on both scenarios (Bog −0.103/−0.111; Fuma −0.029/−0.066). On Bog the effect exceeds the within-arm repeat spreads (0.018/0.010 ms); on Fuma the outer effect (−0.048 ms) is only marginally above the control spread (0.042 ms) and is read as weak on the bracket, while the pre-registered mechanism metric `dyn_descs_ms` (−0.037 ms) separates cleanly from its repeat spreads (0.011/0.0003 ms) and carries the acceptance.

### Candidate 2 (strict) — capacity reuse with equal geometry count (`4c8121f6`) — rejected

The handle was kept while the current shape fit the create-time envelope with the same geometry count. 3+3 per scenario: recreations did not move (Bog 0.541→0.559 per frame; Fuma 1.331→1.317), outer deltas stayed inside the control spreads (−0.045 Bog, +0.024 Fuma). The same-count envelope reuses nothing in this workload.

### Candidate 2b — capacity reuse, geometry count may shrink (`5b89b25a`) — accepted

`vkGetAccelerationStructureBuildSizesKHR` guarantees the create-time size for any build whose `geometryCount` is at most the queried count and whose per-index primitive and vertex counts are at most the queried maxima (bounds may shrink; only increases recreate), so dropping trailing geometries is a spec-safe reuse. 3+3 per scenario (control = the candidate-1 executable `93DA90F5…`):

| metric | Bog control | Bog cand. | Δ | Fuma control | Fuma cand. | Δ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| outer RHI setup, ms | 4.161 | 4.172 | +0.011 | 3.103 | 2.559 | **−0.544** |
| recreations / frame | 0.514 | 0.450 | −0.064 | 1.262 | 0.336 | −0.925 |
| `dyn_create_ms` | 0.150 | 0.128 | −0.022 | 0.759 | 0.210 | −0.549 |
| `dyn_build_ms` | 2.628 | 2.653 | +0.025 | 1.977 | 1.449 | −0.528 |
| `dyn_descs_ms` | 1.346 | 1.336 | −0.010 | 0.726 | 0.709 | −0.017 |
| `dyn_record_ms` | 2.457 | 2.471 | +0.014 | 1.202 | 1.209 | +0.007 |

Fuma per-run outers: control 3.118/3.114/3.078 vs candidate 2.503/2.517/2.657 — complete separation (0.42 ms gap); spreads 0.040/0.154. Bog is neutral on the bracket (Δ inside the spread) while the mechanism still moves down (~12 % fewer recreations); no bucket regresses beyond noise. On Fuma the gained time equals the removed create path (Δcreate ≈ Δouter).

Safety: the predicate uses the spec's per-build properties (equal type/flags, per-index primitive-count and maxVertex maxima, equal vertex format, index type, stride and transform presence, `geometryCount` ≤). Geometry flags, vertex format and build flags are invariants of the filter key, `MakeGeometryDesc` and the module constants. The guarantee conditions were checked against the spec text and the pinned NVRHI paths: the create-time size query (`vulkan-raytracing.cpp:341-402`) and the build-time re-query with a skip on insufficiency (`:839-851`), so a predicate error degrades to a skipped build, never to an out-of-bounds write. The predicate's transform word is the engine address non-zero test, which is at least as strict as the descriptor's effective transform presence for reuse; any residual mismatch is covered by the guard. Runtime checks: no `build requires at least`/skip errors in any candidate capture logs, and the screenshots render (the `6/6 dynamic BLAS` counter counts active entries before the NVRHI call and is supporting evidence only).

Scope and limits: Debug builds only; two scenarios (Bogbottom/AD swampy, Fuma/id1); 6 s captures at ~25–38 FPS; the accepted functional chain is `b58abacf` → `4c8121f6` → `5b89b25a` on top of the shared base (the strict policy of `4c8121f6` is superseded by `5b89b25a`, which builds on the scratch infrastructure it introduces; only the 5b89b25a policy is accepted), with `c4557972` as the runner fix worth keeping (the binding/instrumentation commits are temporary diagnostics). Raw evidence: `build/Debug/audit-stage5c-*` (manifests with revision/executable/asset hashes, `frames.csv`, `rhi_setup_diag.csv`, screenshots, console logs); build logs binding each executable hash to its revision are retained at `build/Debug/audit-stage5c-build-logs/`.

Conclusion: both separable mechanisms were measured. Descriptor churn removal is a small win (−0.11 ms Bog / −0.05 ms Fuma); handle recreation on geometry-count shrink is a large win on Fuma (−0.54 ms, −17.5 % of the bracket, reproduced at −0.65 ms on the clean build) and neutral on Bog. The build-recording share (~2.5 / ~1.2 ms) is not reduced by this reuse scope and is not decomposed further here.

## Acceptance on the clean functional branch, tests and runner fixes (2026-10-09, post-audit)

The audit of the candidate section found three gaps: the functional state lived only on the diagnostics branch, the Debug test suite had not been run for it, and the runner dismissal/wait had two defects. All three are closed:

- **Clean functional branch** `perf/rhi-dyn-blas-clean` (pushed): base `600a9278` → `9bce4f4b` (runner) → `96b1b45a` (descriptor/capacity reuse) → `79d91902` (shape-fit test). `git diff` against the diagnostics branch shows only the timer insertions; against the base, only the functional changes (no diagnostics, no CMake change — the predicate test extends the existing `frame_timing_tests` target).
- **Tests**: `.\build_win.ps1 Debug -Tests` + `ctest --test-dir build/Debug` — 7/7 passed at the diagnostics head `b792c995` (twice, including the final redeploy) and again with the tree at the clean functional head `79d91902` (the test sources are byte-identical between the branches). `frame_timing_tests` now covers the shape-fit policy (identity, count and per-index bound shrink, empty shape, growth of the count, of bounds and of a later geometry, stride/index-type/transform mismatch, malformed lengths). Evidence: `build/Debug/audit-stage5c-tests/ctest.log`; the build receipt at the deployed revision matches the executable.
- **Runner**: the wait marker matched the startup NVRHI capability line (`...acceleration structures yes`) and never actually waited; it now waits for `RHI: acceleration structures:`. The spawn scenario (`qr_swampy_start`) needs no dismissal — `Host_Map_f` clears the menu — and a spawn capture is valid without one (160/160 frames in game); the save flows keep the per-load dismissals.
- **Paired acceptance on the clean builds** (2+2 per scenario, alternating, one frozen pack `E2CA480B…`, exes control `9314AEFE…` (`9bce4f4b`) / candidate `75A5852B…` (`79d91902`)):

| metric, Fuma | control | candidate | Δ |
| --- | ---: | ---: | ---: |
| outer RHI setup, mean | 3.172 | 2.526 | **−0.646** |
| outer RHI setup, p95 | 3.594 | 3.232 | −0.362 |
| frame mean | 26.834 | 25.889 | −0.946 |
| frame p95 | 28.114 | 27.075 | −1.039 |
| FPS | 37.27 | 38.63 | +1.36 |

  Complete separation on the outer bracket: candidate 2.546/2.507 vs control 3.148/3.196. Bogbottom remains neutral (Δ +0.033 ms inside a 4.05–4.38 candidate spread), the two spawn captures are neutral (4.097 vs 4.061), and a Quality smoke on Fuma shows the same reduction (−0.724 ms, one run per arm, not a statistical claim). On Fuma every frame metric improves, which also addresses the earlier p95 concern from the diagnostics-branch pair.
- **Coverage limits**: Balanced on Bogbottom and Fuma plus the spawn scenario, Quality on Fuma; save reload and fresh spawn cover level transitions, but long gameplay, other maps and task-agent integration are not covered by this set. The task-agent integration limit is closed by the post-merge section below; long gameplay and other maps remain untested.

## Post-merge task-graph adaptation (2026-10-09)

The merged tips were built and measured after `origin/master` `0c04f86f` (engine 0.40.0, render task
graph); this closes the earlier "task-agent integration not covered" limit. No functional code change
was required for `r_tasks 1`: the RHI recording still runs inside the single ordered end task
(`GL_EndRenderingTask` -> `qrDrawFrame`), serialized by `prev_end_rendering_task ->
begin_rendering_task` and the end-task join, and the reuse state is per frame slot. The
`bench_active` diagnostic sample reads `rt_bench_active` exactly as the pre-existing end-task read
in `gl_vidsdl.c` does; the commands that flip it run after the join, so the read is ordered and was
left unchanged.

Builds and tests: every tip passes `.\build_win.ps1 Debug -Tests` plus CTest 8/8 (receipts and logs
under `build/Debug/audit-stage5d-*`).

| Tip | Revision | exe sha256 (16) | Role |
| --- | --- | --- | --- |
| `perf/run-stress-capture-fixes` | `9d7a6804` | `5374A9CB…` | control (sources byte-equal to master) |
| `perf/rhi-dyn-blas-clean` | `dc89557a` | `910D42E8…` | functional deliverable |
| `perf/rhi-dyn-blas-reuse` | `3b5a18ec` | `FCD31349…` | diagnostics twin (this branch) |
| `diag/rhi-setup-stage5` | `2b49b3c1` | `4FA11F71…` | historical diagnostics validation |

Same-binary captures (staged runner, Balanced, 8 s warmup + 6 s capture, one save per invocation,
ABBA order, `cpu.wait_ms` as the mode check), mean/p95 ms:

| Scenario | Arm | Control | Clean | Reuse | Diag |
| --- | --- | --- | --- | --- | --- |
| Fuma | `r_tasks 0` | 27.51/28.48; 27.47/28.33 | 27.07/28.92; 26.93/28.93 | 26.80/28.32; 27.08/28.46 | 27.59/28.58; 28.05/29.45 |
| Fuma | `r_tasks 1` | 20.04/22.78; 20.72/24.20 | 20.15/23.06; 20.01/22.72 | 20.01/22.65; 20.13/22.94 | 19.92/22.75; 20.01/22.82 |
| Bogbottom | `r_tasks 0` | 41.04/42.10; 40.97/42.17 | 40.58/41.82; 40.53/41.61 | 40.88/41.87; 40.95/41.84 | - |
| Bogbottom | `r_tasks 1` | 31.59/32.46; 31.78/33.82 | 31.56/32.50; 32.09/36.62 | 31.67/33.33; 31.41/32.17 | - |

Mechanism (`rhi_setup_diag` on the reuse tip, `bench_active` window; `total_ms` alignment on diag,
mean |Δ| ≈ 0.002 ms):
handle recreations per frame are 0.27-0.52 serial (Fuma/Bogbottom) and 2.00 under `r_tasks 1` on
both scenarios, with `dyn_create_ms` 1.02 (Fuma) and 1.45-1.49 (Bogbottom); the diag branch's
equality predicate also shows 2.00 recreations per frame under tasks (serial 1.32-1.36). The cause is
the task-scheduled order of dynamic geometry inside a filter against the per-index create-time
envelope: allowing bounds to shrink does not help once the order changes.

Percentiles are the higher-order statistic over `interval_ms` (first partial frame excluded). A
third clean Bogbottom tasks repeat (35.56/40.94 ms; identical executable, pack and checks) was
uniformly elevated across independent counters and is retained raw; it does not change the no-gain
reading (median 32.09 vs the control's 31.69 ms).

Extended coverage (2026-10-10): AD hub Balanced and Fuma/AD/Bogbottom Quality were captured with
`r_tasks 1` on rebuilt matched binaries (control `EF499999…`, clean `8018D4B4…`). Control means:
20.55/19.43 (AD Balanced), 23.52/23.66 (Fuma Quality), 24.03/24.36 (AD Quality), 33.84/33.34 (Bog
Quality) ms; clean: 20.29/19.31, 23.63/23.57, 23.13/24.46, 34.52/33.57 ms. Every cell stays inside
its observed spread; no cell separates the candidate from the control. One contaminated clean Fuma
Quality pair (25.99/25.52 ms) is retained raw; a same-binary rerun matched the control. Full table,
captures and identities: `PERFORMANCE.md` on `perf/rhi-dyn-blas-clean`.

Window note: the raw diagnostics show the 2.00 recreations per benchmark-window frame (load/ramp
frames excluded); full-play windows include a 6-create ramp and rare 1/3-create frames, and the diag
CSV predates the `bench_active` binding (its serial ~1.3 figure comes from a trailing diagnostic
window).

Decision: no code change on this evidence branch. The order-insensitive create-time envelope (or a
different reuse policy) is the next bounded candidate, with its upside bounded by the measured
recreation cost. Raw captures: `build/Debug/audit-s5d-{ctrl,clean,reuse,diag}-*`; receipts and
CTest logs: `build/Debug/audit-stage5d-*`.
