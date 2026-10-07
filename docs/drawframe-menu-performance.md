# DrawFrame menu performance

## Scope

Worktree: `drawframe-menu`. Branch: `perf/drawframe-menu`, based on `ade13ae0`.

- Skip the console draw when an opaque menu background will cover it. Preserve the console itself, translucent menu previews, and normal console rendering.
- Add an explicit UI-only frame flag for an absent or not-yet-signed-on world. A menu over a loaded game must keep rendering the scene.
- Clear and render the UI at display resolution without tracing, denoising, exposure, or upscaling. Preserve post-UI CRT/wipe processing, screenshots, presentation, and frame synchronization.
- Skip legacy TLAS/preprocessing and redundant raster staging copies on UI-only frames. Keep the native submission because it can contain resource uploads and owns the engine frame fence.
- Publish GPU timings as a complete snapshot of one frame slot. A skipped pass reports zero, not a previous frame's measurement. Make compose exclusive of upscale/post/UI timings.
- Add optional CPU timings for DrawFrame preparation, hot reload, descriptors, staging, legacy AS work, RHI slot wait/collection, query readback, setup, scene recording, compose, upscale, post effects, UI, submission, and presentation. Report window averages and maxima separately.
- Do not remove legacy scene-frame work without validating its remaining consumers.
- On return from UI-only rendering, reject ASVGF/TAAU history and reset FSR/exposure accumulation for the first scene frame. `ShGlobalUniform.restirParams[2]` carries this reset flag; the existing ReSTIR enable/candidate fields remain unchanged.
- Fit the additional CPU timing column to the available overlay width, including 1280x720 windows.

## Verification

- Build and test Debug with runtime assets using `./build_win.ps1 Debug -Tests`.
- Exercise UI-only frame selection and opaque-console policy with regression tests.
- Verify CPU profiler reset/disabled behavior.
- Run renderer GPU tests and menu/console/game transition checks when the local runtime and game data are available.
- Compare menu raster call counts, CPU average/max, and GPU pass timings against the baseline. Do not infer performance improvements from a successful build alone.

### Local commands

```powershell
.\build_win.ps1 Debug -Tests
ctest --test-dir build/Debug --output-on-failure -j 1
.\tests\perf\run_menu.ps1 -Baseline <baseline-debug-runtime> -Seconds 8 -Smoke
.\tests\perf\run_menu.ps1 -Seconds 2 -Smoke -Validation
.\tests\perf\run_menu.ps1 -Baseline <baseline-debug-runtime> -BaselineOnly -Seconds 2 -Smoke -Validation
```

Both runtimes need the same base-game PAK files. The menu runner restores existing `config.cfg` and `qray.txt` files after its run. It keeps generated stats captures, screenshots, and diagnostic logs in each runtime directory. The candidate-only smoke checks cover a plain menu, CRT processing, a loaded level, the in-game menu, returning to the disconnected menu, and a resize.

The renderer GPU tests load loose SPIR-V files from `renderer/Build`. For verification here, those files are extracted from the freshly deployed `build/Debug/id1/qray.pkz`, so the tests run the shaders shipped by the Debug build.

### Results so far

- Debug build and runtime asset deployment succeeded.
- All five CTest tests passed: frame timing/policy, lighting, clouds GPU, post-effects GPU, and DTAL performance.
- The post-effects GPU test verifies that normal TAAU reuses valid history and the UI-to-scene reset ignores it.
- An exact pre-change Debug baseline is built from the `ade13ae0` Git archive in the approved OpenCode temporary directory. It does not modify the original worktree.

### Menu A/B measurement

Both arms use Debug, a 1920x1080 window, the same base-game PAKs and default rendering settings, VSync off, `rt_stats 3`, a five-second warmup, and an eight-second capture. The machine reports a Ryzen 9 7950X and Radeon RX 9070 XT.

| Metric | Baseline | Candidate |
| --- | ---: | ---: |
| Median of CPU DrawFrame window maxima | 3.305 ms | 0.460 ms |
| Median of CPU DrawFrame window averages | Not available | 0.351 ms |
| Median RHI GPU frame time | 5.730 ms | 0.230 ms |
| Median raster API calls at sampling | 1815 | 8 |

CPU maxima and averages are different statistics; the comparison deliberately uses the old maximum-window metric in both arms. These are local work-time measurements, not a promise about wall FPS or another resolution/GPU. The enlarged candidate overlay is included in its measurements.

Raw captures:

- Baseline: `<OpenCode temporary directory>/drawframe-menu-baseline-ade13ae0/build/Debug/id1/stats-20261007-212331.dump`.
- Candidate: `build/Debug/id1/stats-20261007-212352.dump`.
- In-game menu: `build/Debug/id1/stats-20261007-212403.dump`; primary tracing remains active.
- Returned disconnected menu: `build/Debug/id1/stats-20261007-212411.dump`; scene GPU passes and legacy-AS/staging/scene CPU phases are zero.

The UI-only CPU breakdown averages roughly 0.115 ms for presentation, 0.061 ms for UI recording, 0.048 ms for RHI submission, 0.028 ms for slot collection, and 0.024 ms for GPU query readback. Scene recording, compose, upscale, and legacy AS work are absent.

### Validation outcome

The final Debug build deploys its runtime assets and passes all five CTest tests again. Runtime smoke checks also pass for both plain and CRT menus, a loaded level, its in-game menu, returning to UI-only rendering, screenshots, and a 1280x720 resize. The additional timing column fits the resized window.

Full-engine Vulkan validation is **not clean**. The same five VUID families occur in the unchanged baseline and the candidate:

- `VUID-VkDeviceCreateInfo-pNext-06532`: overlapping promoted device feature structures.
- `VUID-VkSamplerCreateInfo-pNext-06726`: min/max sampler reduction without its device feature.
- `VUID-vkQueueSubmit-pSignalSemaphores-00067`: present semaphore reuse by frame slot instead of acquired swapchain image.
- `VUID-vkCmdDispatch-None-08600`: descriptor/pipeline-layout compatibility around the native FSR interop path.
- `VUID-VkImageMemoryBarrier2-oldLayout-01197`: native image-layout hand-off mismatches.

This comparison reports matching error families, not a clean validation pass or proof that every occurrence is harmless. Existing bloom storage-image format warnings also remain. Device initialization, swapchain synchronization, bloom formats, and FSR interop fixes are outside this menu-performance change and are not silently suppressed by the runner.

Diagnostic logs are retained as `build/Debug/menu-validation-candidate.log` and `build/Debug/menu-validation-baseline.log`. The menu runner intentionally fails its validation gate for these errors. The original worktree retains only its pre-existing NVRHI submodule modification.

## Progress

- [x] Create an isolated worktree and branch.
- [x] Initialize independent submodules.
- [x] Implement the console draw policy.
- [x] Implement coherent GPU timing snapshots and exclusive compose timing.
- [x] Implement optional CPU timing breakdowns.
- [x] Implement the UI-only frame path.
- [x] Build, run regression checks, and record results, including the non-clean full-engine validation limitation.
