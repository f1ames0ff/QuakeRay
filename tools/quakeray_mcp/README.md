# QuakeRay MCP developer laboratory

Local MCP server with a read-only default and explicitly enabled runtime/index/research layers.
Python 3.10 or newer, official `mcp==2.3.0`, locked dependencies and stdio transport. The
engine renderer/counter code is unchanged. Build and capture entry points now share a machine
ownership helper; supervised runtime adapters use private copies of registered runtimes.

## Implemented

- Six tools: `server_status`, `list_scenarios`, `read_stats`, `read_benchmarks`,
  `compare_runs`, `get_architecture`.
- Resources for AGENTS.md, ARCHITECTURE.md, PERFORMANCE.md, documentation and readiness.
- Parsers for benchmark frame CSV, reporting-window CSV, one-shot snapshots and benchmark
  blocks, with bounded pages, finite values, explicit aggregation and artifact SHA256.
- Frame statistics reuse `tests/perf/analyze_stress.py`; no alternative percentile algorithm.
- Legacy (2025-11-25) and modern (2026-07-28) protocol clients, plus a read-only investigation
  prompt. Protocol errors and expected tool-execution errors use distinct paths.

Real build/capture, verified retained controls, named baselines, scoped AST call queries,
owned experiment worktrees/suites and research/image diagnostics have been exercised.
No generic shell/console, arbitrary Git command or arbitrary-PID-killing tool is exposed.
Whole-program/dynamic graph completeness and automatic visual/gameplay equivalence are
deliberately not claimed.

## Opt-in job foundation

Default configuration remains read-only. An operator can set `QUAKERAY_ENABLE_JOBS=1` in the
server environment to advertise build/capture/menu jobs, job inspection/cancellation,
retained run listing/verification/comparison, protected pruning and explicit baseline promotion.
Real integration checks complement fake-worker tests; enabling a tool is not certification
that a different checkout's assets, settings or hardware are ready.

- Only the registered `debug` runtime, Debug builds and documented scenarios are accepted.
- Starts persist before spawning and require a request nonce of at least 16 characters. Reuse
  it only for identical arguments. A retry returns the same job without reissuing its control
  capability. Keep the original `control_token` securely; it is required for cancellation.
- Workers are created suspended, assigned to kill-on-close Job Objects, then resumed.
  Containment failure executes no worker code. Cancellation/timeouts target that job tree only;
  server exit terminates contained descendants. Other servers cannot cancel an owned job.
  Cancellation first publishes a private stop-request marker and allows two seconds for a
  cooperative exit. The stress capture loop requests benchmark stop/quit; loading checks
  abort through owned cleanup. Nonresponsive preparation/build/menu work falls back to
  contained termination. `stop_mode` distinguishes cooperative and forced outcomes; neither
  grants capture acceptance. A live Fuma cancellation completed cooperatively in integration.
- Build jobs have a 1,500-second deadline; runtime jobs 300 seconds. Polling never resets them.
  Compiling `-Tests` is not running CTest. Failed builds block launch until reviewed/rebuilt.
- The shared `Local\QuakeRayPerformanceRun` guard covers supported direct scripts and nested
  supervised calls. It is logon-session scoped and cannot atomically control manual launchers.
- Capture/menu jobs copy a registered runtime into their job directory and reject reparse
  points. Mutable configs/logs do not share writable links with the source runtime. Copies are
  limited to 8 GiB and must fit the deadline; large build directories should be staged into an
  independent minimal runtime first. Menu A/B uses the operator-approved baseline registry.
- Persistent jobs and capture records live under `%LOCALAPPDATA%/QuakeRayMCP/<repo-id>/jobs`.
  Captures remain unaccepted until binary/settings/assets/control compatibility is established.
  Pruning and experiment removal protect retained evidence and dirty worktrees as documented.
  Capture batches are fully validated before atomic publication. Frame captures require their
  benchmark block and manifest; effective resolution/FSR and sample/map associations are
  checked. Quality uses the explicit 45-FPS budget instead of the Balanced budget. Manifest
  and benchmark hashes are retained, but missing binary/asset/control verification still
  prevents acceptance. Legacy single-record catalog files remain readable.

The Windows tests use fake workers, including grandchildren and forced owner death. They do
not build or launch QuakeRay. Asset preflight errors are typed and launch no process.

## Setup and connection

