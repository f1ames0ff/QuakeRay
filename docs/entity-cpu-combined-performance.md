# Entity CPU work: transform reuse and exact SIMD bounds

Branch: `perf/entity-cpu-combined`.

This integration branch combines the independently retained brush-transform change (`adc3cd32`, `perf/entity-transform-reuse`) with the existing exact SIMD scene-bounds implementation (`d419e38a`, cherry-picked as `8b4c0664`). It does not include compact brush-texture iteration, cluster optimizations, particle changes, geometry persistence or GPU-shading experiments.

The transform-only profile leaves approximately 8.4 ms of alias geometry API calls and 3.8 ms of brush geometry API calls on Bogbottom. `Scene::Upload` visits every submitted vertex to transform it, reject invalid/extreme coordinates and accumulate scene bounds before the staging/metadata path. The SIMD candidate retains that exact traversal and scalar arithmetic order while doing three components together. This comparison measures its incremental benefit inside those upload slots, rather than relying on the earlier standalone benchmark or unmatched FPS observations.

## Protocol

- Debug runtime, tests and artifacts only.
- One supplied save and one FSR preset per invocation, eight seconds warmup and six seconds live capture.
- 3840x2160, `qr_audit_max.cfg`, volumetric sky Low, `rt_stats 0`.
- At most 300 seconds per invocation; close the game before analysis and never overlap another game/GPU test.
- Retain `control.exe`, `transform-only.exe` and `combined.exe` under `build/entity-profile-binaries/`; swap only the executable while no game is running.
- Require identical effective settings and recorded asset/save hashes between arms.
- Compare alias/brush upload slots, opaque entity time and actual frame intervals. Nested timers are inclusive and must not be summed.

## Status

`build_win.ps1 Debug -Tests -Parallel 12` succeeded. Debug CTest passed all five tests, including the bitwise scalar-reference bounds checks, brush-transform cache checks and cloud/post-effects GPU smoke tests. The transform-only comparison is recorded in `docs/entity-transform-reuse-performance.md`. SIMD semantics and edge-case coverage are recorded in `docs/scene-bounds-performance.md`.

Neither independent change has been merged into master.

## Controlled results

All paired captures below matched effective settings and every recorded asset/save hash. The engine pack was regenerated during the combined build, so new full-control captures were taken rather than comparing with the earlier transform-only campaign's different archive hash.

| Save / preset | Full control FPS | Combined FPS | Mean interval, control → combined | Opaque entities, control → combined |
| --- | ---: | ---: | ---: | ---: |
| Fuma / Balanced | 29.64 | 37.18 | 33.74 → 26.90 ms | 18.44 → 12.63 ms |
| Bogbottom / Balanced | 19.58 | 23.89 | 51.06 → 41.87 ms | 31.60 → 22.62 ms |
| Fuma / Quality | 29.76 | 37.46 | 33.61 → 26.70 ms | 18.62 → 12.59 ms |

Balanced combined repeats measured 37.55 FPS / 12.65 ms entities on Fuma and 23.89 FPS / 22.82 ms entities on Bogbottom. The first paired Balanced observations show approximately 25.4% and 22.0% higher FPS and 5.8 ms / 9.0 ms less opaque entity CPU work. P95 frame intervals improved from 34.97 to 28.09 ms on Fuma and from 52.33 to 42.90 ms on Bogbottom.

### Incremental SIMD contribution

Separate transform-only control captures isolate the bounds change:

| Save / Balanced | Transform-only FPS | Combined FPS | Opaque entities, transform-only → combined | Alias uploads, transform-only → combined | Brush uploads, transform-only → combined |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fuma | 33.62 | 37.18 | 15.22 → 12.63 ms | 3.46 → 1.80 ms | 2.86 → 2.02 ms |
| Bogbottom | 21.15 | 23.89 | 27.75 → 22.62 ms | 8.44 → 4.47 ms | 3.81 → 2.84 ms |

SIMD therefore removes approximately 4.0 ms from Bogbottom alias uploads and another 1.0 ms from brush uploads. This confirms that the bounds traversal is a substantial part of the previously observed geometry API cost. It does not eliminate the remaining staging/metadata cost or acceleration-structure setup.

### Evidence

Executable SHA256:

- Full control: `5F1ABE72D851DF877DBC07F458FD58D4050225FF069A468C937354900991379B`.
- Transform-only: `1E4FC2CD5D6CDD140ED7C939E91B9491BCA43E7BB73450FB6A0B9E6EB45148BE`.
- Combined: `A7D9F5657EF3918D1E31CED8299876EA6EC6DE981C3CD74FB661DF120B26269F`.

Common engine asset pack SHA256: `360D3E586C3C77E8B416C6E643319E6CD0F093010899E7763937897FE6770CEB`.

Capture directories under `build/Debug/`:

- `audit-entity-combined-bogbottom-20261008-155908-9396f0`
- `audit-entity-combined-bogbottom-transform-control-20261008-155935-42af18`
- `audit-entity-combined-fuma-20261008-160000-b1c91d`
- `audit-entity-combined-fuma-transform-control-20261008-160028-74743a`
- `audit-entity-combined-bogbottom-full-control-20261008-160106-4f1ed7`
- `audit-entity-combined-fuma-full-control-20261008-160133-3aa81d`
- `audit-entity-combined-bogbottom-repeat-20261008-160200-738857`
- `audit-entity-combined-fuma-repeat-20261008-160225-04cdd4`
- `audit-entity-combined-fuma-quality-20261008-160249-e3c3b9`
- `audit-entity-combined-fuma-quality-full-control-20261008-160316-a977c0`

## Remaining limits and priorities

These are short, instrumented local Debug observations on the supplied saves, not all-map performance guarantees. Bogbottom Quality, AD hub and long gameplay transitions have not been compared for the combined binary. The 60 FPS Balanced and 45 FPS Quality targets remain unmet; every captured interval is still over its target budget.

On Bogbottom the remaining entity costs include alias uploads approximately 4.5 ms, brush uploads 2.8 ms, brush light-style selection 3.1 ms and alias pose preparation 2.3 ms. RHI setup takes approximately 4.6 ms outside the entity pass. The next work should isolate the remaining staging/metadata and AS costs or eliminate repeated invariant geometry work with explicit ownership and invalidation; it should not blindly enable experimental persistence. Cluster and particle work remains separate.
