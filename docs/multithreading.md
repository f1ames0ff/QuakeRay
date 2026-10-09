# Multithreading

How QuakeRay runs the CPU part of a frame on worker threads, and the rules for building on it.
Read [ARCHITECTURE.md](../ARCHITECTURE.md) first for subsystem routes; measured numbers live in
[PERFORMANCE.md](../PERFORMANCE.md).

## Status and scope

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
  copied; keep it small and do not put pointers to stack locals in it);
- add every dependency **before** the task is submitted — `Task_AddDependency` is epoch-checked and
  silently drops an edge to an already-finished task, which can hide a missing order;
- keep the dependents of any one task (its fan-out; `num_dependents` in `Task_AddDependency`)
  below `MAX_DEPENDENT_TASKS` (16);
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
| `draw_gui_task -> draw_done_task`, producers `-> draw_done_task` | the GUI and every producer feed the join point |
| `draw_done_task -> end_rendering_task` | the renderer recording/end task runs last |

`draw_entities_task` is indexed with `NUM_ENTITIES_CBX` (6) slices, so several workers draw entity
ranges in parallel.

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
  are written by the cluster upload in the viewmodel task and read on the main thread only after
  the frame's join.
- Engine memory (`Mem_*`) and `Con_*` output are thread-safe (mimalloc, the console lock), and a few
  single-threaded channels (map load, one-time warnings) use them from workers; do not allocate or
  print per object on a parallel path.

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
  DTAL cache follows the same ownership rule through `RT_BuriedCacheReset`;
- the emissive skip-table spinlock (`RT_EmisSkipLock`) — covers lookup/insertion, reset and report
  snapshotting; printing happens after the snapshot lock is released;
- the styled-light index (`RT_BuildStyledLightIndex`) — built on the single-threaded map-load path
  right after `RT_ParseElights`, only read by draw tasks afterwards;
- the colour cache (`rt_colors`) — refreshed by `RT_ColorsRefresh` on the main thread in
  `R_RenderView`, workers only read it.

When you add a producer that runs on a worker, ensure its shared writer/reader pairs are ordered by
a dependency edge or protected by a lock, and keep `r_tasks 0` byte-compatible with `r_tasks 1`.

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

## Known limitations

- `r_tasks` stays off by default until the stability matrix and the default-flip review complete.
- `r_gpulightmapupdate`'s GPU lightmap path is an unimplemented stub (assert); task mode does not
  require it.
- No worker watchdog: a hung task hangs the join loop; the join reports the `wait` slot so a
  persistent value is visible in captures.
