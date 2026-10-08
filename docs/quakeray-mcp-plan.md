# QuakeRay MCP server — analysis and implementation plan

Status: API implementation and scoped integration verification, 2026-10-08. P0–P5 interfaces
live in `tools/quakeray_mcp`, with opt-in mutation/indexing. Coverage and evidence limitations
remain explicit: this is not an all-map visual/performance guarantee or a measured agent-cost
improvement. Effort numbers are estimates, not measurements.

Reviewed source snapshot: `2b12ebbc` (`origin/master`, 2026-10-08). Read
[ARCHITECTURE.md](../ARCHITECTURE.md) and [PERFORMANCE.md](../PERFORMANCE.md) first; these now
provide the canonical navigation and measurement contracts. Counts and source links below
describe this snapshot, not a permanent telemetry schema. No build or game run was performed
for this document review.

The repository entry policy is [AGENTS.md](../AGENTS.md): start with the architecture routes,
then select performance evidence and verify source/binary/settings before forming hypotheses.
Inspect the implicated caller/callee/ownership boundary before expanding the search. The MCP
must reinforce this workflow, not replace it with another repository-wide discovery pass.

The target is a local, developer-workflow MCP server named **quakeray**: a deterministic layer
between an AI agent and the engine that turns orientation, measurement, comparison and
experimentation into typed tool calls with provenance instead of long chains of grep, file
reads, console commands and log parsing.

## 1. Executive summary

The proposal's core idea is plausible for this repository: a specialized MCP can move repeated
navigation, launch and parsing work out of the model's context into deterministic code.
How much agent effort it saves has not been measured. The engine already emits structured
telemetry, and master now includes a bounded stress runner, a per-frame analyzer, manifests
and architecture/performance indexes. Reuse these instead of building a second laboratory.

Several of the proposal's details do not survive contact with the actual codebase:

- The numbers, files and scenario names in the advice are illustrative, not QuakeRay evidence.
  Its proposed per-pass draw calls, visible-object, triangle and memory fields are not in the
  exported captures. The available formats now include a 124-column reporting-window dump
  and a separate 90-column per-frame CSV; the older `feff66d5` dump had 110 columns.
- The example culling diagnosis cannot be adopted without investigation: world frustum and
  backface rejection and PVS selection exist, with configuration-dependent behavior. That
  does not prove complete hierarchical culling or make camera-only rejection safe for ray
  tracing. Findings need precise scope, actual code and measured evidence.
- MCP Tasks is an opt-in extension; the selected Python SDK v2 (`mcp` 2.3.0 as checked on
  2026-10-08) does not implement it. The protocol idea is valid, but this implementation
  needs application-level jobs instead of assuming SDK support.

What is worth building is therefore narrower and more useful than the proposal suggests: an
artifact-intelligence layer over the existing harness (parser, comparison, provenance,
shared ownership in the supported Windows session), then a supervised run layer, then code
intelligence, then
experiments and a run knowledge base. The first two phases are deliberately small enough to be
falsified: a pilot gate compares the MCP against a plain skill-and-scripts workflow on defined
metrics before later phases are funded.

| Phase | Deliverable | Game required |
|---|---|---|
| P0–P1 | Read-only artifact intelligence: parsers, status, scenarios, comparison, docs | No |
| P2 | Supervised run layer over existing stress/menu harnesses: jobs, capture, build, cancellation | Yes |
| P3 | Code intelligence: clangd-backed symbol/reference/call queries | No |
| P4 | Experiment control: tool-owned worktrees, suites, cleanup | Yes |
| P5 | Run history and knowledge base; optional screenshot diff | No |

### Completed implementation scope and verification matrix

| Layer | Implemented | Direct verification / remaining evidence limits |
|---|---|---|
| P0–P1 | Locked SDK, four parsers, schema/resource/prompt contracts, allowlists and bounded reads | In-memory/stdio legacy+modern tests; malformed/blank/nonfinite/context-invalid data rejected |
| P2 | Contained jobs, cooperative stop, ownership, disk reservations/pruning, build receipts, matched provenance, retained comparisons and explicit baseline promotion | Real Fuma captures, verified matching controls, AD-hub overlay capture and isolated menu A/A completed; visual/gameplay/validation remain separate checks |
| P3 | Local approved clangd, symbol/references/callers/callees, bounded path tracing, source/CDB/version freshness | Actual R_DrawViewModelTask → R_DrawViewModel caller resolved; selected-TU coverage and Clang/MSVC diagnostics retained, no complete dynamic/HLSL graph claim |
| P4 | Owned detached worktrees, independent local pinned dependencies, Debug builds, bounded baseline/candidate suites and clean-only removal | Real no-op experiment build and Fuma two-arm suite completed; dirty-removal refusal tested; arm order configurable for repeated/interleaved controls |
| P5 | Retained run/finding search, experiment decisions, protected evidence, diagnostic PNG pixel comparison | Real finding/search roundtrip and menu PNG comparison completed; pixel similarity is not geometry equivalence or a visual acceptance gate |

Defaults remain read-only. Operators configure `QUAKERAY_ENABLE_JOBS`, `QUAKERAY_ENABLE_RESEARCH`
and the approved `QUAKERAY_CLANGD` path explicitly. `QUAKERAY_BASELINE_RUNTIME` is an approved
runtime registry setting, not an arbitrary tool path. Historical milestone text below records
the earlier development state and is superseded by this matrix for current API availability.

Final verification on this implementation: 74 package tests ran with the explicit real-clangd
integration enabled (73 passed; one Windows symlink-privilege skip), six existing analyzer
tests and ownership/budget checks passed, and the Debug CTest suite passed 3/3. Actual retained
controls verified provenance and baseline promotion; live cancellation was cooperative.
The developer pilot's token/cost savings and broad visual/gameplay/driver compatibility remain
unmeasured evaluation questions, not missing API implementations or promised guarantees.

### Historical implementation milestones

- P0 package, exact `mcp==2.3.0` dependency/`uv.lock`, four artifact parsers, explicit schemas,
  stdio entry point and offline startup are implemented on Python 3.10.6.
- P1 implements the six read-only tools, the three repository entry resources and a scoped
  prompt, with header-driven parsing, existing-analyzer reuse and diagnostic comparison.
  OpenCode's project configuration connects the server without changing permission policy.
- Artifact IDs currently refer to a bounded immutable **session cache**, not persisted
  RunRecords. Imported evidence cannot become an accepted baseline; focus and runtime identity
  remain unknown unless established by a later supervised capture. Real retained-capture
  import/acceptance remains pending an approved fixture location.
- Game/build jobs, persistent history, clangd and experiments are not advertised or enabled.
  P2 still needs the documented shared ownership, containment and recovery gates.
  This describes the default read-only mode; the opt-in P2 foundation below is now available.
- Implementation verification: 40 package tests ran (39 passed; the unprivileged Windows
  symlink test skipped, while the junction-escape test passed), plus the existing six analyzer
  tests and stress-budget checks. Both protocol versions passed in-memory and real stdio
  tests. Live OpenCode calls verified readiness, the entry document and all six scenarios.
  The shell's `opencode` CLI was unavailable; connection was checked through the actual MCP
  integration instead. No engine build or game launch was performed.

