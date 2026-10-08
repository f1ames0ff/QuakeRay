# Renderer bottleneck audit

## Scope and ownership

Baseline revision: `feff66d5` (master, QuakeRay 0.31.0).
Working branch: `perf/render-bottleneck-audit`.

The cluster workstream belongs to the agent in `crisp-pixel-2`; particle/FTE optimization belongs to the agent in `calm-eagle`. This audit does not edit either worktree or duplicate those implementations. It measures their contribution and investigates viewmodel cost, visibility, geometry preparation, acceleration-structure work, uploads, synchronization, and unaccounted CPU time.

Changes to testing/profiling infrastructure belong to this branch. Renderer optimizations will use isolated branches and before/after measurements, with a reproducible baseline rather than undocumented preset changes.

## Targets

- 3840x2160, FSR Balanced: stable 60 FPS, a 16.667 ms frame budget.
- 3840x2160, FSR Quality: 45 FPS, a 22.222 ms frame budget.
- Highest graphics quality settings, except volumetric sky at Low.
- Record the exact effective settings, hardware, build configuration, save/map identity, and frame-time distribution. Artistic controls are not arbitrarily increased merely because they are numerical sliders.
- Three saves establish stress-point coverage, not proof of performance at every position on every Quake/AD map. Broader map/route regression remains necessary for an all-maps claim.

## Supplied stress points

Source directory (read-only): `C:/Users/f1am3d/.local/share/opencode/worktree/2754e1/crisp-pixel-2/build/Debug/ad`.

| Save | Map | Save time | Difficulty |
| --- | --- | ---: | ---: |
| `qr_fuma_start.sav` | `ad_tfuma` (Terror Fuma) | 854.075013 | 1 |
| `qr_ad_start.sav` | `start` (AD hub) | 16.102778 | 1 |
| `qr_gpu_heavy.sav` | `ad_swampy` (Foggy Bogbottom) | 30.227778 | 1 |

Copy the saves and matching AD game data into this worktree's runtime before testing. Do not reuse or overwrite another agent's live configuration or diagnostic logs.

## Verified starting observations

1. `viewmodel` is an inclusive task timer, not just the weapon draw. `R_DrawViewModelTask` uploads entity lights, draws the weapon/debug geometry, uploads world-model lights and teleports, and calls `RT_ClusterLightListsUpload` before ending `RT_PROF_VIEWMODEL` (`Quake/gl_rmain.c:1207-1240`). `vm draw` is the narrower weapon/debug-draw timer. A 30 ms `viewmodel` value cannot yet be attributed to weapon geometry.
2. Visibility code exists, but `rt_enable_pvs` defaults to zero (`Quake/gl_vidsdl.c:116`). The scalar world walk gates leaf visibility, frustum tests, and backface rejection on that setting (`Quake/r_world.c:733-818`). Alias frustum rejection is also conditional (`Quake/r_alias.c:857-861`). The SIMD world path differs: `R_MarkVisSurfacesSIMD` calls leaf-frustum and surface-backface rejection even with PVS disabled (`Quake/r_world.c:507-557`). `r_simd` defaults to one and selects this path on SSE/SSE2 hardware (`Quake/gl_rmain.c:78`, `Quake/gl_rmisc.c:121-124`). Therefore the claim that the default renderer has no visibility rejection is not supported. Static RT world residency, dynamic submission, and camera-visible efrags still need separate analysis.
3. Camera-frustum visibility and RT contribution are different. Any proposed reduction of RT geometry must preserve off-screen shadows, reflections, refractions, GI, and emissive-light contribution. Enabling existing raster-style culling blindly is not a quality-preserving optimization.
4. The post-effects GPU smoke test uses numeric framebuffer bindings from the old 124-image schema. Master has 131 framebuffer images, so PRE_FINAL's sampled binding changed from 151 to 158. This is a test-layout mismatch, not evidence of a renderer FPS bottleneck. Replace production-image test bindings with generated tables before relying on regression results.

## Measurement protocol

