# Multithreading

How QuakeRay runs the CPU part of a frame on worker threads, and the rules for building on it.
Read [ARCHITECTURE.md](../ARCHITECTURE.md) first for subsystem routes; measured numbers live in
[PERFORMANCE.md](../PERFORMANCE.md).

All agents must follow the [mandatory engine-MCP workflow](../AGENTS.md#mandatory-engine-mcp-workflow).
Use the engine MCP to establish the current source, implicated callers/callees and retained evidence
before adapting a producer; document any unavailable tooling or incomplete coverage.

## Status and scope

- This is a reusable **CPU task-graph multithreading platform**: the existing worker pool and
  scalar/indexed task API are integrated with render producers, dependency ordering, data-ownership
  rules, synchronized upload channels and frame-aware profiling. Other CPU work can reuse it only
  after satisfying the same ownership and join contract.
- It is not a claim that the whole engine is parallelized or that arbitrary code is worker-safe.
  Input, command execution and server/client simulation remain on the main-thread host path.
  Backend recording/submission is still an ordered end-frame operation; this platform does not
  authorize concurrent NVRHI command-list recording or bypass existing renderer locks.
- Swapchain acquisition is a separate task after begin and may overlap the current frame's CPU
  producers. The end task records the scene after `draw_done`, then joins acquire at the present
  boundary. `FrameState` still has one active frame index, so this is not full two-frame
  producer/backend overlap.
- The render frame always runs as a dependency graph of tasks. The `r_tasks` cvar and its serial
  fork were retired on 2026-10-14 (`changelog.md`); [SCR_UpdateScreen](../Quake/gl_screen.c#L1146)'s
  flag now only selects whether an out-of-band refresh joins the frame's end task before returning,
  while the host frame keeps deferring the end task into the next begin.
- With `r_parallelmark 1` the world texture chains are rebuilt in surface-index order instead of the
  scalar leaf first-touch order (owner-accepted 2026-10-11); at `0` the scalar `R_MarkVisSurfaces`
  runs as a single task. The code-level guarantee is predicate equivalence, not an identical chain:
  both paths chain exactly the surfaces that pass the same PVS/backface/sky tests, and the parallel
  path only reorders the chaining within one texture. The retained audit captures match in the modal/mean `calls_geometry` (not frame-exact), and the
  water-shot on/off provenance was not recorded (its fixtures were deleted), so those captures are
  not evidence of an identical chained set. Two shared dummy textures keep `rt_chain_index == 0` and
  are gathered into one surface bucket; this is benign because the slice chains each surface to its
  own `texinfo->texture`, which it re-reads. The order change is safe only while every head-of-chain
  flag (`SURF_DRAWTURB`, `SURF_DRAWTILED`, `SURF_DRAWSKY`, `SURF_NOTEXTURE`, `SURF_DRAWFENCE`) stays
  uniform across all surfaces of one texture; if a future change makes such a flag per-texinfo,
  the per-texture slicing must be revisited because batch grouping would stop being
  order-invariant.
- `cpu.wait_ms` is the main thread's join wait, not idle time.

## The task system

`../Quake/tasks.c` provides a fixed worker pool (`CLAMP (1, SDL_GetCPUCount (), 32)` workers),
function tasks, indexed tasks (each index claimed exactly once), and dependencies between
submitted tasks. When adding work:

- allocate with `Task_AllocateAndAssignFunc` or `Task_AllocateAndAssignIndexedFunc` (the payload is
  copied and limited to 32 bytes; pointed-to buffers are not copied and must outlive their consumers);
- add every dependency **before** the task is submitted — `Task_AddDependency` is epoch-checked and
  silently drops an edge to an already-finished task, which can hide a missing order;
- keep the dependents of any one task (its fan-out; `num_dependents` in `Task_AddDependency`)
  at no more than `MAX_DEPENDENT_TASKS` (16);
- keep the graph bounded: task storage/queues are fixed-size, so do not allocate an unbounded task
  per object before submission; partition long lists with indexed work instead;
- submit each task once, and join from a non-worker thread (`Task_Join`, typically with
  `SDL_MUTEX_MAXWAIT`; `Tasks_IsWorker()` tells the caller whether it may be a worker).

## The render frame graph

Entry: [SCR_UpdateScreen](../Quake/gl_screen.c#L1146) builds the task graph, builds the render graph
in [R_RenderView](../Quake/gl_rmain.c#L1334), and joins `draw_done` while pumping extra audio updates.

| Edge | Meaning |
| --- | --- |
| `prev_end_rendering_task -> begin_rendering_task`, `prev_acquire_rendering_task -> begin_rendering_task` | a new frame never reuses swapchain/gate state before the previous frame's end and acquire tasks finished |
| `begin_rendering_task -> setup_frame_task`, `setup_frame_task -> before_mark`, `begin_rendering_task -> before_mark` | frame start (`qrStartFrame`: frame fence, slot preparation, scene collector reset, light-registry frame preparation) happens before the setup pass, and the setup pass before everything that reads it; `qrStartFrame` does not acquire the swapchain image |
| `begin_rendering_task -> acquire_rendering_task` | acquisition starts only after the frame slot is prepared and advances in parallel with the setup/producer branch |
| `acquire_rendering_task -> qrDrawFrame present boundary -> end_rendering_task` | no task-graph edge blocks end at `draw_done`; the end task records scene commands, then `FrameAcquireGate` joins the already-running acquire and pending pre-frame submission before final present recording |
| `before_mark -> prepare_mark -> mark_leafs`, then `mark_leafs -> store_efrags` and `mark_leafs -> chain_slices -> chain_finish` (`r_parallelmark 1`; `cull_surfaces` aliases `mark_leafs`; with `r_parallelmark 0` one scalar `mark_surfaces` serves all three handles) | the efrag store and the visibility chain run after the view is set up; `R_MarkSurfaces` decides `rt_mark_parallel_frame` and `rt_chain_full_frame` and writes both **before** `Task_Submit(prepare_mark)`, `prepare_mark` clears the world texture chains and (on the parallel path) zeroes the shared `surfvis` bitmask, `mark_leafs` fills `surfvis` with atomic word runs, `chain_slices` rebuilds the world texture chains per texture in parallel (in steady frames only for the consumed set - the textures whose chains the sky, water and animated passes read; the full set when a static-submit or world-light-recollect window is open or `r_showtris` is on) and `chain_finish` publishes the chain totals |
| `store_efrags -> draw_sky_and_water_task`, `chain_surfaces -> draw_sky_and_water_task`, `store_efrags -> draw_entities_task`, `-> draw_alpha_entities_task` | the sky/water producer runs after the efrag pass and the rebuilt chains; the entity drawers run after the efrag pass added the entities to the frame's visible list |
| `chain_surfaces -> draw_world_task` | the visibility output feeds the world task |
| `draw_world_task -> draw_sky_and_water_task`, `-> draw_entities_task`, `-> draw_alpha_entities_task`, `-> draw_particles_task` | every dynamic uploader runs after the world task closed the static-geometry window (see below) |
| `draw_world_task, draw_sky_and_water_task, draw_entities_task, draw_alpha_entities_task, draw_particles_task -> draw_view_model_task` | the viewmodel producer runs last among the producers |
| `draw_view_model_task -> cluster_prepare_task` | the cluster pipeline starts after the last registration: the registry is complete once the viewmodel task's light uploads have run |
| `cluster_prepare_task -> cluster_topup_task` (indexed, `RT_CLUSTER_SLICES` = 16) `-> cluster_finish_task -> cluster_publish_task` (indexed, `RT_CLUSTER_SLICES`) `-> cluster_commit_task` | the per-cluster top-up runs in slices over the frame's work list; the finish merges the slices, fills the lists, rebuilds the tails and prepares the publication; the publish slices resolve the list words in parallel and the commit writes the tails and the publication bookkeeping |
| `cluster_commit_task -> draw_done_task` | the publication and the cluster stats readback are complete before the join |
| `draw_gui_task -> draw_done_task`, producers `-> draw_done_task` | the GUI and every producer feed the join point |
| `draw_done_task -> end_rendering_task`, `acquire_rendering_task -> qrDrawFrame present boundary -> end_rendering_task` | the end task waits for the producer/GUI branch, performs ordered scene recording, then joins acquisition before final present recording |

`GL_AcquireRenderingTask` calls `qrAcquireFrame`; `VulkanDevice::AcquireFrame` waits for the
waitable-FIFO present dependency, submits the pending pre-frame command through the
`CommandBufferManager` slot that owned it, and then installs the main frame wait semaphore.
`FrameAcquireGate` is an exactly-once, failure-propagating barrier between that worker call and the
end task. `qrDrawFrame` still acquires lazily when a host does not call `qrAcquireFrame`. Acquisition
overlaps the current frame's CPU producers and scene command recording; only final present recording
waits for the image, and this is not another frame's backend overlap.

`draw_entities_task` is indexed with `NUM_ENTITIES_CBX` (6) slices, so several workers draw entity
ranges in parallel. `cluster_topup_task` is indexed with `RT_CLUSTER_SLICES` (16) slices over
`dirtyClusters` (incremental frames) or clusters `1..numClusters-1` (compose frames); every cluster
is processed by exactly one slice, and the slice counters and tail-dirty marks are merged in
`cluster_finish_task`; that task also prepares the list publication. `cluster_publish_task` is
indexed with `RT_CLUSTER_SLICES` (16) slices over the publication's word range (a disjoint
`floor(w*s/16)..floor(w*(s+1)/16)` interval each) and `cluster_commit_task` writes the tails and
the publication bookkeeping. The serial branch runs the same prepare/slice/finish/publish/commit
stages inline with one slice.

**Static-geometry window.** `R_DrawWorldTask` calls `qrBeginStaticGeometries` … `qrSubmitStaticGeometries`
(`gl_rmain.c#L1138`/`#L1150`). Inside that window only `QR_GEOMETRY_TYPE_STATIC` and
`STATIC_MOVABLE` uploads are accepted; `Scene::Upload` throws `QR_WRONG_FUNCTION_CALL` for dynamic
geometry. Order every dynamic uploader after `draw_world_task`, and keep static uploads inside the
world task.

**The end-task boundary.** `GL_EndRenderingTask` is the one producer that outlives its frame: the
main thread joins `draw_done` inside `SCR_UpdateScreen`, then continues into the host tail while the
end task still runs. `SCR_DrawDone` copies `QrDrawFrameInfo`, every pointed-to parameter/post-effect
value and the nested referenced values into an immutable heap-owned snapshot after `setup_frame`,
viewmodel and every producer has completed. It must not move earlier: `V_CalcRefdef` and
`R_SetupMatrices` run on that branch, and capturing before them pairs a stale camera matrix with the
current viewmodel transform. The end task's draw inputs come only from that snapshot, never host
stack values, cvars or `cl.*` globals, and it frees the snapshot after `qrDrawFrame` and profiler
recording. That removes the end task's host-global read dependency, but
`_Host_Frame` still joins it with `GL_SynchronizeEndRenderingTask()` before handling input:
`previous-end -> begin`, `previous-acquire -> begin`, host input synchronization and ordered
backend/frame-state lifetime remain requirements. The synchronizer joins the end first, then the
acquire; normally the end's present-boundary gate has already joined it, and the second join covers
early renderer exits. Debug builds check the boundary: the end task raises `rt_end_task_running` and
the input phase asserts it is clear, and `rt_end_task_delay_ms` stretches the task to exercise the
check.
The task's profiler results carry a frame serial and are merged into the sample of their own frame
(`RT_Prof_EndTaskRecord`). The shared production component [rt_prof_window.h](../shared/rt_prof_window.h)
owns the reporting-window lifecycle. `RT_Prof_FrameStart` joins any previous end task before applying
enable/disable changes, so the first captured frame cannot be reset after submission. An end-result
callback writes the slot sums, renderer samples and benchmark data under the profiler lock; only
then does the component release its pending count, in the same critical section. Publication and
accumulator clearing also share one critical section. A window must have closed frames, no open
frame and no outstanding end result to publish. Both the host-start join and the screen-end update
can publish; the latter remains deferred while the current end task is running.

[prof_window_tests.cpp](../tests/prof_window_tests.cpp) exercises this same component with real
threads and a paused result writer, both completion orders, multi-frame windows, and disable/re-enable
transitions. It checks payloads, denominators and exactly-once publication, not just a counter predicate.

## Worker rules

The contract for work added to the graph:

- Put producer buffers in task-owned or thread-local storage; do not add new shared mutable state
  without a lock or an ordering edge. The shared channels the graph relies on are listed below.
- Tasks inside the frame's draw graph are joined before the main thread executes the next command
  buffer, so their reads of cvars and `cl.*` are ordered by that join. The end task is the
  exception, kept in order by its own boundary (see above); do not extend a task past the join.
- Call renderer `qr*` entry points only through the synchronized ones listed below, and follow the
  static-geometry window rule for uploads.
- Touch profiler/bench state only through the `RT_Prof_*` helpers: per-invocation slots accumulate
  into a thread-local buffer, and the per-frame merge and the window publication/reset run under
  the profiler lock. The cluster statistics are written by the cluster prepare/slice/finish chain
  (the finish task runs on a worker) and read on the main thread only after the frame's join. The
  DTAL LOD selection, its effective-representation pass and the combined upload arrays follow the
  same rule: they are written inside the viewmodel task's light stage and read by the light report
  only after the frame's join.
- `Mem_*` uses mimalloc, but allocator thread safety does not make its owner/container safe.
  Audit allocation, publication, reallocation and freeing together; do not assume hunk/zone helpers
  are interchangeable worker allocators. The console's buffer lock likewise does not authorize
  arbitrary worker-side command, cvar, UI or logging mutations. Avoid per-object allocation/printing.

The graph currently relies on:

- `VulkanDevice::geometryUploadMutex` in [VulkanDevice.h](../renderer/Source/VulkanDevice.h#L370) —
  held by `UploadGeometry`, `UploadRasterizedGeometry`, `UpdateGeometryTransform`,
  `UpdateGeometryTexCoords`, `UploadParticles`, `SetParticleProxyGate`, `UploadDecal`, `UploadPortal`,
  `SubmitStaticGeometries`, `StartNewStaticScene`; the renderer's API counters are `std::atomic`;
- `LightManager::registryMutex` — held by `AddLight`, `PrepareForFrame` and `Reset`
  ([LightManager.h](../renderer/Source/LightManager.h#L173));
- the profiler spinlock in [gl_vidsdl.c](../Quake/gl_vidsdl.c#L533) — the once-per-frame TLS merge,
  the window publication/reset and `RT_Bench_Report`'s snapshot; per-invocation accumulation runs
  into a thread-local buffer without the lock (a thread claims its pool slot once);
- the parallel-mark state in [r_world.c](../Quake/r_world.c#L418): `rt_mark_parallel_frame` is
  decided and written by the submitting thread before `Task_Submit(prepare_mark)`, which is what
  lets `R_MarkSurfacesPrepare` read it without a lock; `rt_mark_parallel_built` is fixed at map
  load. The same pre-submit path writes `rt_chain_full_frame` (full chains when
  `rt_require_static_submit` or `rt_require_world_light_recollect` is set, or `r_showtris` is on),
  and the `rt_chain_textures`/`rt_chain_consumed` arrays built in `RT_MarkParallel_NewMap` hold the
  steady-state consumed-texture subset the chain slices walk. `surfvis` (allocated with the world
  model in [r_brush.c](../Quake/r_brush.c#L772), freed in
  `Mod_FreeModelMemory`) is written by atomic word runs in the chunked marker and read by the chain
  slices after the `mark_leafs` edge; the `rt_mark_tex_start`/`rt_mark_tex_surfaces`/
  `rt_mark_tex_polys` arrays are built once in `RT_MarkParallel_NewMap`, each chain slice writes its
  own `rt_mark_tex_polys[index]` and `chain_finish` merges them;
- thread-local scratch: the alias pose buffer (`tempstorage` in
  [r_alias.c](../Quake/r_alias.c#L135)) and the fan/scratch buffers in
  [gl_heap.c](../Quake/gl_heap.c#L42);
- thread-local caches with a reset generation: the surface-pack entity cache
  ([r_world.c](../Quake/r_world.c#L980)) and the brush-cluster cache
  ([r_world.c](../Quake/r_world.c#L4702)). `RT_SurfacePacksReset` / `RT_BrushClusterCacheReset` bump
  the generation at map load and each thread clears its own table on first use. The buried-answer
  DTAL cache follows the same ownership rule through `RT_BuriedCacheReset`, and so does the
  point-cluster resolve cache ([gl_rlight.c](../Quake/gl_rlight.c#L1114)): one slot table per thread
  with the map identity kept in the table, and per-thread hit/miss/nanosecond counters that
  `RT_PointClusterCacheStats` sums. Its readers all run on the main thread after the frame's join
  (`RT_Prof_FrameStart`, `RT_Prof_FrameEnd`, `RT_Prof_Update`), which is what keeps the unlocked sum
  correct;
- the profiler/upload-trace thread-local slots: one `rt_prof_tls_pool` accumulator per thread
  ([gl_vidsdl.c](../Quake/gl_vidsdl.c#L510)) and the upload-lane attribution stack
  (`g_uploadLane`/`g_uploadLaneStack` in [VulkanDevice.cpp](../renderer/Source/VulkanDevice.cpp#L1479));
  the wait/hold counters they feed are atomic device counters, read once per frame in
  `RT_Prof_FrameEnd`;
- the emissive skip-table and watch-table spinlock (`RT_EmisSkipLock`) — covers skip
  lookup/insertion and reset, the watch-table lookup and filter reset, and the non-atomic
  per-watch style fields (`min_style_scale`, `styles`); the per-watch counters are atomic, and
  the report snapshots the skip table under the lock before printing;
- the styled-light index (`RT_BuildStyledLightIndex`) — built on the single-threaded map-load path
  right after `RT_ParseElights`, only read by draw tasks afterwards;
- the colour cache (`rt_colors`) — refreshed by `RT_ColorsRefresh` on the main thread in
  `R_RenderView`, workers only read it.

When you add a producer that runs on a worker, ensure its shared writer/reader pairs are ordered by
a dependency edge or protected by a lock. Preserve scene content, stable IDs, transforms, material
flags and renderer semantics in both task and serial modes; parallel submission order need not be
byte-identical, and FPS alone does not establish visual equivalence.

## Agent checklist: adapting an existing solution

Apply this to optimizations and bug fixes as well as new tasks. A solution that works only in the
serial branch is not ready for integration.

1. **Locate and measure.** Use MCP to identify the producer, its caller/callee and consumers.
   Select the matching baseline from PERFORMANCE.md and verify binary/assets/settings identity.
   Do not move work to threads merely because it looks expensive; measure the critical path.
2. **Write the ownership map.** For every input, output and cache, identify its writer, readers,
   execution phase and lifetime. Include function/file statics, lazy parsing, cache misses,
   diagnostic tables, counters, map/material invalidation and teardown, not just the main loop.
   Each shared writer/reader pair needs an ordering edge, a single owner or complete lock coverage.
3. **Choose the execution model.** Keep pure preparation independent from renderer upload/commit
   where possible. Use task-owned/per-index outputs or thread-local scratch. Distinct indexed
   slices must not mutate the same model/cache entry without protection. Thread-local caches need
   reset generations; shared caches need protected lookup, computation/publication and invalidation.
   Do not create a separate worker pool or parallel backend recording path without a design review.
4. **Wire the task graph.** Put the shared kernel in the appropriate existing producer, or add a
   producer to `R_RenderView`. Register dependencies before submission. Require
   setup/visibility/entity-list readiness for the data read; require `draw_world_task` completion
   before dynamic uploads. Join CPU-producer outputs before `draw_done_task`; keep acquisition on its
   own `begin -> acquire -> end` branch and keep backend recording behind both branches. Some
   visibility handles alias each other outside parallel-mark mode; do not assume they always name
   distinct tasks or add self-edges.
5. **Respect frame lifetime.** A copied task payload or outer `QrDrawFrameInfo` does not extend
   pointed-to stack or temporary data. Work that outlives the producer graph must own every nested
   value and have an explicit synchronization contract; `SCR_DrawDone`'s capture and the end task's
   heap-snapshot ownership are the example. Keep the end-task join before input, menus and commands. Map changes, resource
   rebuilding and freeing must wait for the tasks that still reference the old generation. Do not
   join from a worker or call main-thread device-idle helpers there.
6. **Preserve instrumentation.** Use `RT_Prof_*` and the shared window component, not direct writes
   or a new ad hoc pending counter. Result data and completion acknowledgement are one locked
   transaction; enable/reset precedes submission. Compare interval/FPS between modes, not summed
   worker slot times. Update ARCHITECTURE.md/PERFORMANCE.md when boundaries or counters change.
7. **Verify the final revision.** Build through `build_win.ps1 Debug -Tests`, run the relevant tests,
   and compare matched captures against the task-frame baseline (the serial fork was retired on
   2026-10-14; its bisection baseline no longer exists). Check visual/scene correctness as
   well as timing. Exercise applicable collisions, capacity limits, invalidation, map transitions,
   cold enable/disable and early/late completion. Test the production kernel/state machine with
   real payloads and controlled interleavings, not only predicates or manually simulated counters.
8. **Hand off evidence.** Record source/binary identity, MCP inspections and gaps, the ownership map,
   new DAG edges, tests and capture identities. Separate observed gains from hypotheses and state
   untested cases. The task frame is the engine's only render path since 2026-10-14.

## Measurement

- The task frame is the only render path; no override selects it.
- Use `-StatsLevel 3 -EnableStatsAfterWarmup` to enable profiling in an already loaded game.
  Combine it with `-Overrides @('rt_end_task_delay_ms 30')` for delayed-end activation.
  Window dumps expose `cpu.window_id`, `cpu.window_frames` and `cpu.renderer_samples`; the runner
  requires multiple advancing windows, nonzero draw-frame averages and matching frame/result counts.
- Task mode changes the meaning of the CPU counters: `cpu.wait_ms` is the join wait, and per-slot
  sums accumulate worker wall times across parallel tasks — they can exceed the frame interval and
  are **not** comparable to serial slot values. Use the frame interval/FPS for on/off acceptance.
- Paired Debug captures (same binary, 8 s warmup + 6 s capture): FSR Balanced — Fuma
  28.49 → 20.00 ms, Bogbottom save 39.48 → 29.76 ms, AD hub 22.41 → 21.01 ms; FSR Quality — Fuma
  27.84 → 23.97 ms, Bogbottom fresh spawn 44.06 → 35.93 ms, AD hub 24.28 → 24.03 ms. The frame is
  still CPU-bound at 4K: the GPU time scales with the FSR mode (Bogbottom fresh spawn
  26.9 → 15.0 ms from Quality to Ultra) while the graph's wall time stays at ~31–34 ms. The
  Debug-era reading is kept for provenance; on the 2026-10-14 Release binary the acceptance
  profile is GPU-bound instead (the interval tracks the GPU snapshot down to a 1280x720 render).
  See PERFORMANCE.md for the full matrix, this scaling fit and the identities.

## Roadmap to production-ready whole-frame multithreading

The current reusable task platform is the starting point, not completion of this roadmap. A mature
multithreaded engine does not run every function on a worker: input, UI/window ownership, ordered
QuakeC execution and final submission may retain a single owner. The goal is to put independent,
material CPU work on tasks and make every remaining serial boundary explicit, safe and measured.
The milestones below are **open acceptance goals**, not claims about implemented or tested features.

| Milestone | Work required | Completion evidence |
| --- | --- | --- |
| M1. Complete render-path ownership | Audit all producer inputs, lazy caches, diagnostics, mutable model/material state, resource rebuilds and teardown, including optional paths. Preserve serial rendering, stable IDs, scene content and temporal history. | An ownership map for every shared writer/reader pair, production-code concurrency/invalidation regressions, and serial/tasks correctness comparisons across the supported render modes. No unresolved threading-safety findings in the changed paths. |
| M2. Remove measured serialization bottlenecks | Profile task imbalance, upload-lock contention, main-thread joins and ordered backend preparation. Split costly independent geometry/pose/light preparation into task-owned work and a controlled commit where justified. Parallel backend recording is a separate reviewed design, not an assumption. | Same-binary Debug captures and task/lock timings show reduced CPU critical-path time without moving cost into a larger join or another global lock. Remaining serial stages have a documented owner and measured budget. |
| M3. Extend beyond render producers | Evaluate client interpolation/relink preparation, classic/smoke/scripted-particle simulation and independent server/physics calculations. Use immutable tick/frame inputs and deterministic result commit. Keep mutable QuakeC/gameplay state ordered unless a separate safe design exists. | Each materially expensive host phase is either integrated into the task platform or has a justified measured serial boundary. Gameplay, particle lifecycle, networking and tick/frame-rate separation remain correct under parallel preparation. |
| M4. Harden the task runtime | Add deterministic coverage for empty indexed ranges, queue/pool capacity, handle reuse, dependency ordering/cycles, slow tasks and low-core/single-worker operation. Add diagnosable stall/watchdog behavior and define safe shutdown/timeout ownership. | Automated runtime tests and stress runs complete without deadlock, stale handles or capacity corruption. Stalls identify the blocked task/dependency; a timeout never permits freeing data still used by a worker. |
| M5. Establish production validation and default-on readiness | Maintain a scene/transition matrix for supported Quake content, mission packs and Arcane Dimensions. Include representative busy scenes, repeated sessions, map/save/restart/disconnect, menu/editor/photo modes, resize/fullscreen/minimize, profiling toggles and available core counts. | Final-revision Debug build/CTest results, repeated serial/tasks captures, visual/gameplay checks, memory/race diagnostics where supported, and an explicit review of coverage gaps. No unexplained correctness failures or material regressions in the maintained matrix. |
| M6. Meet the performance program | On the declared reference hardware, target at least 45 FPS at 3840x2160 FSR Quality throughout the maintained loaded-gameplay scene matrix. The mean and p95 frame intervals must be at most 22.22 ms; also report p99/max and hitches. Remove the CPU ceiling so lowering FSR internal resolution improves the frame interval instead of leaving it flat. | Matched captures for every maintained scene/preset meet the budget and demonstrate resolution-driven scaling. CPU critical-path and GPU timings are checked independently: GPU work above 22.22 ms requires GPU optimization too, not more CPU threads. Fixed-cost work and deviations from proportional scaling are reported, not hidden. |

**Two release decisions, not one:** M1, M2, M4 and M5 gated the default-on *render* task path; the
owner authorized that default on 2026-10-11 (`changelog.md`), and the serial fallback was retired
outright on 2026-10-14, so the M5 scene/transition matrix runs on the single task path. Broader
whole-frame CPU task parallelism additionally requires M3. M6
is the separate performance target; meeting one FPS number does not certify thread safety, and a
GPU bottleneck does not make the task platform incomplete. Neither one short capture nor the
historical ~40% uplift closes these gates.

Every milestone handoff must include MCP source/evidence identities, the changed ownership/DAG
boundaries, exact tests/captures, measured deltas and remaining gaps. Keep the milestone open until
its completion evidence exists; do not mark it done merely because tasks were added.

## Known limitations

- The serial fork is retired (2026-10-14): the engine always runs the multithreaded frame, and
  `r_parallelmark 0` is the remaining serial-vs-chunked marker choice for world marking.
- `r_gpulightmapupdate` now only selects the non-interpolated lightstyle path in `R_AnimateLight`;
  the GPU lightmap update task and the `lightmaps[].modified` field were removed in the Tier A port
  (see H14 in `docs/upstream-porting-synthesis.md`).
- No worker watchdog: a hung task hangs the join loop; the join reports the `wait` slot so a
  persistent value is visible in captures.
