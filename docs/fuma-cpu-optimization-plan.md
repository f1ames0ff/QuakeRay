# Terror Fuma CPU critical-path and producer optimization plan

Updated: 2026-10-10.

Status: planning only. No engine changes, builds, test runs, gameplay manipulation or new captures were performed for this plan. Source investigation used `perf/rhi-dyn-blas-clean` at `7841298f`; the last deployed Debug receipt names `6613e196`, executable SHA256 `8018D4B456D17BBF836953066B36902A6FE209AECEB8FCE373790C2BAECC0925`. The intervening changes are documentation. Recheck identities before execution.

## 1. Contract and ownership

The operator reports approximately 70+ FPS on many maps but only approximately 40-46 FPS in Terror Fuma with `r_tasks 1`. Explain why `ents`, `sky` and `alpha` are expensive, distinguish an expensive worker from a blocked worker, and select evidence-backed ways to reduce the CPU critical path without lowering quality.

Latest steering supersedes this session's earlier WR01/WR04 implementation queue:

- Clusters, particles and BLAS reuse/parallelization belong to the other agent in `calm-eagle`. Do not implement or duplicate those changes here.
- This lane investigates opaque entity slices, alpha, sky/water/animated-world preparation and their upload boundaries. Other producers may be observed as blockers without changing their algorithms.
- Shared renderer instrumentation, profiler interfaces and scheduler changes require file-ownership coordination before implementation. Keep independent candidates separate; remeasure combinations afterward.
- Preserve objects, lighting, transforms, stable IDs, transparency order and motion history. Retain both `r_tasks 0` and `r_tasks 1`.
- Do not lower quality, cull RT contributors using camera visibility alone, alter simulation rate, bypass synchronization or introduce a new pool for an FPS claim.
- Do not close or send input to an operator's game without explicit permission. Later builds/runs obey the machine guard and one-game-instance rule.

Read [ARCHITECTURE.md](../ARCHITECTURE.md), [PERFORMANCE.md](../PERFORMANCE.md) and the adaptation checklist in [multithreading.md](multithreading.md). The broader integration roadmap lives on `perf/rhi-dyn-blas-reuse`; this is the scoped producer-lane plan, not authorization for its excluded workstreams.

## 2. Targets and the literal all-green requirement

### Frame and quality budget

- Actual output: 3840x2160; actual successful FSR Balanced execution, not merely its selected cvar.
- Volumetric sky Medium: `rt_sky_clouds_quality 1`; verify cloud enablement and applicable sky gates.
- Task mode: `r_tasks 1`; serial remains a separate correctness/regression arm.
- At least 60 FPS in the maintained scene matrix: mean and p95 frame intervals at most 16.67 ms. Report p99, maximum, hitches and over-budget fraction.
- Preserve the other declared graphics settings. Existing Low captures are historical context, not Medium acceptance.

“Any map” is a goal, not something a few captures prove. Broad acceptance needs a named supported-content matrix, representative positions/encounters, transitions and longer sessions. Keep untested coverage open.

### Actual graph colours

| Colour | Current condition | Source |
| --- | --- | --- |
| Green | less than 5.5 ms | `Quake/rt_stats_gui.c:10` |
| Yellow | at least 5.5 ms, less than 11.1 ms | `Quake/qr_gui.cpp:1937-1944` |
| Red | at least 11.1 ms | `Quake/rt_stats_gui.c:11` |

The same thresholds apply to FRAME, MAIN, draw-frame, producer, renderer CPU, GPU total/pass and host rows. WAIT is neutral. Sparkline segments retain peak-based colouring, so a green latest value alone does not establish an all-green history (`Quake/qr_gui.cpp:1978-1981`).

**60 FPS and an entirely green panel are not equivalent obligations.** A 16 ms full-frame timer is still red. A 5.5 ms frame interval corresponds to approximately 182 FPS, illustrating the strictness of the colour target; individual timer scopes are not all frame intervals.

Maintain two acceptance ledgers:

1. **Frame-budget ledger:** actual rendering cadence, requested quality and correctness against 16.67 ms.
2. **Literal colour ledger:** all budget-coloured values/history below 5.5 ms during the declared steady-state validation window, without yellow/red segments. Report full-frame, host, GPU and excluded-lane rows explicitly; do not silently waive them.

This lane cannot guarantee all-green GPU/particle/BLAS rows without the other lane's work. State any unmet obligation. Do not increase thresholds, hide timers, reset history after a slow sample, cosmetically split scopes, disable the overlay for the acceptance screenshot, or discard valid expensive frames. Keep loading/transition results separately identified, not silently removed from an all-gameplay claim.

## 3. Screenshot evidence and missing reproduction inputs