- Use Debug unless explicitly instructed otherwise.
- Check for any running QuakeRay/headless GPU test or performance runner before heavy work. Do not steal focus, launch a competing instance, or terminate another agent's process. Bound waits and report blockers.
- Reload the same save before each arm. Preserve live simulation for the main result; paused/frozen and feature-ablation runs are diagnostic controls, not substitutes for the requested settings.
- Warm up shaders, temporal history, simulation, and caches before capture. Use multiple runs and controlled before/after ordering.
- Measure both graphics workloads and end-to-end frame intervals. Separate CPU/GPU work from frame pacing, and label inclusive timers. Window maxima in `rt_stats 3` are not per-frame percentiles.
- Validate image quality and gameplay behavior as well as timing. Reject changes that gain FPS by deleting required RT contributions or silently lowering settings.

## id Tech 8 references

- Tiago Sousa, SIGGRAPH 2025: https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf
- Hammer/Lazarek, GDC 2026 frame breakdown: https://gdcvault.com/play/1035744/ and https://schedule.gdconf.com/session/rip-tear-breaking-down-the-renderingofdoom-the-dark-ages/915274
- Microsoft, April 2026 VRCS overview: https://developer.microsoft.com/en-us/games/articles/2026/04/variable-rate-compute-shaders-doom-the-dark-ages/
- GPC 2025 archive and primary slide decks: https://graphicsprogrammingconference.com/archive/2025/
- NVIDIA/Khan interview, September 2025: https://developer.nvidia.com/blog/how-id-software-used-neural-rendering-and-path-tracing-in-doom-the-dark-ages/
- CEDEC session: https://cedil.cesa.or.jp/cedil_sessions/view/3185
- Digital Foundry architecture coverage: https://www.digitalfoundry.net/articles/digitalfoundry-2025-creating-doom-the-dark-ages-how-id-tech-8-took-shape?pubDate=20250526#1

The references motivate work elimination, material/tile classification, software variable-rate compute, and better AS lifetime/refit policies. They do not establish that QuakeRay should replace primary ray tracing with a visibility-buffer raster renderer, nor do vendor-specific SER/OMM/DLSS claims establish a gain on the local Radeon GPU. Applicability will be checked against the actual pipeline and measured bottlenecks.

### Primary-material takeaways

- Sousa, SIGGRAPH slides 12-14: visibility sampling, a world-space radiance cache, irradiance-volume updates, final gather, denoise/upscale. The RT light structure is world-space rather than camera-frustum clustered; coarse/fine hierarchical overlap tests reduce irrelevant light processing. This supports separating raster visibility from RT contribution, not deleting everything outside the view.
- Sousa, slides 18-22: update active radiance-cache entries and reuse them across frames; final gather indexes screen/world/probe caches rather than repeating full shading at every hit. This is a longer-term GI option, requiring invalidation, collision handling, dynamic-light correctness, and temporal-quality testing.
- Lazarek/Hammer, GPC slides 11-19: persistent GPU geometry, GPU gather/triangle culling, a 64-bit visibility buffer, material-uniform wave dispatch. Their first quad-dispatch prototype did not materially beat Forward+, so a renderer rewrite is not itself a performance result.
- Lazarek/Hammer, slides 35-43: classify tiles, use smaller feature-specific shader workloads, skip empty tiles, and keep visual parity tools. Candidate applications here are reflection/refraction, glass, sky, and sparse denoiser workloads, only if profiling shows those dispatches dominate.
- Fuller/Hammer, GPC slides 19-25 and 29-42: VRCS must retire complete waves or avoid dispatching duplicate pixels; merely skipping some lanes is insufficient. Temporal rotation, deblocking, depth/normal boundaries, and noisy fog/shadow behavior are important quality hazards. VRCS is a GPU optimization, not a remedy for CPU entity preparation.

The CEDEC page identifies the matching Sousa GI session, but its deck requires login. The GDC session descriptions are public; the recording was not consumed. Digital Foundry returned HTTP 403. Claims above use the downloaded SIGGRAPH/GPC slide decks and the public Microsoft/NVIDIA material rather than inaccessible content.

## Additional verified CPU candidates

