# Multithreading integration and follow-up optimization plan

Updated: 2026-10-10.

Status: the first bounded integration campaign was executed on the merged branches on 2026-10-09 and extended on 2026-10-10 (AD hub Balanced and Fuma/AD/Bogbottom Quality with tasks enabled on matched rebuilt control/clean binaries; all cells inside spread). Remaining step 1 coverage: fresh spawn, long gameplay, serial-mode parity beyond Fuma/Bogbottom Balanced and direct per-slice critical-path/join attribution. The id Tech 8 research and prioritized work-reduction experiments below were added on 2026-10-10. They authorize no implementation or default-setting change by themselves.

## Objective and principles

Reduce the CPU critical path without removing geometry, lights or required acceleration-structure work, or lowering graphics quality. Targets remain 60 FPS at 3840x2160 FSR Balanced and 45 FPS at FSR Quality, using the audited preset with volumetric sky Low. These are frame budgets of 16.67 ms and 22.22 ms, not promised gains from any individual change.

Prefer eliminating repeated work before adding parallel execution. Current command-list recording uses the render thread and the frame slot's single list; that is a property of this implementation, not a universal prohibition on parallel recording or device-object creation. Any change to that ownership model requires a separate design and verification.

Start from [ARCHITECTURE.md](../ARCHITECTURE.md) and [PERFORMANCE.md](../PERFORMANCE.md). Use QuakeRay MCP inspection where it covers the task and inspect the implicated caller, callee and ownership boundary before expanding the search.

## Existing evidence and its limits

- The clean stage-5 implementation was accepted before the master merge at `79d91902`; its merged code was measured at `dc89557a`, with evidence corrections at `6613e196`. It retains descriptor/shape storage and reuses BLAS handles while each build fits its create-time envelope. Full dynamic BLAS builds still occur; this is not refit or persistent per-mesh instancing.
- The pre-merge sequential comparison reproduced a Fuma RHI setup reduction of about 0.646 ms; Bogbottom was neutral. The merged campaign reproduced the serial Fuma reduction, but did not establish a task-mode frame gain. Keep these comparisons separate. See [the stage-5 report](../stage5-rhi-setup-report.md), including its post-merge section.
- Previously measured BLAS recording costs of about 2.5 ms on Bogbottom and 1.2 ms on Fuma include conversion, temporary storage, transform uploads, size queries, scratch management and driver-facing recording. They are not an established irreducible animation cost.
- No comparison with another engine establishes the cause of an FPS difference here. Persistent geometry, instancing, refit and GPU-driven processing are candidate directions, not facts inferred from another game's throughput.

## id Tech 8: verified techniques and transfer limits

The purpose of this comparison is to borrow work-reduction principles, not promise id Tech 8 frame rates or replace QuakeRay's renderer wholesale. The fast baseline rendering path and the optional path-tracing mode of DOOM: The Dark Ages must not be conflated.

### Public sources