| Operator screenshot observation | Value | Limitation |
| --- | ---: | --- |
| FPS | 45.6 | Not a retained matched benchmark |
| CPU FRAME / WAIT | 21.8 / 21.8 ms | Reporting-window values; WAIT is the main-thread draw-graph join |
| CPU MAIN | 0.02 ms | FRAME minus WAIT, not all host work |
| CPU ents / sky / alpha | 13.8 / 9.75 / 7.58 ms | Invocation maxima, potentially from different frames |
| qrDrawFrame average / maximum | 6.02 / 7.14 ms | End-task scope, partly overlapping host tail |
| GPU RHI total | 10.0 ms | GPU snapshot; does not verify target resolution |
| Host total / server / gfx | 20.9 / 6.05 / 14.8 ms | Different scope/cadence from the CPU window |
| Geometry calls | 1251 | Calls, not unique entities or triangles |
| GPU clouds | 0.00 ms | Effective enablement/settings/path need checking |

Do not sum ents + sky + alpha or CPU + GPU. Tasks overlap, nested slots include children, and shared-lock wait may inflate several producers simultaneously. “One saturated compute thread” is a hypothesis, not established by the screenshot.

HOST SERVER includes previous-end joining, input/commands, networking, simulation and client update (`Quake/host.c:934-1011,1036-1044`); GFX also includes post-screen particle/smoke updates. Do not label the entire host-server timer as QuakeC/physics or add it to overlapping screen/end-task waits.

Missing inputs: exact Fuma map/save/camera/FOV and encounter state, actual graphics configuration, and a named faster map/position representing the reported 70+ FPS. Existing `qr_fuma_start` is not established equivalent to this screenshot. AD hub is a regression scene, not a proven fast reference: its older approximately 49 FPS capture does not establish the requested contrast.

## 4. Verified source map and hypotheses

| Boundary | Actual work / ownership | Diagnostic need |
| --- | --- | --- |
| `R_DrawEntitiesTask` | Six count-based contiguous visible-list ranges; opaque filtering happens inside each range | Slice imbalance, rejected entries and preparation/upload costs |
| `R_DrawAlphaEntitiesTask` | One scalar full-list scan preserving current order | Traversal versus actual transparent work and its finishing time |
| `R_DrawSkyAndWaterTask` | Sky geometry, world water and animated surfaces | Separate the three CPU subpaths; this is not cloud generation |
| Entity dispatch | Alias/brush/sprite producers; type slots cover both passes | Model/surface/vertex/byte distributions |
| Geometry/raster uploads | Same `geometryUploadMutex`, held over validation and collector work | Acquisition wait, held work and blocker identity |
| Scene/collectors | Bounds, material/ID lookup, copies, descriptors, history and raster append/proxy processing | Private versus shared mutable work |
| `RT_Prof_End` | Samples elapsed time before entering a global profiler spinlock | Own publication wait is excluded; outer scopes can include nested waits |
| DAG/end/host boundary | World precedes dynamic uploaders; producer fan-in precedes viewmodel/draw_done/end | Actual last prerequisite and cross-frame overlap |

Routes: `Quake/gl_rmain.c:790-835,1135-1183,1315-1345`; `renderer/Source/VulkanDevice.cpp:1404-1494,1522-1562`; `Scene.cpp:94-133`; `VertexCollector.cpp:305-429`; `RasterizedDataCollector.cpp:232-295`; `Quake/gl_vidsdl.c:574-585`; `Quake/gl_screen.c:1211-1220`. Function names are durable; recheck line anchors after changes.

Preserve the static-geometry window, world-to-dynamic dependencies and end-task join before input/commands. This plan does not authorize parallel backend recording.

| ID | Hypothesis | Supporting observation required | Falsifier / decision |
| --- | --- | --- | --- |
| C01 | Upload serialization dominates | Critical waiter intervals overlap long identified holder intervals; held copying/metadata explains the cost | Negligible wait: optimize preparation/scheduling instead |
| C02 | Opaque range imbalance dominates | Finishing slice has substantially more off-lock work/expensive models | Similar preparation but different waits: more workers do not fix serialization |
| C03 | Sky/water traversal or packing dominates | Most sky time is off-lock surface/chain work or repeated invariant preparation | Upload wait dominates: sky compute is not the main lever |
| C04 | Scalar alpha preparation is the tail | Alpha is last with substantial independent preparation | Earlier finish or mostly waiting: parallel alpha is unjustified |
| C05 | Profiler/scheduling inflates timings | Publication waits or descheduling explain the excess | Low overhead and prompt starts: investigate elsewhere |
| C06 | Host/end task sets final cadence | Timeline identifies host preparation/previous-end completion outside producer savings | Producer tail dominates: focus this lane and hand off excluded backend work |

Compare Fuma and the faster scene by workload and critical-path components at identical settings, not by a generic “complex map” label.

