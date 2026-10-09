# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- Build only through `.\build_win.ps1 Debug`: it prepares the MSVC environment, applies the pinned NVRHI patch and deploys the runtime assets. A bare `cmake --build build\Debug` in a shell without that environment fails with C1083 on standard headers and must not be used for builds or verification.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- All agents changing CPU producers, caches, uploads or their callers must read [docs/multithreading.md](docs/multithreading.md), including its adaptation checklist, before designing a change. New and existing solutions must preserve both `r_tasks 0` and `r_tasks 1`; adding a worker is not a substitute for auditing ownership and lifetime.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- **QuakeRay MCP is mandatory for every agent working in this repository**, including implementers, reviewers and any delegated agents. It is the primary engine-inspection and evidence interface, not an optional convenience. Follow the workflow below; do not silently substitute shell/IDE searches for available MCP inspection tools.
- Runtime jobs are disabled by default. Operator activation requires `QUAKERAY_ENABLE_JOBS=1` in the MCP server environment; research additionally requires `QUAKERAY_ENABLE_RESEARCH=1`. The project configuration explicitly sets both to `0`.
- Runtime jobs use the machine guard and enforce the one-game-instance rule. After changing server code or its environment configuration, disconnect/connect `quakeray` through `/mcps` in OpenCode.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.

## Mandatory engine-MCP workflow

1. After reading the architecture entry point, discover the registered `quakeray` tools and call `server_status` before investigation or changes. Check its source HEAD, working changes, runtime readiness and process ownership against this worktree. Do not use evidence from a different checkout or stale binary.
2. Use `get_architecture` or the MCP documentation resources for the implicated subsystem. For code work, use `find_symbol` scoped with `file`, then the available references/callers/callees tools. Inspect the returned source locations and ownership boundary before editing. Tool schemas are authoritative; do not guess tool names or arguments.
3. For performance work, use `list_scenarios`, `read_stats`, `read_benchmarks` and `compare_runs` where applicable. Preserve hashes, aggregation semantics, diagnostics, partial AST coverage and provenance limitations. An unresolved edge is not proof of no dependency, and imported measurements are not an acceptance PASS.
4. File reads, edits, Git operations, Debug builds and scoped searches remain necessary where MCP does not cover the operation. Use them to supplement MCP, not to bypass it. If MCP is unavailable, busy, mismatched or lacks coverage, explicitly report the reason and the bounded fallback; never pretend the MCP check succeeded. Stop a verification claim that requires unavailable MCP evidence, and request operator help where needed.
5. Use advertised runtime-job tools only after explicit operator activation and within their ownership rules. Do not enable jobs/research yourself or replace a denied job with an unrestricted launch. The supported guarded build/test scripts remain the documented execution route when jobs are disabled; their use does not waive MCP inspection or the one-instance rule.
6. In the handoff, name the MCP inspections/evidence used and any fallback or coverage gaps. If work is delegated, pass this policy, the current worktree and the relevant threading contract to every agent.

Connection and capability details: [tools/quakeray_mcp/README.md](tools/quakeray_mcp/README.md).
