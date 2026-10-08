# Entity CPU bottleneck profile

Branch: `perf/entity-cpu-profile`, based on `385211ef`.

The added CPU slots split brush chaining, emissive lights, vertex/index packing and geometry API calls, and alias posing, emissive lights and geometry API calls. Packing is further split into matrix construction, light-style selection and per-surface cluster resolution. These are inclusive timers: nested slots must not be added to their parents. The entity-type slots also include transparent entities, while `ents` measures the opaque pass alone.

All observations below use Debug, live simulation, the supplied AD save, 3840x2160 FSR Balanced, the maximum audit preset with volumetric sky Low, `rt_stats 0`, eight seconds of warmup and a short per-frame capture. Fine-grained timers add overhead; these captures locate work rather than provide a clean FPS baseline.

## Fuma

`build/Debug/audit-entities-detail-fuma-20261008-140803-360675` measured 30.82 FPS and a 32.45 ms mean interval over 197 intervals. Opaque entities averaged 17.44 ms. Brush packing averaged 6.76 ms, brush geometry uploads 2.89 ms, alias uploads 3.50 ms, alias pose preparation 1.03 ms and brush emissive lights 1.45 ms. Brush chaining averaged 0.46 ms.

`build/Debug/audit-entities-pack-fuma-retry-20261008-154928-639fa0` measured 29.30 FPS and a 34.13 ms mean interval over 156 intervals. Brush packing averaged 8.25 ms, including matrix construction 3.81 ms, light styles 0.94 ms and cluster resolution 1.70 ms. Additional nested timers explain part of the increase; this is not an optimization regression.

The previous packing capture lost window focus and is excluded.

## Next experiment

`RT_GetBrushModelMatrix` reconstructs an identical matrix for successive surfaces of one entity, and again for lights and geometry uploads. A last-entity, thread-local cache keyed by entity identity and the exact origin/angle bytes can eliminate these repeated matrix operations without changing geometry, visibility, lighting or numerical output. Cluster changes and particle changes remain separate workstreams. Geometry API costs and alias preparation remain substantial and are not solved by this experiment.