P2 foundation now implements persistent jobs, start idempotency, bounded polling, creator-only
capability cancellation, Windows kill-on-close containment and interrupted-owner reconciliation.
The build and three direct runners share `machine_guard.ps1`. Runtime adapters use private
copied directories instead of relying on `finally` to undo writes to source-runtime configs.
Failed builds block MCP launch pending review/rebuild; no pre-existing NVRHI patch is reversed
automatically. Build output, runtime/submodule recovery and full live acceptance still require
real deployments and owner-approved assets. Capture records are retained but remain unaccepted.

`QUAKERAY_ENABLE_JOBS=1` is an explicit operator opt-in; the default configuration keeps only
the six inspection tools. The initial P2 surface adds seven jobs/catalog tools, one runtime
ID and candidate-only menu capture. Multi-runtime A/B, destructive pruning, full verified
RunRecords and experiments are not claimed complete. Control tokens are returned only with
the first start receipt, not reissued on an idempotent retry or exposed in history.

Fake-worker tests establish success/failure, conflict/idempotency behavior, timeout/cancel,
cross-owner refusal, assignment-failure fail-closed behavior, grandchildren containment,
forced owner death, restart reconciliation and failed-build launch refusal. These checks are
not a real game benchmark, CTest GPU run or proof of visual correctness.

Cancellation now has a two-second cooperative stop-request phase before contained termination,
with explicit stop-mode recording and stress-loop benchmark-stop/quit handling. The package
suite now runs 56 tests (55 passed, one privilege-dependent symlink skip). Submodules remain
uninitialized and no Debug runtime is present; live acceptance requires an approved runtime
or approval to initialize dependencies/build, plus an approved game/mod/save fixture source.

RunRecord collection now checks complete frame/benchmark/manifest associations, effective
4K/FSR preset and map/sample counts, retains original provenance and publishes the entire
validated batch atomically. Quality captures use `1000/45`, not the Balanced frame budget.
This closes partial-publication and budget-selection defects without claiming live capture
acceptance; binary/control/asset compatibility and image checks remain pending.

Setup, API bounds and current tests are documented in
[tools/quakeray_mcp/README.md](../tools/quakeray_mcp/README.md). The implementation does not
change engine execution boundaries or profiler counters; existing engine performance indexes
remain authoritative.

## 2. The proposal on trial

### 2.1 Protocol claims, corrected

Checked on 2026-10-08 against the primary sources linked in section 11:

- The current MCP specification revision is **2026-07-28** (stateless requests, per-request
  capability negotiation). Servers expose `tools`, `resources` and `prompts`; this part of the
  proposal is correct.
- Asynchronous long-running operations are an **opt-in extension**
  (`io.modelcontextprotocol/tasks`, with `tasks/get` / `tasks/update` / `tasks/cancel`), not a
  universal SDK/client capability. The advice mentions protocol support, not an explicit
  core-only guarantee; the necessary qualification is negotiated support on both sides.
- The Python SDK v2 (package `mcp`, current **2.3.0**, requires Python >= 3.10) explicitly
  **does not implement the Tasks extension**. One server serves both 2026-07-28 and earlier
  clients. TypeScript SDK v2 exists and implements the current revision; the plan targets
  Python by the owner's choice.
- OpenCode v2 configures local MCP servers under `mcp.servers.<name>` with
  `{ "type": "local", "command": [...], "cwd"?, "environment"?, "timeout"?, "codemode"? }`.
  Tool names are `<server>_<tool>`; the local default protocol is `legacy` (up to 2025-11-25),
  which the SDK v2 serves; the tool-execution timeout defaults to 12 hours. Expected execution
  failures must be explicitly mapped to tool results with `isError: true`; malformed protocol
  requests and resource failures can instead be JSON-RPC errors. A structured error envelope
  is our server contract, not an automatic property of OpenCode or every SDK exception.
  Async jobs allow short MCP calls and separately enforced operation deadlines.

### 2.2 Item-by-item assessment

