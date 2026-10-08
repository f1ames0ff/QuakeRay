# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- The [QuakeRay MCP server](tools/quakeray_mcp/README.md) is registered as `quakeray` in `opencode.jsonc`. Prefer its available inspection tools/resources where they cover the task; otherwise use scoped source searches.
- Runtime jobs are disabled by default. Operator activation requires `QUAKERAY_ENABLE_JOBS=1` in the MCP server environment; research additionally requires `QUAKERAY_ENABLE_RESEARCH=1`. The project configuration explicitly sets both to `0`.
- Runtime jobs use the machine guard and enforce the one-game-instance rule. After changing server code or its environment configuration, disconnect/connect `quakeray` through `/mcps` in OpenCode.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.
