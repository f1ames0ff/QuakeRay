# Terror Fuma and Bogbottom Swamp optimization plan

Updated: 2026-10-11. Supersedes the 2026-10-10 planning revision and the temporary pause notes.

Status: active. Branch `perf/rhi-dyn-blas-clean`; the tip carries the Tier A port, the parallel world
marking and the stage-2.3 cleanup (the changelog below records the trail and the PR-gate numbers).

## 1. State of the tree

- `r_tasks` default is **1** (`69798c01`); `r_tasks 0` is the deprecated serial path kept for bisection.
  Task-mode joins show up as `cpu.wait_ms > 0`.
- The particle/cluster branch landed with PR #41 (cluster list publication, particle point transport, the
  stand-in gate, the water composite). The former "other lane" ownership is gone; new work on those paths
  must keep the contracts in `docs/multithreading.md` and `docs/branch-handoff-particle-cluster-cache.md`.
- This branch carries the dynamic BLAS shape/reuse policy (`RhiAccelStructs`, `DynamicBlasShape.h`). The
  post-merge water stand verified the interaction: gate open, capture on, no runtime errors, and the TLAS
  instance count increases by one while the proxy capture is armed.
- The pre-existing `VUID-VkRayTracingPipelineCreateInfoKHR-layout-07990` is resolved (2026-10-12): the
  smoke ray-stats declaration now follows `SMOKE_RAY_STATS_SET`, which `RaygenPrimary.hlsli` sets to the
  pass's ray-stats set (11), so `RtQ2ReflectRefract.rgen` no longer aliases the set-7 cubemap table; the
  raster particle pipelines keep set 7. The interface recording was re-recorded and the full
  CheckShaderProperties run passes; the validation-enabled confirmation lands with the next capture
  batch. Validation layers are enabled by the renderer config file `qray.txt` (`vulkanvalidation`); the
  engine `-validation` argv flag is dead code.
- Acceptance-profile note: the audited runner preset pins `rt_sky_clouds_quality 0` (Low). The operator
  target is clouds **Medium**; a dedicated profile must land before any 4K/Medium capture.

## 2. Scenes, assets and harness recipes

| Role | Save | Map | SHA256 |
| --- | --- | --- | --- |
| Heavy (swamp) | `qr_ad_swamp` | `ad_swampy` ("Foggy Bogbottom") | `9318AE3161547A666C3AAC50113CBC1765C27A93B311DB4368846A931D2AFC13` |
| Heavy (Terror Fuma) | `qr_fuma_start` | `ad_tfuma` | `27D2992C64A14332E1804C139580778A7AB595C86F28FD1C39289BDF4ECC3326` |
| Fast reference | `qr_ad_start` | `start.bsp` | `A1FCE672F441BE2938A9D9D717868A25CB87E83230552BDD23CE8F6A3BDFEE88` |
| Water shot (owner) | `qr_start_water` | `start.bsp` | `A2A93A56CBA00046F46C08D1D9482B88004126B2F8F96C0C043C1C420DDA5DFE` |

The demos `ad_particle_heavy` / `ad_particle_idle` live in `build/Debug/ad` (untracked).

Harness recipes (one game instance at a time, machine guard; always pass `-Repo <this worktree>` because the
temporary scripts default to the `calm-eagle` checkout):

- Attribution runs: `%LOCALAPPDATA%\Temp\opencode\run_tf_attribution.ps1 -Repo <wt> -Save <save> -Tag <tag>
  [-Extra @('cvar value')]`. The fixture is `tests/perf/qr_audit_max.cfg` + FSR Balanced + `r_tasks 1` +
  the extras; artifacts land in `%LOCALAPPDATA%\Temp\opencode\<tag>`.
- Water-shot A/B: `run_water_shot.ps1 -Repo <wt> -Save qr_start_water -AimSteps 1 -HoldMs 250`. The default
  6-step look-down self-damages the player mid-arm; one step gives a matched camera for the
  `rt_glass_particles 1/0` pair.
- 4K demo: `perf/stage0/run_points_demo_ab.ps1 -Demos ad_particle_heavy -Arms points_on -ExtraCvars
  'r_tasks 1; rt_particle_proxy_gate 1'`.
- Validation: create `build/Debug/qray.txt` containing the line `vulkanvalidation`, run, then delete the file.

## 3. Active targets (operator report, 2026-10-11)

Red overlay rows on **Terror Fuma and Foggy Bogbottom**:

- T-E: `ents` up to ~12 ms per frame.
- T-S: `sky` up to ~12 ms per frame.
- T-A: `alpha` up to ~9-12 ms per frame.
- T-I: `indirect` (GPU pass) red on both maps.

Every target needs the same first answer: is the time compute, shared-state wait, or observer cost, and which
sub-work dominates. No optimization starts from an unmatched screenshot.

## 4. Established evidence

- The base engine is not the wall: upstream vkQuake 1.37 (Debug) holds its 165 Hz vsync cap on the heavy
  swamp save with the same assets; the operator observes 200 FPS (its default cap) on the other scenes. Our
  CPU cost is created by the RT/upload architecture, not by the maps.
- 2026-10-10 screenshot (unmatched): `ents 13.8`, `sky 9.75`, `alpha 7.58` invocation maxima, FRAME/WAIT
  21.8 ms, clouds GPU 0 (gating unverified).
- Merge section-3 stands on `97ec30cf` (audit fixture, 1280x720 window, profile panel on - see the observer
  note):
  - start: gate closes (proxies 0; `fte convert` 0.81/2.14 ms; `particles upload` 0.15/0.52 ms; TLAS 7);
    slot maxima include `ents 22.87`, `sky 4.37`, `alpha 3.01`; `wait` 7.42/11.41 ms on 621/621 frames.
  - swamp: `frame` slot 24.46/29.16 ms, `qrDrawFrame` 8.36/9.44 ms, `particles resolve` 2.70/3.18 ms; gate
    open with a small proxy set.
  - 4K demo `ad_particle_heavy`: 38.9 fps; `particles upload` 0.32 ms; `fte convert` 1.08 ms.
