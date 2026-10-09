# Agent performance evidence index

Use [ARCHITECTURE.md](ARCHITECTURE.md) to navigate from a counter to its producer/backend entry point. This file separates measured results from architectural facts and unverified leads. Do not begin a repository-wide search or an optimization before selecting the relevant scenario and binary.

**Evidence date:** 2026-10-08. **Source map:** `7a14d0fe`, inherited by `perf/architecture`. No new game run was performed to create this index. Numbers below are read from the retained per-frame captures, not from the example scenarios in the task or from overlay maxima.

## Targets and current status

| Output / preset | Target | Frame budget |
| --- | ---: | ---: |
| 3840x2160 / FSR Balanced (`rt_upscale_fsr31 3`) | Stable 60 FPS | 16.67 ms |
| 3840x2160 / FSR Quality (`rt_upscale_fsr31 2`) | Stable 45 FPS | 22.22 ms |

The audited maximum-quality preset is [tests/perf/qr_audit_max.cfg](tests/perf/qr_audit_max.cfg), with volumetric sky Low (`rt_sky_clouds_quality 0`). Particles/FTE, smoke, enhanced models, viewmodel, GI level 2, NEE 2, reflection/refraction depth 8, denoising and glass effects remain enabled. Main captures use `rt_stats 0`, uncapped host FPS, vsync off and `vid_maxframelatency 0`; artistic strengths are the recorded preset values, not arbitrary slider maxima. The runner [sets/validates 4K and the effective preset](tests/perf/run_stress.ps1#L169).

**Targets remain unmet.** The verified combined entity changes remove roughly 5.8 ms on Fuma and 9.0 ms on Bogbottom from opaque-entity CPU preparation, but the latest controlled results are only about 37 / 24 FPS. Supplied saves are stress points, not all-map or long-gameplay coverage.

## Measurement contract

| Quantity | What it actually measures | Source / constraint |
| --- | --- | --- |
| `interval_ms`, FPS, p95/p99 | Screen-end to screen-end intervals; FPS = `1000 / mean(interval_ms)`. Includes work and waits between completed screen frames. The first partial interval is zero and excluded. | [`RT_Prof_FrameEnd`](Quake/gl_vidsdl.c#L541), [`summarize`](tests/perf/analyze_stress.py#L55). |
| `host_interval_ms` | Unclamped host frame interval, distinct from simulation-scaled time. | [Raw host time assignment](Quake/host.c#L747), [sample field](Quake/gl_vidsdl.c#L555). |
| `cpu.frame_ms` | Inclusive `SCR_UpdateScreen` wall time, including begin-frame waits and backend calls. **Not** the whole host frame, input, server, audio or all particle simulation. | [Screen bracket](Quake/gl_screen.c#L1161), [post-screen work](Quake/host.c#L1008). |
| Quake CPU slots | Sum of calls to a slot in each captured frame. Parents contain children. `ents` is the opaque pass; entity-type slots also count the alpha pass. Fine-grained upload slots can also be invoked by world/weapon producers. | [Slot accumulator](Quake/gl_vidsdl.c#L488), [entity dispatch](Quake/gl_rmain.c#L790), [frame accumulation](Quake/gl_vidsdl.c#L563). |
| `cpu.viewmodel_ms` / `cpu.vm_draw_ms` | Inclusive task versus narrower weapon/debug draw. Task includes entity/world-model lights, teleports and cluster publication. | [`R_DrawViewModelTask`](Quake/gl_rmain.c#L1211). Neither is a GPU weapon timer. |
| `cpu.qrDrawFrame_ms` / `cpu.draw.*` | Inclusive renderer call versus its CPU phases. Entity geometry API preparation occurs earlier; `RHI_setup` contains several setup operations, not AS work alone. | [Call bracket](Quake/gl_vidsdl.c#L3213), [RHI setup recording](renderer/Source/RHI/NvrhiFrameSkeleton.cpp#L666). |
| `gpu.frame_ms` / `gpu.*` | Asynchronously completed timestamp-query snapshots for the timed NVRHI command list. Query results come from a reused frame slot, not the CPU sample's synchronized present latency. The separately submitted legacy Vulkan list, acquire/display pacing and scanout are not this timer's full scope. | [Read/poll queries](renderer/Source/RHI/NvrhiFrameSkeleton.cpp#L496), [frame-query bounds](renderer/Source/RHI/NvrhiFrameSkeleton.cpp#L662), [legacy submission](renderer/Source/VulkanDevice.cpp#L1105). |
| `rt_stats 3` / statistics dumps | CPU/GPU inspection overlay and reporting-window aggregates. Quake slot maxima are per invocation in a window, not whole-frame percentiles; sequential entity chunks can differ from benchmark frame totals. | [`RT_Prof_End`](Quake/gl_vidsdl.c#L507), [`RT_Prof_Update`](Quake/gl_vidsdl.c#L600), [GUI](Quake/rt_stats_gui.c). |
| `cpu.window_id`, `cpu.window_frames`, `cpu.renderer_samples` | Reporting-window identity, completed-frame denominator and available renderer CPU samples. Repeated window IDs are repeated snapshots, not new publications. These fields belong to statistics dumps, not the per-frame benchmark CSV. | [Window lifecycle](shared/rt_prof_window.h), `RT_StatsRecordWrite`; `run_stress.ps1 -StatsLevel 3` checks advancing complete windows. |
| `calls_geometry`, `calls_raster`, `calls_lights` | Geometry/raster/light API call counts recorded with the frame. Useful workload checks, not triangle counts or standalone cost attribution. | [CSV serialization](Quake/gl_vidsdl.c#L763), [geometry API counter](renderer/Source/VulkanDevice.cpp#L1396). |

`rt_bench` enables CPU and GPU-pass collection while `rt_stats 0` avoids the overlay and per-ray statistics flag: [debug flags](Quake/gl_vidsdl.c#L3150), [CPU enable](Quake/gl_vidsdl.c#L3209). Benchmark samples are buffered; CSV writes happen [at reporting](Quake/gl_vidsdl.c#L732), not once per measured frame.

Do not sum CPU and GPU frame times, add nested slots, assign `compose` entirely to particles, or derive an "unknown CPU" value by subtracting overlapping counters. **Unattributed/unknown:** dedicated input/server/audio/CPU-GUI costs, begin-frame fence/acquire wait, per-entity GPU costs and the internal staging/metadata/AS split not separately instrumented here. `cpu.wait_ms` is a task-join timer, not total GPU waiting.

## Scenarios and binary identities

Hardware reported by the B0 console capture: **Ryzen 9 7950X, Radeon RX 9070 XT, AMD 26.8.1, Vulkan 1.4.349**. Engine version 0.31.0. All captures and verification builds below are **Debug**.

| Scenario | Supplied save | Map | Save SHA256 |
| --- | --- | --- | --- |
| Fuma | `qr_fuma_start.sav` | `maps/ad_tfuma.bsp` | `27D2992C64A14332E1804C139580778A7AB595C86F28FD1C39289BDF4ECC3326` |
| AD hub | `qr_ad_start.sav` | `maps/start.bsp` | `A1FCE672F441BE2938A9D9D717868A25CB87E83230552BDD23CE8F6A3BDFEE88` |
| Bogbottom | `qr_gpu_heavy.sav` | `maps/ad_swampy.bsp` | `07CE5BB16B6498521AEF63951F46AEAA3E9785255B3CD3099624DC2631BA6427` |

The name `qr_gpu_heavy` is not evidence that Bogbottom is GPU-bound: its measured CPU preparation dominates these captures. Matching AD PAK/material/light assets are required; a save name alone is not a reproducible scenario. The Bogbottom save was replaced with the synced revision on 2026-10-09; captures before that recorded `F9C09773…` under the same name. Match the manifest's asset hash, never the name alone.

| ID | Source / role | Executable SHA256 |
| --- | --- | --- |
| B0 | Initial repeated audit baseline on `feff66d5` + GPU-test fix + basic audit instrumentation. Manifest records `1d7c0ddc` **with dirty profiling files**; instrumentation was subsequently committed as `393323cf`. Do not call this a clean master binary. | `F046D7361EC4702D8A39982AD9F4F54396A3004A78F9A69F2E76EDA3DEB2A317` |
| C0 | Detailed entity-profile control, code captured by `2ece9b9f`; no transform/SIMD change. Local `build/entity-profile-binaries/control.exe`. | `5F1ABE72D851DF877DBC07F458FD58D4050225FF069A468C937354900991379B` |
| C1 | Exact brush-transform reuse, `adc3cd32`, `perf/entity-transform-reuse`. Local `transform-only.exe`. | `1E4FC2CD5D6CDD140ED7C939E91B9491BCA43E7BB73450FB6A0B9E6EB45148BE` |
| C2 | C1 + SIMD bounds (`8b4c0664`, cherry-pick of `d419e38a`), documented by `7a14d0fe`, `perf/entity-cpu-combined`; inherited by this branch. Local `combined.exe`. | `A7D9F5657EF3918D1E31CED8299876EA6EC6DE981C3CD74FB661DF120B26269F` |

B0 engine pack SHA256: `3121CCC6419109E1CCE19087328C8E9C9BA262709DDF3F613A0B07861CF65208`. The controlled C0/C1/C2 integration campaign uses `360D3E586C3C77E8B416C6E643319E6CD0F093010899E7763937897FE6770CEB`. The earlier transform-only campaign has a different pack hash; its pairs were checked separately. **Manifest `Revision` alone cannot identify a swapped executable**; use executable and asset hashes plus working changes and effective settings.

## B0 repeated baseline

Two live runs per save/preset, **8-second warmup + 10-second capture**, particles and sound enabled, `rt_stats 0`. Each table cell is the **median of the two per-run summaries**; with two runs this is their arithmetic midpoint. P95/p99 columns are medians of per-run percentiles, **not** pooled-frame percentiles. CPU/GPU cells are medians of per-run means. Raw locations and filename rules are in [capture index](#capture-index).

| Scenario / preset | FPS | Mean interval | CPU screen mean | GPU snapshot mean | p95 | p99 | Intervals over target |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fuma / Balanced | 31.52 | 31.73 ms | 30.18 ms | 20.72 ms | 33.24 ms | 35.19 ms | 100% |
| Fuma / Quality | 31.86 | 31.39 ms | 29.91 ms | 23.21 ms | 32.93 ms | 35.44 ms | 100% |
| AD hub / Balanced | 48.97 | 20.42 ms | 19.95 ms | 17.50 ms | 24.32 ms | 25.19 ms | 100% |
| AD hub / Quality | 43.86 | 22.80 ms | 22.28 ms | 21.47 ms | 28.43 ms | 32.82 ms | 42.97% |
| Bogbottom / Balanced | 20.52 | 48.72 ms | 46.43 ms | 20.74 ms | 49.91 ms | 53.43 ms | 100% |
| Bogbottom / Quality | 20.56 | 48.63 ms | 46.33 ms | 23.73 ms | 49.69 ms | 50.33 ms | 100% |

### B0 CPU routing breakdown

Balanced, milliseconds; the same two-run aggregation. Indented/type/subphase rows overlap their parents. Counter scopes and real functions are indexed in [ARCHITECTURE.md](ARCHITECTURE.md#subsystem-index).

| Counter | Fuma | AD hub | Bogbottom |
| --- | ---: | ---: | ---: |
| `cpu.mark_ms` | 2.18 | 0.70 | 3.99 |
| `cpu.world_ms` | <0.01 | <0.01 | <0.01 |
| `cpu.sky_ms` (also water / animation) | 0.85 | 0.39 | 1.25 |
| `cpu.ents_ms` (opaque) | **16.79** | 3.86 | **29.55** |
| `cpu.ents_alias_ms` (both passes) | 5.34 | 1.94 | 13.27 |
| `cpu.ents_brush_ms` (both passes) | 12.02 | 1.89 | 16.66 |
| `cpu.alpha_ms` | 0.84 | 0.08 | 0.56 |
| `cpu.particles_ms` | 3.48 | **8.41** | 3.77 |
| `cpu.viewmodel_ms` (inclusive) | 0.159 | **2.357** | 0.096 |
| `cpu.vm_draw_ms` (weapon / debug draw only) | **0.027** | **0.017** | **0.017** |
| `cpu.clusters_ms` (inside viewmodel task) | 0.069 | **2.170** | 0.047 |
| `cpu.qrDrawFrame_ms` | 5.62 | 3.78 | 6.91 |
| `cpu.draw.RHI_setup_ms` (inside renderer) | 3.27 | 1.71 | 4.47 |
| `cpu.draw.slot_wait_ms` | 0.002 | 0.001 | 0.003 |
| `cpu.draw.present_ms` | 0.208 | 0.112 | 0.227 |

Observed priorities: entity preparation on Fuma/Bogbottom; particles and cluster publication in the hub. The supplied saves **do not reproduce a 30 ms weapon-only CPU draw**. Tiny RHI slot-wait values do not rule out unmeasured begin-frame acquire/fence waits.

### B0 GPU routing breakdown

Balanced GPU timestamp snapshots, milliseconds. These are backend pass buckets, not world/particle/viewmodel attribution.

| Counter | Fuma | AD hub | Bogbottom |
| --- | ---: | ---: | ---: |
| `gpu.frame_ms` | 20.72 | 17.50 | 20.74 |
| `gpu.setup_ms` (copies / AS / preprocessing) | 4.03 | 1.97 | 6.43 |
| `gpu.clouds_ms` | 0.65 | 0.54 | 0.67 |
| `gpu.sky_ms` | 0.17 | 0.15 | 0.17 |
| `gpu.primary_ms` | 0.92 | 0.76 | 0.86 |
| `gpu.godrays_ms` (includes its shadow-map work) | 0.28 | 0.23 | 0.28 |
| `gpu.reflrefr_ms` | 0.15 | 0.19 | 0.51 |
| `gpu.gradient_ms` | 1.03 | 1.14 | 0.29 |
| `gpu.direct_ms` | 3.02 | 3.13 | 1.89 |
| `gpu.indirect_ms` | **6.77** | **5.58** | **6.37** |
| `gpu.compose_ms` (also raster particles / smoke / alpha, bloom) | 2.50 | 2.64 | 2.01 |
| `gpu.upscale_ms` | 0.83 | 0.80 | 0.87 |
| `gpu.post_ms` | 0.28 | 0.27 | 0.30 |
| `gpu.ui_ms` | 0.008 | 0.008 | 0.009 |
| `gpu.present_ms` (fullscreen draw) | 0.040 | 0.037 | 0.032 |

`decals`, `reflgodr` and `postui` were zero in these Balanced summaries. Standalone GPU **world / viewmodel / all-shadows** times are **unknown**: those counters do not exist at this snapshot; the particle branch (`calm-eagle`, not part of these captures) adds a dedicated `particles` pass timer (`gpu.particles_ms`) and the `rays.particle` counter. The measured GPU means already exceed 16.67 ms, so CPU improvements alone cannot establish the 60-FPS target. GPU shading experiments remain a separate, deferred priority.

## Controlled entity improvements

**Do not use B0 as the paired control for C2.** C0 adds fine-grained timers and has more profiling overhead; its entity totals are higher than B0. The following comparisons use newly captured full controls after deployment, with matching effective settings and every recorded asset/save hash within each pair. They use **8-second warmup + 6-second live capture**, one save/preset per invocation.

| Scenario / preset | C0 FPS → C2 FPS | Mean interval | Opaque entities | p95 interval |
| --- | ---: | ---: | ---: | ---: |
| Fuma / Balanced | 29.64 → **37.18** | 33.74 → 26.90 ms | 18.44 → **12.63 ms** | 34.97 → 28.09 ms |
| Bogbottom / Balanced | 19.58 → **23.89** | 51.06 → 41.87 ms | 31.60 → **22.62 ms** | 52.33 → 42.90 ms |
| Fuma / Quality | 29.76 → **37.46** | 33.61 → 26.70 ms | 18.62 → **12.59 ms** | 34.69 → 27.88 ms |

C2 Balanced repeats: Fuma **37.55 FPS / 12.65 ms entities**; Bogbottom **23.89 FPS / 22.82 ms entities**. First paired Balanced observations show 25.4% / 22.0% higher FPS, not an all-map guarantee. C2 has not been compared on AD hub, Bogbottom Quality or long transition/gameplay routes.

Independent contributions, measured on their respective controlled arms:

| Change | Concrete mechanism / code | Measured evidence |
| --- | --- | --- |
| C1: brush transform reuse | [`RT_GetBrushModelMatrix`](Quake/r_world.c#L958) retains the exact last entity/origin/angle matrix per thread; misses use unchanged arithmetic. | Fuma packing matrix slot **3.71 → 0.53 ms**; Bogbottom **4.14 → 0.62 ms** in the transform-only campaign. [Report](docs/entity-transform-reuse-performance.md). |
| C2: SIMD bounds on C1 | [`Scene::Upload`](renderer/Source/Scene.cpp#L94) → [`AccumulateGeometryBounds`](renderer/Source/GeometryBounds.h#L52); same vertices, finite/range rules and operation order, scalar fallback. | C1 → C2 Bogbottom alias uploads **8.44 → 4.47 ms**, brush uploads **3.81 → 2.84 ms**; Fuma alias **3.46 → 1.80 ms**, brush **2.86 → 2.02 ms**. [Integration report](docs/entity-cpu-combined-performance.md), [SIMD semantics/tests](docs/scene-bounds-performance.md). |

Both changes passed the full Debug CTest suite, **5/5**, including scalar-reference bounds checks, transform-cache tests and cloud/post-effects GPU smoke tests. Transform-copy correctness and input invalidation are tested; Fuma screenshots provide a visual smoke check, not pixel-identical temporal validation. No visibility deletion, quality preset reduction, particle/cluster rewrite, geometry persistence or GPU-shading change is part of C2. Neither solution was merged into master during this campaign.

## Merged-master pose gate (2026-10-09)

First per-frame capture on the merged tree (`b4269845`): Debug `quakeray.exe` SHA256
`D0D27C09161CC7B148D09624733B9640B69FBF69BEFB0FA6BC6FEA4FF663DE1B`, engine pack
`034CBC93986E3CC64EB2D0793E3B16730696AC8CEE7532F4A685D4F1430ADB1D`, balanced preset
(`qr_audit_max.cfg`, FSR Balanced, 3840x2160 windowed, particles/FTE on, `rt_stats 0`), one
Bogbottom run, 8 s warmup + 6 s capture; capture
`build/Debug/audit-pose-gate-manual-20261009-101307`. Result: 19.8 FPS, mean 50.53 ms, p95 58.65 ms,
p99 63.72 ms. CPU means: `frame` 47.13, `ents` 25.59 (alias 11.60, brush 15.28), `alias_pose` 2.79
(p50 2.72, p95 3.36, max 4.36, every frame above 1.5), `alias_upload` 4.94, `alias_lights` 1.00,
`particles` 4.29, `qrDrawFrame` 8.03; `gpu.frame` 25.38. Alias pose preparation is the measured gate
for the multithreading workstream and matches the C2 order of magnitude.

Runner note: the strict analyzer (`393323cf`) rejects captures where the startup main menu is still
up (`key_game=0`, frozen `client_time`); `run_stress.ps1` sends Escape after the save-loaded marker
and before warmup (`18b6005e`). The pose-gate capture above predates that change and came from an
equivalent local script. After the change and the Bogbottom save sync, runner captures are
`build/Debug/audit-esc-fix-20261009-114605-52d1c1` (exe `B01F8831…`, revision `bd559b6a`, old save,
23.3 FPS / mean 42.96 / `alias_pose` 2.38) and
`build/Debug/audit-pose-gate-0313b-20261009-131828-c4873b` (exe `01852035…`, revision `664b3ac7`,
synced save `07CE5BB1…`; 25.8 FPS, mean 38.73 ms, p95 39.78, `alias_pose` 2.24; focus verified,
completed). The analyzer also rejects manual contamination: `audit-pose-gate-0313-20261009-131150-b7d6cc`
was rejected because a typed console `restart` executed mid-capture and spawned a second map,
resetting the measured state. Do not interact with the live window during a capture.

Black-load workaround (staged sequence, `426d2e07`): the fixture loads the save once and marks
`QR_LOADED0`; the runner waits 2 s, triggers `restart` (F8, `QR_RESTART`), waits 2 s, triggers the
reload (F9, `QR_RELOAD`), and only then dismisses the menu and proceeds. An immediate restart in the
same command block did not cure the intermittent partial-load bug: the map must be loaded before the
restart, so the waits are real wall time, not frames. Historical variants: in-block sequence
(`audit-pose-gate-0313d-…`, 25.6 FPS / mean 39.07) and restart without reload (22.3 / 44.93).
Staged-sequence captures: `audit-seq-check-serial-20261009-141242-bc8451`,
`audit-render-tasks-fix2-20261009-141316-215209`, `audit-render-tasks-fix3-20261009-141434-2389f2`.

Clean set on the load → restart → load runner (synced saves, exe `01852035…`, revision `92c79365`,
balanced preset, 8 s warmup + 6 s capture):

| Scenario | Capture | FPS | mean ms | p95 ms | `alias_pose` ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Fuma | `build/Debug/audit-clean-fuma-20261009-133342-e997c2` | 36.0 | 27.82 | 31.48 | 1.08 |
| AD hub | `build/Debug/audit-clean-ad-20261009-133404-e03797` | 42.7 | 23.41 | 32.21 | 0.26 |
| Bogbottom | `build/Debug/audit-clean-heavy-20261009-133426-c640d1` | 25.3 | 39.52 | 44.05 | 2.34 |

Task-graph enablement (branch `perf/render-tasks`): the `SCR_UpdateScreen` task path is real again
(guard decoupled from the unimplemented `r_gpulightmapupdate`; `r_tasks` still defaults 0). Paired
same-binary runs with the staged sequence:

| Scenario | Serial | Tasks (`r_tasks 1`) |
| --- | --- | --- |
| Fuma | 35.1 FPS / mean 28.49 ms | 50.0 / 20.00 |
| AD hub | 44.6 / 22.41 | 47.6 / 21.01 |
| Bogbottom | 25.3 / 39.48 | 33.6 / 29.76 |

`cpu.wait_ms` is nonzero only under tasks (the main thread's join wait); per-slot sums accumulate
worker wall times across parallel tasks and can exceed the frame interval, so do not compare `ents`
totals across modes. The only recorded tasks-on crash is `audit-render-tasks-diag2-…` (`0xC0000374`);
after the frame-start ordering edges, thread-local scratches, the geometry-upload mutex and the
profiler/light-registry lock coverage landed, the recorded runs completed without corruption.
Coverage limits: Debug, one repeat per scenario (two for Bogbottom); the WER crash dump was not
analyzed; the serial path's added uncontended lock cost is unmeasured; the black-world workaround is
the staged restart, not the task graph.

Fresh-spawn scenario `qr_swampy_start` (`map ad_swampy`, no save; the Bogbottom save intermittently
triggers the black-world load bug) and the FSR scaling check on it (tasks on unless noted):

| FSR mode (`rt_upscale_fsr31`) | FPS | interval mean ms | `cpu.frame` ms | `cpu.wait` ms | `gpu.frame` ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Quality (2) | 27.8 | 35.93 | 33.79 | 33.79 | 26.86 |
| Balanced (3) | 29.4 | 34.02 | 31.95 | 31.95 | 23.96 |
| Performance (4) | 30.2 | 33.16 | 31.21 | 31.20 | 20.79 |
| Ultra (5) | 29.5 | 33.86 | 31.93 | 31.93 | 14.98 |
| Balanced, serial control | 22.5 | 44.53 | 42.39 | 0.0 | 23.94 |

The GPU time scales with the upscale factor (26.9 → 15.0 ms), but the frame does not: the main thread
is blocked in the join (`wait` ≈ `frame`) and the task DAG's wall time stays at ~31–34 ms in every
mode, so the frame is CPU-bound at the 4K targets. Reaching 45 FPS at FSR Quality (22.2 ms) and
linear FSR scaling requires cutting the CPU critical path below the per-mode GPU time; the component
targets are in the CPU priorities table. Captures: `audit-scale-q-bog-…`, `audit-spawn-bog2-…`,
`audit-scale-p-bog-…`, `audit-scale-u-bog-…`, `audit-scale-s-bog-…`.

FSR Quality on/off (tasks vs serial, same binary), with repeats (tasks): Fuma 35.9 → 41.7/42.1/40.7 FPS
(27.84 → 23.97/23.73/24.54 ms), `gpu.frame` ≈ 22.1–22.7 ms — the frame sits at the GPU balance point;
AD hub 41.2 → 41.6/41.5/42.3 (24.28 → 24.03/24.10/23.64), `gpu.frame` ≈ 21.9–22.0 ms — same balance
point; Bogbottom fresh spawn 22.7 → 27.8/30.9/31.5 (44.06 → 35.93/32.37/31.70), `gpu.frame` ≈
26.9–28.1 ms — 45 FPS at Quality there needs GPU-side reduction as well, and its run-to-run spread
reaches ~4 ms. Captures: `audit-q-*`, `audit-final-*`. A 25 s capture on the Bogbottom fresh spawn
(tasks, Quality) holds 31.3 FPS / mean 31.99 ms, p95 34.51 — matching the 6 s repeats.

End-task review fixes (branch `perf/render-tasks` on top of `f9711ea7`: the end task is joined in
`_Host_Frame` before `Cbuf_Execute`, its draw-frame cost and renderer stats are merged into the
sample of its own frame by serial, the brush caches are per-thread and the bench report releases
the profiler lock before I/O). Same-binary fresh-spawn pair (Balanced): 22.3 FPS / mean 44.78 ms
serial vs 26.8 / 37.26 ms tasks; the tasks
capture attributes `qrDrawFrame` to 225/225 frames (avg 9.32 ms) and the serial control to 188/188
(7.10 ms), `dropped=0` in both, focus verified. Captures:
`audit-pe-final-serial-20261009-155044-a78f40`,
`audit-pe-final-tasks-20261009-155024-c7fec8`.

Second review round (same branch, exe `B7C69FA8…`): the end task is joined in `_Host_Frame` before
the frame handles any input (key events and the menus mutate render cvars outside `Cbuf_Execute`),
the profiler window waits for every frame it covers, the brush and buried-answer caches are
per-thread and the bench report hands the run to exactly one caller. Same-binary fresh-spawn pair
(Balanced): 21.7 FPS / mean 46.07 ms serial vs 28.0 / 35.66 ms tasks; both captures attribute
`qrDrawFrame` to every frame (236/236 tasks, avg 8.95 ms; 182/182 serial, avg 7.39 ms), `dropped=0`,
focus verified. A control with `rt_end_task_delay_ms 30` (tasks) adds the delay to the frame
(65.61 ms) and completes with the same attribution (129/129, avg 8.96 ms) — the boundary check: the
input phase asserts the end task is not running. Captures:
`audit-review2-serial-20261009-162730-c3684b`, `audit-review2-tasks-20261009-162707-796c38`,
`audit-review2-delay-20261009-162751-72fc90`.

Follow-up on the profiler window (same branch, exe `5709F64D…`): the wait is booked when the end
task is submitted and released when it reports, so an early report cannot leave it stuck, and the
window is published from the frame-start join, where every frame of the window has reported. Both
cases were exercised with `rt_stats 3` captures whose dump profile columns are filled on every
sample: tasks (35.70 ms mean, 40/40 samples, `qrDrawFrame` 8.75 ms average) and tasks with
`rt_end_task_delay_ms 30` (67.50 ms, 37/37 samples, 8.92 ms) — in the delayed run every frame's end
task provably outlives the frame-end update, so every publication happens at a join. Captures:
`audit-review3-stats-20261009-164835-f59510`, `audit-review3-statsdelay-20261009-164856-50dd49`.

Those review-round captures remain historical throughput/readout observations: filled dump columns
did not establish atomic result publication or correct first-window activation. The split pending/data
transaction and activation reset were subsequently replaced by the shared production window component.

Final window-lifecycle verification on top of `d361af64` (Debug exe
`1ED2CC40EB8FD27058F43B7E444E9C54BA8DFF9FE1430855BA801E5DA16AF2E2`):
`RT_ProfWindowRecordEnd` writes the full result before releasing pending under the same lock;
`RT_ProfWindowBeginFrame` applies mode changes before submission, after joining the previous end task.
`prof_window_tests` covers real component transactions, a paused concurrent writer/publisher, both
completion orders, several frames per window and disable/re-enable. Twenty repetitions passed, as
did the full Debug CTest suite (8/8).

All runs below use the save-free `qr_swampy_start` scenario, Balanced, `rt_stats 3`, 5 s warmup and
8 s capture. Cold activation uses `-EnableStatsAfterWarmup`, so profiling stays off while loading
and warming up and is enabled in the already loaded game at the capture-start command.

| Capture | Mode | Mean interval ms | Distinct published windows | First observed window frames / renderer samples |
| --- | --- | ---: | ---: | ---: |
| `audit-polish-window-normal-20261009-171930-d92ac8` | Tasks, profiler enabled before warmup | 35.72 | 39 | 6 / 6 (window 27) |
| `audit-polish-window-cold-delay-20261009-171951-4fb168` | Tasks, cold activation, 30 ms end-task delay | 67.15 | 35 | 4 / 4 (window 1) |
| `audit-polish-window-cold-serial-20261009-172011-a2b9b4` | Serial, cold activation | 45.34 | 37 | 5 / 5 (window 1) |

The runner checked every observed published window for a positive draw-frame average and equal
completed-frame/result counts, not just nonempty columns. All three captures passed those checks
and recorded `FocusLosses=0`. QuakeRay MCP reads the added window-identity/count columns as
reporting-window metrics; imported artifacts still do not carry MCP-verified provenance.

## Current CPU priorities

Use C2's Bogbottom Balanced capture for the next CPU investigation, not a stale pre-fix profile. These are **observed inclusive costs**, not predicted savings:

| Observed work | C2 cost | Functions to inspect | Unverified lead / smallest discriminating experiment |
| --- | ---: | --- | --- |
| Alias geometry APIs | 4.47 ms | [`R_DrawEnhancedModel`](Quake/r_alias.c#L533) → [`Scene::Upload`](renderer/Source/Scene.cpp#L94) → [`VertexCollector::AddGeometry`](renderer/Source/VertexCollector.cpp#L285) / [`WriteGeomInfo`](renderer/Source/GeomInfoManager.cpp#L303) | Separate remaining bounds/copy/metadata costs; record actual model surfaces and duplicated vertex bytes. Surface-local submission or invariant-attribute reuse helps only if those bytes dominate. Preserve indices, stable IDs and motion history. |
| RHI setup | 4.60 ms | [RHI setup bracket](renderer/Source/RHI/NvrhiFrameSkeleton.cpp#L666) → [`AppendDynamicSlot`](renderer/Source/RHI/RhiAccelStructs.cpp#L1146) / [`BuildTopLevel`](renderer/Source/RHI/RhiAccelStructs.cpp#L1799) | Split descriptor/shape preparation, copy/build recording and normal preprocessing. Persistent topology/refit is a hypothesis, not an enabled or proven solution; require deletion/opacity/animation/ownership invalidation. |
| Brush light styles | 3.12 ms | [`RT_PackSurfaceLightStyles`](Quake/r_world.c#L1144) → [`RT_SurfacePackLightStyles`](Quake/r_world.c#L1106) → [`RT_NearestStyledLightDistance`](Quake/gl_rlight.c#L828) | Measure cache hits/collisions and reach-query work; transform/light/settings changes versus cache eviction must be distinguished before designing reuse. |
| Other entity costs | Brush uploads 2.84 ms; alias posing 2.30 ms | [`RT_FlushBatch`](Quake/r_world.c#L1428), [`GetPoseVertices`](Quake/r_alias.c#L107) | Keep these separate from their parent entity/packing totals. Legacy brush light marking is only about 0.045 ms here, not a main target. |

**Workstream boundary, as recorded on 2026-10-08:** cluster implementation belongs to `crisp-pixel-2`; particle/FTE implementation belongs to `calm-eagle`. This branch does not contain their candidate changes. Coordinate ownership before editing those paths; the index does not authorize duplicate parallel work.

## Capture index

Raw captures are **local build artifacts, not tracked Git files**. The curated numbers and identities above remain in Git, but if raw data is absent in another checkout, obtain the archive or rerun only the required save/preset. Do not invent a missing capture or treat a narrative report as a new measurement.

### B0 archive

Local dataset root:

```text
C:/Users/f1am3d/.local/share/opencode/worktree/2754e1/drawframe-menu/build/Debug/audit-audit-baseline-20261008-093046-9d38b7/
```

`manifest.json` identifies executable/assets/settings intent. `qr_fuma_start-balanced-1.console.log:29-31` records hardware. For each save stem in the scenario table, preset `balanced|quality` and repeat `1|2`, open:

```text
<save>-<preset>-<repeat>.summary.json   per-run CPU/GPU means and interval percentiles
<save>-<preset>-<repeat>.frames.csv     individual frame/context/counter samples
<save>-<preset>-<repeat>.bench.log      effective settings and complete-sample metadata
<save>-<preset>-<repeat>.cfg            requested fixture
```

This is the historical 12-capture baseline, not permission to repeat its old long multi-save invocation. Current runners require one save/preset and a 300-second maximum invocation budget.

### C0 / C1 / C2 integration pairs

Links are relative to this worktree; each directory contains the same file types plus its own `manifest.json`. C1 controls here use the **same regenerated pack as C2**, unlike the earlier standalone C1 campaign.

| Scenario / preset | Full control C0 | Transform-only C1 | Combined C2 |
| --- | --- | --- | --- |
| Fuma / Balanced | [capture](build/Debug/audit-entity-combined-fuma-full-control-20261008-160133-3aa81d/) | [capture](build/Debug/audit-entity-combined-fuma-transform-control-20261008-160028-74743a/) | [capture](build/Debug/audit-entity-combined-fuma-20261008-160000-b1c91d/) |
| Bogbottom / Balanced | [capture](build/Debug/audit-entity-combined-bogbottom-full-control-20261008-160106-4f1ed7/) | [capture](build/Debug/audit-entity-combined-bogbottom-transform-control-20261008-155935-42af18/) | [capture](build/Debug/audit-entity-combined-bogbottom-20261008-155908-9396f0/) |
| Fuma / Quality | [capture](build/Debug/audit-entity-combined-fuma-quality-full-control-20261008-160316-a977c0/) | Not measured in this integration campaign | [capture](build/Debug/audit-entity-combined-fuma-quality-20261008-160249-e3c3b9/) |

C2 repeats: [Fuma Balanced](build/Debug/audit-entity-combined-fuma-repeat-20261008-160225-04cdd4/), [Bogbottom Balanced](build/Debug/audit-entity-combined-bogbottom-repeat-20261008-160200-738857/). Exact C1 standalone captures are indexed in [its report](docs/entity-transform-reuse-performance.md#controlled-runtime-results). Earlier [audit](docs/renderer-bottleneck-audit.md) and [entity profile](docs/entity-cpu-profile.md) documents are historical context; their line numbers/status checklists are not the canonical current map.

## Reproduction and update protocol

### MCP live verification after the ImGui fix

Subsequent integration verification exercised a no-op owned experiment at `e55982af`, with
independently initialized local dependencies and successful contained Debug build
(`job_727a9a30229645bb98cce5d8c530d57d`). Its two-arm Fuma suite
`job_9ad4d5533e294a5e82e0e682a0c87236` completed baseline then candidate with ownership released
between invocations. These short no-op observations are not an optimization claim.

Isolated menu A/A `job_f3860abe2ce743ab9449410fc243d5a7` completed and produced two PNGs;
their diagnostic pixel comparison was intentionally not classified as visual equivalence.
The research finding/search roundtrip retained the suite's two run IDs as an inconclusive
operator interpretation. A later receipt-matched Fuma control
`job_76c7a188be37419f89dd42ff9792adeb` verified retained provenance and was explicitly promoted
as `fuma_balanced_debug_control`; matched mean/p95 comparison was within the labeled 5% policy.
Visual/gameplay/validation statuses remain independent, not silently PASS.

On 2026-10-08 the approved Debug runtime and documented AD saves were available locally.
`job_074a7aa9047946e790edd4f60c292642` completed an 8-second warmup / 6-second Fuma Balanced
benchmark through the contained supervisor, with 34.80 FPS and 28.74 ms mean interval.
This is a tooling smoke observation, not a paired optimization result or new canonical control.

The AD-hub StatsLevel-3 capture initially hit the 16-bit ImGui draw-list assertion. The fix
from `3bc99d9b` was cherry-picked as `e55982af`; Debug deployment and all three CPU CTests,
including `gui_draw_tests`, passed. The repeated diagnostic job
`job_4e99860f7ead4dfbbef571911d01bfbe` succeeded with no assertion. Its overlay-enabled timings
are diagnostic only and must not be compared to StatsLevel-0 performance controls.

Raw evidence remains under `%LOCALAPPDATA%/QuakeRayMCP/acd0cbf5d5998a3e91a76882/jobs/<job-id>/runtime/`.
Failed jobs from missing assets, focus loss and console/instrumentation changes were retained
and rejected, not promoted to baselines. Binary/asset acceptance, long gameplay, visual
equivalence and the remaining MCP phases are not certified by these short verification runs.

Build and runner ownership now uses the shared
[machine guard](tests/perf/machine_guard.ps1). It defers when another owner is detected and
does not stop foreign processes. The MCP supervisor's runtime adapters work on private copied
runtime directories; source-runtime configs are not their recovery targets. Failed/canceled
builds require runtime/submodule review or a successful rebuild before MCP launches that
runtime. Real MCP capture acceptance is not established by fake-worker containment tests;
missing assets, workload identities or visual checks remain explicitly unverified. This
tooling update adds no performance measurements to the historical tables above.

1. Verify source branch, dirty files, executable SHA256, engine pack and matching mod/save hashes. Retained baseline PAK copies may be read-only hardlinks: never overwrite linked assets to prepare a candidate.
2. Check machine ownership before builds, GPU tests or game launch. Never run a second game, steal another run's focus, or stop another owner's process. If occupied, defer; no unbounded wait.
3. Build/deploy **Debug** with `build_win.ps1`, not only a plain CMake build. Run the applicable CPU/reference tests and, when ownership permits, GPU smoke tests.
4. Capture one save/preset per invocation; reload the same save, use live advancing simulation and warmup, keep focus, close the game before analysis. Maximum 300 seconds; the current runner allows at most two repeats.
5. Require matching effective settings and workload/assets between arms. Feature ablations, frozen scenes, low-resolution modes and `-StatsLevel 3` are labeled diagnostic controls, not target-quality results. Legacy `run_place.ps1` presets disable particles and are not this baseline.
6. Inspect `.frames.csv` and analyzer validity, not just FPS. The analyzer rejects dropped/nonfinite/incomplete/menu/paused/nonadvancing captures. [Screenshots occur after benchmark stop](tests/perf/run_stress.ps1#L240).
7. Preserve independent solution branches/binaries; compare against a compatible control, repeat/interleave, check image/gameplay behavior, then update this index with scope and remaining gaps.

```powershell
Get-Process quakeray, qray_* -ErrorAction SilentlyContinue
.\build_win.ps1 Debug -Tests -Parallel 12
ctest --test-dir build\Debug -C Debug --output-on-failure
.\tests\perf\run_stress.ps1 -Saves qr_gpu_heavy -Presets balanced -Warmup 8 -Seconds 6 -Repeats 1 -MaxRunSeconds 300 -Tag hypothesis-name
python tests/perf/analyze_stress.py build/Debug/audit-<tag>
```

The process check is a guard, not a command to ignore its output and continue building. `run_stress.ps1` also [waits/checks other owners](tests/perf/run_stress.ps1#L93). CPU capture-tool tests: [test_analyze_stress.py](tests/perf/test_analyze_stress.py), [test_stress_budget.ps1](tests/perf/test_stress_budget.ps1). Source/reference tests: [frame_timing_tests.cpp](tests/frame_timing_tests.cpp), [rt_lighting_tests.c](tests/rt_lighting_tests.c); GPU tests: [renderer/Tests](renderer/Tests/).

Index validation on 2026-10-08 reanalyzed all 12 B0 and 10 integration captures with the current analyzer and matched their stored summaries. The 126 documented baseline timing values and 24 controlled comparison values were checked against the stated aggregation/rounding rules; paired effective settings and every recorded asset/save hash matched. This was read-only evidence validation, not a new benchmark or GPU test run.

### Next-agent task packet

> Read ARCHITECTURE.md, PERFORMANCE.md and the chosen C2 capture. Do not survey the repository again unless the mapped route is insufficient. Form three bottleneck hypotheses with specific functions, counter scope, expected work-elimination mechanism, a falsifying observation and quality/lifetime risks. Pick the highest measured CPU opportunity, run the smallest discriminating experiment, then implement only a supported hypothesis on its own branch. Record binary/settings identities and per-frame before/after results. Do not claim 60/45 FPS or all-map coverage from these short saves.
