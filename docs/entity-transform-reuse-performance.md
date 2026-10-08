# Exact brush transform reuse

Branch: `perf/entity-transform-reuse`, based on `2ece9b9f` (`perf/entity-cpu-profile`).

## Measured problem

The detailed Debug profile identifies repeated brush matrix construction as a substantial part of entity preparation:

| Save | Opaque entities | Brush packing | Packing matrix construction | Alias uploads |
| --- | ---: | ---: | ---: | ---: |
| Fuma | 18.78 ms | 8.25 ms | 3.81 ms | 3.50 ms |
| Bogbottom | 32.01 ms | 11.51 ms | 4.12 ms | 8.54 ms |

Sources: `build/Debug/audit-entities-pack-fuma-retry-20261008-154928-639fa0` and `build/Debug/audit-entities-pack-bogbottom-control-20261008-155019-ac4a51`.

These measurements include fine-grained profiler overhead. Nested slots are inclusive, and the model-type counters also include the transparent pass. They must not be summed with the opaque entity total.

## Change

`RT_GetBrushModelMatrix` retains only the last non-null entity's transform in thread-local storage. Reuse requires the same entity pointer and bit-identical origin and angles. A miss executes the original matrix construction unchanged and copies its result into the cache. All callers, including per-surface light/style preparation and batch uploads, use the same function.

There is no visibility reduction, geometry persistence, animation suppression, lighting change or altered matrix arithmetic. A model or map change does not need special invalidation: the matrix depends only on origin and angles, not on model data. Pointer reuse with identical input bytes has the same mathematical result. Translation, rotation, entity alternation and signed-zero changes miss automatically. The null-entity world path still returns its original identity matrix.

The cache contains no surface, model, texture or renderer-owned allocation. Its size is fixed, and a compile-time assertion checks the copied matrix size against `QrTransform`.

## Validation status

Unit tests check empty-cache misses, exact matrix copying, entity identity, all translation and rotation components, entity alternation and signed zero. `build_win.ps1 Debug -Tests -Parallel 12` succeeded. Debug CTest passed all five tests, including the cloud and post-effects GPU smoke tests. The lighting test executable reported 1,413,890 checks and zero failures.

The instrumented control executable is retained at `build/entity-profile-binaries/control.exe`. Comparisons swap only the executable in the same deployed Debug runtime while no game is running; saves, settings and assets remain identical. Each invocation runs one save and one preset, warms up for eight seconds and releases the game before analysis. Runtime budget is at most 300 seconds.

This does not include the separate SIMD-bounds or compact brush-texture iteration experiments. The large alias upload cost remains unresolved.

## Controlled runtime results

All runs below used six-second live captures after eight-second warmup, the same deployed Debug assets and supplied save, 3840x2160, `rt_stats 0`, and the audit maximum preset with volumetric sky Low. Each paired comparison's effective settings and all recorded asset/save hashes matched exactly. The only binary change is transform reuse. The executable SHA256 values are:

- Control: `5F1ABE72D851DF877DBC07F458FD58D4050225FF069A468C937354900991379B`.
- Candidate: `1E4FC2CD5D6CDD140ED7C939E91B9491BCA43E7BB73450FB6A0B9E6EB45148BE`.

| Save / preset | Control FPS | Candidate FPS | Mean interval, control → candidate | Opaque entities, control → candidate | Matrix slot, control → candidate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fuma / Balanced | 29.91 | 33.14 | 33.43 → 30.17 ms | 18.40 → 15.16 ms | 3.71 → 0.53 ms |
| Bogbottom / Balanced | 19.53 | 21.35 | 51.20 → 46.83 ms | 31.60 → 27.49 ms | 4.14 → 0.62 ms |
| Fuma / Quality | 30.20 | 33.93 | 33.12 → 29.47 ms | 18.36 → 15.01 ms | 3.70 → 0.53 ms |

Candidate repeats measured 33.81 FPS / 15.05 ms entities on Fuma Balanced and 21.35 FPS / 27.55 ms entities on Bogbottom Balanced. The first paired Balanced observations correspond to approximately 10.8% and 9.3% FPS improvements. The entity CPU reduction is approximately 3.2–4.1 ms per frame, not a claim that all entity work is fixed.

Capture directories under `build/Debug/`:

- `audit-transform-reuse-fuma-candidate-20261008-155249-82c70c`
- `audit-transform-reuse-fuma-control-20261008-155316-23b4c0`
- `audit-transform-reuse-bogbottom-candidate-20261008-155341-af6d8c`
- `audit-transform-reuse-bogbottom-control-20261008-155409-a83060`
- `audit-transform-reuse-fuma-candidate-repeat-20261008-155439-27821e`
- `audit-transform-reuse-bogbottom-candidate-repeat-20261008-155507-ab367c`
- `audit-transform-reuse-fuma-quality-candidate-20261008-155544-bec434`
- `audit-transform-reuse-fuma-quality-control-20261008-155612-b023c3`

Fuma Quality screenshots show the same camera, brush geometry and lighting structure; the live animated effects differ between captures. This is a visual smoke check, not pixel-exact temporal validation. The cache copies the original transform bytes and does not change the matrix calculation on a miss.

Fine-grained timers remain enabled in both binaries and incur overhead. Captures are short local observations, not an all-map guarantee. Bogbottom Quality, AD hub and long gameplay transitions have not been compared for this candidate. The 60 FPS Balanced and 45 FPS Quality targets remain unmet.

## Remaining priority

Bogbottom still spends approximately 8.4 ms inside alias geometry uploads, 3.8 ms inside brush uploads, 3.1 ms selecting brush light styles and 2.3 ms posing alias vertices. Geometry API time includes renderer-side bounds accumulation, staging copies and metadata preparation. The already isolated SIMD-bounds candidate should be measured against this profile before pursuing topology persistence. Cluster and particle work remains assigned elsewhere.
