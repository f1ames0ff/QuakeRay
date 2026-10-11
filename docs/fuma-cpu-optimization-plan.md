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
- Known documented defect, pre-existing: validation-enabled runs emit one
  `VUID-VkRayTracingPipelineCreateInfoKHR-layout-07990` (`RtQ2ReflectRefract.rgen`, set-7 `smokeRayStats`;
  `docs/particle-plan.md:182`). It is not merge-introduced; the fix is scheduled by the particle docs
  (move the smoke stats set). Validation layers are enabled by the renderer config file `qray.txt`
  (`vulkanvalidation`); the engine `-validation` argv flag is dead code.
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
