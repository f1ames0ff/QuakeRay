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
- The alias pose job module (`../Quake/alias_pose_jobs.{h,c}`) is a separate, standalone stage for
  task-owned pose preparation. It is not wired into the renderer yet; do not confuse it with the
  graph below.

## The task system

`../Quake/tasks.c` provides a fixed worker pool (`CLAMP (1, SDL_GetCPUCount (), 32)` workers),
function tasks, indexed tasks (each index claimed exactly once), and dependencies between
submitted tasks. When adding work:

- allocate with `Task_AllocateAndAssignFunc` or `Task_AllocateAndAssignIndexedFunc` (the payload is
  copied; keep it small and do not put pointers to stack locals in it);
- add every dependency **before** the task is submitted — `Task_AddDependency` is epoch-checked and
  silently drops an edge to an already-finished task, which can hide a missing order;
- keep the dependency fan-in below `MAX_DEPENDENT_TASKS` (16);
- submit each task once, and join from a non-worker thread (`Task_Join`, typically with
  `SDL_MUTEX_MAXWAIT`; `Tasks_IsWorker()` tells the caller whether it may be a worker).

## The render frame graph

Entry: [SCR_UpdateScreen](../Quake/gl_screen.c#L1149) takes the tasks branch when the condition
holds, builds the frame in [R_RenderView](../Quake/gl_rmain.c#L1281), and joins `draw_done` while
pumping extra audio updates.

| Edge | Meaning |
| --- | --- |
| `prev_end_rendering_task -> begin_rendering_task` | a new frame never reuses swapchain/frame state before the previous frame's end task finished |
| `begin_rendering_task -> setup_frame_task`, `begin_rendering_task -> before_mark` | frame start (`qrStartFrame`: fence/acquire, scene collector reset, light-registry frame preparation) happens before anything reads or uploads |
| `store_efrags -> before_mark` | dynamic light collection runs after the efrag pass |
| `cull_surfaces -> chain_surfaces -> draw_world_task` | the visibility output feeds the world task |
| `draw_world_task -> draw_sky_and_water_task`, `-> draw_entities_task`, `-> draw_alpha_entities_task`, `-> draw_particles_task` | every dynamic uploader runs after the world task closed the static-geometry window (see below) |
| `draw_world_task, draw_sky_and_water_task, draw_entities_task, draw_alpha_entities_task, draw_particles_task -> draw_view_model_task` | the viewmodel producer runs last among the producers |
| `draw_gui_task -> draw_done_task -> end_rendering_task` | GUI recording, then the renderer recording/end task |

`draw_entities_task` is indexed with `NUM_ENTITIES_CBX` (6) slices, so several workers draw entity
ranges in parallel.

**Static-geometry window.** `R_DrawWorldTask` calls `qrBeginStaticGeometries` … `qrSubmitStaticGeometries`
(`gl_rmain.c#L1107`/`#L1119`). Inside that window only `QR_GEOMETRY_TYPE_STATIC` and
`STATIC_MOVABLE` uploads are accepted; `Scene::Upload` throws `QR_WRONG_FUNCTION_CALL` for dynamic
geometry. Order every dynamic uploader after `draw_world_task`, and keep static uploads inside the
world task.

## Worker rules

A worker thread must not:

- allocate or free engine memory (`Mem_*`, hunk/zone helpers);
- call renderer `qr*` entry points outside the synchronized ones;
- write profiler/bench state except through the locked helpers;
- read or write cvars, or print through `Con_*`.

Put producer buffers in task-owned or thread-local storage. The graph currently relies on:

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
  [gl_heap.c](../Quake/gl_heap.c#L42).

When you add a producer that runs on a worker, ensure its shared writer/reader pairs are ordered by
a dependency edge or protected by a lock, and keep `r_tasks 0` byte-compatible with `r_tasks 1`.

## Measurement

- Enable with `r_tasks 1`; the official runner takes it as
  `-Overrides @('r_tasks 1')` (`tests/perf/run_stress.ps1`).
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
- The pose job module awaits the geometry kernel hand-off before integration.
- No worker watchdog: a hung task hangs the join loop; the join reports the `wait` slot so a
  persistent value is visible in captures.