## 5. Phase P0: freeze scenes and validate 4K Balanced Medium

1. Obtain an exact save for the screenshot location and identify the fast reference. Record hashes, map/assets, camera/FOV and gameplay state.
2. Freeze producer control; inspect what the other lane actually integrated. Old executable versus their new executable is not an isolated producer comparison.
3. Record Debug source/receipt/working changes, executable/pack hashes, hardware/driver, output/internal dimensions, executed upscaler, enabled clouds/quality and task gates.
4. Plan a dedicated Medium validation profile. Existing `qr_audit_max.cfg:24-25` requests Low and `run_stress.ps1:345-348` validates quality 0; merely overriding Medium would fail it. Preserve the historical profile's meaning.
5. Require complete samples, advancing gameplay, zero focus losses and unchanged identities. Ray counters do not establish resolution. Upscaler fallback must not masquerade as successful FSR (`NvrhiFrameSkeleton.cpp:1455-1496`).
6. Capture exact Fuma and the faster scene separately, then Fuma motion/combat and AD/Bogbottom regressions. Quality/culling ablations may diagnose, not serve as acceptance.

**Deliverable:** reproducible scene/configuration inventory, baseline frame/GPU/colour ledger and explicit gaps. Do not optimize an unmatched screenshot.

## 6. Phase P1: minimal causal diagnostics

Proposed design only; separate instrumentation from optimization branches.

### Task timeline and workload

- Preallocate bounded frame-serial-owned records for six opaque slices, alpha, sky subphases, draw completion and end task. Include map generation and worker/thread identity.
- Distinguish dependency-ready/enqueue, execution claim, start and finish. Initial submission is not dependency readiness; instrument queue release only if scheduling delay is material.
- Count scanned/rejected/dispatched entities by type, examined/accepted surfaces, batches/split reasons, pose vertices, geometry/raster calls and attempted/accepted bytes.
- Separate sky geometry, world water and animation. Add relevant chain/animation, packing and nonexcluded light/style preparation subphases where needed.
- Keep bounded IDs/capped heavy-model summaries, not unlimited per-surface tracing or pointers retained beyond lifetime.

### Upload wait and hold

- Use private scoped producer/slice attribution restored on every exit; unknown/GUI/world callers retain an explicit unattributed label.
- Timestamp request/acquisition/release around the existing lock scope, preserving validation, early returns and exception behavior. Include every mutex user when identifying blockers.
- Split held work into validation/ID/material lookup, bounds, vertex/index/transform copies, metadata/history and raster/proxy processing. No per-surface allocation, logging or profiler calls inside the geometry lock.
- Keep capped holder-interval traces with overflow reporting. A waiter can span several holders; do not infer ownership from a racy live-owner variable.
- Measure profiler publication wait separately. Optional task-boundary thread-CPU time distinguishes computation from descheduling; unexplained wall-time residual is not automatically compute.

### Lifetime and overhead

Each task owns its record while executing. Main reads only after the applicable joins; end data retains its original frame serial. Do not reset storage while a prior end task owns it. Measure instrumentation on/off overhead and reduce detail if it changes the bottleneck. Publish bounded data after completion, not CSV/logs in the measured hot path.

**Deliverable:** per-frame last-finisher, ready-to-start delay, preparation, upload wait/hold, blocker lane, call/byte distribution and host/end overlap. Report distributions, not just maxima/summed slots. Include missing attribution, overflow and clock limits.

## 7. Evidence-selected optimization candidates

### O1: reduce redundant sky/water preparation

If C03 holds, evaluate immutable per-model sky-surface enumeration or cached local polygon/index data with transform/material/map-generation invalidation. Brush-entity sky traversal scans surfaces (`Quake/gl_sky.c:1095-1136`). Water/animated preparation may reuse invariant local data while preserving warp/material animation.

Batch merging already exists: check effective settings and genuine state/capacity splits before adding a second system. Recursive sky clipping is compiled out in this build, so it is not an established target. Do not change sky/water content or omit RT contributors.

**Gate/tests:** reduce examined/repacked work and critical-path time; verify geometry, animation, materials, lights/reflections, motion, reload and both task modes.

### O2: balance opaque preparation

If C02 holds, evaluate measured-cost weighted contiguous ranges within existing contexts before adding slices. Partition only after the visible list is final. Preserve setup side effects, exactly-once dispatch, context/chain ownership, capacities and dependencies. Invalidate cached estimates on model/topology changes.

**Gate/tests:** shorter finishing-slice tails without greater total preparation, queue/commit overhead or lock wait. Test small/empty lists, heterogeneous models, alpha filtering and both modes. More workers cannot fix a serialized collector.

### O3: shorten alpha preparation with ordered consumption

If C04 holds, first separate rejected-entry scanning from transparent preparation. Consider a frame-owned filtered list or immutable preparation reuse before parallel submission.