- `SCR_UpdateScreen` forces `use_tasks = false` (`Quake/gl_screen.c:1162-1166`), so entity preparation and all render-view tasks run serially. Re-enabling the old task path is not automatically safe: uploads mutate shared renderer state and `GetPoseVertices` uses shared scratch storage. A parallel design needs task-owned gather buffers followed by a controlled commit/upload stage.
- `Scene::Upload` transforms and validates every uploaded vertex to expand the scene AABB (`renderer/Source/Scene.cpp:93-139`). This is additional CPU work before vertex staging/metadata construction, on every dynamic mesh upload. Cached local bounds transformed conservatively by corners, or producer-supplied validated bounds, are candidates; the current finite-value/extreme-coordinate behavior must be preserved and the gain measured.
- `R_DrawBrushModel` still scans dynamic lights and calls legacy `R_MarkLights` (`Quake/r_brush.c:333-342`). Its writes are the lightmap dynamic-light bits (`Quake/gl_rlight.c:119-149`); `R_UpdateLightmaps` is a stub that asserts if the GPU-lightmap option is enabled (`Quake/r_brush.c:1137-1143`). Audit remaining lightmap consumers before removing this potential legacy CPU path.
- Dynamic geometry is copied every frame into RHI buffers, organized into large filter-group BLASes, and rebuilt with `PreferFastBuild` rather than an update/refit flag (`renderer/Source/RHI/RhiAccelStructs.cpp:61-62, 1194-1335`). Shape changes replace the BLAS handle. Persistent mesh identity/topology, explicit transform-vs-deformation dirtiness, and per-mesh/refit policy are architecture candidates, not yet tested optimizations.
- The old `ents` maximum is per chunk even though the average sums all chunks: `R_DrawEntitiesTask` ends the same profiler slot for each of `NUM_ENTITIES_CBX` invocations (`Quake/gl_rmain.c:1149-1161, 1348-1349`). Thus an entity average exceeding the printed maximum is possible. Do not mistake that maximum for the whole entity stage or compute frame percentiles from the sampled overlay windows.
- Enhanced MD3/MD5 models have surface-local vertex blocks, but `R_DrawEnhancedModel` currently uploads the model's entire pose vertex array for every material surface (`Quake/r_alias.c:589-671`, baseline revision). The loaders add each surface's vertex base to its indices (`Quake/gl_model.c:3762-3767, 4348-4350`). Keeping surface-local indices and uploading only that surface's contiguous vertex range could remove duplicated CPU bounds/staging, GPU copies, and preprocessing without removing triangles. This needs triangle/attribute parity tests and measured coverage of actual multi-surface models before any FPS claim.
- Static entity ID lookup is a linear scan through `cl.static_entities` (`Quake/gl_rmisc.c:533-548`). Dynamic and temporary entities already use direct array offsets. Stable static indices assigned at creation could eliminate repeated scans; the additional cost has not yet been isolated in a target capture.

## Verification and baseline status

The generated-binding fix passes the full Debug CTest suite (5/5), including the formerly crashing post-effects test. Matching AD PAK files, the three saves, and loose AD material/light definitions have been copied into this worktree's runtime without changing source-worktree files.

The test fix is committed as `1d7c0ddc`, with a dedicated `fix/posteffects-framebuffer-bindings` branch pointing to it.