| Proposal component | Verdict | What the plan does |
|---|---|---|
| Tools/Resources/Prompts as the server surface | Adopt | Core of the design; resources carry docs and state, tools carry actions |
| Long-running task-oriented operations | Qualify | Valid negotiated extension; no Tasks support in the selected Python SDK; use application jobs |
| `analyze_frame` returning structured hotspots | Adapt | Prefer validated per-frame captures; distinguish measured costs, static facts and hypotheses |
| Provenance for every claim (file, function, counter, commit) | Adopt | Every run carries commit, build config, scenario, settings, artifact paths and hashes |
| `capture_frame` with warmup/frames JSON | Adapt | Wrap the existing stress runner; distinguish per-frame benchmarks from 5 Hz reporting-window dumps |
| Experiment runner with branches, build, suites | Adapt and defer | Tool-owned worktrees (not branch switching in the user's tree), explicit consent, capped arms |
| `compare_frames` with SSIM | Defer | Screenshot PNGs exist; image diff is a later phase and noisy with temporal denoisers |
| Semantic code graph (`find_symbol`, callers, callees) | Adopt, scoped | clangd over `compile_commands.json`; limits for MSVC flags, macros and HLSL stated |
| Static analyzer queries (`find_render_submission_sites`, ...) | Defer, curated | Query packs over the index plus grep fallbacks; automatic invariant detection is research |
| Architecture invariants as machine-checkable rules | Adapt | Serve ARCHITECTURE.md and real specifications; camera visibility is not a universal RT submission invariant |
| Three layers: static, runtime, experimental | Adopt | Maps to phases P1–P2, P3, P4 in this plan |
| History of runs and experiments | Adopt | Per-run manifests and an append-only index; knowledge-base phase P5 |
| Performance knowledge base | Adopt | `record_finding` / `search_findings` over the run store |
| "15–25 specialized tools" | Adapt | 6 read-only tools first, then ~8 run tools; more only when a phase needs them |
| No single `run_shell_command` tool | Adopt | No generic shell; argument-array subprocess calls only |
| UML/architecture/performance documents as resources | Adopt | Serve the new root indexes and existing `docs/` specifications/diagrams; no duplicate architecture |
| "No evidence of hierarchical visibility culling" | Do not infer | Existing visibility mechanisms do not establish full coverage; evaluate the active path and RT consumers |
| "80% of infrastructure work removed", "$70 burned", SSIM 0.9987 | Not evidence | No measured savings or visual guarantee follows from these examples; use the pilot gate |

### 2.3 What the proposal missed

Operational realities that shape this plan: the single-game-instance rule; focus-dependent
wall FPS; full shader rebuilds; unavailable raw captures/saves in a fresh checkout;
`qconsole.log` truncation; append-only benchmark logs; buffered capture loss on crash; and
the historical non-clean Vulkan validation result in the menu report. The new stress runner
already has focus monitoring, an invocation budget, config cleanup and a named mutex.
The remaining work is shared ownership across all runners/builds, supervised cancellation,
restart recovery and a versioned cross-worktree evidence API.

### 2.4 The honest counterweight

A skill plus the existing scripts (`run_stress.ps1`, `analyze_stress.py`, `run_menu.ps1`,
`run_place.ps1`) already provides much of the runtime value for a single agent on a
quiet machine. The MCP earns its cost where the work is *shared*: one ownership guard across
agents and worktrees, one authoritative parser and comparison, typed errors instead of failed
shell lines, and durable provenance. The plan protects that bet: the core is a plain Python
package usable from a CLI and as a skill fallback, and the pilot gate (section 8.3) can stop
the project after P2 if the server does not demonstrably reduce agent effort.

## 3. What QuakeRay already provides

| Artifact | Produced by | Content | Verified |
|---|---|---|---|
| `stats-<datetime>.dump` | `rt_stats_dump_start/end` | **Reporting-window** CSV: 124 columns at this snapshot (38 non-frame/wait Quake slots, 17 GPU buckets, 38 backend avg/max columns and counters); default 0.2 s interval, 30 s / 700-sample cap; empty means unavailable | [writer](../Quake/gl_vidsdl.c#L1364), [slot enum](../Quake/glquake.h#L661) |
| `qperfdump.log` | `rt_stats_dump` | one readout snapshot, not a synchronized CPU/GPU frame capture | [writer](../Quake/gl_vidsdl.c#L1154) |
| `benchmark.log` | `rt_bench` | appended header, 40 CPU-slot avg/max rows, `cpu.main`, cluster counts, effective settings and `frame_samples file=... samples=... dropped=...` | [report](../Quake/gl_vidsdl.c#L786) |
| `benchmark-frames-*.csv` / `*.frames.csv` | `rt_bench` / stress runner copy | 90 columns: frame/time/context, 40 CPU slots, 19 backend CPU phases, 17 GPU buckets, call counters; buffer capacity 32,768 frames | [writer](../Quake/gl_vidsdl.c#L732), [buffer](../Quake/gl_vidsdl.c#L422) |
| Stress runner and analyzer | `tests/perf/run_stress.ps1`, `analyze_stress.py` | one save/preset per invocation, at most two repeats, 300 s runtime budget, focus checks, config restore, artifact copies, manifest hashes; validates frames and calculates interval p95/p99 | [runner](../tests/perf/run_stress.ps1), [analyzer](../tests/perf/analyze_stress.py) |
| Legacy cluster diagnostic runner | `tests/perf/run_place.ps1` | `gpumax`/`cpumax`, sampling 1/0 arms; particles disabled; restores config only on the normal exit path, not in `finally`; lacks a shared instance guard | [runner](../tests/perf/run_place.ps1), [settings](../tests/perf/qr_perf_common.cfg) |
| Menu A/B runner | `tests/perf/run_menu.ps1` | baseline/candidate runtimes, captures stats dumps, UI-only frame assertions, smoke checks | `docs/drawframe-menu-performance.md` |
| Legacy comparison tooling | `perf/compare_benchmark.ps1`, `tests/perf/analyze_bench.py` | coarse 20% tolerance and tables; not a safe acceptance oracle: missing metrics are skipped, absent baselines are created automatically, hits/misses share the FPS direction | [comparison](../perf/compare_benchmark.ps1#L75) |
| Build | `build_win.ps1` | Debug/Release (script default: Release; the server always passes Debug explicitly), optional tests, unconditional full shader rebuild on full builds, `compile_commands.json` export | `build_win.ps1:3,105,176-181` |
| Repository entry policy | `AGENTS.md` | architecture-first navigation, evidence selection before hypotheses, scoped inspection and index maintenance | [entry point](../AGENTS.md) |
| Architecture and evidence indexes | `ARCHITECTURE.md`, `PERFORMANCE.md`, `docs/` | real entry points, counter scope, scenario/save hashes, binary identities, retained capture locations and scoped results | [architecture](../ARCHITECTURE.md), [performance](../PERFORMANCE.md) |

Known gaps: no MCP facade or consolidated cross-worktree run catalog; no semantic call index;
no shared build/game/GPU-test ownership across all entry points; no general process-tree
supervision or visual-diff acceptance system. `run_stress.ps1` already supplies a capture arm,
manifests and a Python per-frame parser; reuse them. Both recorders buffer data, so a crash
before reporting can lose the current measurement even when earlier log blocks survive.

### 3.1 Measurement contract (mandatory for every API response)

Use [PERFORMANCE.md](../PERFORMANCE.md#measurement-contract), not similar-looking column
names, as the authority:

- `cpu.frame_ms` brackets `SCR_UpdateScreen`, not the entire host/game frame. CPU slots can
  contain other slots; viewmodel includes light/cluster publication. Never add overlapping
  timers or infer an "other CPU" remainder by subtracting them.
- Overlay dumps contain window maxima/averages; p95 of those samples is a **window-statistic
  percentile**, not frame-time p95. Return `aggregation_kind` and `sample_count`; a one-shot
  `qperfdump.log` cannot yield frame percentiles.
- For performance, reuse `analyze_stress.py`: FPS is `1000 / mean(interval_ms)`, excluding
  the initial zero partial interval. Preserve its linear-interpolation percentile rule and
  invalid-capture checks. Legacy header FPS and GUI-derived FPS are different quantities.
- GPU values are delayed completed-query snapshots of the timed RHI command list, not a
  synchronized GPU duration for the CPU row, legacy submission, display latency or scanout.
  Filter by `gpu_valid`; no valid GPU samples means unavailable, not zero. Store valid sample
  counts separately. No GPU world/particle/viewmodel attribution exists in these buckets.
- Preserve raw headers, unknown fields, validity flags and producer version. Never hardcode
  110/124/90 columns as a parsing requirement; those are snapshot descriptions. Missing data,
  a skipped measured pass with value zero, and an invalid reading are distinct states.
- Take screenshots after benchmark stop; run analysis after the game exits. High-detail overlays,
  validation layers and extra timers have overhead: compare only equivalently instrumented
  arms, and label diagnostic controls rather than claiming target-quality improvements.

### 3.2 Scenarios and readiness

Seed the planned registry from the real saves in PERFORMANCE.md: `qr_fuma_start` → `ad_tfuma`,
`qr_ad_start` → `start`, `qr_gpu_heavy` → `ad_swampy`, each with Balanced/Quality presets from
`qr_audit_max.cfg`. A scenario identifies the save/map/mod hashes, expected settings, live
simulation policy, warmup, capture duration and sound/profiling modes, not just a filename.
These saves and proprietary game/mod assets remain external prerequisites; documentation
does not imply they exist in every checkout.

P1 lists scenarios and missing prerequisites. P2 uses `run_stress.ps1` as the default live
quality-preserving runner; `run_menu.ps1` is a separate menu/transition profile. Legacy
`run_place.ps1` remains an explicitly labeled cluster ablation and cannot certify particle
or target-quality performance. New scenarios require an approved fixture location and map
check, not arbitrary console input.

### 3.3 Reuse of all three repository entry files

| File | MCP responsibility | What must not be inferred |
|---|---|---|
| `AGENTS.md` | Serve the entry policy; prompts follow architecture → relevant evidence → source/binary/settings verification → scoped caller/callee/ownership inspection | Workflow text does not authorize launches, edits or permission changes |
| `ARCHITECTURE.md` | Reuse subsystem routes, real entry points, CPU/GPU ordering, visibility facts and synchronization/lifetime boundaries | A curated graph is not a complete dynamic call graph; static facts are not measured costs |
| `PERFORMANCE.md` | Reuse the measurement contract, targets, scenarios, binary/capture identities, compatible controls, priorities and workstream boundaries | Historical summaries are not fresh measurements or proof that referenced raw files exist locally |

Seed target budgets explicitly from PERFORMANCE.md: 3840x2160/FSR Balanced targets 60 FPS
(`1000/60` ms), Quality targets 45 FPS (`1000/45` ms); validate the effective preset before
reporting target attainment. These targets remain unmet in the recorded campaign. The B0
and detailed C0/C1/C2 captures have different instrumentation histories: B0 must not silently
become C2's paired control. Also, `qr_gpu_heavy` is a scenario name, not a GPU-bound diagnosis.

When an agent changes a listed execution boundary or profiler counter, it must update the
affected ARCHITECTURE.md route/scope/snapshot and PERFORMANCE.md evidence as required by
AGENTS.md. New conclusions carry binary/settings/asset identities and raw evidence; unknown
costs stay unknown and unverified hypotheses stay unverified. The MCP supplies a bounded
documentation-update packet with an experiment result; the agent applies approved changes
through its normal editing workflow. No automatic rewrite or second authoritative copy of
these indexes is introduced; P5 stores experiment evidence and links back to them.

## 4. Architecture

```
Agent (OpenCode / any MCP client)
        │  stdio, tools / resources / prompts
        ▼
quakeray MCP facade (thin)          tools/quakeray_mcp
        │
        ├── core library (CLI-usable, testable without MCP)
        │     ├── parsers      frame CSV/analyze_stress, window dump, benchmark blocks
        │     ├── compare      explicit comparability/metric policy (not legacy script verdicts)
        │     ├── jobs         run manifests, polling, cancellation, salvage
        │     ├── ownership    shared measurement/build guard (scope explicit)
        │     ├── store        runs/index.jsonl, baselines, docs index
        │     └── codeintel    clangd driver (phase P3)
        │
        └── subprocess drivers (reuse existing procedures, harden lifecycle in P2)
              run_stress.ps1   run_menu.ps1   run_place.ps1   build_win.ps1
```

### 4.1 Decisions

- **Location:** `tools/quakeray_mcp/` in this repository, now implemented for P0–P1;
  the tool is coupled to the runners, formats and worktrees, and must version with them.
  Layout: `pyproject.toml`, committed lockfile, `src/quakeray_mcp/`, `tests/`, `fixtures/`.
- **Runtime:** Python 3.10+ (3.10.6 observed locally), managed by `uv`; initially pin
  `mcp==2.3.0` and commit the full `uv.lock`. An interval such as `mcp>=2.3,<3` is not an exact
  pin. The parsing/comparison core needs stdlib only; the MCP SDK has its own transitive
  dependencies. Install explicitly before connecting, not during each MCP startup. Entry
  point `python -m quakeray_mcp`; stdio transport; server name `quakeray`.
- **Single measurement authority:** use the existing PowerShell driving sequences and the
  existing `analyze_stress.py` calculations. Reuse its functions or CLI; if a shared library
  extraction is needed, prove parity before replacing it. P2 must harden shared ownership,
  cancellation, exit-code/marker checks and cleanup. This is more than adding a capture flag;
  no duplicate keyboard-driven measurement implementation is planned.
- **Jobs:** only approved build/runtime/index operations use background jobs. Read-only
  git/process inspection stays synchronous. `job_id` identifies an operation; each completed
  measurement gets a separate `run_id` (a job can yield two repeats or menu baseline/candidate
  runs). `get_run` becomes `get_job(job_id, wait_seconds <= 10)`; no long MCP wait is needed.
  Detailed lifecycle and process-containment requirements are in section 4.3.
- **State:** `%LOCALAPPDATA%\QuakeRayMCP\<repo-id>\` contains `jobs/`, `runs/`, `index.jsonl`
  and, in P4, `experiments/`. Derive `repo-id` from the canonical Git common directory so
  sibling worktrees share a catalog. This is per-user state, not the machine reservation.
  Manifests are atomically replaced; index appends use a store lock across server processes.
  Evidence copies are immutable; imports are explicitly marked as imported, never executed.
- **Ownership:** one shared guard, not independent build/game locks that can miss each other,
  serializes builds, game runs and GPU tests. Reuse and extend the existing stress-runner
  mutex; define scope and direct-runner integration before exposing runtime tools (4.3).
- **Measurement validity:** reuse existing focus/context/completeness checks; a contaminated
  run is retained as evidence but excluded from acceptance. Missing or invalid metrics are
  not zero. Comparisons follow section 5.6, including build/assets/instrumentation identity.
- **Safety:** no generic shell or raw console-command tool. Resolve registered runtime/scenario
  IDs to canonical allowlisted roots; reject path traversal and symlink/junction escapes.
  Validate save/map tokens and cvar overrides, since argument arrays do not prevent injection
  inside generated Quake config text. Cap responses, never publish inherited environment
  secrets, and treat logs/repository text as untrusted data. Experiment/cleanup modes default
  to disabled at the server and still require client approval when enabled.
- **Degradation:** the server starts and answers `server_status` even with no build, no game
  data or no saves; action tools return typed `ENV_MISSING_*` errors with remediation.

### 4.2 OpenCode configuration

Project-local `opencode.jsonc` is now registered and tested. Install the package/environment
explicitly with `uv sync --locked --project tools/quakeray_mcp --python 3.10`; normal startup
uses the installed environment without downloading or resolving dependencies. The reference
snippet below includes a recommended ask policy for an operator to apply deliberately;
the actual project configuration inherits existing permissions and does not override them.
Merge with existing configuration rather than replacing it:

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "servers": {
      "quakeray": {
        "type": "local",
        "command": ["uv", "run", "--offline", "--frozen", "--no-sync", "--project", "tools/quakeray_mcp", "python", "-B", "-m", "quakeray_mcp"],
        "cwd": ".",
        "environment": { "QUAKERAY_ROOT": ".", "QUAKERAY_BUILD": "build/Debug" },
        "protocol": "legacy",
        "timeout": { "startup": 30000, "catalog": 30000, "execution": 30000 }
      }
    }
  },
  "permissions": [
    { "action": "quakeray_*", "resource": "*", "effect": "ask" }
  ]
}
```

Keep the default local protocol (`legacy`; the SDK serves both revisions). Tool permissions
initially ask for every tool; approve the six read-only tools individually after review.
Keep launch/cancel/cleanup/experiment tools approval-gated. Permission hints in tool metadata
are not enforcement, and other MCP clients need equivalent consent. Code Mode remains on;
OpenCode exposes `tools.quakeray.<tool>` and permission names `quakeray_<tool>`.
Resolve `QUAKERAY_ROOT` against the configured workspace at startup, not against whichever
worktree an IDE happens to have indexed. Verify this exact launch in P0.

### 4.3 Jobs, shared ownership and recovery

These are requirements to implement and test, not claims about today's scripts:

1. Persist a job and its normalized arguments before spawning. Every `start_*` accepts an
   `idempotency_key`; a repeated key with the same operation/arguments returns the same job,
   while different arguments return `IDEMPOTENCY_CONFLICT`. Losing a start response must not
   launch a second game. Workers belong to the server lifespan, not the request handler scope.
2. Use the existing `Local\QuakeRayPerformanceRun` guard for the supported single Windows
   logon session. It already serializes stress runners, but not all builds or other scripts.
   A shared helper must extend that guard to every supported heavy entry point. Local mutexes
   are **session-scoped**, not universal machine-wide locks; multi-logon support needs a
   deliberately shared `Global\` guard/ACL and migration of all cooperating launchers.
3. One dedicated worker owns the guard on its acquiring thread through child exit and cleanup.
   Do not hold it in a Python supervisor and then deadlock a child runner that reacquires it.
   Supervised and direct script paths must use the same helper and a verified ownership
   handoff, not an agent-supplied `skip_lock` flag. Owner metadata is diagnostic, not a second
   competing lock. Check game/build/runner processes **under** the guard and again at launch.
   Facade admission waits at most five seconds; foreign activity returns `GAME_BUSY`/
   `MACHINE_BUSY`. No unbounded queue and no focus stealing.
   A lock-unaware manual launch cannot be atomically prevented: detect it and invalidate the
   owned measurement without stopping the foreign process.
4. Before running any child code, create its Windows Job Object, set kill-on-close, create the
   worker suspended, assign it and then resume. The supervising server retains the only
   non-inheritable job handle; no breakaway is permitted. Assignment/nested-job failure is
   `PROCESS_CONTAINMENT_FAILED`, not permission to launch uncontained. Test grandchildren,
   OpenCode's actual parent-process context and server death (Microsoft sources in section 11).
5. Stress jobs keep the runner's one-save/preset and at-most-two-repeat policy. All runtime jobs
   initially have a **300-second** facade budget, including admission/loading/cleanup; menu
   and legacy profiles additionally declare their bounded arm count. Build jobs have a separate
   bounded default (proposed 1,500 seconds), not the MCP execution timeout. Polling never
   resets them or the caller's cumulative **30-minute subagent-wait cap**; an occupied machine
   is deferred rather than repeatedly polled indefinitely.
6. Normal completion requests benchmark stop/quit, awaits exit and validates artifact
   completeness. `cancel_job` first requests graceful stop, then terminates only its own Job
   Object after a bounded grace period. Completed files survive; in-memory samples may not.
   Enforce creator/control ownership; another agent's session cannot cancel a job merely by
   knowing its ID. OpenCode's optional session metadata is correlation, not authentication;
   cross-owner control requires separately verified consent.
   Canceling a `get_job` call does not cancel the job. Server disconnect/death terminates its
   contained children; jobs do not continue across reconnects in the stdio MVP.
7. Journal original mutable config bytes and whether files existed **before** changing them.
   `finally` alone does not run after forced termination. After containment is confirmed empty,
   restore only job-owned changes, with expected-byte/hash checks; unexpected edits need human
   review. Failed/canceled builds can leave the NVRHI patch or an incomplete `qray.pkz`: mark
   that runtime unusable pending recovery/rebuild, never reverse a pre-existing user patch.
8. On restart, identify owners by instance ID plus process creation identity, not PID alone.
   Mark orphaned job manifests `interrupted`; salvage existing evidence. Do not adopt or kill
   an unrelated survivor based on a stale PID. There is no broad `cleanup_orphans` kill tool.

## 5. Tool, resource and prompt catalog

Tables are API sketches, not finished JSON Schemas. P0 must publish and test explicit input
and output schemas (`type: object`, bounded arrays/strings, enums/ranges and no unknown input
properties) against the pinned SDK. IDs resolve through server configuration/registries;
untrusted arguments never select an executable, arbitrary script, Git command or shell.

### 5.1 P1 — read-only artifact intelligence (no game)

| Tool | Input → output | Depends on |
|---|---|---|
| `server_status` | `{}` → readiness checks, source identity, registered runtimes, ownership scope and server capabilities | git/filesystem/process scan; engine version is not an executable hash |
| `list_scenarios` | `{runtime_id?, kind?}` → registered scenarios, presets, expected map/save hashes and missing prerequisites | scenario registry, PERFORMANCE.md, approved runtime roots |
| `read_stats` | `{source, summary=true, cursor?, limit<=100}` → format, aggregation kind, validity, metrics/hotspots, evidence routes and bounded raw-row page | existing analyzer for frame CSV; window/one-shot parsers; `source` selects exactly one approved artifact ID or import path |
| `read_benchmarks` | `{source, last_n<=20}` → complete/incomplete blocks, settings and validated `frame_samples` references | `benchmark.log`; imports are labeled and never trusted from their filename alone |
| `compare_runs` | `{baseline, candidate, policy_id?}` → comparability, metric deltas, regression/quality/validation states and excluded metrics | verified RunRecords and metric policy (5.6); legacy block imports remain limited |
| `get_architecture` | `{query?, document?, cursor?}` → entry policy, root index, counter-to-function routes or requested document section with source identity | AGENTS.md, ARCHITECTURE.md, PERFORMANCE.md, `docs/`; not an automatically inferred call graph |

Resources: `quakeray://status`, `quakeray://scenarios`, `quakeray://docs` (index),
`quakeray://docs/{name}`, `quakeray://runs/latest`, `quakeray://benchmarks/latest`, and
`quakeray://artifacts/{id}` (metadata/explicit bounded reads of stored raw evidence).
Prompt: `investigate_slow_scene` (status → existing capture → measurement contract → code
route). Only advertise resources/prompts the installed phase supports; `latest` returns an
explicit empty state when there are no runs. Launch/code-query steps are added in P2/P3.

Allowlisted resource aliases `quakeray://docs/entry`, `quakeray://docs/architecture` and
`quakeray://docs/performance` map to AGENTS.md, ARCHITECTURE.md and PERFORMANCE.md respectively.
These are the actual root documents, not generated copies under `docs/`.

### 5.2 P2 — supervised runtime layer (game required)

| Tool | Input → output | Depends on |
|---|---|---|
| `start_capture` | `{scenario_id, runtime_id, idempotency_key, warmup_s=8, duration_s=6}` → job receipt | `run_stress.ps1 -StatsLevel 3`; collects diagnostic window + frame artifacts, not a target-performance control |
| `start_benchmark` | `{scenario_id, runtime_id, idempotency_key, warmup_s=8, seconds=6, repeats=1, profile="stress"}` → job receipt | default `run_stress.ps1 -StatsLevel 0`; legacy cluster profile is opt-in and labeled diagnostic |
| `start_menu_ab` | `{candidate_runtime, baseline_runtime?, idempotency_key, seconds=8, smoke=false, validation=false}` → job receipt and per-arm run IDs when complete | `run_menu.ps1`; validation mode is never silently disabled or declared clean |
| `start_build` | `{runtime_id, idempotency_key, config="Debug", tests=true, parallel?}` → job receipt/build identity | explicit `build_win.ps1 Debug -Tests`; compiling tests does not run them, so test result remains `not_run` until a separately authorized check |
| `get_job` | `{job_id, wait_seconds=0 (0..10)}` → job state, resulting run IDs, validity, artifacts and bounded log tails | job store; P1 `read_stats`/run resources read finished measurements |
| `cancel_job` | `{job_id}` with verified control ownership → cancel-requested/completed state and salvage status | only approved jobs owned by this server instance; idempotent, no arbitrary PID |
| `list_runs` | `{last_n?, scenario?}` → summaries | `index.jsonl` |
| `prune_runs` | `{keep_last?|days?, dry_run=true}` → plan/result | permission-gated |

Resources: `quakeray://runs/{id}`, `quakeray://baselines/{scenario}`.
Prompt: `compare_ab` (approved baseline → identical scenario/profile → short repeated runs →
compare → record evidence). Warmup is 2–30 seconds, measurement 2–25 seconds and stress repeats
1–2, matching the current runner; scenario policy may narrow these ranges. Job admission can
return busy without launching. Build completion alone never implies benchmark/test success.

### 5.3 P3–P5 — code intelligence, experiments, knowledge base

- P3: `find_symbol(query)` → stable symbol ID/locations; `get_references(symbol_id)` → locations;
  `get_callers/get_callees(symbol_id)` → bounded resolved edges; `trace_path(from_id, to_id,
  depth<=8)` → known paths/unresolved edges; `refresh_index(runtime_id)` → index job/status.
  All results carry backend, source/compile-database fingerprints, coverage and truncation.
- P4: `create_experiment` (tool-owned worktree under the state root, never the user's tree),
  `start_experiment_suite` (capped candidate/baseline runs with identical fixture identities,
  at most a small fixed matrix — no 2^n ablation), `finish_experiment`
  (record/reject), `remove_experiment` (permission-gated cleanup).
- P5: `record_finding`, `search_findings`, optional `diff_screenshots` (PNG pairs with matched
  camera/settings/assets and temporal/noise controls; record differing build hashes explicitly).
  Calibrate same-binary repeatability before using scores. SSIM alone does not establish
  geometry/lighting/gameplay equivalence; this is not an MVP pass/fail gate.

Later API schemas must distinguish experiment decisions from measurement facts. A finding
references immutable run/artifact IDs, affected symbols and a hypothesis/verdict; an
experiment's `rejected` state does not delete its evidence or edits. Resources remain useful
even if a client does not expose MCP prompts; important operations never depend on prompts.

### 5.4 Error taxonomy

`INVALID_ARGUMENT`, `ENV_MISSING_BUILD`, `ENV_MISSING_GAME_DATA`, `ENV_MISSING_SCENARIO`,
`GAME_BUSY`, `MACHINE_BUSY`, `LOCK_TIMEOUT`, `BUILD_FAILED`, `RUN_TIMEOUT`, `RUN_CRASHED`, `RUN_INTERRUPTED`,
`FOCUS_UNVERIFIED`, `PARSE_FAILED` (raw kept), `INCOMPARABLE`, `VALIDATION_ERRORS`,
`INDEX_UNAVAILABLE`, `CANCELLED`, `PERMISSION_DENIED`, `IDEMPOTENCY_CONFLICT`,
`PROCESS_CONTAINMENT_FAILED`, `RECOVERY_REQUIRED`, `INTERNAL`. Expected tool failures use
`isError: true` with a stable JSON error object (code/message/retryable/remediation/evidence),
plus a short text summary for clients without structured-output support. Use SDK error/result
APIs explicitly; never return a plain error string as a successful result. A successful
`get_job` response may describe a failed job: distinguish transport/tool status, job status
and measurement validity. Unknown resources follow the resource protocol-error path.

### 5.5 Common data and provenance contracts

- **JobRecord:** `schema_version`, `job_id`, `operation`, idempotency key, normalized arguments,
  owner-instance/caller correlation, created/deadline times, state (`accepted`, `running`,
  `cancelling`, `succeeded`, `failed`, `cancelled`, `interrupted`), exit/error, artifact IDs,
  `run_ids` and recovery status. Persist ownership/deadline transitions, not just final output.
- **RunRecord:** `schema_version`, immutable `run_id`, optional producing `job_id`, scenario
  and profile, format/aggregation kind, source HEAD and dirty-source fingerprint, submodule
  SHAs, executable SHA256, build/compiler/config identity, engine pack/mod/save/config hashes,
  effective settings, hardware/driver, instrumentation mode, focus/context validity, frame
  counts/dropped samples, typed metric summaries and artifact hashes. Record unknown fields
  as unknown; HEAD alone cannot identify a swapped executable or an uncommitted candidate.
- **MetricSummary:** name, unit, scope/parent, aggregation, valid/unavailable sample counts,
  value/statistics and evidence (artifact/column/rows, source symbol/location, source snapshot).
  Separate `measurement`, `static_observation` and `hypothesis` evidence. No synthesized GPU
  particle pass or definitive bottleneck diagnosis is allowed without an actual measurement.
- **ArtifactRef:** ID, kind, producer, relative stored path, byte count, SHA256 and completeness.
  Preserve the `frame_samples` association, not whichever CSV/dump is newest in the directory.
  Verify sample counts/header metadata and stable complete files after the writer/game exits;
  file existence alone is not writer completion.

Reuse the stress manifest and `.summary.json`, adding missing provenance rather than
overwriting their original fields. Imports with insufficient metadata remain inspectable but
cannot silently become accepted baselines. Index writes are serialized; cache keys include
artifact hashes, schema/producer and analyzer version. Initial bounds: 32 KiB inline response,
8 KiB log tail, 100 rows per page (whichever bound is reached first); full raw evidence is
explicitly retrieved via artifacts.
No NaN/Infinity JSON values and no zero substitution for unavailable metrics.

### 5.6 Comparison and acceptance policy

Do **not** copy `compare_benchmark.ps1` acceptance semantics unchanged. That script skips
missing/nonpositive baseline metrics, creates a baseline if none exists, and treats both cache
hits and misses as higher-is-better. It is a historical convenience, not a scientific oracle.

Require matching scenario/save/map/mod assets, profile/resolution/effective settings,
hardware/driver, build configuration and instrumentation before issuing an acceptance verdict.
Executable/source hashes are recorded but intentionally differ for a code experiment. Permit
only an explicit, recorded set of treatment differences; otherwise return `incomparable`.
An inspection that successfully explains incomparability is not a failed tool call; its
comparison verdict is distinct from `isError` execution failures such as an unknown run ID.
Unknown identity, dropped/partial frames, lost focus, paused/menu samples or missing required
metrics produce `not_checked`/`invalid`, never PASS. Overlay captures and per-frame captures
are not interchangeable controls. Validation-layer captures are diagnostic, not performance
baselines; known historical VUID families must not be silently suppressed.

Timing metrics are lower-is-better; interval-derived FPS is higher-is-better. Workload counts
(API calls, particles if later available, cache hits/misses) are diagnostic/integrity metrics,
not automatic performance directions. Report absolute deltas and percentage deltas; a zero
baseline has no percentage denominator. Track performance, visual/gameplay checks, crash and
validation results independently; no screenshot means visual status `not_checked`, not PASS.

The old 20% tolerance is available only as a labeled legacy diagnostic policy. Set target
budgets and regression thresholds explicitly per scenario/metric. Repeat and interleave short
baseline/candidate runs on a quiet machine; report per-run summaries and observed variability.
One short run supports a scoped observation, not a statistical or all-map guarantee. Promoting
a run to a named baseline is a separate explicit owner decision, never a compare side effect.

## 6. Code intelligence (P3)

Use ARCHITECTURE.md's entry points and counter routes immediately in P1; an AST graph is not
a prerequisite for useful orientation. The Debug build exports `compile_commands.json`, and
clangd/clang-cl 17.0.1 was observed locally. This does not establish parsing coverage for this
MSVC build, nor availability on another contributor's machine.

P3 begins with `clangd --check` on selected Quake C and renderer C++ translation units. Retain
diagnostics; validate definitions/references and call-hierarchy support before advertising
them. Fingerprint the CDB, source/dirty state, tool version and flags; return stale/incomplete
coverage explicitly. Do not rewrite the build database or allow arbitrary `--query-driver`
executables. Refresh/indexing is scheduled outside measurements.

Golden routes include `R_DrawViewModelTask` → `R_DrawViewModel`, `RT_Bench_Report` →
`RT_Bench_WriteFrames`, and `qrUploadGeometry` → `VulkanDevice::UploadGeometry` → `Scene::Upload`.
Validate overloads, static functions and macro expansions rather than guessing edges from
matching names. Function-pointer dispatch, string command registration and HLSL remain
partially unresolved; no complete dynamic call graph or universal visibility proof is promised.
`trace_path` follows only known bounded edges and includes unresolved/truncated frontiers.
A grep fallback returns **text matches**, not fabricated callers/callees or proof of absence.

## 7. Experiment control (P4)

An experiment uses independent baseline/candidate worktrees at recorded revisions, with
private build/runtime directories and pinned submodule checkouts. Git objects may be shared;
mutable NVRHI files, generated shaders, `qray.pkz`, configs and logs must not be shared through
writable symlinks/hardlinks. Seed licensed assets/saves only from approved sources, verify
hashes and never overwrite a hardlinked baseline asset.

`git archive` is an optional source export, **not a complete build snapshot**: it does not
populate `third_party/nvrhi` or `third_party/openal-soft` gitlinks. The default is independent
worktrees with separately initialized exact submodule SHAs; an archive baseline must explicitly
reconstruct those dependencies. Missing objects/network access need owner approval, not an
implicit fetch/install. Preserve existing user branches, dirty files and submodule changes;
importing an uncommitted patch into a candidate requires explicit scope/consent.

Build each source/config identity once, deploy and hash its runtime, then reuse that frozen
binary for scenario repeats. **Each scenario run does not require a rebuild.** Shared timers,
instrumentation and harness changes must exist in both arms; an old baseline without compatible
captures needs a separate instrumentation-only control before timing comparisons.

A suite is a sequence of bounded jobs (one save/preset per game invocation), not one long GPU
reservation. Cap builds/runs/disk use up front; run baseline/candidate in interleaved order and
release ownership between invocations. Include a primary and a guard scenario with live
simulation and image/gameplay checks. No automatic 2^n ablation, bisect, merge or branch switch
inside the user's worktree. `finish_experiment` records a decision; it never rolls back another
owner's edits or deletes evidence. Removal requires explicit confirmation, registered ownership
and dirty/untracked-file inspection; `rejected` is not authorization for force-removal.

## 8. Roadmap and verification

### 8.1 Phases

The MVP is P0–P2; P3–P5 are optional expansion. P0 starts from the reviewed tree; P1 follows
P0. P2 requires parser/API parity and approved
runtime ownership. P3 can follow P1 independently of game-data availability. P4 requires P2's
failure/ownership checks and owner consent, not a complete call graph; P5 history/search can
grow from P2 while screenshot work remains optional. These are gates, not a demand to build
every layer before testing its value.

| Phase | Content | Exit criteria (verification) | Effort estimate |
|---|---|---|---|
| P0 | Package, exact SDK/lockfile, four artifact formats, schemas, `--selfcheck`, OpenCode wiring | Python 3.10 install/import succeeds; fixture parsing works; legacy and modern in-memory clients validate input/output/error schemas; real stdio startup is offline/clean | 1–2 days |
| P1 | Six inspection tools/resources, analyzer reuse, metric policy, all three repository entry documents | Entry/architecture/performance resources return the current files and source identity; prompts follow AGENTS.md routing; analyzer summaries match; missing/incompatible evidence never PASS; no runtime launch | 2–4 days |
| P2 | Job supervision, common runner/build guard, durable cleanup, capture/menu/build adapters | Two server processes/direct runner/build requests do not overlap; duplicate start keys launch once; failed containment launches nothing; timeout/server death terminates only owned children; restore config after forced termination; artifacts equal direct-harness results | 1–2 weeks |
| P3 | clangd spike, selected symbol/reference/call tools, freshness and text fallback | Selected real routes and unresolved cases are verified; diagnostics/coverage recorded; stale index never masquerades as current AST evidence | 3–8 days |
| P4 | Isolated worktrees/dependencies, bounded suites, owner-approved cleanup | No-op paired control measures repeatability; an actual candidate has matched assets/instrumentation; hashes and user's files/submodules are preserved; dirty worktree removal is refused | 1–2 weeks |
| P5 | History/finding search, optional existing-image comparisons | Seeded evidence queries return expected run IDs; retention protects baselines/active evidence; absent visual checks remain `not_checked` | 2–5 days |

Effort is not measured. The shader script's comment estimates about a minute; it is not a
measured end-to-end build duration. Startup deadlines, 300-second stress budgets, 30-second
window captures and 32,768-frame buffers are coded limits, not observed operation costs.

### 8.2 Testing without hardware

Parser tests use committed synthetic fixtures plus approved real captures with private paths
stripped; never require missing local `build/audit-*` archives. Cover both the old 110-column
and current 124-column dumps, frame CSV, blank/zero/unknown fields, nonfinite values,
truncated/interrupted/duplicate samples, invalid context, missing GPU validity and renamed
producer fields. Import paths escaping an approved root must fail.

Reuse `test_analyze_stress.py` and `test_stress_budget.ps1`. Job/lock tests launch only fake
sleep/crash/child-tree workers: test server death, nested jobs, PID reuse, idempotency, malformed
job manifests, restore conflicts and cross-process index writes. Test SDK `Client(server)`
in legacy and modern modes **and** a real stdio subprocess; in-memory success alone does not
prove framing, startup or cleanup works. Stdout must contain only protocol messages; SDK,
worker and build logs go to stderr/artifacts.

Commands available now (no game/GPU):

```powershell
python -B -m unittest discover -s tests/perf -p test_analyze_stress.py
powershell -NoProfile -File tests/perf/test_stress_budget.ps1
```

Planned after package creation/setup (not commands executed by this review):

```powershell
uv sync --locked --project tools/quakeray_mcp
uv run --offline --frozen --no-sync --project tools/quakeray_mcp python -B -m unittest discover -s tools/quakeray_mcp/tests
uv run --offline --frozen --no-sync --project tools/quakeray_mcp python -m quakeray_mcp --selfcheck
opencode mcp list
```

Opt-in supervised P2/P4 checks require owner-approved licensed assets/saves and a free machine:
the Debug build/deployment, applicable CTest checks, the one-save/preset stress command and
artifact verification are specified in PERFORMANCE.md's reproduction section. Capture budgets
and test selection are explicit; never run the real game/GPU tests in the default unit suite.

### 8.3 Pilot gate (falsification)

After P2, run five matched investigation tasks within a pre-agreed time budget, comparing MCP
against the **current** architecture/performance indexes and skill+scripts (not an artificially
unstructured baseline). Fix the agent model/effort, source/binary/settings, task acceptance
requirements and cold/warm-cache policy; counterbalance task order where practical. Record
turns, tool calls/failures, wall time, comparable evidence produced and measurement mistakes.
Use actual usage/cost data when available; otherwise mark it unavailable rather than estimate
token savings from transcript length.

Agree a continuation criterion before running (for example lower median time to valid
evidence without more measurement failures). Five tasks are a feasibility pilot, not a claim
of model equivalence or a statistically proven 80% saving. If the benefit is unclear, stop
expansion and retain parsers/ownership/history as CLI/skill functionality.

## 9. Operations, risks, exclusions

### 9.1 Failure modes

| Situation | Behavior |
|---|---|
| Another agent or a human runs the game/build | Shared guard + process scan → busy/defer; no foreign kill or focus stealing; lock-unaware concurrent launches invalidate the owned capture |
| Game crashes mid-run | `RUN_CRASHED`; already written blocks/logs may survive, but the current in-memory benchmark/window data can be lost; retain partial evidence without manufacturing a completed result |
| Focus lost mid-run | Existing stress focus monitoring rejects the run; preserve evidence as invalid and require rerun for acceptance |
| Missing builds, saves or game data | Structured `ENV_MISSING_*` with the checked path and a remediation hint; server stays connected |
| Build failure/cancellation | Exit/error and pre/post patch/assets state recorded; incomplete runtime not runnable; restore only proven job-owned changes, else `RECOVERY_REQUIRED` |
| Server death mid-run | Required tested containment stops only owned descendants; startup marks jobs interrupted and performs guarded config/evidence recovery |
| Logs contain instruction-like text | Returned as delimited untrusted data with byte caps; never executed or followed |
| Disk growth | Owner-configured quota, refuse oversized jobs; dry-run pruning protects active runs, named baselines, retained experiment evidence and all external files |

### 9.2 Risks

Machine contention and focus fragility (high; mitigated by serialization, explicit validity
flags, and the pilot gate); parser drift when the engine renames passes or columns (medium;
header-driven parsing, unknown columns preserved, engine version and schema hash recorded);
clangd quality on MSVC/macro-heavy C (medium; spike before promising, grep fallback);
save/game-data prerequisites (medium; degradation and explicit fixture preparation);
experiment build/disk cost (high; frozen binary reuse and capped suites); forced-exit recovery
and Windows containment (high; prove with fake workers before real launches); and scope creep
toward "automatic findings" (guarded by explicit evidence types and coverage).

### 9.3 Exclusions

Windows-only; Debug remains the default build configuration (Release only on explicit
request). No remote/HTTP transport, no CI service, no protocol Tasks dependency, no SSIM
pass/fail gate, no automatic commit/merge/push/stash/reset, no agent code edits through MCP, no
generic shell tool, no shipping-product feature. Engine instrumentation (section 9.4) is a
separate, optional workstream. Approved builds can generate/deploy files and temporarily apply
the known NVRHI patch; document those expected writes rather than promise zero filesystem
mutation. Existing source/config changes remain protected.

### 9.4 Engine instrumentation backlog (optional, separate funding)

Per-frame capture and fine entity timers now exist and must not be scheduled again. Remaining
optional work: attribution/workload counters, explicit CPU/GPU frame/query IDs and CPU-valid
flags, crash-safe bounded streaming, a structured command/status channel and stable producer
schema versions. Distinct GPU particle/viewmodel attribution may require renderer restructuring,
not merely adding a JSON field. Each change needs a hypothesis and matched instrumentation
controls; coordinate with the ownership boundaries in PERFORMANCE.md.

## 10. Open questions for the owner

1. May the documented Fuma/AD-hub/Bogbottom saves be registered as the first scenarios, and
   which approved machine-local asset/fixture location supplies them?
2. Are shared-guard, durable-cleanup and containment patches to the existing runners/build
   wrapper acceptable? Without them, keep the server read-only rather than claim safe runs.
3. Which tools should be auto-allowed versus asked per call?
4. Is the experiment workflow (tool-owned worktrees, capped suites) wanted in P4, or should
   experiments stay a manual, documented process?
5. Does the owner want the instrumentation backlog (section 9.4) scheduled, and in what order?

## 11. Primary references and verification limits

External facts were checked on 2026-10-08; recheck SDK/client capability at implementation time:

- [MCP 2026-07-28 specification](https://modelcontextprotocol.io/specification/2026-07-28)
  and [Tasks extension](https://modelcontextprotocol.io/extensions/tasks/overview).
- [Python SDK v2 changes](https://py.sdk.modelcontextprotocol.io/whats-new/),
  [error handling](https://py.sdk.modelcontextprotocol.io/servers/handling-errors/) and
  [PyPI mcp metadata](https://pypi.org/pypi/mcp/json) (2.3.0, Python >=3.10 at review).
- [TypeScript SDK v2](https://ts.sdk.modelcontextprotocol.io/v2/) and
  [OpenCode V2 MCP configuration](https://opencode.ai/v2/docs/mcp-servers).
- [Windows Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
  and [process assignment](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject);
  [kernel object namespaces](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces)
  explains the Local/Global ownership scope.

Checks actually run during this review: six `test_analyze_stress.py` unit tests and the
`test_stress_budget.ps1` checks passed; source-based column counts recomputed as 124/90
(historical dump: 110); local document references/line anchors, code fences, English prose,
configuration JSON syntax and the installed uv flags were checked. This is not a live MCP
connection test or a new performance measurement.

Repository claims are anchored to the reviewed source and its two root indexes. This review
does not certify the future MCP implementation, raw archive availability in this checkout,
clangd coverage, nested-job behavior, visual correctness or hardware performance. The phase
checks exist specifically to establish those facts rather than inherit an earlier PASS.
