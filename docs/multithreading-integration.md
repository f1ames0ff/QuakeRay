# Multithreading integration and follow-up optimization plan

Updated: 2026-10-09.

Status: proposed follow-up after the multithreading changes and accepted sequential RHI changes are integrated into master. This document does not assert that either integration has already happened or authorize implementation of every candidate below.

## Objective and principles

Reduce the CPU critical path without removing geometry, lights or required acceleration-structure work, or lowering graphics quality. Targets remain 60 FPS at 3840x2160 FSR Balanced and 45 FPS at FSR Quality, using the audited preset with volumetric sky Low. These are frame budgets of 16.67 ms and 22.22 ms, not promised gains from any individual change.

Prefer eliminating repeated work before adding parallel execution. Current command-list recording uses the render thread and the frame slot's single list; that is a property of this implementation, not a universal prohibition on parallel recording or device-object creation. Any change to that ownership model requires a separate design and verification.

Start from [ARCHITECTURE.md](../ARCHITECTURE.md) and [PERFORMANCE.md](../PERFORMANCE.md). Use QuakeRay MCP inspection where it covers the task and inspect the implicated caller, callee and ownership boundary before expanding the search.

## Existing evidence and its limits

- The clean stage-5 implementation is `perf/rhi-dyn-blas-clean` at `79d91902`: persistent descriptor/shape storage and BLAS-handle reuse while each build fits its create-time envelope. Full dynamic BLAS builds still occur; this is not refit or persistent per-mesh instancing.
- The clean sequential comparison reproduced a Fuma RHI setup reduction of about 0.646 ms; Bogbottom was neutral. These measurements do not establish the effect on the future combined multithreaded master. See [the stage-5 report](../stage5-rhi-setup-report.md).
- Previously measured BLAS recording costs of about 2.5 ms on Bogbottom and 1.2 ms on Fuma include conversion, temporary storage, transform uploads, size queries, scratch management and driver-facing recording. They are not an established irreducible animation cost.
- No comparison with another engine establishes the cause of an FPS difference here. Persistent geometry, instancing, refit and GPU-driven processing are candidate directions, not facts inferred from another game's throughput.

## 1. Freeze and profile the integrated master

Before choosing another optimization:

1. Verify that master includes the accepted task integration and the clean sequential RHI changes. Record the actual combined commit and any local changes.
2. Build through `./build_win.ps1 Debug`, verify the build receipt against the deployed executable and pack, and retain a matching control runtime.
3. Capture Bogbottom, Fuma and AD hub with tasks enabled, at Balanced and Quality. Keep the save-based Bogbottom scenario and fresh-spawn scenario separately identified; do not compare them as the same workload.
4. Measure the longest worker path, join wait, the end-render task and its `qrDrawFrame`/RHI phases, alongside frame mean/p95 and GPU timings. Use the benchmark window, not an arbitrary overlay snapshot.
5. Use a bounded tasks-off comparison where necessary to distinguish work reduction from scheduling effects. Preserve identical settings and workload identity.

Do not add historical sequential bucket times or divide aggregate worker time by the worker count to derive the new critical path. Do not add CPU and GPU times. If GPU execution already exceeds the target frame budget, reducing CPU cost alone cannot establish the FPS target.

**Exit condition:** a reproducible combined control and a measured critical path that selects the next small experiment. Update the architecture/performance evidence for the integrated execution path.

## 2. First bounded experiment: remove repeated CPU preparation work

Select one workstream from the new critical-path evidence rather than starting several speculative changes together.

### A. If the end-render task limits the frame: decompose BLAS recording

Route: `RhiAccelStructs::AppendDynamicSlot` in [RhiAccelStructs.cpp](../renderer/Source/RHI/RhiAccelStructs.cpp) -> NVRHI `CommandList::buildBottomLevelAccelStruct` in [vulkan-raytracing.cpp](../third_party/nvrhi/src/vulkan/vulkan-raytracing.cpp).

Separate the following CPU costs with identical temporary instrumentation on both arms:

- The five temporary vectors and their initialization.
- Geometry conversion and transform uploads.
- Build-size queries.
- Scratch allocation and resource-state/barrier preparation.
- Vulkan build-command recording.

Only then choose a minimal candidate, such as reusable temporary storage or cached size requirements with complete compatibility keys and invalidation. Preserve scratch sizing, resource lifetime, frame-slot ownership and all required builds. Retain correctness checks; a silently skipped build is not an acceptable fallback.

The saving is unknown until measured. Changes to the pinned NVRHI patch or shared interfaces must be coordinated and kept separate from scheduler changes.

### B. If entity preparation limits the frame: continue stage 6, brush styles

Route: `RT_PackSurfaceLightStyles` -> `RT_SurfacePackLightStyles` -> `RT_NearestStyledLightDistance` in [r_world.c](../Quake/r_world.c) and [gl_rlight.c](../Quake/gl_rlight.c).