Earlier runtime probes failed before `Host_Init` completed, with `VK_ERROR_UNKNOWN` (-13) from `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, reported at `renderer/Source/Swapchain.cpp:137`. It reproduced at both 4K and 1280x720 startup, including `-nosound` and an implicit-layer-disabled probe. Later independent SDL/Vulkan and `vulkaninfo` surface probes succeeded on the same GPU. An API-dump-assisted engine probe subsequently completed startup, switched to 3840x2160, and loaded the AD hub, but crashed before capture. The changed layer/timing and desktop conditions do not establish the root cause or a renderer fix. A later normal Debug build passes 5/5 CTests and completes the target-mode tool check without API dump or a startup-error workaround. Failed or API-dump-assisted runs are not FPS baselines.

The new target runner validates effective resolution, FSR mode, cloud quality, map identity, and foreground focus. It uses a separate runtime/configuration and does not reuse the cluster/particle ablation presets that disable particles or upscaling.

### First tool-check capture

Hardware: Ryzen 9 7950X, Radeon RX 9070 XT, AMD 26.8.1 (Vulkan 1.4.349). Debug, 3840x2160, FSR Balanced, preset `qr_audit_max.cfg`, particles enabled, `rt_stats 0`, 8-second warmup, 4-second capture. This short tooling check is not the repeated baseline or an all-map result.

| Metric | AD hub (`qr_ad_start`) |
| --- | ---: |
| Mean interval / FPS | 21.42 ms / 46.7 FPS |
| p95 / p99 | 25.59 / 26.59 ms |
| GPU snapshot mean | 17.68 ms |
| CPU particles | 8.44 ms |
| CPU entities (alias / brush) | 3.90 ms (1.96 / 1.91 ms) |
| CPU viewmodel task / weapon draw | 3.08 / 0.019 ms |
| CPU cluster portion of the task | 2.89 ms |
| CPU DrawFrame / RHI setup | 4.02 / 1.87 ms |

The narrow weapon draw is not the dominant CPU cost in this capture. Particles and clusters are the largest measured preparation costs and remain the other agents' workstreams. The GPU snapshot already exceeds the 60-FPS budget, so CPU-only improvements cannot establish 60 FPS here without a GPU reduction too. Raw data: `build/Debug/audit-audit-toolcheck-normal-20261008-092813-33e2f4/`.

## Per-frame capture tooling

The audit instrumentation adds buffered per-frame CSV output to `rt_bench`. Samples contain screen-end-to-screen-end intervals, unclamped host intervals, summed CPU slots, renderer CPU phases, asynchronous GPU timing snapshots, API-call counts, and loaded-game/paused/menu context. Disk writes happen after capture, not once per measured frame. The partial first interval is explicitly zero and excluded from interval percentiles. Buffer overflow invalidates a target capture rather than silently truncating it.

Benchmark slot maxima now compare each frame's accumulated slot cost, so sequential entity chunks no longer produce a whole-frame average larger than a per-chunk benchmark maximum. The older `rt_stats` overlay still reports per-invocation window maxima; its samples are not used for frame percentiles.

`tests/perf/run_stress.ps1` defaults to `rt_stats 0` while `rt_bench` enables CPU phase and GPU timestamp collection. Thus the primary capture does not include the visible ImGui panel or per-ray atomic statistics. A separate `-StatsLevel 3` arm measures those costs. Entity type timers and the legacy brush-lightmap marker isolate CPU preparation without editing cluster or particle implementations.

`tests/perf/analyze_stress.py` checks sample completeness, clock consistency, finite values, full signon, in-game context, and advancing simulation. Its six unit tests pass, including paused/menu rejection and correct treatment of the first partial interval. Renderer GPU data is deliberately labeled as snapshots: asynchronous completion is not a synchronized per-present GPU latency measurement.

Examples:

```powershell
.\tests\perf\run_stress.ps1 -Repeats 2 -Seconds 10 -Warmup 8 -Tag baseline
.\tests\perf\run_stress.ps1 -Saves qr_ad_start -Presets balanced -StatsLevel 3 -Tag stats-overhead
python tests/perf/analyze_stress.py build/Debug/audit-<tag>
python -m unittest discover -s tests/perf -p test_analyze_stress.py -v
```

## Progress

- [x] Create the isolated audit branch.
- [x] Identify all three saves and their maps.
- [x] Read initial viewmodel/visibility paths and distinguish facts from hypotheses.
- [x] Repair and run GPU regression infrastructure.
- [x] Establish effective target-quality settings and reproducible capture tooling.
- [ ] Measure the three saves at both target FSR presets.
- [ ] Run controlled ablations and isolate the main costs.
- [ ] Implement and test justified optimizations on dedicated branches.
- [ ] Publish an evidence-backed architecture proposal and remaining risks.
