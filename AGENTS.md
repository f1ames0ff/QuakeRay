# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- The [QuakeRay MCP server](tools/quakeray_mcp/README.md) is registered as `quakeray` in `opencode.jsonc`. Prefer its inspection resources/tools over raw scans for engine sources, docs and capture data.
- The server is read-only by default; runtime jobs and research require `QUAKERAY_ENABLE_JOBS=1` / `QUAKERAY_ENABLE_RESEARCH=1`. Jobs follow the machine guard and the one-game-instance rule; use `/mcps` in OpenCode to reconnect after changes.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.
