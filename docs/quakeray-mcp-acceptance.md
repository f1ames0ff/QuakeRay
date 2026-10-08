# QuakeRay MCP acceptance record

Date: 2026-10-09 local time (matrix runs started 2026-10-08 UTC). Source: `b84b08e7` plus the
renderer validation fix `b7bf2414`. Formal agent-cost/usability pilot is excluded by owner decision.

## Outcome

MCP safety/API corrections, capture-tool integration and the renderer validation fix passed the
scoped checks below. **Vulkan validation is VUID-clean in the recorded runs.** One third-party
advisory warning remains and is documented below; passing GPU tests, successful capture jobs
or the audit result are still not a claim of visual equivalence or stable 60/45-FPS achievement.

## Directly executed checks

| Check | Observed outcome | Scope |
|---|---|---|
| Debug build/deployment with `-Tests` | Success | Rebuilt shaders and deployed the engine pack; temporary NVRHI patch reverted |
| Full Debug CTest | 6/6 passed | Frame timing, lighting, ImGui draw, clouds GPU, post-effects GPU, DTAL GPU perf |
| Game matrix | 12/12 jobs succeeded | Fuma, AD hub, Bogbottom; Balanced and Quality; two independent repeats, second sweep reversed |
| Matrix provenance revalidation | 12/12 verified | Original captures/manifests retained; PowerShell array-envelope compatibility decoded without rewriting evidence |
| New inventory-file interchange | Success | `job_a46aef8d08744ae4b4c9a4668cbaec7b`, verified Quality capture using hashed `assets.json` |
| Menu/transition smoke | Success | `job_6a860627482047bd9aea73de911a6575`; menu, CRT, level, in-game menu, disconnect return and resize checks |
| Baseline Vulkan validation, pre-fix | FAIL, retained history | `job_3c84bc7c0c0e466195f731e6c2f7a8ed`; device/sampler/semaphore VUIDs |
| Candidate transition validation, pre-fix | FAIL, retained history | `job_3193e6beea244d9993ecec463f38bcfc`; layout and pipeline VUIDs |
| Renderer validation fix re-verification | PASS, VUID-clean | `job_8164f677bc164c35bd01d39100062e3c` (baseline + candidate arms, full transition smoke) and a direct `run_menu.ps1 -Smoke -Validation` run; zero `VUID-`, zero `Vulkan::ERROR`, zero `NVRHI::ERROR` |
| Post-fix Debug CTest | 6/6 passed | Rebuilt with the renderer fix |

Raw matrix, revalidation and paired-policy results are under
`%LOCALAPPDATA%/QuakeRayMCP/acceptance_20261008/`; `matrix.json` records its exact job-store root.
Menu records are under `menu_jobs/`. The post-fix validation job is under
`%LOCALAPPDATA%/QuakeRayMCP/acceptance_20261009/menu_jobs/`. Raw build/test artifacts remain in
`build/Debug`.

## Matrix observations

| Scenario / preset | Repeat 1 FPS | Repeat 2 FPS | Mean/p95 5% policy on the two unchanged controls |
|---|---:|---:|---|
| Fuma / Balanced | 36.78 | 36.94 | Within tolerance |
| Fuma / Quality | 36.12 | 36.55 | Within tolerance |
| AD hub / Balanced | 48.74 | 47.73 | Regression flag (p95 variability) |
| AD hub / Quality | 42.10 | 42.72 | Within tolerance |
| Bogbottom / Balanced | 23.50 | 23.43 | Within tolerance |
| Bogbottom / Quality | 23.63 | 23.25 | Regression flag (p95 variability) |

These are short controlled tooling observations, not a claimed optimization. The two flagged
unchanged controls demonstrate variance in short-run tails; flags were not changed into PASS.
Target budgets remain unmet. More samples are required before causal performance conclusions.

## Renderer validation errors found and fixed

The pre-fix runs found these error families; all were reproduced with the acceptance runner and
then cleared by `b7bf2414` (each fix re-verified with the same runner):

- `VUID-VkDeviceCreateInfo-pNext-06532` - the device features chained both
  `VkPhysicalDeviceVulkan13Features` and the promoted `VkPhysicalDeviceSynchronization2Features`;
  the 1.3 block now carries `synchronization2` alone.
- `VUID-VkSamplerCreateInfo-pNext-06726` - the shadow-map sampler uses
  `VK_SAMPLER_REDUCTION_MODE_MIN` without the `samplerFilterMinmax` device feature; the feature is
  now queried, required and enabled.
