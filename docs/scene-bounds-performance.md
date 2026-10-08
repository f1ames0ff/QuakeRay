# Scene geometry bounds experiment

## Branch and scope

Candidate branch: `perf/scene-bounds-simd`.
Baseline source: `393323cf`, the validated audit instrumentation on top of master `feff66d5` and the post-effects test fix.

This experiment changes only the CPU accumulation of transformed scene bounds in `Scene::Upload`. It does not change visibility, geometry submission, model animation, materials, acceleration-structure flags, GPU shaders, particle processing, or cluster-list construction.

## Rationale

The baseline transforms every uploaded vertex, rejects non-finite or extreme world coordinates, and updates six scalar extrema. This happens during geometry submission, before the DrawFrame timers. The scalar implementation performs its matrix arithmetic and extrema updates separately for each axis and writes the bounds repeatedly.

The candidate processes the three world-coordinate components together with SSE2, prepares matrix columns once per upload, keeps extrema in registers, and writes them back once. It still visits every vertex; no stale-pose bounds, approximate corner bounds, or camera-frustum deletion is introduced. Non-SSE2 builds retain the scalar implementation.

The SIMD arithmetic retains the scalar multiply/add order. Ordered selection retains the previous value on equal extrema, including signed zero. A bitwise finite check precedes the range comparison; the extra lane duplicates the first coordinate instead of multiplying infinity by a dummy zero coefficient.

## Correctness checks

`tests/frame_timing_tests.cpp` compares candidate and scalar extrema by their 32-bit representations. Cases include:

- empty uploads and preinitialized bounds;
- 8192 vertices and 24 deterministic random affine transforms;
- NaN and positive/negative infinity;
- the inclusive +/-10,000,000 coordinate limit and adjacent rejected values;
- wholly invalid uploads and an invalid transform;
- equal positive/negative-zero extrema.

The old scalar algorithm remains an explicit reference and portability fallback in `renderer/Source/GeometryBounds.h`.

## Performance protocol

Use Debug for the build, tests, standalone benchmark, and game captures. Do not compile, benchmark, or run a GPU test while another game/performance owner is active.

```powershell
.\build_win.ps1 Debug -Tests -Parallel 12
ctest --test-dir build/Debug --output-on-failure -j 1
.\build\Debug\frame_timing_tests.exe --bench-bounds
.\tests\perf\run_stress.ps1 -Saves qr_fuma_start -Presets balanced -Seconds 10 -Warmup 8 -Tag bounds-candidate
```

The standalone benchmark uses deterministic transformed vertices, alternates scalar/candidate ordering, and checks that the result remains observable and finite. Its ratio is an isolated Debug CPU result, not an FPS claim.

End-to-end A/B uses identical saves, graphics settings, particle/cluster behavior, and runtime assets. Compare per-frame interval distributions and CPU alias/brush submission totals, not the old per-invocation overlay maxima. Keep the baseline executable and shader pack before deploying the candidate, so both arms can be rerun without reverting user work.

## Status

- [x] Implement the candidate on its own branch.
- [x] Add bitwise reference checks and a standalone benchmark entry point.
- [x] Run Debug correctness tests after the baseline capture finishes.
- [x] Run the isolated CPU benchmark.
- [ ] Compare all three supplied saves at both target FSR presets.
- [ ] Retain the change only with correctness and measured benefit established.

The Debug build and all five CTests pass. The deterministic bitwise bounds checks pass. The isolated Debug benchmark measured 74.948 ms for the scalar arm and 27.0734 ms for SIMD over 2,097,152 vertices, a 2.77x ratio. This does not establish an end-to-end FPS gain.

The initial gameplay attempt was rejected before capture because the runtime lacked foreground focus. No contaminated FPS result is retained. Further A/B attempts use one save/preset per invocation and a 180-second runtime budget, yielding the machine between short runs instead of holding a long multi-save queue.

### First short gameplay measurement

Terror Fuma, Debug, 4K FSR Balanced, unchanged `qr_audit_max.cfg`, 8-second warmup and 6-second capture:

| Metric | Earlier repeated baseline | First SIMD capture |
| --- | ---: | ---: |
| FPS | 31.5 | 34.5 |
| Mean interval | 31.73 ms | 29.00 ms |
| Opaque entity CPU | about 16.8 ms | 14.41 ms |
| Alias CPU, including alpha | about 5.3 ms | 3.65 ms |
| Brush CPU, including alpha | about 12.0 ms | 11.34 ms |

Candidate data: `build/Debug/audit-bounds-short-fuma-20261008-102803-39ce70/`. The short paired-control attempt was deferred after the machine remained busy for three minutes. The first capture is promising but not a completed interleaved A/B campaign; the other saves/presets remain unmeasured for this candidate.

Four independent read-only reviewers found no concrete regression in the SSE2 bounds calculation, its scalar fallback, Scene integration, or tests. Their consensus findings concerned the short-run harness lifecycle, which is being repaired and checked separately.

### Controlled entity-profile follow-up

The same-runtime integration comparison in `docs/entity-cpu-combined-performance.md` now isolates this change against a transform-only control. On Bogbottom Balanced, alias geometry API time decreased from 8.44 to 4.47 ms, brush API time from 3.81 to 2.84 ms and FPS increased from 21.15 to 23.89. On Fuma Balanced, alias API time decreased from 3.46 to 1.80 ms and FPS increased from 33.62 to 37.18. Both arms used identical effective settings and recorded asset/save hashes. The current short-run budget is 300 seconds per invocation, superseding the earlier 180-second limit. This is still not an all-save/all-preset validation.
