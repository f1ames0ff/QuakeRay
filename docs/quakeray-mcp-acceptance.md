# QuakeRay MCP acceptance record

Date: 2026-10-09 local time (runs started 2026-10-08 UTC). Source: `b60efef9` plus the
corrective acceptance diff. Formal agent-cost/usability pilot is excluded by owner decision.

## Outcome

MCP safety/API corrections and capture-tool integration passed the scoped checks below.
**Renderer Vulkan validation is not clean.** Do not interpret passing GPU tests, successful
capture jobs or the audit result as a validation-clean engine, visual equivalence or stable
60/45-FPS achievement. Engine validation fixes are a remaining separate blocker.

## Directly executed checks

| Check | Observed outcome | Scope |
|---|---|---|
| Debug build/deployment with `-Tests` | Success | Rebuilt shaders and deployed the engine pack; temporary NVRHI patch reverted |
| Full Debug CTest | 6/6 passed | Frame timing, lighting, ImGui draw, clouds GPU, post-effects GPU, DTAL GPU perf |
| Game matrix | 12/12 jobs succeeded | Fuma, AD hub, Bogbottom; Balanced and Quality; two independent repeats, second sweep reversed |
| Matrix provenance revalidation | 12/12 verified | Original captures/manifests retained; PowerShell array-envelope compatibility decoded without rewriting evidence |
| New inventory-file interchange | Success | `job_a46aef8d08744ae4b4c9a4668cbaec7b`, verified Quality capture using hashed `assets.json` |
| Menu/transition smoke | Success | `job_6a860627482047bd9aea73de911a6575`; menu, CRT, level, in-game menu, disconnect return and resize checks |
| Baseline Vulkan validation | FAIL | `job_3c84bc7c0c0e466195f731e6c2f7a8ed`; errors retained, not suppressed |
| Candidate transition validation | FAIL | `job_3193e6beea244d9993ecec463f38bcfc`; errors retained, not suppressed |

Raw matrix, revalidation and paired-policy results are under
`%LOCALAPPDATA%/QuakeRayMCP/acceptance_20261008/`; `matrix.json` records its exact job-store root.
Menu records are under `menu_jobs/`. Raw build/test artifacts remain in `build/Debug`.

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

## Validation failures

Baseline menu errors:

- `VUID-VkDeviceCreateInfo-pNext-06532`
- `VUID-VkSamplerCreateInfo-pNext-06726`
- `VUID-vkQueueSubmit-pSignalSemaphores-00067`

Candidate full-transition errors additionally include:

- `VUID-VkDescriptorImageInfo-imageLayout-00344`
- `VUID-VkImageMemoryBarrier2-oldLayout-01197`
- `VUID-vkCmdDispatch-None-08600`
- `VUID-vkCmdDrawIndexed-None-08114`

The baseline invocation stopped on its validation gate before candidate execution, so the
candidate was tested separately. Different coverage is not evidence of a new MCP regression;
it is also not proof that every error is harmless. No validation family was allowlisted away.

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
- Vulkan-clean renderer acceptance: **FAIL**, requires engine work before being certified clean.
- Broad image/gameplay/all-map equivalence: not established by these short tests.