Measure cache hits, misses, collisions and actual repeated reach-query work. Distinguish legitimate movement, material, light and settings invalidation from cache eviction. Optimize only the established mechanism, preserving packed-style and reach decisions. Do not reduce reach, remove lights or simply enlarge the cache and assume a gain.

**Exit condition:** a separate, measured work-reduction candidate that shortens the critical path without a demonstrated regression, or a documented rejection.

## 3. First architectural prototype: movement does not require geometry rebuilding

Start with rigid moving brush objects such as doors and platforms whose vertex/index topology does not change. Do not begin by replacing the whole renderer with a GPU-driven design.

Target model:

- Keep local-space vertex/index geometry resident.
- Build a persistent BLAS for the eligible geometry.
- Represent rigid movement with a TLAS instance transform rather than retransmitting geometry and rebuilding its BLAS solely because it moved.
- Rebuild or fall back when geometry, compatible material/filter state or other required invariants change.

Investigate the existing `rt_brush_persistent` path first. `RT_StaticMovableUpdate` in [r_world.c](../Quake/r_world.c) updates movable geometry; `ASManager::UpdateStaticMovableTransform` in [ASManager.cpp](../renderer/Source/ASManager.cpp) increments the movable revision, which RHI checks for rebuilding. Persistent storage alone therefore does not establish that movement avoids a build. Do not enable that experimental path blindly or duplicate it without understanding its contract.

Preserve geometry IDs, material/opacity and visibility rules, shader attribute indexing, per-entity data, motion history, map/model lifetimes and in-flight references. Sharing a mesh BLAS across entities is conditional on their geometry and shader-data contracts being compatible, not merely on equal model names. Account for any change to TLAS instance limits and per-instance uniform/shader mappings.

Prototype on a small eligible subset with an explicit fallback to the existing path. Measure preparation/uploads, bytes transferred, BLAS build counts, end-task time and GPU tracing cost. Validate moving, stationary and overlapping instances, spawn/despawn, material/filter changes and reloads.

**Exit condition:** equivalent scene behavior and a reproducible reduction in repeated work across the measured critical path. No predicted millisecond gain is an acceptance result.

## 4. Stable-topology animation: evaluate update/refit

After the rigid-geometry lifecycle is established, evaluate UPDATE/refit for deforming geometry whose topology and other update-required properties remain compatible.

Create eligible structures with the required update capability, retain a validated initial build and use explicit dirty keys. Fall back to BUILD when the update contract is violated. Changing visibility or collector membership must not silently make an update invalid.

Compare both CPU preparation/recording and GPU setup/tracing costs. A faster update can leave a less efficient acceleration structure and slow ray traversal; a CPU-only improvement is insufficient. If periodic rebuilding is necessary, derive the policy from measured total frame behavior rather than an arbitrary interval.

**Exit condition:** a CPU/GPU-aware acceptance result with topology-change and fallback tests, or a documented rejection.

## 5. Later directions

Consider broader batching, GPU pose/deformation preparation and GPU-driven submission only after the earlier measurements identify a remaining bottleneck they address. Preserve previous-frame geometry and motion-history behavior when moving pose work to the GPU. No implementation or benefit is assumed by listing these directions.

## Verification and ownership rules

- One candidate per solution branch, based on a frozen control; measure combinations separately instead of assuming gains add together.
- Use Debug builds and actual test execution, not merely successful test compilation.
- Use the shared machine guard; never overlap another game instance or GPU performance run. Each runtime invocation is at most 300 seconds, with one save/preset per invocation and GPU release between runs. The existing short baseline protocol is eight-second warmup plus six-second capture; extend coverage explicitly when needed.
- Retain source/build/binary, asset/save and effective-setting identities, focus/context checks, raw captures and visual evidence. Compare mean and tail latency, not FPS alone; compute residuals frame-wise, never by subtracting percentiles.
- Include deterministic policy tests, relevant renderer tests, visual/gameplay checks and transition coverage. State untested maps, long gameplay and task-integration limits rather than claiming universal correctness.
- Coordinate profiler schemas, test/CMake registrations, renderer interfaces, task ownership and pinned backend changes. Do not duplicate the separately owned cluster or particle/FTE workstreams.
- Keep [ARCHITECTURE.md](../ARCHITECTURE.md) and [PERFORMANCE.md](../PERFORMANCE.md) current when execution boundaries or accepted evidence change. Keep diagnostics and machine-local activation/configuration out of functional commits.

## Immediate next action

After integration, perform step 1 on the actual combined master. If the end-render task is still limiting, begin with the NVRHI recording decomposition; otherwise prioritize the measured entity bottleneck, including the stage-6 brush-style investigation. The first larger architectural experiment remains persistent rigid geometry with instance transforms, followed by a separately measured animation-refit candidate.