Parallel private preparation plus ordered commit is a larger candidate requiring every draw-order consumer to be audited. Raster collectors append records and the overlay consumes their order. Preserve entity/surface sequence, blending, portal/material behavior, setup state and history. Do not upload transparent records independently from workers merely to spread the load.

**Gate/tests:** shorter alpha critical path without a larger commit/join. Test intersecting transparency, moving sprites/models, material transitions and identical output/order.

### O4: reduce unnecessary held upload work

If C01 holds, select one measured cost: invariant-byte copying, immutable metadata, or genuinely private computation movable outside the lock with stable inputs and controlled publication. Bounds/material/ID handling may access shared mutable state; prove ownership first.

Atomic collector reservation, lock partitioning and producer/commit redesign are separate architectural candidates. Making counters atomic does not make shared containers/history safe. Do not remove the mutex or duplicate particle/BLAS work in `calm-eagle`.

**Gate/tests:** less held work and blocked critical-path time; no data races, capacity/ID corruption, skipped uploads or history/visual regression. Include increased scratch/commit costs, entity/model/material changes and in-flight lifetime.

## 8. Verification and stopping rules

- Current phase is documentation only. Implementation/runs await the next authorized phase.
- Later work uses one candidate per branch and one writer per shared file with a bounded edit/check/repair loop. No code comments unless explicitly requested.
- Build only through `.\build_win.ps1 Debug -Tests`; execute applicable CTest tests, not merely compile them. No Release/RelWithDebInfo line is authorized here.
- Compare candidate/control with `r_tasks 1` on both arms. Use safe same-binary toggles or separately frozen matched builds. Serial parity is a different comparison.
- Begin with at least two alternating valid repeats per arm/scene, 8-second warmup/6-second capture; then extend gameplay/transitions. Guarded invocations are at most 300 seconds and one save/preset. No concurrent game/build campaign.
- Use official analyzer percentiles; retain all valid elevated samples. Classify infrastructure failures by evidence, not preference for faster results.
- Check collisions/capacity/overflow, worker changes, cold/hot caches, enable/disable, map/entity/material invalidation, movement, transparency and end-task lifetime as applicable.
- Report frame mean/p95/p99/max, FPS, GPU snapshots, scene correctness, workload and task/lock/profiler attribution. Worker sums are mechanism evidence, not frame savings.
- Stop on correctness failures or unexplained frame/tail/GPU regressions. A smaller internal timer without frame benefit is mechanism-only. Keep inconclusive results instead of stacking candidates.
- All-green is a measured obligation, not permission to recolour or hide the load. Green producers alone are partial progress; full-frame/GPU/excluded-lane failures remain explicit.

## 9. Required report and readiness checklist

| Scene/settings | Source/exe/assets | Task/slice | Work quantities | Preparation | Upload wait/hold | Scheduler/profiler wait | Critical-path role | Frame mean/p95/p99 | GPU | Colour ledger | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Exact Fuma scene / 4K Balanced Medium | Pending freeze | Six slices, alpha, sky/water/animation | Pending | Not measured | Not measured | Not measured | Not established | Not measured | Not measured | Unmet/unverified | Baseline needed |
| Named faster scene / identical settings | Pending identity | Same breakdown | Pending | Not measured | Not measured | Not measured | Not established | Not measured | Not measured | Unverified | Contrast needed |

Final explanation must identify the actual finishing task, whether it computes or waits, its blocker, and which Fuma workload differs from the fast scene. Preserve host/end/GPU limitations. Classify accepted, rejected, mechanism-only and inconclusive candidates separately.

- [ ] Operator supplies exact Fuma save and faster reference scene.
- [ ] Actual 4K/FSR execution/clouds Medium settings are verified; Low evidence is not promoted.
- [ ] Shared interfaces/file ownership are coordinated with `calm-eagle`.
- [ ] Bounded frame/task diagnostics and overhead are reviewed before implementation.
- [ ] Frame-correlated attribution identifies computation versus blocking and the last-finisher.
- [ ] One conditional candidate is selected from evidence, not screenshot maxima.
- [ ] Debug tests and visual/gameplay/history checks pass in both modes.
- [ ] Frame-budget and literal colour ledgers state failures and coverage honestly.
- [ ] Broader map/encounter/transition coverage supports any wider claim.

## Planning evidence limits

Four complementary read-only investigations and four blind verifications covered producer paths, frame accounting, competing bottlenecks and target reproducibility. QuakeRay MCP status/architecture/scoped symbols and available call edges were checked; busy/partial results were supplemented with bounded reads of the same source. Empty edges were not proof of absent dependencies. No screenshot-only diagnosis is certified as a measured cause. Fresh final document review follows source/link checks; no executable performance result exists for this plan.