| Source | Evidence used | Limits |
| --- | --- | --- |
| Tiago Sousa, [Fast as Hell: idTech8 Global Illumination, SIGGRAPH 2025](https://advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf) | PDF pages 16, 18-23: interleaved irradiance-volume updates, spatial radiance cache, cached final gather and reconstruction; pages 27-30: reflection fallbacks, shared transparency lighting and detail recovery; page 32: hotspot GPU timings | Describes the shipped GI solution, not every CPU subsystem or total frame cost. |
| Philip Hammer and Dominik Lazarek, [Rip & Tear: Breaking Down the Rendering of DOOM: The Dark Ages, GDC 2026](https://schedule.gdconf.com/session/rip-tear-breaking-down-the-renderingofdoom-the-dark-ages/915274) | The public session description identifies visibility buffers, deferred texturing, compute material dispatch, tile classification, variable-rate compute shading and GPU triangle culling | The session description establishes techniques, not their individual measured gains. |
| Billy Khan interview, [How id Software Used Neural Rendering and Path Tracing, September 2025](https://developer.nvidia.com/blog/how-id-software-used-neural-rendering-and-path-tracing-in-doom-the-dark-ages/) | Distinguishes the more expensive path-tracing mode; discusses SER, opacity micromaps, reconstruction and BUILD versus refit costs | Vendor-hosted interview; hardware-specific benefits and quoted speedups are not QuakeRay acceptance evidence. |
| Khronos, [vkGetAccelerationStructureBuildSizesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetAccelerationStructureBuildSizesKHR.html) | Allocation guarantees depend on corresponding per-index geometry properties and primitive/vertex maxima | A size-compatible BUILD is not automatically an update-compatible refit. |

### What makes the fast path inexpensive

1. **Reuse lighting, not just previous images.** The GI pipeline traces world visibility, shades active radiance-cache entries and updates irradiance volumes. The presentation reports approximately 20,000 cache-entry updates per frame, with results reused across frames.
2. **Update subsets rather than everything.** Irradiance-volume updates are interleaved: one cascade and one local volume per frame in the described scheme. Temporal reuse and validity handling are part of the algorithm, not an instruction to omit necessary updates.
3. **Gather from caches.** Final gather uses one ray per pixel at half or quarter resolution on each axis. It looks up screen-space lighting, world radiance or irradiance volumes rather than performing new full shading at each final-gather hit. Denoising and upscaling reconstruct the result.
4. **Share work between surface classes.** Transparent surfaces, particles and fog reuse lighting through a froxel irradiance volume. Reflections use probes and screen-space/RT paths where appropriate; the described console configuration disabled RT reflections for performance.
5. **Organize visible work before expensive shading.** Visibility buffers, deferred texturing, material dispatch and tile classification reduce unnecessary or incoherent shading. Variable-rate compute shading controls sampling density. These techniques are not direct drop-in replacements for our traced primary-visibility path.

The SIGGRAPH hotspot table reports approximately 1.7 ms for the GI pipeline on an RTX 4080 and approximately 2.05 ms serial / 1.55 ms async on PS5. These are **GPU GI costs, not total frame times**, and are not comparable to QuakeRay's CPU `RHI_setup` bracket. Our verification builds are Debug; comparisons with a commercial optimized executable also require explicit qualification. Report native rendering intervals separately from generated-frame display rates.

### What transfers to QuakeRay now

| Principle | Near-term application | Larger follow-up |
| --- | --- | --- |
| Retain stable resources | Stop unnecessary BLAS handle creation and repeated geometry submission | Persistent object/mesh BLAS with TLAS instance transforms |
| Separate immutable and changing data | Keep indices and stable attributes resident; update only dirty data | GPU deformation and GPU-managed scene processing |
| Cache expensive answers with explicit validity | Improve brush-style cache behavior after measuring misses | Spatial lighting caches with scene/material/light invalidation |
| Reduce submission overhead | Eliminate redundant uploads before adding more producer threads | Batched commit channels and reviewed collector ownership changes |
| Reconstruct selectively computed lighting | Profile current GPU passes and reuse existing temporal infrastructure | A separately scoped radiance-cache/probe GI experiment |

QuakeRay already has static submission, motion history and temporal denoising. Do not describe these systems as missing or introduce duplicate implementations. Do not lower quality, cull reflection/shadow contributors using only camera visibility, or adopt probe-based GI as an invisible performance patch: those changes need separate visual requirements and approval.

## Priority queue: bounded work-reduction experiments

This order ranks evidence strength and implementation scope, not guaranteed millisecond savings. Diagnostics must select one candidate per branch; gains from separate candidates are not assumed additive.

| Priority / ID | Candidate | Current evidence | First check | Potential / status |
| --- | --- | --- | --- | --- |
| 1 / WR01 | Stabilize dynamic BLAS allocation under changing submission order | Reuse captures show exactly 2 handle recreations per frame under tasks; create/retire costs about 1.0 ms on Fuma and 1.45-1.49 ms on Bogbottom | Attribute every failed envelope check, recording the filter, geometry count, mismatching index and compatibility fields | Measured affected CPU cost, not promised frame savings; hypothesis verification first |
| 2 / WR02 | Re-validate existing `rt_brush_persistent` | Existing code avoids repeated uploads for eligible brush objects; the audit preset keeps it off | Same-binary on/off comparison under tasks plus transitions and moving-object checks | Lowest-cost functional experiment; task-mode benefit and correctness remain open |
| 3 / WR03 | Reduce brush-style cache recomputation | A 512-entry direct-mapped, thread-local entity cache already exists | Separate collisions, movement/settings invalidation, cold entries and worker migration | Saving unknown until miss causes are measured |
| 4 / WR04 | Reduce NVRHI build-path temporary work | Each bottom-level build creates five temporary vectors and queries build sizes | Time vector initialization, conversion, query, scratch/barriers and recording separately | Limited backend candidate, not permission to remove correctness guards |
| 5 / WR05 | Retain invariant geometry data and reduce upload volume | Collector copies complete vertices, indices, transforms and metadata for each submitted geometry | Measure bytes by category, dirty frequency and upload-lock wait versus lock-held work | Broader collector/lifetime change; start with one eligible model class |

### Current task-mode reference, not a new benchmark

These values were read from two individual control captures at source `9d7a6804`, executable `5374A9CB...`, Balanced, on 2026-10-09. They are routing evidence, not a statistically aggregated acceptance result.

| Quantity | Fuma | Bogbottom | Interpretation |
| --- | ---: | ---: | --- |
| Frame interval mean | 20.04 ms | 31.59 ms | Whole observed rendering cadence |
| GPU frame snapshot mean | 19.03 ms | 23.68 ms | Timed RHI-list GPU snapshot; not all host/presentation cost |
| CPU `RHI_setup` mean | 3.40 ms | 5.59 ms | Ordered end-task setup work |
| Geometry API calls per frame | 1285.6 | 2333.4 | API calls, not unique meshes or triangle counts |
| `host.pass server` mean | 6.57 ms | 9.78 ms | Coarse host-pass timing; do not label all of it render preparation or infer its detailed contents |

Captures: `build/Debug/audit-s5d-ctrl-fuma-t1-r1-20261009-222225-b36c7a` and `build/Debug/audit-s5d-ctrl-bog-t1-r1-20261009-222027-eb38e8`. Use the full campaign for acceptance and the manifests for asset, save and configuration identity.

The GPU snapshots already exceed the 16.67 ms Balanced target in both scenes. CPU improvements alone cannot establish 60 FPS. A hypothetical 1 ms reduction of a 31.6 ms critical path is approximately 3.3% more FPS, not a route to doubling throughput. Reducing submission and rebuild work may also help the GPU, but that benefit must be measured separately.

## 1. Freeze and profile the integrated master

Before choosing another optimization:

1. Freeze a combined control that includes the accepted task integration and clean sequential RHI changes. Do not assume they have both reached master: the recorded campaign used merged solution branches. Record the actual commit and any local changes.
2. Build through `./build_win.ps1 Debug`, verify the build receipt against the deployed executable and pack, and retain a matching control runtime.
3. Capture Bogbottom, Fuma and AD hub with tasks enabled, at Balanced and Quality. Keep the save-based Bogbottom scenario and fresh-spawn scenario separately identified; do not compare them as the same workload.
4. Measure the longest worker path, join wait, the end-render task and its `qrDrawFrame`/RHI phases, alongside frame mean/p95 and GPU timings. Use the benchmark window, not an arbitrary overlay snapshot.
5. Use a bounded tasks-off comparison where necessary to distinguish work reduction from scheduling effects. Preserve identical settings and workload identity.

Do not add historical sequential bucket times or divide aggregate worker time by the worker count to derive the new critical path. Do not add CPU and GPU times. If GPU execution already exceeds the target frame budget, reducing CPU cost alone cannot establish the FPS target.

**Exit condition:** a reproducible combined control and a measured critical path that selects the next small experiment. Update the architecture/performance evidence for the integrated execution path.

### Step 1 results (bounded campaign executed 2026-10-09)

The merged branches were built and measured after `origin/master` `0c04f86f`: `perf/rhi-dyn-blas-clean` (`dc89557a`, exe `910D42E8…`) and `perf/rhi-dyn-blas-reuse` (`3b5a18ec`, exe `FCD31349…`) pass CTest 8/8; the control is `perf/run-stress-capture-fixes` (`9d7a6804`, engine sources byte-equal to master, exe `5374A9CB…`). Same-binary `r_tasks 0/1` captures (staged runner, Balanced, Fuma + Bogbottom) reproduce the sequential Fuma win in serial mode (frame 27.0 vs 27.5 ms; `RHI_setup` 2.6 vs 3.1 ms) while under `r_tasks 1` the reuse does not separate. The diagnostics twin shows chronic handle churn (2.00 recreations per frame under tasks; `dyn_create_ms` 1.0-1.5). Task-scheduled geometry order interacting with the per-index envelope is the leading explanation, not a directly instrumented causal result. WR01 must verify it before choosing a new allocation policy. Evidence: `PERFORMANCE.md` on the clean branch ("Dynamic BLAS reuse re-validated on the merged task graph") and `stage5-rhi-setup-report.md` ("Post-merge task-graph adaptation").

Extended on 2026-10-10: AD hub Balanced and Fuma/AD/Bogbottom Quality were captured with `r_tasks 1` on rebuilt matched binaries (control exe `EF499999…`, clean exe `8018D4B4…`). Every cell stays inside its observed spread; no cell separates the candidate from the control. One contaminated clean Fuma Quality pair is retained raw and a same-binary rerun matched the control. Full table, identities and captures: `PERFORMANCE.md` on the clean branch. Remaining step 1 coverage: fresh spawn, long gameplay, serial-mode parity beyond Fuma/Bogbottom Balanced and direct per-slice critical-path/join attribution. Retain every valid repeat, including elevated ones; do not discard a repeat merely because it weakens a performance claim.

## 2. First bounded experiment: remove repeated CPU preparation work

Select one workstream from the new critical-path evidence rather than starting several speculative changes together.

### A. WR01: stabilize dynamic BLAS allocation without changing scene semantics

Route: [DynamicBlasShape.h](../renderer/Source/RHI/DynamicBlasShape.h) -> `RhiAccelStructs::AppendDynamicSlot` in [RhiAccelStructs.cpp](../renderer/Source/RHI/RhiAccelStructs.cpp) -> NVRHI create/build paths in [vulkan-raytracing.cpp](../third_party/nvrhi/src/vulkan/vulkan-raytracing.cpp).

**Observation:** task-mode captures show two handle recreations every measured frame. **Hypothesis:** varying producer order makes corresponding geometry sizes incompatible with the remembered per-index envelope. Other causes, such as count growth or different compatibility fields, must be distinguished explicitly.

1. Measure refusal reasons by filter: missing handle, count growth, primitive/vertex-bound growth, incompatible index type, stride or transform presence. Record enough stable geometry identity to distinguish membership changes from permutations. Collect aggregate diagnostics outside the measured hot region; do not print each failure.
2. Verify ownership: producers populate collector arrays before the end task; per-slot descriptors, envelopes and handles remain end-task-owned; replaced resources retire through the frame context. Do not alter upload synchronization to obtain stable order.
3. Select a spec-backed allocation policy only after the mismatch evidence exists. One candidate is a conservative create-time envelope with compatible per-index maxima that remains sufficient for allowed permutations. Record memory/scratch overhead and geometry-count capacity; use an explicit compatibility fallback.
4. Never accept an order-insensitive comparison against an unchanged order-sensitive allocation merely because sorted shapes match. Vulkan's size guarantee is per corresponding index. A sorted bookkeeping vector is not proof that the existing allocation supports the actual build.
5. Do not reorder actual BLAS geometry independently of shader metadata, primitive indexing or history mappings. Stable-order collection is a different candidate that requires all those mappings to remain coherent.

Tests: randomized permutations and membership changes; count/bound growth and shrink; mixed compatibility fields; alternating frame slots; disappearance/reappearance; map reload; in-flight retirement. Verify allocation sizing and the actual build path, not just a predicate in isolation. Any NVRHI insufficient-size warning or skipped build is a correctness failure.

Measure create/retire count and CPU cost, descriptor and recording costs, scratch/AS memory, GPU setup/tracing and frame mean/p95/p99. Removing handle creation does not remove the full dynamic BLAS BUILD. The affected 1.0-1.5 ms CPU bucket is an opportunity budget, not a guaranteed net saving.

**Exit condition:** safe sizing under the tested permutations, fewer unnecessary recreations, and repeatable frame improvement without memory/GPU/visual regression; otherwise document rejection or a mechanism-only result.

### B. WR02: re-validate the existing persistent brush path

Route: `RT_StaticMovablePrepare` / `RT_StaticMovableCovers` / `RT_StaticMovableUpdate` in [r_world.c](../Quake/r_world.c) -> `ASManager::UpdateStaticMovableTransform` in [ASManager.cpp](../renderer/Source/ASManager.cpp) -> `RhiAccelStructs::BuildStatic`.

- The current audited preset uses `rt_brush_persistent 0`. Start with a same-binary `0/1` experiment under `r_tasks 1`; also retain the serial-mode regression check. This experiment does not authorize changing the default or audited preset.
- A supplied multithreading-agent report cites 292 fewer uploads out of 598 and approximately 1.4 ms saved on Fuma. That report is a lead, not fresh acceptance for the current source/binary/settings; recover its original artifacts before treating the numbers as a control.
- Existing eligibility is restrictive: the preparation path selects a single owning entity for a brush model, and covered surfaces exclude several dynamic material cases. Test those exclusions and fallbacks rather than extending eligibility immediately.
- Transform updates increment the static-movable revision, and RHI rebuilds movable structures when that revision changes. This implementation can save uploads without eliminating builds during motion; measure both sides of the tradeoff.
- Cover stationary and moving doors/platforms, rotation, model changes, despawn, multiple entities sharing a model, alpha/animated surfaces, reload and runtime enable/disable. Check lighting, transforms, occlusion and motion history.

Record eligible objects, geometry calls, uploaded vertex/index bytes, transform updates, static/movable/dynamic build counts, CPU/GPU timings and frame tails. Stop on an incorrect fallback or lifecycle failure.

**Exit condition:** the existing path passes task/serial correctness checks and measurably reduces frame work; otherwise retain it as experimental and proceed without enabling it globally.

### C. WR03: improve brush-style answer reuse before changing scheduling

Route: `RT_PackSurfaceLightStyles` -> `RT_SurfacePackLightStyles` -> `RT_NearestStyledLightDistance` in [r_world.c](../Quake/r_world.c) and [gl_rlight.c](../Quake/gl_rlight.c).

The entity cache is already a 512-entry direct-mapped thread-local table, keyed by entity, surface, texture, transform and relevant settings. World surfaces have a separate cache. This is not a missing-cache problem.

Measure hits and miss causes: cold/generation reset, direct-map eviction, actual movement, material/settings changes and different worker assignment across frames. Count expensive reach queries, not only cache lookups. A thread-local cache can lose useful history when an entity is drawn by a different worker; determine whether this matters in the actual scheduler before redesigning ownership.

Candidates, selected by evidence: a better collision policy; bounded task-independent entity-owned results with explicit publication/lifetime; precomputed local surface centers to reduce miss cost; incremental recomputation only for genuinely dirty inputs. Do not share a TLS table without synchronization, force scheduling affinity as a substitute for ownership analysis, or simply enlarge the table and assume a gain.

Tests: collisions, worker changes, translation/rotation, texture and reach-setting changes, generation reset, entity reuse and map reload. Compare packed style values and reach decisions against the reference, with real production payloads.

**Exit condition:** fewer redundant reach queries and a shorter entity critical path without stale styles or new lock contention; no millisecond target is justified before the miss breakdown.

### D. WR04: decompose and reduce NVRHI BLAS recording preparation

Route: `RhiAccelStructs::AppendDynamicSlot` in [RhiAccelStructs.cpp](../renderer/Source/RHI/RhiAccelStructs.cpp) -> NVRHI `CommandList::buildBottomLevelAccelStruct` in [vulkan-raytracing.cpp](../third_party/nvrhi/src/vulkan/vulkan-raytracing.cpp).

Separate the following CPU costs with identical temporary instrumentation on both arms:

- The five temporary vectors and their initialization.
- Geometry conversion and transform uploads.
- Build-size queries.
- Scratch allocation and resource-state/barrier preparation.
- Vulkan build-command recording.

Only then choose a minimal candidate, such as reusable temporary storage or cached size requirements with complete compatibility keys and invalidation. Preserve scratch sizing, resource lifetime, frame-slot ownership and all required builds. Retain correctness checks; a silently skipped build is not an acceptable fallback.

The saving is unknown until measured. Changes to the pinned NVRHI patch or shared interfaces must be coordinated and kept separate from scheduler changes.

### E. WR05: retain immutable geometry data and reduce submission volume

Route: `VulkanDevice::UploadGeometry` in [VulkanDevice.cpp](../renderer/Source/VulkanDevice.cpp) -> `Scene::Upload` -> `VertexCollector::AddGeometry` / `CopyDataToStaging` in [VertexCollector.cpp](../renderer/Source/VertexCollector.cpp) -> `GeomInfoManager::WriteGeomInfo`.

- The collector currently copies full vertex records, indices and transforms and constructs metadata per geometry. Upload calls share the geometry-upload mutex. This makes fewer calls/bytes potentially valuable, but aggregate upload-slot time includes contention and is not the lock-held work cost.
- First separate vertex/index copying, material/metadata preparation, bounds work, allocations, lock wait and lock-held work. Record model identity, dirty frequency and repeated invariant bytes. Do not use the tiny backend staging slot as a measurement of all producer copying.
- Start with one eligible class: stable indices, stable vertex attributes or unchanged rigid geometry. Preserve stable IDs and previous/current-frame mappings. Any retained GPU memory must remain valid for in-flight slots and model/map reloads.
- Use real dirty keys for topology, material/filter, animation, transforms and entity lifetime. A pointer staying equal is not a sufficient content/version key.
- Do not reopen surface-local alias slicing as the main solution for the measured content without new exposure evidence: the earlier content audit found no relevant multi-surface duplication. Do not reintroduce the rejected pose producer; inline posing already runs within entity tasks, and its separate producer measured a frame regression.

**Exit condition:** fewer calls or bytes on a measured critical path with identical geometry/material/history behavior and no larger commit/join/lock cost. Broader batching or GPU deformation follows only after this narrow result.

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

A radiance-cache/probe GI prototype inspired by id Tech 8 belongs here, not in WR01-WR05. It changes the lighting algorithm and requires separate quality criteria for emissive lighting, dynamic objects, disocclusion, leakage, reflections, transparent surfaces and temporal response. Reuse existing history/denoising infrastructure where appropriate; profile the GPU passes before choosing the cache or update budget. GPU-driven visibility buffers, variable-rate shading, SER/opacity micromaps and async compute are also separate capability/design projects, not assumed portable quick wins for the recorded Radeon hardware.

## Verification and ownership rules

- One candidate per solution branch, based on a frozen control; measure combinations separately instead of assuming gains add together.
- Use Debug builds and actual test execution, not merely successful test compilation.
- Use the shared machine guard; never overlap another game instance or GPU performance run. Each runtime invocation is at most 300 seconds, with one save/preset per invocation and GPU release between runs. The existing short baseline protocol is eight-second warmup plus six-second capture; extend coverage explicitly when needed.
- Retain source/build/binary, asset/save and effective-setting identities, focus/context checks, raw captures and visual evidence. Compare mean and tail latency, not FPS alone; compute residuals frame-wise, never by subtracting percentiles.
- Compare work-reduction candidates against their controls with `r_tasks 1` on both arms. A same-binary `r_tasks 0/1` pair checks scheduling and serial compatibility; it does not isolate the new optimization. Use a runtime toggle for same-binary candidate/control only where its lifecycle is safe; otherwise use separately frozen matched builds.
- Start with Fuma and the Bogbottom save at Balanced, alternating control/candidate order with at least two valid repeats per arm. Add AD hub, Quality, fresh spawn and relevant transition coverage before broad acceptance. Use the official analyzer's percentile convention for new captures; disclose a different convention for historical tables and keep all valid elevated repeats.
- Require complete samples, advancing gameplay, effective mode/settings checks and zero focus losses. Preserve diagnostic window binding: `bench_active` where available, or explicit per-frame timing alignment with residual checks. Benchmark frame IDs and lifetime diagnostic frame IDs are not interchangeable.
- A smaller internal timer without a separable frame benefit is a mechanism result, not an FPS claim. Reject unexplained mean/tail/GPU regressions; record inconclusive results rather than stacking more candidates. Keep unmeasured lock, server and GPU costs explicitly unknown.
- Include deterministic policy tests, relevant renderer tests, visual/gameplay checks and transition coverage. State untested maps, long gameplay and task-integration limits rather than claiming universal correctness.
- Coordinate profiler schemas, test/CMake registrations, renderer interfaces, task ownership and pinned backend changes. Do not duplicate the separately owned cluster or particle/FTE workstreams.
- Keep [ARCHITECTURE.md](../ARCHITECTURE.md) and [PERFORMANCE.md](../PERFORMANCE.md) current when execution boundaries or accepted evidence change. Keep diagnostics and machine-local activation/configuration out of functional commits.

## Immediate next action

1. Freeze the actual combined control and preserve the bounded step 1 evidence and its remaining coverage gaps.
2. Start WR01 with refusal-reason diagnostics. Confirm the churn explanation before implementing a spec-backed allocation envelope; do not merely relax the predicate.
3. Re-validate WR02 on the same task-enabled control as the lowest-code-cost independent experiment. Serialize runtime/build work across lanes using the machine guard.
4. Continue WR03 (stage 6) after measuring miss causes; use WR04 if ordered backend preparation remains limiting. Keep WR05 scoped to one measured immutable-data opportunity.
5. Combine only accepted results and remeasure the combination. Then consider persistent rigid BLAS/TLAS instancing, animation refit and the larger GPU/lighting directions.

### Handoff checklist

- [ ] Control source, binary, assets, scenarios and settings are frozen; remaining step 1 coverage is explicit.
- [ ] Every candidate has a writer/reader/lifetime map and no unresolved mandatory threading finding.
- [ ] WR01 refusal reasons establish the failure mechanism; allocation and actual-build tests preserve Vulkan's guarantees.
- [ ] WR02 eligibility, movement, reload and fallback behavior are validated before any default change.
- [ ] WR03 distinguishes redundant cache misses from legitimate dirty work.
- [ ] WR04/WR05 separate saved work from shifted cost and synchronization effects.
- [ ] Debug build and actual tests pass; CPU/GPU/visual and paired frame evidence is retained.
- [ ] Each result is classified as accepted, rejected, mechanism-only or inconclusive; no unmeasured gain is added to a total.
- [ ] The final comparison table names scenario, mode, candidate/control revisions and hashes, repeat counts, mean/p95/p99, CPU/GPU mechanism metrics, validity and remaining limitations.
