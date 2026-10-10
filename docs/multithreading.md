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
- The render frame runs as a dependency graph of tasks when `r_tasks 1` — the switch is the
  `use_tasks` condition in [SCR_UpdateScreen](../Quake/gl_screen.c#L1162); `r_tasks` defaults to
  `0` until the broader stability validation flips it.
- In the serial path (`r_tasks 0`) the same producers run in fixed order on the main thread; the
  frame contents and the NVRHI ray-tracing renderer are identical in both modes.
- `cpu.wait_ms` is nonzero only in task mode: it is the main thread's join wait, not idle time.

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

Entry: [SCR_UpdateScreen](../Quake/gl_screen.c#L1149) takes the tasks branch when the condition
holds, builds the frame in [R_RenderView](../Quake/gl_rmain.c#L1281), and joins `draw_done` while
pumping extra audio updates.

| Edge | Meaning |
| --- | --- |
| `prev_end_rendering_task -> begin_rendering_task` | a new frame never reuses swapchain/frame state before the previous frame's end task finished |
| `begin_rendering_task -> setup_frame_task`, `setup_frame_task -> before_mark`, `begin_rendering_task -> before_mark` | frame start (`qrStartFrame`: fence/acquire, scene collector reset, light-registry frame preparation) happens before the setup pass, and the setup pass before everything that reads it |
| `before_mark -> prepare_mark -> mark_surfaces`, then `mark_surfaces -> store_efrags` and `mark_surfaces -> cull_surfaces -> chain_surfaces` | the efrag store and the visibility chain run after the view is set up |
| `store_efrags -> draw_sky_and_water_task`, `-> draw_entities_task`, `-> draw_alpha_entities_task` | the producers that draw the frame's visible-entity list run after the efrag pass added the entities to it |
| `chain_surfaces -> draw_world_task` | the visibility output feeds the world task |
| `cull_surfaces -> update_lightmaps_task`, `draw_world_task -> update_lightmaps_task` | the lightmap updates run after the world surfaces are culled and drawn |
| `draw_world_task -> draw_sky_and_water_task`, `-> draw_entities_task`, `-> draw_alpha_entities_task`, `-> draw_particles_task` | every dynamic uploader runs after the world task closed the static-geometry window (see below) |
| `draw_world_task, draw_sky_and_water_task, draw_entities_task, draw_alpha_entities_task, draw_particles_task -> draw_view_model_task` | the viewmodel producer runs last among the producers |
| `draw_view_model_task -> cluster_prepare_task` | the cluster pipeline starts after the last registration: the registry is complete once the viewmodel task's light uploads have run |
| `cluster_prepare_task -> cluster_topup_task` (indexed, `RT_CLUSTER_SLICES` = 16) `-> cluster_finish_task -> cluster_publish_task` (indexed, `RT_CLUSTER_SLICES`) `-> cluster_commit_task` | the per-cluster top-up runs in slices over the frame's work list; the finish merges the slices, fills the lists, rebuilds the tails and prepares the publication; the publish slices resolve the list words in parallel and the commit writes the tails and the publication bookkeeping |
| `cluster_commit_task -> draw_done_task` | the publication and the cluster stats readback are complete before the join |
| `draw_gui_task -> draw_done_task`, producers `-> draw_done_task` | the GUI and every producer feed the join point |
| `draw_done_task -> end_rendering_task` | the renderer recording/end task runs last |

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
(`gl_rmain.c#L1107`/`#L1119`). Inside that window only `QR_GEOMETRY_TYPE_STATIC` and
`STATIC_MOVABLE` uploads are accepted; `Scene::Upload` throws `QR_WRONG_FUNCTION_CALL` for dynamic
geometry. Order every dynamic uploader after `draw_world_task`, and keep static uploads inside the
world task.

**The end-task boundary.** `GL_EndRenderingTask` is the one producer that outlives its frame: the
main thread joins `draw_done` inside `SCR_UpdateScreen`, then continues into the host tail while the
end task still runs. The task reads frame inputs that only the main thread owns (cvars, `cl.time`,
`cl.items`, `cl.stats`, the editor flags), so `_Host_Frame` joins it with
`GL_SynchronizeEndRenderingTask()` before the frame handles input: key events, the menus and
`Cbuf_Execute` must all follow the join; menu and key handlers can mutate cvars without going
through the command buffer. Do not move that join later without moving those reads into a frame-owned
snapshot. Debug builds check the boundary: the end task raises `rt_end_task_running` and the input
phase asserts it is clear, and `rt_end_task_delay_ms` stretches the task to exercise the check. The
task's profiler results carry a frame serial and are merged into the sample of their own frame
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
- Touch profiler/bench state only through the locked helpers (`RT_Prof_*`); the cluster statistics
  are written by the cluster prepare/slice/finish chain (the finish task runs on a worker) and read
  on the main thread only after the frame's join.
- `Mem_*` uses mimalloc, but allocator thread safety does not make its owner/container safe.
  Audit allocation, publication, reallocation and freeing together; do not assume hunk/zone helpers
  are interchangeable worker allocators. The console's buffer lock likewise does not authorize
  arbitrary worker-side command, cvar, UI or logging mutations. Avoid per-object allocation/printing.

The graph currently relies on:

- `VulkanDevice::geometryUploadMutex` in [VulkanDevice.cpp](../renderer/Source/VulkanDevice.cpp#L1404) —
  held by `UploadGeometry`, `UploadRasterizedGeometry`, `UpdateGeometryTransform`,
  `UpdateGeometryTexCoords`, `UploadDecal`, `UploadPortal`, `SubmitStaticGeometries`,
  `StartNewStaticScene`; the renderer's API counters are `std::atomic`;
- `LightManager::registryMutex` — held by `AddLight`, `PrepareForFrame` and `Reset`
  ([LightManager.cpp](../renderer/Source/LightManager.cpp#L351));
- the profiler spinlock in [gl_vidsdl.c](../Quake/gl_vidsdl.c#L421) — profiler/bench accumulation,
  the frame reset/copy and `RT_Bench_Report`;
- thread-local scratch: the alias pose buffer (`tempstorage` in
  [r_alias.c](../Quake/r_alias.c#L120)) and the fan/scratch buffers in
  [gl_heap.c](../Quake/gl_heap.c#L42);
- thread-local caches with a reset generation: the surface-pack entity cache
  ([r_world.c](../Quake/r_world.c#L1072)) and the brush-cluster cache
  ([r_world.c](../Quake/r_world.c#L4300)). `RT_SurfacePacksReset` / `RT_BrushClusterCacheReset` bump
  the generation at map load and each thread clears its own table on first use. The buried-answer
  DTAL cache follows the same ownership rule through `RT_BuriedCacheReset`, and so does the
  point-cluster resolve cache ([gl_rlight.c](../Quake/gl_rlight.c#L1176)): one slot table per thread
  with the map identity kept in the table, and per-thread hit/miss/nanosecond counters that
  `RT_PointClusterCacheStats` sums. Its readers all run on the main thread after the frame's join
  (`RT_Prof_FrameStart`, `RT_Prof_FrameEnd`, `RT_Prof_Update`), which is what keeps the unlocked sum
  correct;
- the emissive skip-table spinlock (`RT_EmisSkipLock`) — covers lookup/insertion, reset and report
  snapshotting; printing happens after the snapshot lock is released;
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
4. **Wire both paths.** Put the shared kernel in the appropriate existing producer, or wire the new
   producer into both branches of `R_RenderView`. Register dependencies before submission. Require
   setup/visibility/entity-list readiness for the data read; require `draw_world_task` completion
   before dynamic uploads. Join outputs before GUI/viewmodel/end-frame consumers read them, and
   include new frame work in the `draw_done_task` boundary. Some visibility handles alias each other
   outside parallel-mark mode; do not assume they always name distinct tasks or add self-edges.
5. **Respect frame lifetime.** A copied payload does not extend pointed-to stack or temporary data.
   Do not let work outlive the frame's join unless it has an explicit frame-owned snapshot and
   synchronization contract. Keep the end-task join before input, menus and commands. Map changes,
   resource rebuilding and freeing must wait for the tasks that still reference the old generation.
   Do not join from a worker or call main-thread device-idle helpers there.
6. **Preserve instrumentation.** Use `RT_Prof_*` and the shared window component, not direct writes
   or a new ad hoc pending counter. Result data and completion acknowledgement are one locked
   transaction; enable/reset precedes submission. Compare interval/FPS between modes, not summed
   worker slot times. Update ARCHITECTURE.md/PERFORMANCE.md when boundaries or counters change.
7. **Verify the final revision.** Build through `build_win.ps1 Debug -Tests`, run the relevant tests,
   and compare the same binary with `r_tasks 0` and `r_tasks 1`. Check visual/scene correctness as
   well as timing. Exercise applicable collisions, capacity limits, invalidation, map transitions,
   cold enable/disable and early/late completion. Test the production kernel/state machine with
   real payloads and controlled interleavings, not only predicates or manually simulated counters.
8. **Hand off evidence.** Record source/binary identity, MCP inspections and gaps, the ownership map,
   new DAG edges, tests and capture identities. Separate observed gains from hypotheses; state
   untested cases and keep `r_tasks` opt-in until broader validation explicitly authorizes a change.

## Measurement

- Enable with `r_tasks 1`; the official runner takes it as
  `-Overrides @('r_tasks 1')` (`tests/perf/run_stress.ps1`).
- Use `-StatsLevel 3 -EnableStatsAfterWarmup` to enable profiling in an already loaded game.
  Combine it with `-Overrides @('r_tasks 1', 'rt_end_task_delay_ms 30')` for delayed-end activation.
  Window dumps expose `cpu.window_id`, `cpu.window_frames` and `cpu.renderer_samples`; the runner
  requires multiple advancing windows, nonzero draw-frame averages and matching frame/result counts.
- Task mode changes the meaning of the CPU counters: `cpu.wait_ms` is the join wait, and per-slot
  sums accumulate worker wall times across parallel tasks — they can exceed the frame interval and
  are **not** comparable to serial slot values. Use the frame interval/FPS for on/off acceptance.
- Paired Debug captures (same binary, 8 s warmup + 6 s capture): FSR Balanced — Fuma
  28.49 → 20.00 ms, Bogbottom save 39.48 → 29.76 ms, AD hub 22.41 → 21.01 ms; FSR Quality — Fuma
  27.84 → 23.97 ms, Bogbottom fresh spawn 44.06 → 35.93 ms, AD hub 24.28 → 24.03 ms. The frame is
  still CPU-bound at 4K: the GPU time scales with the FSR mode (Bogbottom fresh spawn
  26.9 → 15.0 ms from Quality to Ultra) while the graph's wall time stays at ~31–34 ms. See
  PERFORMANCE.md for the full matrix and identities.

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

**Two release decisions, not one:** M1, M2, M4 and M5 gate a default-on *render* task path; broader
whole-frame CPU task parallelism additionally requires M3. A named final review must authorize the
default change, and the serial fallback remains available. M6 is the separate performance target;
meeting one FPS number does not certify thread safety, and a GPU bottleneck does not make the task
platform incomplete. Neither one short capture nor the historical ~40% uplift closes these gates.

Every milestone handoff must include MCP source/evidence identities, the changed ownership/DAG
boundaries, exact tests/captures, measured deltas and remaining gaps. Keep the milestone open until
its completion evidence exists; do not mark it done merely because tasks were added.

## Known limitations

- `r_tasks` stays off by default until the stability matrix and the default-flip review complete.
- `r_gpulightmapupdate`'s GPU lightmap path is an unimplemented stub (assert); task mode does not
  require it.
- No worker watchdog: a hung task hangs the join loop; the join reports the `wait` slot so a
  persistent value is visible in captures.