From the repository root:

```powershell
uv sync --locked --project tools/quakeray_mcp --python 3.10
uv run --offline --frozen --no-sync --project tools/quakeray_mcp python -B -m quakeray_mcp --selfcheck
```

The project `opencode.jsonc` registers `quakeray`. It inherits existing permission policy;
it does not change global configuration or add blanket permission grants. A missing Debug
runtime does not prevent documentation/status inspection. Package setup requires dependency
downloads once; normal startup is offline and does not sync/install packages.
Use `/mcps` in OpenCode to inspect/reconnect the server after development changes. If the
OpenCode CLI is installed on PATH, `opencode mcp list` provides the same connection check.
The implementation was connected and called through this session's OpenCode MCP integration;
the shell environment used for development did not have the `opencode` CLI on PATH.

Any MCP host can launch the same command array over stdio. Keep its working directory at
the repository root and set `QUAKERAY_ROOT` explicitly when necessary. Logging goes to stderr;
normal server stdout is reserved for MCP. `--selfcheck` and `--inspect` are explicit CLI modes
and print JSON instead of serving MCP.

## Importing captures

By default only `build/Debug` is an approved evidence root. `QUAKERAY_BUILD` overrides that
registered runtime root. Additional directories require explicit operator configuration via
`QUAKERAY_IMPORT_ROOTS` (semicolon-separated on Windows) or repeatable `--import-root` flags.
No tool argument can expand the allowlist. Resolved traversal, symlink and junction escapes
are rejected. Supported evidence extensions are `.csv`, `.dump`, `.log`, `.json`; file content
must still match a supported capture format, so an arbitrary JSON file is not a RunRecord.

For a synthetic CLI smoke check, without game data:

```powershell
uv run --offline --frozen --no-sync --project tools/quakeray_mcp python -B -m quakeray_mcp --import-root tools/quakeray_mcp/tests/fixtures --inspect tools/quakeray_mcp/tests/fixtures/frames.csv
```

Tool argument examples:

```json
{"source":{"import_path":"build/Debug/ad/stats-example.dump"},"summary":true,"cursor":0,"limit":20}
```

```json
{"source":{"artifact_id":"artifact_returned_by_a_previous_read"},"summary":false,"cursor":0,"limit":20}
```

`summary=true` pages metric summaries; `summary=false` pages numeric rows. Follow `next_cursor`
to obtain the rest. Frame CSV results also return interval/FPS summaries. `budget_ms` is an
explicit diagnostic parameter (default `1000/60`); request `1000/45` for the Quality target
rather than inferring a preset from the filename. Percentiles of
window maxima remain window statistics, not frame-time percentiles; snapshots return no
percentiles. GPU summaries exclude invalid GPU readings and represent asynchronously completed
RHI snapshots, not display latency or a synchronized CPU/GPU pair. CPU counters overlap and
are never added to derive an invented remainder.

Artifact IDs identify immutable byte-hashed imports in a **bounded session cache**, not a
persistent run catalog. Reimport after restart/eviction. Headers and CRLF/UTF BOM handling do
not change the SHA256 of the original bytes. Artifact resources currently expose metadata;
bounded decoded rows are obtained through `read_stats`. No automatic baseline is created.

`compare_runs` accepts the same source selectors as `read_stats`. P1 can return diagnostic
metric deltas but always reports unverified imports as incomparable for acceptance. It does
not infer verified focus, binary/assets/settings identity or visual PASS from filenames,
JSON assertions or equal FPS. Full compatible-run acceptance requires P2 RunRecords and an
explicit comparison policy; the legacy PowerShell comparison is not its oracle.

Bounds: 64 MiB per file and session-cache byte budget, 32,768 CSV rows, 512 columns, three
million numeric cells, two concurrent tool inspections, 100 rows per page and 32 KiB structured
response. Large responses fail or paginate explicitly, never silently become accepted evidence.
Documentation pages are bounded too; use `get_architecture` with its document alias/cursor.
Raw logs, document prose and metadata are untrusted data, not authorization for other actions.

## Resources and prompt

