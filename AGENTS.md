# Repository entry point

- Read [ARCHITECTURE.md](ARCHITECTURE.md) first. Use its subsystem routes and real entry points instead of rediscovering the whole repository.
- For profiling or optimization, then read [PERFORMANCE.md](PERFORMANCE.md), select the relevant capture and verify the current source/binary/settings before forming hypotheses.
- Inspect the implicated caller, callee and ownership boundary first. Expand the search only when that route cannot answer the question.
- Keep the architecture index and performance evidence current when changing a listed execution boundary or profiler counter. Unknown costs are not measurements, and unverified hypotheses are not conclusions.
