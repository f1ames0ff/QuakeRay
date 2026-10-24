# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- Build only through `.\build_win.ps1 Debug`: it prepares the MSVC environment, applies the pinned NVRHI patch and deploys the runtime assets. A bare `cmake --build build\Debug` in a shell without that environment fails with C1083 on standard headers and must not be used for builds or verification.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- All agents changing CPU producers, caches, uploads or their callers must read [docs/multithreading.md](docs/multithreading.md), including its adaptation checklist, before designing a change. The render frame always runs as the task graph now - the `r_tasks` cvar and its serial fork were retired on 2026-10-14 - so new and existing solutions must keep the task-frame invariants (ownership, lifetime, the adaptation checklist) instead of relying on a serial fallback; adding a worker is not a substitute for auditing ownership and lifetime.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- **Before the first MCP call, install the locked QuakeRay MCP dependencies** with the commands in [MCP dependency setup](#mcp-dependency-setup). Both commands must exit 0.
- **QuakeRay MCP is mandatory for every agent working in this repository**, including implementers, reviewers and any delegated agents. It is the primary engine-inspection and evidence interface, not an optional convenience. Follow the workflow below; do not silently substitute shell/IDE searches for available MCP inspection tools.
- Runtime jobs are disabled by default. Operator activation requires `QUAKERAY_ENABLE_JOBS=1` in the MCP server environment; research additionally requires `QUAKERAY_ENABLE_RESEARCH=1`. The project configuration explicitly sets both to `0`.
- Runtime jobs use the machine guard and enforce the one-game-instance rule. After changing server code or its environment configuration, disconnect/connect `quakeray` through `/mcps` in OpenCode.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.

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

`.mcp.json` is machine-local and gitignored. The engine server must be configured under
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

## Codex CLI and desktop

Codex does not read OpenCode's `.mcp.json` or `opencode.json`. It loads MCP servers from
`%CODEX_HOME%\config.toml` (normally `%USERPROFILE%\.codex\config.toml`) under
`[mcp_servers.quakeray]`.

Register the server once with the Codex CLI. Use `uv --directory` so stdio startup does not
depend on the Codex process working directory:

```powershell
codex mcp add quakeray `
  --env QUAKERAY_ROOT=C:\absolute\path\to\rhi-setup-stage5 `
  --env QUAKERAY_BUILD=C:\absolute\path\to\rhi-setup-stage5\build\Debug `
  --env QUAKERAY_IMPORT_ROOTS=C:\absolute\path\to\rhi-setup-stage5\build\Debug\ad `
  --env QUAKERAY_ENABLE_JOBS=0 `
  --env QUAKERAY_ENABLE_RESEARCH=0 `
  -- C:\Users\YOUR_USER\.local\bin\uv.exe `
  --directory C:\absolute\path\to\rhi-setup-stage5 `
  run --offline --frozen `
  --project C:\absolute\path\to\rhi-setup-stage5\tools\quakeray_mcp `
  python -B -m quakeray_mcp
```

If PowerShell script execution policy blocks `codex.ps1`, invoke the installed
`codex.exe` directly. Verify registration with `codex mcp list` and `codex mcp get quakeray`.
The server command itself must also pass the `--selfcheck` command above.

Codex builds the MCP tool catalog when a thread starts. Adding or changing
`mcp_servers.quakeray` does not hot-add tools to the current thread: restart Codex or
resume the thread after the configuration change. A running thread that reports
"MCP is unavailable" before `server_status` can be queried is not evidence that the
registered server is broken.

## Mandatory engine-MCP workflow

1. After reading the architecture entry point, discover the registered `quakeray` tools and call `server_status` before investigation or changes. Check its source HEAD, working changes, runtime readiness and process ownership against this worktree. Do not use evidence from a different checkout or stale binary.
2. Use `get_architecture` or the MCP documentation resources for the implicated subsystem. For code work, use `find_symbol` scoped with `file`, then the available references/callers/callees tools. Inspect the returned source locations and ownership boundary before editing. Tool schemas are authoritative; do not guess tool names or arguments.
3. For performance work, use `list_scenarios`, `read_stats`, `read_benchmarks` and `compare_runs` where applicable. Preserve hashes, aggregation semantics, diagnostics, partial AST coverage and provenance limitations. An unresolved edge is not proof of no dependency, and imported measurements are not an acceptance PASS.
4. File reads, edits, Git operations, Debug builds and scoped searches remain necessary where MCP does not cover the operation. Use them to supplement MCP, not to bypass it. If MCP is unavailable, busy, mismatched or lacks coverage, explicitly report the reason and the bounded fallback; never pretend the MCP check succeeded. Stop a verification claim that requires unavailable MCP evidence, and request operator help where needed.
5. Use advertised runtime-job tools only after explicit operator activation and within their ownership rules. Do not enable jobs/research yourself or replace a denied job with an unrestricted launch. The supported guarded build/test scripts remain the documented execution route when jobs are disabled; their use does not waive MCP inspection or the one-instance rule.
6. In the handoff, name the MCP inspections/evidence used and any fallback or coverage gaps. If work is delegated, pass this policy, the current worktree and the relevant threading contract to every agent.

Connection and capability details: [tools/quakeray_mcp/README.md](tools/quakeray_mcp/README.md).