- `VUID-vkQueueSubmit-pSignalSemaphores-00067` - render-finished semaphores were reused per frame
  in flight while their presents could still be pending; the swapchain now owns one
  render-finished semaphore per image, indexed by the acquired image (the validation layer's own
  recommended shape).
- `VUID-VkImageMemoryBarrier2-oldLayout-01197`,
  `VUID-VkDescriptorImageInfo-imageLayout-00344`, `VUID-vkCmdDrawIndexed-None-08114` - two state
  contracts were wrong: the compose chain left the glass-chain sampled reads in the read-only
  layout instead of restoring GENERAL, and the raster overlay announced its sampled-only glass
  mask as UnorderedAccess, so the next compose pass emitted an SRV transition whose GENERAL old
  layout the image was not in. The compose restores the glass reads, hands the overlay's images
  over in GENERAL, and the overlay announces the mask read-only.
- `VUID-vkCmdDispatch-None-08600` - the post-effect pipelines shared binding sets but had
  different push-constant ranges, and the pinned backend only re-binds descriptor sets when the
  set array changes, so consecutive effects dispatched against a stale pipeline layout; every
  post-effect pipeline now shares one push-constant range.
- `Undefined-Value-StorageImage-FormatMismatch-ImageView` on `bloomDest` - the bloom storage
  images are RGBA16_FLOAT while the shaders declared the default rgba32f format operand; the two
  bloom shaders now declare `vk::image_format("rgba16f")`.

The baseline invocation in the historical jobs stopped on its validation gate before candidate
execution, which is why the pre-fix records list different coverage per arm. No validation family
was allowlisted away; the post-fix job's gate passed on both arms.

## Third-party advisory warning resolved (2026-10-09)

`Undefined-Value-StorageImage-FormatMismatch-ImageView` was reported with variable
`rw_luma_history` (format operand Rgba8 against an R16G16B16A16_SFLOAT image view) while the FSR
3.1 upscale provider was active. The qualifier is compiled into the vendor shader, so the fix
required rebuilding the provider from AMD's FidelityFX SDK 1.1.4 sources (one-line
`rgba8`->`rgba16f` change) and vendoring the result; the full analysis, recipe and evidence are
in [fsr-luma-history-format-fix.md](fsr-luma-history-format-fix.md). Validation smoke runs with
the rebuilt provider report zero mismatch warnings with the upscaler active, and the signed
original warned 10 times per run. The rebuilt provider is unsigned (documented trade-off); the
original signed binary remains in git history and the AMD SDK archive.

## Independent safety/API audit and corrections

Four read-only independent reviewers audited the implementation and rechecked corrections.
Consensus findings closed with source evidence and regression coverage:

- Refreshed provenance and original supporting hashes now determine retained comparisons.
- Both suite/menu arms honor failed-build recovery; build/recovery history is not prunable.
- Header freshness includes renderer/generated/third-party/Windows header roots/extensions.
- Idempotent receipt lookup precedes new-job quota admission.

Further directly validated corrections include complete runtime asset inventory with exact
generated-output exclusions, exact overload positions in path tracing, diagnostic-overlay
baseline/performance exclusion, foreign-heavy-process monitoring and robust PowerShell JSON
interchange. Inventory covers extra archives and loose assets, not only pak0–pak2.

All four reviewers' final scoped rechecks found no remaining critical MCP blocker in the
corrective diff. This is a scoped review result, not proof against all races or threats.
Process monitoring samples once per second and cannot detect every sub-second manual action.
The Local mutex coordinates one Windows logon session, not every session on the machine.

## Remaining status

- Formal benefit pilot: excluded/deferred to practical use; no cost-saving claim.
- MCP safety/API acceptance: scoped checks passed, with documented limits.
- Capture/GPU/transition tooling: exercised; provenance and invalid-result gates enforced.
- Renderer Vulkan validation: passed the recorded validation runs (VUID-clean); the FSR provider's
  `rw_luma_history` warning was fixed on 2026-10-09 by the rebuilt provider described in
  [fsr-luma-history-format-fix.md](fsr-luma-history-format-fix.md), with the unsigned trade-off
  recorded there. This is not a general spec-cleanliness proof beyond the recorded scenarios.
- Broad image/gameplay/all-map equivalence: not established by these short tests.
