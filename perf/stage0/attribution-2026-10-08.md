# Particle attribution — 2026-10-08 (Stage 0 exit evidence)

Session: exclusive machine window, one arm per engine run, each run `+exec` a config with the
particle cvars and `rt_bench ad_particle_heavy quit`, one ESC sent after the window appeared (the
startup menu stays over the game otherwise), run capped at 170 s. Build `691e02e1`
(branch `perf/particle-cluster-cache`), Debug, 3840x2160@165, vsync off, FSR 2, dtal master.

Demo: `ad_particle_heavy` re-recorded by the owner, 2026-10-08 14:45, 2,997,302 B, SHA256
`2653D5878152894B2400514C282D7462094B1CBBCEEEA73FD10C5FEA5AD93C0F`, 69.4 s of recorded time.
Idle reference demo: `ad_particle_idle`, SHA256
`1A9642CA4D9FF36916B66BB2146FE34E9C9E01A3EC4A539BBBF5D5B28AB1C8C6`.

The baselines bracket the session (39.5 and 39.3 fps), so the arm deltas are comparable within the
session. All values are `cpu.slot avg_ms` from the `rt_bench` blocks; `frame` is the whole-frame
bracket, the `particles*`/`fte convert`/`ents` slots are measured per frame, while the cluster slots
(`clusters`, `clust topup`, `clust upload`, `viewmodel`) repeat the renderer's last-composition
readout (the cluster counters show `misses=1`, `hits=frames`: the lists are composed once at load
and reused), so treat them as latched values, not per-frame costs.

| arm | fps | frames | frame | particles | resolve | upload | fte convert | clusters (latched) | topup (latched) | clust upload (latched) | viewmodel | ents |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline A | 39.5 | 2744 | 24.93 | 4.83 | 2.68 | 0.97 | 4.08 | 7.21 | 4.15 | 1.75 | 7.53 | 4.17 |
| no_fte (`r_fteparticles 0`) | 47.6 | 3301 | 20.62 | 4.51 | 2.52 | 0.90 | 3.51 | 2.47 | 0.70 | 0.98 | 2.72 | 4.15 |
| no_classic (`r_particles 0`) | 48.6 | 3367 | 20.25 | 0.00 | 0.22 | 0.00 | 0.00 | 6.84 | 3.84 | 1.66 | 7.08 | 4.00 |
| no_smoke (`r_smoke 0`) | 39.5 | 2741 | 24.95 | 4.84 | 2.67 | 0.99 | 4.09 | 7.21 | 4.06 | 1.75 | 7.46 | 4.12 |
| flat (`r_particle_lighting 0`) | 43.9 | 3047 | 22.42 | 2.01 | 0.23 | 1.00 | 1.25 | 7.11 | 3.97 | 1.74 | 7.35 | 4.02 |
| points_off (`r_particles_points 0`) | 39.6 | 2749 | 24.87 | 4.83 | 2.68 | 0.99 | 4.09 | 7.19 | 4.05 | 1.74 | 7.44 | 4.13 |
| partcache_off (`rt_particle_resolve_cache 0`), contaminated | 19.2 | 1334 | 51.62 | 6.81 | 4.56 | 1.01 | 6.00 | 31.93 | 5.05 | 25.29 | 32.20 | 4.14 |
| baseline B | 39.3 | 2729 | 25.05 | 4.84 | 2.68 | 0.99 | 4.09 | 7.28 | 4.12 | 1.75 | 7.53 | 4.17 |
| partcache_off (rerun), contaminated | 18.7 | 1298 | 53.04 | 6.81 | 4.56 | 1.00 | 5.99 | 33.39 | 5.19 | 26.56 | 33.66 | 4.13 |

## Findings

1. Particle-attributable cost on this demo is about **4.7 ms of a 25 ms frame** (~19%):
   `r_particles 0` moves `frame` 24.93/25.05 -> 20.25 and fps 39.5/39.3 -> 48.6. Within it the
   classic path is empty (`particles_classic = 0`, `points_off` changes nothing) - the bucket is
   the FTE path: `fte convert` 4.08, the per-vertex `particles resolve` 2.68, `particles upload`
   0.97.
2. The FTE lighting path is the largest single lever: `r_particle_lighting 0` drops
   `particles resolve` 2.68 -> 0.23 and `fte convert` 4.08 -> 1.25, `frame` -> 22.42, fps -> 43.9
   (~2.6 ms recovered). Stage 4's one-evaluation-per-particle change targets exactly this.
3. A further **~4.3 ms** sits outside the particle slots: `r_fteparticles 0` reaches nearly the
   `r_particles 0` frame (20.62 vs 20.25) while leaving the particle slots almost intact
   (`resolve` 2.52, `fte convert` 3.51). The demo's effects drive per-frame work that the latched
   cluster readout does not show; killing the FTE emitters removes it. This corroborates the
   owner's `r_particles 0` observation: the particle bucket plus the effect-driven work together
   account for roughly double what the particle slots alone suggest.
4. `rt_particle_resolve_cache 0`: two runs inside the matrix (19.2 and 18.7 fps, `frame` 51.6/53.0,
   `clust upload` 25.3/26.6) looked like a collapse, but a dedicated diagnostic the same day did
   not reproduce it. Three controlled runs with dumps (cache on / cache off / cache off + flat)
   measured fps 52.9 / 51.4 / 51.0 on the same demo window, `gpu.frame_ms` 18.14 / 18.38 / 17.84
   and `rays_particle` 14.5k / 14.6k / 0, and a confirmation pair put cache-off at 37.5 and 37.7
   fps against the 39.3-39.8 baselines. The two collapse rows above are environmental
   contamination, not a property of the cache; the inflated latched cluster readout belongs to the
   same contaminated runs. The clean cost of disabling the cache is the resolve delta alone -
   `particles resolve` 2.56 -> 4.28 ms, about 1.2-1.7 ms per frame end to end - at a 62% hit rate
   (a hit costs about 63 ns against a miss's 194 ns), with the GPU unchanged. Keep the cache on;
   the earlier warning is withdrawn and no rescue diagnostic is needed.
5. `r_smoke 0` and `r_particles_points 0` are no-ops on this content (no smoke, no classic
   particles) - recorded for completeness.

## Consequences for the plan

- D2 (resolve first or conversion/upload first): on the owner's content the measured order is
  lighting/resolve (~2.6 ms) > FTE conversion (~2.8 ms) > upload (~1 ms), with a comparable
  effect-driven share outside the bucket. The Stage-3 volume removes the resolve walks for classic
  and FTE alike (the biggest single item), Stage 4 collapses the per-vertex lighting, and the FTE
  conversion stays deferred.
- Stage 2 (classic compact points) is inert on this content; it remains the right transport for
  classic-particle scenes and for the stress caps.
- `r_fteparticles 0` does not disable the PSET/FTE geometry (the particle slots stay); it removes
  the effect-driven peripheral work. The arm documentation must say so.
- Every future run: send ESC after the window appears, keep one arm per run, cap the run at about
  170 s, and bracket a session with a baseline before comparing arms.