- `quakeray://docs/entry` → AGENTS.md.
- `quakeray://docs/architecture` → ARCHITECTURE.md.
- `quakeray://docs/performance` → PERFORMANCE.md.
- `quakeray://docs` → document alias index. Other allowlisted docs use `docs/<stem>` aliases.
- `quakeray://status`, `quakeray://scenarios` → readiness and documented scenarios.
- `quakeray://artifacts/{id}` → cached imported-artifact metadata.
- `quakeray://runs/latest`, `quakeray://benchmarks/latest` → explicit empty P2-pending state.
- `investigate_slow_scene` → architecture-first evidence workflow; it never launches the game.

The three documented saves are external prerequisites. Scenario listing checks existence
and expected save hashes, but does not certify mod assets or claim the target FPS is reached.
The word `gpu_heavy` is not a GPU-bound classification.

## Tests

```powershell
uv run --offline --frozen --no-sync --project tools/quakeray_mcp python -B -m unittest discover -s tools/quakeray_mcp/tests -v
python -B -m unittest discover -s tests/perf -p test_analyze_stress.py
```

Tests use synthetic fixtures, temporary directories and short-lived MCP subprocesses, never
the game or GPU. They cover both protocol revisions, input/output schemas, resources/prompts,
typed errors, parser/analyzer parity, malformed/nonfinite/context-invalid captures, old/current
column counts, unknown columns, immutable imports, pagination, incomparable measurements,
missing runtime and Windows junction escapes. The symbolic-link test skips when the OS does
not allow test symlink creation; the separate Windows junction test needs no elevation.

Additional ownership/budget checks (no GPU or game):

```powershell
powershell -NoProfile -File tests/perf/test_machine_guard.ps1
powershell -NoProfile -File tests/perf/test_stress_budget.ps1
```

See [the plan](../../docs/quakeray-mcp-plan.md) for P2 prerequisites and subsequent phases.

## Additional opt-in interfaces (not full phase acceptance)

`QUAKERAY_CLANGD` selects an operator-approved clangd executable. `find_symbol`, references,
callers/callees, bounded `trace_path` and refresh operate on selected translation units with
source/CDB fingerprints. Index-only `/clang:-Wno-error` is applied to a temporary database;
the build database is not changed. Diagnostics and partial coverage are returned. Cross-TU,
function-pointer and HLSL completeness are not claimed. The locally approved clangd 23.1.0
archive was verified against official SHA256
`23412a240756a162e7b98a282f36aa2a23a88db5ce16a0cbc4fef7253768c810`.

`QUAKERAY_ENABLE_RESEARCH=1` together with enabled jobs provides finding/search interfaces and
tool-owned detached experiment worktrees. Creation accepts only explicit commit SHA/name;
dependency preparation uses approved local submodule checkouts with no implicit remote fetch.
Experiment builds use Debug/private mutable dependencies. Removal previews by default and
refuses dirty worktrees without force. Findings reference retained runs and remain operator
interpretations, not verified measurements.

`prune_runs` protects all published capture evidence; deletion is explicit and limited to
terminal jobs without run IDs. Admission reserves disk space against a 32 GiB job-store quota.
An operator-registered `QUAKERAY_BASELINE_RUNTIME` enables isolated menu baseline/candidate
capture; tool arguments do not select arbitrary filesystem runtimes.

`prepare_experiment` initializes local pinned dependencies, `start_experiment_build` compiles
the private source, and `start_experiment_suite` executes one primary/guard scenario per
invocation on copied baseline/candidate arms, releasing the shared guard between invocations.
The end-to-end no-op build/suite was exercised, as was isolated menu baseline/candidate capture.
`diff_screenshots` supports bounded RGB/RGBA 8-bit noninterlaced PNG, checks CRC/filter/data
integrity and reports diagnostic RMSE/pixel differences, never geometry-equivalence PASS.
`verify_run` verifies retained artifact hashes; `compare_retained_runs` compares catalog IDs.

New captures match retained asset hashes, focus/completion checks, hardware/driver output and
the deployment receipt before provenance verification. `compare_retained_runs` requires those
identities and reports the explicit mean/p95 5% policy separately from visual/gameplay/validation
status. `promote_baseline` previews by default, needs explicit confirmation and never overwrites
an existing name. The map-assets fingerprint identifies map name plus its asset bundle, not
a claim of a separately extracted BSP hash.

Statistical claims, all-map coverage, image/geometry/gameplay equivalence and model-cost savings
are not implied by implementation completion or short integration tests. Visual/noise output
remains diagnostic; the pilot evaluation is an operator exercise, not an automatic PASS.
