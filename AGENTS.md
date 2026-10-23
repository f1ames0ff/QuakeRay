# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- Renderer source lives in the `renderer` submodule (`QuakeRaySDK`). Make and commit renderer changes in that repository first, push the SDK branch, and then update the host submodule pointer; do not edit the submodule as an ordinary vendored tree.
- Build only through `.\build_win.ps1 Debug`; see [Build policy](#build-policy) for the toolchain, configuration and environment rules.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- All agents changing CPU producers, caches, uploads or their callers must read [docs/multithreading.md](docs/multithreading.md), including its adaptation checklist, before designing a change. The render frame always runs as the task graph now - the `r_tasks` cvar and its serial fork were retired on 2026-10-14 - so new and existing solutions must keep the task-frame invariants (ownership, lifetime, the adaptation checklist) instead of relying on a serial fallback; adding a worker is not a substitute for auditing ownership and lifetime.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- **Before the first MCP call, install the locked QuakeRay MCP dependencies** with the commands in [MCP dependency setup](#mcp-dependency-setup). Both commands must exit 0.
- **QuakeRay MCP is mandatory for every agent working in this repository**, including implementers, reviewers and any delegated agents. It is the primary engine-inspection and evidence interface, not an optional convenience. Follow the workflow below; do not silently substitute shell/IDE searches for available MCP inspection tools.
- Runtime jobs are disabled by default. Operator activation requires `QUAKERAY_ENABLE_JOBS=1` in the MCP server environment; research additionally requires `QUAKERAY_ENABLE_RESEARCH=1`. The project configuration explicitly sets both to `0`.
- Runtime jobs use the machine guard and enforce the one-game-instance rule. After changing server code or its environment configuration, disconnect/connect `quakeray` through `/mcps` in OpenCode.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.

## Build policy

**Always build the Debug configuration unless the user explicitly asks for another one.**

- The only supported entry point is `.\build_win.ps1 Debug`: it prepares the MSVC environment
  (`VsDevCmd`), applies the pinned NVRHI patch, takes the machine guard and deploys the runtime
  assets.
- Never run a bare `cmake --build build\Debug` (or `ninja`) in a shell without that environment: it
  fails at compile time with C1083 on standard headers (`assert.h`, `sys/types.h`), so it must not be
  used for builds, tests or verification.
- Debug is what builds, tests, verification runs and artifacts handed to the user use.
- A Release build happens only when the request names it explicitly; do not move to Release on your
  own initiative.

## One game instance at a time

**Never launch QuakeRay while another instance is already running.**

Several agents share this machine, and a running test owns the GPU, the window focus and the
timings.

- Before launching `quakeray.exe` - directly or through a runner such as
  `tests/perf/run_place.ps1` or `tests/perf/run_menu.ps1` - check for a running instance first
  (`Get-Process quakeray, qray_* -ErrorAction SilentlyContinue` on Windows).
- If one is running, wait until it exits before launching your own. Two concurrent instances
  compete for the GPU, steal window focus and break the other agent's automated input and
  measurements.
- While another agent's performance run is in flight, prefer waiting until it finishes before
  starting other heavyweight work on the same machine - GPU smoke tests, benchmarks, or a fully
  parallel build.

## MCP dependency setup

The QuakeRay MCP is a local Python server. Every agent must install its locked dependencies before
the first MCP call. `uv` and Python 3.10 or newer are required. From the repository root run:

```powershell
uv sync --locked --project tools/quakeray_mcp --python 3.10
uv run --offline --frozen --project tools/quakeray_mcp python -B -m quakeray_mcp --selfcheck
```

`uv sync` must exit 0. The selfcheck must exit 0 and report `server_status`, `get_architecture`,
`list_scenarios`, `read_stats`, `read_benchmarks` and `compare_runs`. If `uv` is missing, install
it from <https://docs.astral.sh/uv/getting-started/installation/> first. If either command fails,
stop and report the exact failure; do not bypass the engine MCP with shell or IDE searches.

`.mcp.jsonc` is machine-local and gitignored. The engine server must be configured under
`mcp.servers.quakeray` as a local server running the command above. If the file is absent, create
it with this engine entry; do not commit the file:

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "servers": {
      "quakeray": {
        "type": "local",
        "command": ["uv", "run", "--offline", "--frozen", "--project", "tools/quakeray_mcp", "python", "-B", "-m", "quakeray_mcp"],
        "cwd": ".",
        "environment": {
          "QUAKERAY_BUILD": "build/Debug",
          "QUAKERAY_IMPORT_ROOTS": "build/Debug/ad",
          "QUAKERAY_ENABLE_JOBS": "0",
          "QUAKERAY_ENABLE_RESEARCH": "0"
        },
        "protocol": "legacy",
        "codemode": true,
        "disabled": false,
        "timeout": { "startup": 60000, "catalog": 30000, "execution": 30000 }
      }
    }
  }
}
```

The JetBrains IDE endpoint is a separate server (`quakeray_ide`) and is not the engine MCP. If
`server_status` is not in the session catalog, verify this configuration and reconnect
`quakeray` through `/mcps`.

## Mandatory engine-MCP workflow

1. After reading the architecture entry point, discover the registered `quakeray` tools and call `server_status` before investigation or changes. Check its source HEAD, working changes, runtime readiness and process ownership against this worktree. Do not use evidence from a different checkout or stale binary.
2. Use `get_architecture` or the MCP documentation resources for the implicated subsystem. For code work, use `find_symbol` scoped with `file`, then the available references/callers/callees tools. Inspect the returned source locations and ownership boundary before editing. Tool schemas are authoritative; do not guess tool names or arguments.
3. For performance work, use `list_scenarios`, `read_stats`, `read_benchmarks` and `compare_runs` where applicable. Preserve hashes, aggregation semantics, diagnostics, partial AST coverage and provenance limitations. An unresolved edge is not proof of no dependency, and imported measurements are not an acceptance PASS.
4. File reads, edits, Git operations, Debug builds and scoped searches remain necessary where MCP does not cover the operation. Use them to supplement MCP, not to bypass it. If MCP is unavailable, busy, mismatched or lacks coverage, explicitly report the reason and the bounded fallback; never pretend the MCP check succeeded. Stop a verification claim that requires unavailable MCP evidence, and request operator help where needed.
5. Use advertised runtime-job tools only after explicit operator activation and within their ownership rules. Do not enable jobs/research yourself or replace a denied job with an unrestricted launch. The supported guarded build/test scripts remain the documented execution route when jobs are disabled; their use does not waive MCP inspection or the one-instance rule.
6. In the handoff, name the MCP inspections/evidence used and any fallback or coverage gaps. If work is delegated, pass this policy, the current worktree and the relevant threading contract to every agent.

Connection and capability details: [tools/quakeray_mcp/README.md](tools/quakeray_mcp/README.md).