- Matched producer captures on 2026-10-11 (`97ec30cf`, audit fixture, 1280x720 window, profile panel on).
  Report maxima (`stats` dump samples of the per-invocation window maxima) versus bench `cpu.slot` sums
  (summed worker time across the frame's tasks - mechanism evidence, not a frame cost):
  - Fuma (`qr_fuma_start`): report maxima `ents 14.14`, `sky 8.31`, `alpha 8.49`, `particles 1.56`;
    bench worker sums `ents brush 36.63`, `ents alias 13.79`, `ents sprite 1.95`; `frame` slot 14.20/18.82;
    36.8 fps. Brush-model entities dominate the summed entity worker time on this map.
  - Swamp (`qr_ad_swamp`): report maxima `ents 19.06`, `sky 11.33`, `alpha 9.26`; `frame` slot 24.46/29.16.
  - GPU at this window: `gpu.indirect` at or below 0.92 ms on both maps. GPU rows depend on the internal
    render resolution, so T-I evidence must be captured at 3840x2160 before it is used.
- Observer note (verified for the captures this section interprets, i.e. before slice 1 of P1): the
  then-current `RT_Prof_End` took a global spinlock per entity/pose/surface; with the profile panel on -
  the acceptance situation - it inflated the **outer** slots (`ents`, `sky`, `alpha`) and the frame
  wall, while each slot's own sample was taken before the lock. Those maxima mix work, wait and observer
  cost. The thread-local profiler accumulation that landed with P1 removed the lock from the sampling
  path, so current captures no longer carry that inflation.
- Upstream-comparison conclusions relevant to the targets: per-item `rs_*` atomics remain in the sky and
  brush sites; mark/cull still use one atomic per marked surface and one task item per 32-block; world sky
  surfaces are uploaded per surface per frame; water/animated chains are re-batched and re-uploaded per frame
  although their position/texcoord bytes are invariant (light styles, alpha and animation are not); entity
  slices are fixed contiguous ranges without stealing; alias pose copies are forced by the 1-based cluster
  stamp; enhanced models re-upload the whole vertex array per surface; `Scene::dynamicUniqueIDToSimpleIndex`
  and the AABB accumulation are load-bearing (removal/trimming changes behavior or is unsound);
  `RT_StaticMovable` excludes sky/warp/animated and is entity-only.

## 5. Verified candidate tiers (carried over, corrected)

- Tier A (small risk, adopt after P1 evidence): batched `rs_*` stats; mark/cull batching plus `_Relaxed`
  helpers; editor `RT_TRACK` generation gate; load-time sky classification (`texofs`/`used_specials`
  equivalents must be built); devstats gate (with the `developer` clause); vestigial writes and the no-op
  `R_UpdateLightmaps` task removal (DAG rewiring); task-queue robustness assert; Debug-only duplicate-find
  consolidation.
- Tier B (measure first): entity work-stealing (only on proven slice imbalance, not lock wait); entity
  unique-ID caching (needs a link-time hook and invalidation); task-queue internals (microbenchmark first).
- Tier C (design plus cross-cutting): frame-invariant world sky/water/animated upload design (new mechanism,
  invalidation matrix, dynamic-BLAS interaction); alias GPU materialization or per-surface ranges; particle
  transport items resumed post-PR#41 (compact FTE transport, capacity-bounded conversion).
- Excluded: gibs gating, RNG port, any quality reduction, lock removal.

## 6. Diagnostics stage (P0/P1)

- P0: freeze the two heavy scenes and the fast counterpart; implement the dedicated clouds-Medium validation
  profile; verify effective state (FSR executed, internal resolution, clouds enablement/quality, task mode).
- P1 instrumentation (bounded, frame-serial, default-off gating): slice/alpha/sky timeline records with
  worker identity and work counts (the S1 design must be re-derived); sky subphases (S2 design available);
  upload wait/hold per producer lane and holder identity (S3 design available); publication/overlay/dump
  (S4 must be re-derived); hazard register (S6 must be re-derived).
- Mandatory corrections: keep the per-invocation maxima (`ms[]`) and add aggregates in new fields; never call
  the profiler inside the geometry lock; fix the `rt_emis_stats` cross-task writer ownership before gating any
  of it; measure instrumentation overhead on and off with the same binary.
- Deliverable: last-finisher per frame, ready-to-start delay, upload wait/hold with the holder lane, sky
  subphases, work counts, and the observer-cost split.

## 7. Target workstreams

### E - entities

Questions: per-slice compute versus `geometryUploadMutex` wait; alias versus brush versus sprite split; share
of the pose copies; enhanced-model duplication; profiler observer share; slice imbalance.

Candidates: work-stealing slices (Tier B); upload shaping (single duplicate lookup, private preparation
outside the lock after an ownership audit); pose/staging fusion or per-surface ranges; per-texture light
lookup memo.

Gates: shorter finishing slice without more total work; both task modes; identical output (stable unique IDs,
order-safe counters and editor light order).

### S - sky

Questions: per-surface world-sky uploads versus batching; enumeration scans (a `texofs`-style skip); skybox
and layer CPU subdivision with its ~12 uploads per frame; the water/animated share of the same slot; the
observer share.

Candidates: batch or upload-once world sky with explicit invalidation (fog/tint, light styles, animation,
map change); load-time sky classification; layer generation batching and time-gated regeneration without a
quality change; entity sky stays dynamic because it is CPU-translated per frame.

Gates: unchanged visuals on skybox/layer/water scenes; both task modes; the invalidation matrix exercised.

### I - indirect (GPU)

Questions: which sub-work dominates `gpu.indirect_ms` (GI rays, NEE shadow rays, denoiser reprojection,
interleave); correlation with map geometry and lights versus settings; frame-to-frame variance.

Evidence: `gpu.indirect_ms`, `rays_indirect`, `rays_shadow_*`, a settings matrix over `rt_gi_level`,
`rt_nee_samples` and `rt_restir`, plus pass captures.

Candidates: to be established from the evidence; no quality reduction is authorized by default. Coordinate
with the RT renderer side; upstream vkQuake 1.37 has its own RT line as a reference.

## 8. Acceptance and colour ledgers

- 4K output with FSR Balanced actually executing, clouds Medium, `r_tasks 1`; 60+ FPS with mean and p95
  frame intervals at or below 16.67 ms; report p99, max and the over-budget fraction.
- Literal all-green ledger against the 5.5/11.1 ms thresholds without recolouring, hiding, resetting history
  or discarding frames; host, GPU and excluded rows reported separately.
- Both `r_tasks 0` (bisection) and `r_tasks 1` (default) remain preserved while the fork exists.

## 9. Rules and verification

- Build only via `.\build_win.ps1 Debug -Tests`; run CTest; one game instance; machine guard; preserve the
  run fixtures in the evidence tree.
- Same-binary A/B or matched frozen builds; frame-interval acceptance; mechanism-only results stay
  mechanism-only; percentiles per the analyzer convention.
- QuakeRay MCP workflow mandatory; note coverage gaps; empty edges are not absence.

## 10. Change log

- 2026-10-11: actualized after the PR #41 merge; three active targets (entities, sky, indirect);
  upstream-comparison conclusions and corrections folded in; scene hashes, harness recipes and the water/
  validation quirks recorded; ownership note updated (particles and clusters have landed).
- 2026-10-11 (P1 slice 1): the profiler now accumulates slots in thread-local buffers and merges once per
  frame, removing the per-invocation `RT_Prof_End` spinlock from the hot paths while preserving the
  published per-invocation maxima and the bench sums (build and CTest green; a matched Fuma run shows
  comparable magnitudes, with run-to-run variance dominating). The wait/hold upload probe, per-producer
  subphase timers and counters, and the observer call/wait publication are the next slices.
- 2026-10-11 (operator A/B): `rt_brush_persistent 0/1` on Fuma, two repeats per arm: `calls_geometry`
  1283 to 726, `brush upload` worker sum 26.0 to 6.7 ms, `brush pack` 7.2 to 13-15 ms (covered surfaces
  still pack), `ents` sum 47-50 to 37-39, frame/wait slightly down; FPS 40.4/44.6 (0) vs 44.8/48.5 (1).
  The pack rise makes skip-covered-packing (C2) the companion candidate.
- 2026-10-11 (P1 slice 2): the upload-lock probe landed (`rt_upload_trace 1`; per-lane wait/hold around
  `geometryUploadMutex`, `upload.lock`/`upload.lane` rows in the bench block). First Fuma waterfall
  (748 frames): total lock wait about **36 ms per frame summed across lanes** versus about **5 ms per
  frame of actual hold**; entity lanes 4-5 ms each, alpha 3.9, sky 2.4; per-call maximum wait
  0.92-1.25 ms; holds short (about 3-5 microseconds average, <= 0.15 ms maximum). Water needs only
  ~2 uploads per frame (batching works), world sky ~95 calls per frame. Conclusion: the shared geometry
  mutex is a first-order contention point; the brush/alias upload slots are dominated by wait, not by
  copying. Next: sky/alpha/brush subphase and count timers; then the `r_tasks 1/0` comparison.
- 2026-10-11 (`r_tasks 1` vs `r_tasks 0`, same binary, two runs per map): worker sums in task mode are
  3-12x inflated by the lock convoy. Fuma: ents 46.2 (task) versus 13.5 (serial), brush upload 26.6 versus
  2.2, alias upload 12.2 versus 1.9, sky 4.1 versus 0.9, alpha 5.1 versus 0.8; FPS 48.9 versus 22.6.
  Swamp: ents 90.5 versus 23.9, brush upload 38.7 versus 3.1, alias upload 38.9 versus 4.8, sky 8.3
  versus 1.3, alpha 8.0 versus 1.3; FPS 28.5 versus 23.4. Conclusions: the red sky/alpha rows are almost
  pure queue artifacts (real uncontended work <= 1.3 ms on both maps); the real remaining work sits in
  `ents` (13.5 / 23.9 ms serial); task mode buys 2.1x on Fuma but only 1.2x on swamp because the convoy
  eats the parallelism.
- 2026-10-11 (operator A/B): `rt_brush_persistent 0/1` on swamp, two repeats per arm: `calls_geometry`
  2333 to 1224 (-47.5%), `brush upload` sum 35.9 to 3.8 (-89%), `ents` sum 85.1 to 55.3 (-35%), frame
  slot 23.4 to 17.8 ms (-24%); FPS controls 29.3/29.7 versus 34.6/27.8 (noisy, frame slot is the solid
  signal). Sky/alpha sums and `brush pack` change little. Companion candidates: skip-covered-packing (C2)
  and world-sky batching.
- 2026-10-11 (world-sky batching): world sky polygons now append to the per-context batch buffers and
  upload once per chain (`Sky_FlushPolyBatch` in `gl_sky.c`, mirroring the water batch scheme; per-surface
  `rs_brushpasses` kept, entity sky stays dynamic). Swamp: sky lane calls 257 to 7 per frame, `sky` slot
  8.34/15.02 to 1.54/2.30 ms, frame slot 24.57 to 22.87; Fuma: calls 95 to 7, `sky` slot 4.09/6.68 to
  1.69/3.01. No renderer errors in either run; build and CTest green. The swamp FPS reading (24.3 versus
  28.5) contradicted the improved frame slot and is treated as run noise to be re-checked in the next
  matched capture.
- 2026-10-11 (P1 slice 3, per-type upload lanes): uploads are now attributed at their call sites
  (brush/sprite/alias, plus `alias surf` for the enhanced md5/md3 per-surface path). Same-binary swamp
  run (361 frames): about 2.3k uploads per frame total; alias 856 per frame (wait 24.2 ms per frame
  summed, hold 5.4 ms), the `ents`/`alpha` lanes about 1352 per frame (bmodel batch flushes: the
  immediate brush lane is empty on both maps, world and entity brush go through `RT_FlushBatch`), sky 7,
  water 3, animated 5, sprite 11, unattributed (HUD/raster) 86. `alias pose` costs only 2.5 ms per
  frame; the slot is dominated by upload queueing. The enhanced md5/md3 path is unused on both maps
  (`enhanced_draws=0`), so every alias upload is one visible entity on the Q1 path.
- 2026-10-11 (P1 slice 3, per-surface vertex slices for md5/md3): the loaders store the per-surface
  vertex base (`firstvertex_vbo`) and rebase `rtindices` to their surface, so each md5/md3 surface
  uploads only its own vertex range instead of the whole model VBO. Inert on Fuma and swamp (enhanced
  path unused there); no renderer errors on either map.
- 2026-10-11 (C2 skip-covered-packing, matched same-binary with `rt_brush_persistent`): covered
  static-movable surfaces no longer pack (the per-surface DTAL emissive light still runs). Swamp:
  uploads per frame 2082 to 973, `brush pack` 8.96 to 0.64 ms (-93%), `brush upload` 35.7 to 4.4,
  frame 23.86 to 17.02, FPS 23.6 to 28.3. Fuma: 1193 to 636, pack 5.68 to 1.09 (-81%), brush upload
  26.7 to 9.8, frame 14.03 to 10.52, FPS 31.1 to 34.9. The pre-C2 persistence A/B left pack at
  8.4/13-15 ms, so C2 removed the covered packing entirely.
- 2026-10-11 (alias pose probe): of about 750k alias draws on swamp only 116-136 were static poses and
  6-8 repeated one, so essentially every visible alias entity re-lerps its vertices each frame and an
  unchanged-pose alias cache is a dead end on these scenes. With persistence and C2 on, alias uploads
  are about 88% of the remaining 973 uploads per frame on swamp.
- 2026-10-11 (focus sleep, measured): `main_sdl.c` sleeps `SDL_Delay(16)` in the main loop whenever the
  game window has no input focus (or the game is paused). Every automated capture whose window was not
  foreground therefore shows `interval = profiled frame + about 16-18 ms`, which is what produced the
  contradicting FPS readings (for example swamp frame 17.0 ms with 28.3 FPS). Slot-based CPU numbers
  and GPU pass timers stay valid; only the frame interval is contaminated. The acceptance ledger must
  run with the game window focused (the `run_stress.ps1` focus gate exists for exactly this reason).
- 2026-10-11 (acceptance infrastructure): `tests/perf/qr_medium.cfg` (Medium quality: clouds 1,
  godrays 1, GI 1; everything else as the audit profile) plus `run_stress.ps1 -RtProfile audit|medium`
  and the `qr_ad_swamp` save in the allow-list. 4K windows through the attribution harness need
  `vid_unlock` before `vid_width/vid_height`, otherwise the resolution does not change.
- 2026-10-11 (first 4K Medium capture with persistence, matched same-binary, window unfocused):
  TF: CPU frame 10.32 ms, GPU frame 19.74 ms (setup 3.66, indirect 4.78, direct 2.99, compose 2.67,
  clouds 1.37, gradient 0.98, primary 0.98, upscale 0.81, godrays 0.27, sky 0.22, post 0.26, ui 0.21).
  Swamp: CPU frame 16.39 ms, GPU frame 24.24 ms (setup 5.84, indirect 5.19, direct 3.86, compose 2.89,
  clouds 1.44, gradient 1.17, primary 0.97, upscale 0.84, reflrefr 0.41, godrays 0.28, sky 0.28,
  post 0.29, ui 0.23). At 4K Medium the wall is the GPU: the 16.7 ms budget needs about 3 ms less on
  TF and about 7.5 ms less on swamp; the swamp CPU frame (16.4 ms) is right at the budget. The largest
  individual GPU items are the frame setup (TLAS/scene work), indirect GI and direct NEE rays, and
  the compose pass.
- 2026-10-11 (upstream synthesis frozen): the pre-merge upstream-porting synthesis (H01-H16
  classification, the adopted verifier corrections, the decisive cross-conclusion and the proposed
  P0/P1 → measurements → Tier A/B/C order) is recorded in `docs/upstream-porting-synthesis.md`
  together with the P0/P1 status this branch has already delivered.
- 2026-10-11 (Tier A upstream port, first slice): H02 was refuted as a live mechanism (the SIMD
  mark/cull/chain block is compiled out; PDB and frame-CSV evidence) and moved to a separate
  parallel-marking design task. Landed: H15 task-queue sizing invariant with the upstream-style
  asserts; H14 removal of the dead `update_warp` writes, the `lightmaps[].modified` field and the
  no-op `R_UpdateLightmaps` task with its DAG edges; H01 per-call batching of `rs_brushpolys`;
  H12 devstats gate; H04 prefix clearing in `RT_TRACK_BeginFrame`; H09 entity-side per-model sky
  classification (`used_specials`); H13 emissive-diagnostics ownership fix (atomic counters; the gate
  dropped as unmeasurable); the Debug duplicate-find consolidation in the renderer. The runner now
  validates the clouds quality per profile and the bench settings record `r_tasks`/`r_parallelmark`.
  Build and CTest green.
- 2026-10-11 (parallel world marking, v2): the marking pipeline runs as `prepare -> mark_leafs
  (64 leaf chunks, word-run atomic OR into the shared surfvis mask) -> {store_efrags, chain_slices
  (per-texture indexed chain) -> chain_finish}` under `r_parallelmark 1`; `r_parallelmark 0` and
  `r_tasks 0` keep the scalar path. Same-binary audit A/B: swamp frame/wait 23.05 -> 19.62 ms
  (-3.4 ms; fps 30.4 -> 33.6), Fuma 13.12 -> 11.82 ms (-1.3 ms); `calls_geometry` matches
  modal/mean (2082 both, 1194 both). Water-shot on/off pairs showed no difference beyond the within-run
  noise; the pair provenance was not retained (see the review-fixes entry below). Accepted deviation: per-texture chain
  order is surface-index order instead of the scalar leaf first-touch order (docs contract
  updated). The v1 order-preserving attempt (per-chunk first-touch lists plus a serial merge)
  measured no wall win (mark sum 3.56 ms plus an applied serial chain of 3.60 ms against the
  scalar 3.90 ms) and was replaced.
- 2026-10-11 (mark-parallel v2 review fixes): the three final reviewers classified M01-M07 CONFIRMED
  and M08 REFUTED on one added code comment (removed). Also fixed: the surfvis memset now runs only on
  the selected parallel path; the docs order note carries the head-flag homogeneity precondition;
  PERFORMANCE.md notes the new mark/chain slot semantics. Recorded evidence limits: the Fuma wall/fps
  numbers are confounded by a ~16.5 ms unprofiled block and a 5x server-pass difference between the
  arms (frame/wait is the controlled metric), the water-pair toggle provenance is not recorded
  (fixtures deleted), and `calls_geometry` equality is modal/mean rather than frame-exact. Known benign
  exception: the two shared dummy textures keep `rt_chain_index = 0`; their surfaces still chain via
  the texture pointer.
- 2026-10-11 (stage 2.1, remaining producers and the CPU/GPU bound): with parallel marking in place the
  remaining above-threshold rows on the audit profile are `ents` (window max 25.5 / avg 17.3 on swamp,
  the entity worker sum), `alpha` 12.4/7.8, `cpu.draw.RHI_setup` (renderer CPU inside qrDrawFrame)
  11.6/5.7 and `ents_brush` 6.4/4.2; sky is down to 2.3/1.4. At 1280x720 the audit run is CPU-bound
  (CPU frame/wait about 19.6 ms against a GPU frame of 10.6 ms); at 4K Medium it is GPU-bound (CPU about
  13 ms after the marking win against a GPU frame of 24.2 ms). The Fuma arm of this batch was lost to a
  foreign machine owner and the swamp scalar control was disturbed (121 ms outlier); the swamp on-arm
  reproduces the v2 numbers (frame 19.57, mark 1.48, chain 8.28). Next: the code-review stage, then the
  GPU investigation with the Radeon Raytracing Analyzer.
- 2026-10-11 (stage 2.2, code review): six independent review axes covered the changed surface - dead
  code and leftovers, duplication, architecture and responsibilities, code smells, concurrency
  contracts against `docs/multithreading.md`, and documentation/evidence consistency - producing 90+
  findings. Accepted for the cleanup: the dead constructs confirmed by at least two reviewers (the
  compiled-out SIMD mark/cull/chain block and its SoA buffers, `world_texstart/end` with
  `R_SetupWorldCBXTexRanges`, the legacy dynamic-light marking chain with its write-only fields, dead
  counters and lightmap-era statics, six reader-less cvars, stale externs and declaration leftovers),
  plus the diagnostics fixes (sky upload-lane attribution, the never-pushed `gui` lane, compile-time
  lane coupling asserts, the alias pose probe gate, the `QrUploadLockStats` initialization), the
  fan-index single source, the unreachable flush-time covers gate, the mark-flag ordering, the
  warn-once parallel fallback, and the profiler/multithreading documentation corrections. Deferred
  with recorded reasons: the animated light-texture divergence, the emissive counter unification, the
  lightmap material removal project, the warp-image removal (needs a renderer-side check), the
  `cull_surfaces` naming, the emis-watch lock split, the task-runtime capacity asserts and the load-
  time scaffolding questions. Rejected: removing `r_gpulightmapupdate` (documented retention) and the
  `rt_mark_parallel_frame` race hypothesis (submission order already made it safe; the cleanup writes
  the flag before the prepare submit regardless).
- 2026-10-11 (stage 2.3, cleanup): two verification agents falsified the deletion set first (no hidden
  readers; `R_MarkLights`/`R_AddDynamicLights` removed as one unit) and the behavioral risks (all safe;
  the single guard needed was `QrUploadLockStats`: `valid` stays, but the engine now zero-initializes
  the struct so the suspended-call path cannot inject garbage). The cleanup touches 25 files (964
  deletions, 77 insertions), keeps `surfvis`, `cl_dlights` and `rt_dmg_inthisframe`, and fixes the
  mark-flag ordering, the fallback warning, `R_ClearTextureChains` reuse, the fan-index helper, the
  sky lane, the alias probe gate and the lane asserts. Acceptance: Debug build exit 0, CTest 8/8, and
  a matched audit run reproducing the v2 numbers (swamp frame 20.37, mark 1.47, chain 8.41; Fuma frame
  12.65, mark 0.90; no renderer errors; `calls_geometry` modal 2082/2084, means 2082.1/2082.7). Two
  final code reviewers confirmed the removals (no blockers) and the behavior claims; their nits were
  fixed (the `r_parallelmark` gate keeps its exact non-zero test, the `TexMgr_PreMultiply32` banner
  was restored, one comment reworded, `changelog.md` lost the stale `lmap` mention).
- 2026-10-11 (PR gate, standing procedure before every PR): build Release (`build_win.ps1 Release`;
  the runtime needs the base paks and named saves copied forward on a fresh config) and run one matched
  capture per save on both binaries with identical settings (4K FSR Balanced, Medium) through
  `tests/perf/run_stress.ps1`, retrying a capture when the focus gate rejects it (shared machine).
  Result on the stage-2.3 tree: swamp Debug 38.6 fps / 25.90 ms (p95 27.69) against Release 43.2 fps /
  23.15 ms (p95 26.40), i.e. +11.9% fps / -2.75 ms; Fuma Debug 51.7 fps / 19.33 ms (p95 24.47) against
  Release 54.3 fps / 18.40 ms (p95 19.22), i.e. +5.0% fps / -0.93 ms. Both saves remain below 60 fps at
  the acceptance profile, matching the known GPU-bound state (GPU frame about 21.5-22.1 ms swamp and
  17.9 ms Fuma in these captures).
  A Debug-vs-Release gap larger than this is a signal that Debug-only overhead dominates the scene
  rather than the shipping build being slow.
- 2026-10-11 (CPU pass, deep-reasoning round 1 - diagnosis and corrections): eight independent solver
  passes, four blind falsification verifiers and three final reviewers (one round) established, with
  corrections: (a) the visible ~8-10/32 busy threads are the graph's structural width for the draw
  stage (6 entity chunks plus sky/water, alpha and particles; world and viewmodel are 1-wide
  dependents), and workers park by design; no artifact records worker/queue state, so a scheduling
  pathology is unmeasured rather than excluded; (b) on the acceptance profile the Release swamp entity
  per-invocation maximum is 5.61 ms (max 7.65) and the upload-lock waiting is modest (alias 4.4 ms per
  frame summed against 1.3 ms of hold; all lanes 11.9/2.3), unlike the Debug 720p convoy (58.96/9.79),
  so Release is not convoy-bound; (c) the live `rt_stats 3` "CPU 20-24 > GPU" reading is a
  window-maximum-versus-snapshot artifact: the gate means are CPU 18.79/15.44 against GPU 22.15/17.87,
  with interval-minus-gpu of 0.95/0.50 ms; (d) the earlier cited "scalar control" (`mp4-swamp-off-r1`)
  is `r_parallelmark 0`; the real serial arm (`swamp-rt0-r1/r2`, revision 19d1ca2b) is 40.67/40.65 ms
  against the same-revision task runs 24.57 (traced) and 20.37, i.e. 1.65-2.0x; (e) per-invocation
  maxima replace the sum-based "unattributed wall" reading: Release swamp shows ents 5.61,
  particles_resolve 3.35, alpha 2.64, sky 1.1, chain 0.78. A dedicated `begin` profiler slot around
  `qrStartFrame` in `GL_BeginRenderingTask` was added to settle the remaining begin-task pacing
  question and to size the serial corridor in future captures.
- 2026-10-11 (CPU pass, `begin` slot measurement on Release swamp): the new slot reports 12.78 ms mean
  (max 20.97) inside an 18.72 ms `cpu.frame_ms` on the acceptance profile. The frame is therefore
  GPU-pacing-bound - `qrStartFrame` waits on the frame fence of the in-flight slot and on image
  acquire - and the actual CPU work in the frame is about 6 ms (frame minus begin): the top work items
  are the entity stage (per-invocation chunk maximum about 5.6 ms; sums alias 9.88 + brush 14.56),
  `chain` 6.25 summed, `particles resolve` 2.88, `alpha` 1.97. Consequence for the targets: at the
  acceptance profile the CPU has large headroom (the minimum plan is not CPU-limited), and the maximum
  plan (CPU must not limit even at FSR ultra performance) is a matter of shrinking the ~6 ms of work
  and its critical path, not of adding workers.
- 2026-10-11 (CPU pass, `rt_brush_persistent` on the acceptance profile, same binary): Release 4K FSR
  Balanced Medium: swamp `cpu.frame_ms` 18.79 -> 17.34 (-1.45 ms), `calls_geometry` 2083 -> 973; Fuma
  15.44 -> 14.42 (-1.02 ms), 1199 -> 638; fps 43.2/54.3 -> 42.9/54.1 and interval means 23.15/18.40 ->
  23.28/18.50 stay inside the run noise because both saves are GPU-bound on the means. The flag stays
  off by default pending the mover visual acceptance; the perf profiles keep pinning it off until that
  decision is recorded.
- 2026-10-11 (persist mover acceptance, **failed**): the recorded `qr_mover_check` demo shows a visible
  shimmer on moving submodels with `rt_brush_persistent 1`. Still screenshot pairs between the arms are
  identical (phase-matched diffs 0.0005-0.006), so the defect is temporal; the flag stays off by default
  and the perf profiles keep pinning it off. First trail for the fix: `qrUpdateGeometryTransform` ->
  `VertexCollector::UpdateTransform` writes the AS transform staging and
  `GeomInfoManager::WriteStaticGeomInfoTransform` (new `model`, previous kept as `prevModel`, both frame
  slots marked for copy, `movableIDToGeomFrameInfo` updated), while the renderer recreates the movable
  BLAS from the transform staging and retires the old one behind the frame that referenced it; the
  shimmer suggests a frame in which the traced transform and the motion/shading metadata disagree.
  Repro: `playdemo qr_mover_check` with `rt_brush_persistent 1` (a live console toggle works).
- 2026-10-11 (persist mover acceptance, **passed**; default flipped): the recorded `qr_mover_check`
  demo is clean after the per-frame `prevModel` fix (`ce81db59`): `PrepareForFrame` copies each
  movable record's model into `prevModel` before the producers run, so a frame without an update
  reports zero motion while a transform update in the same frame still overwrites the pair. The cvar
  now defaults to `1` (`Quake/gl_vidsdl.c:186`) and neither perf profile pins it any more; `0` remains
  the opt-out. Acceptance: Debug build exit 0, CTest 8/8, Release build exit 0, and the operator's
  visual pass on the demo (the still-pair audit had already shown identical frames).
- 2026-10-11 (CPU pass, chain elision): steady frames now chain only the consumed texture subset -
  the textures whose chains the sky, water and animated passes read - while a static-submit or
  world-light-recollect window, or `r_showtris`, keeps the full set. The mode (`rt_chain_full_frame`)
  is decided and written pre-submit next to `rt_mark_parallel_frame`, and the consumed set is built
  once in `RT_MarkParallel_NewMap` (`rt_chain_textures`/`rt_chain_consumed`). Verified on one binary:
  the mover-demo A/B shows `chain` avg 1.30 -> 0.02 ms (max 8.26 -> 4.13, the maxima being the
  static-window frames), and the acceptance captures show chain 6.25 -> 0.10 ms (swamp) and 6.25-like
  -> 0.05 (Fuma) summed, with frame 18.12 / 15.40 against the pre-change 18.72-19.27 / 15.44 (the wall
  change sits inside the run spread; the win is the removed per-frame work and the chain barrier
  edge). An 18-pair phase-matched demo screenshot comparison (best diffs 0.0005-0.006) shows
  identical drawn output, and the sky/ents/mark slots are unchanged. `rs_brushpolys` counts the
  consumed subset in steady frames (documented in PERFORMANCE.md).
- 2026-10-11 (CPU pass, per-chunk entity timers): `ents c0`..`ents c5` are nested slots inside `ents`,
  one per `NUM_ENTITIES_CBX` chunk, with a compile-time link to the constant. Acceptance-profile means:
  swamp 4.19/3.77/3.28/4.82/4.83/2.12 ms (slowest/fastest 2.28x, `ents` sum 23.01, frame 18.75) and
  Fuma 2.57/2.07/2.83/2.06/2.44/1.06 (frame 15.39). This confirms the post-begin graph wall is set by
  the slowest chunk (4.8 ms against a 5.9 ms graph) rather than by the summed work, and it is the
  baseline for any entity-width or partition change; `chain` measured 0.07 ms in the same captures
  after the elision.
- 2026-10-11 (CPU pass, entity widening 6 -> 12, measured and reverted): raising
  `NUM_ENTITIES_CBX` to 12 (CBX/chain/lane enums grown, deferred light slots 8 -> 14, per-chunk
  slots and names extended; compile-time asserts updated) did not reduce the wall. Acceptance
  captures: the entity work sums roughly doubled (swamp `ents` 23.0 -> 50.5, alias 10.2 -> 25.6,
  brush 14.6 -> 28.4; Fuma 13.0 -> 33.0) while the frame stayed put (18.72 / 15.40) and the slowest
  chunk worsened on swamp (4.83 -> 5.56 with about one entity per chunk). The stage is limited by
  the shared upload and deferred-light-flush path rather than by chunk count, which matches the
  earlier aliasplit/typelanes nulls; the change was reverted. Widening becomes worth re-testing only
  after the upload critical section shrinks.
- 2026-10-12 (step 0 instrumentation for the culling/residency decision): the upload trace now carries the
  per-vertex bounds-scan attribution (`upload.bounds calls/verts/avg_ms/per_call_us`; the
  `AccumulateGeometryBounds` time is sampled in `Scene::Upload` and surfaced through `QrUploadLockStats`),
  and the bench block carries the covered-path counters (`brush.covered surfaces_avg/models_avg/
  emissive_avg_ms`; a model counts as covered when every visited non-sky surface of a discrete entity
  draw was skipped by `RT_StaticMovableCovers`). The FSR ultra-performance arm needs no code:
  `run_stress.ps1 -Presets ultra`. Decision inputs for the next steps: bounds share of lock hold, the
  fully covered persistent movables and their emissive cost, and `frame - begin` against the measured GPU
  period at ultra performance. Debug build exit 0, CTest exit 0, Release build exit 0.
- 2026-10-12 (step 0 results, Release 4K FSR medium, `rt_brush_persistent=1`): the decision inputs landed.
  Lock convoy (trace on): all-lane wait/hold 13.42/1.85 ms per frame on swamp and 8.23/1.23 on Fuma; the
  alias lane owns 9.31/1.53 (69%/82%) and 4.59/0.80 (56%/65%). The bounds scan (972 calls/frame on swamp,
  642 on Fuma) costs 0.378/0.229 ms per frame (0.39/0.36 us per call, about 20% of hold) - above the 15%
  bar, so the critical-section shrink stays a candidate with a small expected hold cut. Covered persistent
  movables: 405 (swamp) / 257 (Fuma) fully covered models per frame, 8943/7361 covered surfaces, covered
  emissive 0.093/0.261 ms per frame - below the 0.5 ms bar, so the emissive retention rework is dropped;
  the covered-model fast path keeps its expected ~0.5-1 ms brush-chain cut. Ultra-performance arm (no
  trace): CPU `frame - begin` 2.37 (Fuma) / 4.07 (swamp) against GPU periods 9.13/11.29 ms and 103/63 fps -
  the max plan is GPU-bound at ultra, and `gpu.setup` (3.35/5.21 ms, 37%/46% of the GPU frame) is the top
  GPU item. Open: on swamp the wall interval exceeds the GPU period by about 4.5 ms at ultra (1.6 at
  balanced) while Fuma is tight (0.5); not attributable to CPU from these captures. Note: a stale
  `rt_brush_persistent "0"` saved in `build/{Debug,Release}/ad/config.cfg` overrode the new default and
  flipped the switch off in the first Debug run; the line was removed from both configs and every reported
  capture shows `rt_brush_persistent=1` in its settings row.
- 2026-10-12 (as-churn counters for the GPU-first pass): the frame stats carry the dynamic BLAS counters
  of a frame - `dynamicBlasBuilds`, `dynamicBlasRecreations` and `dynamicBlasCreateMs` (`QrFrameStats`,
  filled from `RhiAccelStructs::GetDynamicFrameCounters`; zeroed once per frame in `VulkanDevice::DrawFrame`
  so frames that build no TLAS report zeros, counted after each build and in the create branch of
  `AppendDynamicSlot`). The bench block prints `as.dynamic builds_avg/recreations_avg/create_avg_ms`
  (the create time is the per-frame sum over that frame's create path - descriptor construction, allocation
  and the retire of the replaced handle; recreations count handle replacements only and conflate the
  order-only shape mismatches with genuine list growth or reactivation). Purpose: size the task-order
  handle churn before the order-insensitive envelope or the stable-packing refactor. First Release capture
  (Fuma, Balanced, medium, `r_tasks 1`): builds 6.00, recreations 1.00, create 0.563 ms per frame - one
  dynamic BLAS handle recreated per frame at about 0.56 ms, about 19% of the same capture's
  `cpu.draw.RHI_setup_ms` (2.91). Debug build exit 0, CTest 8/8, Release build exit 0.
- 2026-10-12 (per-filter churn attribution, PR-1 of the stable-packing track): `DynamicShapeSortedFits`
  (rank-wise dominance of sorted shape tuples, `DynamicBlasShape.h`) is computed whenever the positional
  envelope fails, and every dynamic filter reports builds/recreates/positional-fails/sorted-pass/sorted-fail
  through `QrFrameStats`; the bench block prints one `as.filter filter=0x... ...` row per filter.
  Acceptance captures (Fuma, Balanced, medium, Release): `r_tasks 1` - 6.00 builds, 1.00 recreations,
  0.423 ms create per frame, all of it in `filter=0x0000004c` (dynamic opaque `PV_WORLD_0`), whose
  failures split 0.24 sorted-pass (order-or-shrink) and 0.76 sorted-fail; `r_tasks 0` - 0.07 recreations,
  0.030 ms, sorted-fail 0.06, sorted-pass 0.00. Consequence: the churn is not order-only; the
  order-insensitive envelope alone (F-C) would remove at most the 0.24 share, and the 0.76 share is a
  task-mode content/shape difference with no serial counterpart, so the next diagnostic splits
  sorted-fail into count-change vs shape-change (and records the list length) before the F-C versus
  canonical-packing decision. Debug build exit 0, CTest 8/8, Release build exit 0.
- 2026-10-12 (churn split, PR-1b of the stable-packing track): sorted-fails split into count-change and
  shape-change, and every `as.filter` row carries the list length (avg/min/max). First capture (Fuma,
  Balanced, medium, `r_tasks 1`): `filter=0x4c` (dynamic opaque `PV_WORLD_0`) recreates every frame with
  `scount 0.74 / sshape 0.01` and `len_avg 555.5, len_min 512, len_max 598` - the task-mode churn is a
  per-frame geometry-COUNT instability of that list (about 43 entries), not tuple reordering; the serial
  arm shows 0.07 recreations per frame. Localizing captures queued: serial length baseline and a
  `r_parallelmark 0` arm. Debug build exit 0, CTest 8/8 (one `qray_dtal_perf` flake passed on re-run),
  Release build exit 0.
- 2026-10-12 (churn mechanism pinned, PR-1c): fail deltas and envelope sizes are exported per filter
  (`env_avg/min/max`, `fdelta_avg/min/max`). Tasks (Fuma, Balanced, medium): `len_avg 561.4` against
  `env_avg 561.3`, `pfail 1.00`, `fdelta_avg +0.37`, `fdelta_min -30`, `fdelta_max +200`; serial:
  `len_avg 560.1` against `env_avg 578.9`, `pfail 0.08`, `fdelta_avg -8.30`. Mechanism: under tasks the
  envelope is re-adopted every frame, so any small count change or reorder-induced per-index bound shift
  fails the positional test and resets it; under serial the deterministic order lets the envelope persist
  as a high-water (579 against a 560 mean) and only genuine growth beyond it fails. The count fluctuation
  itself (about +/-40) is genuine and mode-independent. Consequence: the order-insensitive predicate (F-C)
  should stop the reorder-triggered resets and bring the task-mode churn toward the serial floor; its
  driver-side spec safety rests on the NVRHI build-time re-query guard (skip and log, never out of bounds).
  Debug build exit 0, CTest 8/8, Release build exit 0.
- 2026-10-12 (order-insensitive reuse predicate behind a cvar, PR-2): `rt_dyn_blas_sorted` (default 0)
  switches the dynamic-BLAS reuse predicate to the rank-wise sorted comparison. Same-binary A/B (Fuma,
  Balanced, medium, `r_tasks 1`): off - recreations 1.00, create 0.458 ms, sord 0.26 / scount 0.73; on -
  recreations 0.81, create 0.365 ms, sord 0.00 / scount 0.80; no NVRHI guard messages in either arm. The
  sorted predicate removes the order component entirely but not the churn: the remaining failures are
  genuine geometry-count changes, and the envelope is current-sized by construction, so any count rise
  recreates the handle. Across repeats the serial arm stays flat (0.07-0.11 recreations) while every
  task-mode repeat rose (1.00-1.01); the content-level cause of that mode-correlated trend is still open.
  Next: a create-time headroom envelope (allocate with slack and reuse the handle while the build fits the
  allocation) to absorb the fluctuation, with the memory cost measured. Debug build exit 0, CTest 8/8,
  Release build exit 0.
- 2026-10-12 (list-length trend telemetry): the `as.filter` row now carries `len_first/len_last` per filter, so
  a capture shows whether the churning filter's list length rose or fell across the window. Built and tested
  (Debug exit 0, CTest 8/8, Release exit 0); the trend captures are blocked in the current desktop session by
  `VK_ERROR_OUT_OF_DATE_KHR` at swapchain creation (`Swapchain.cpp:139`), the same non-interactive-session class
  as the earlier focus-gate refusals, and will be re-taken when the desktop is interactive again.

- 2026-10-13 (editor and world-submit fixes, PR #48/#49 merged, cluster defaults flipped): two
  world-visibility bugs are fixed - the editor's light-material edits take the full re-upload path
  again (`is_light` is no longer light-only, and a shape-changing in-place light-material update
  reports itself as a replacement), and a static submit now defers when the frame's chains were built
  before the request, so a partial chain set can never replace the static world (the wall-disappearing
  bug the owner hit in the editor; diagnostic line `static submit was deferred`). `origin/master`
  (PR #48 texture-overlay, PR #49 one-more-leaf) is merged; the interface recording of
  `RtQ2ReflectRefract.rgen` was re-recorded from the merged source and the full CheckShaderProperties
  run passes. The repaired cluster source selection (`rt_cluster_sampling`) and the DTAL grouping
  (`rt_dtal_groups`) default to `1` now after the owner's in-game A/B on the AD `start` hub (93
  clusters at the 128-light cap; the sampled arm there reports them as overflow now, clusters
  0.76 -> 1.08 ms) and neutral matched captures on `ad_tfuma` (A0B0 54.6 / A0B1 54.5 /
  A1B0 54.8 / A1B1 55.8 fps; gpu 17.5-17.8 ms; cluster work 0.05-0.06 ms against 0.76 ms on the
  `start` baseline). The trend captures show the task-mode dynamic-BLAS churn is a genuine count
  oscillation (len_first/last 565/569 and 578/597, +/-40 swing) and that the sorted predicate alone
  does not pay (with `rt_dyn_blas_sorted 1` from the local config, serial churn rose from 0.07 to
  0.85 recreations per frame), so the create-time headroom envelope stays the next geometry step.
  The first validation-enabled capture confirms the ray-stats set move (no `VUID-07990`) and surfaces
  a separate `VUID-vkCmdDrawIndexed-None-08600` (a raster draw against an 88-byte push-constant
  layout where the raster overlay's is 124) for a follow-up. Debug build exit 0, CTest 8/8, Release
  build exit 0 (post-merge).

- 2026-10-13 (dynamic BLAS headroom envelope and the rank-wise reuse fit; `b9e49b9e`): the step the
  2026-10-12 entries left open is closed, and the measurement moved the fix. `rt_dyn_blas_headroom`
  (percent, default 25) makes the create descriptors and the adopted envelope larger than the shape
  that created them (per-geometry counts padded with floors, extra slots sized from the list's mean);
  the sorted test was refined to rank-wise dominance of the primitive counts - the allocation is a
  function of the geometry count and the primitive counts only, so the identity fields (stride,
  format, indexedness, transform presence) need no comparison - and now defaults to 1, which makes
  it a superset of the positional test. Matrix on Fuma (Balanced, medium), filter 0x4c:
  positional/0 - recreations 1.00, create 0.417 ms, env_prims 83.7k; positional/25 - 1.00, 0.69 ms
  (the identity comparisons reject every frame once the list reorders and the reserve slots
  interleave - this is what forced the predicate revision); rank-wise/0 - 0.00, 0.000 ms,
  env_prims 135.9k (the envelope is the natural high-water of the frame that created the handle);
  rank-wise/25 (the default) - 0.00, 0.000 ms, env_prims 135.2k; serial rank-wise/25 - 0.00;
  serial positional/25 - 0.05, 0.033 ms. No NVRHI build-size guard message in any arm;
  `env_prims_avg`/`len_prims_avg` in the `as.filter` row carry the envelope's cost in
  allocation-driving units (the pinned NVRHI exposes no byte size). Debug build exit 0, CTest 8/8
  (the shape tests follow the new contract), Release build exit 0. Follow-ups: the guard's own
  message stays the only exact byte figure (it must never fire), and the serial positional arm is
  the only arm left with recreations above zero.

- 2026-10-13 (entity and upload attribution, the closing recon before the next master merge): the
  upload trace (`rt_upload_trace 1`) on the headroom build separates the mutex convoy from the wall
  again. Fuma (4K FSR Balanced, audit profile) under `r_tasks 1`: summed lock wait about 6.4 ms per
  frame against about 0.9 ms of hold - the alias lane owns 3.38 ms of that wait over about 309 calls
  per frame (hold 0.58), alpha 1.11, ents about 1.0, sprite 0.56 - and the bounds scan runs 635 calls
  and 87.6k vertices per frame at 0.18 ms (about 20% of the hold). Under `r_tasks 0` the same trace
  shows the wait at about zero (alias 0.007 ms) with the hold unchanged, so the convoy is a
  task-mode contention artifact rather than burned CPU, and the entity stage's chunk wall stays at
  1.4-2.0 ms (Fuma) and 2.3-3.5 ms (swamp) against a 2.87 ms serial stage. Both scenes are GPU-bound
  at this preset: `gpu.frame` 19.5/22.4 ms against intervals 20.5/23.75 ms, the acquire/fence wait
  8-12 ms, and the earlier ultra arm's CPU `frame - begin` 2.37/4.07 ms. Consequence: the
  entity/upload work is CPU headroom for the ultra (~120 fps) target and the swamp margin, not the
  lever for the 60 fps balanced target; the next stage works the GPU rows (`gpu.indirect` 6.39/6.55,
  `gpu.setup` 3.45/5.17) once the master merge lands. The alias pose probe is unchanged
  (`enhanced_draws` 0, `pose_static` 0 - every Q1 alias draw re-lerps). MCP note: `server_status`
  matched this worktree (HEAD `329c6ecb`), but the AST tools answered MCP busy (heavy machine owner)
  for the whole window, so the symbol work here is a file-read fallback; re-run the symbol checks
  once the machine is free.
