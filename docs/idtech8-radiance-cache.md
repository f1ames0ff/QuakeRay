# id Tech 8 World Radiance Cache

Status: active implementation (normal-aware hash, 4-slot probing, outgoing radiometry; updated 2026-11-17).

Branch: `feat/idtech8-radiance-cache` (merged), updated in `fix/idtech8-correctness`.

## Scope and architecture

The cache stores outgoing surface radiance between shading and volume/indirect reuse:

- **Cache**: 2^18 entries x 32 B (8 MB), a 1D spatial + normal hash over quantized hit positions
  and quantized geometric normals (`7` bins per axis, 9 bits), with 4-slot linear probing
  (`kProbeSteps = 4` / `Q2_RADIANCE_CACHE_PROBE_STEPS 4u`).
- **Entry**: `float4 radiance` (xyz outgoing radiance), `uint key`, `uint frame`, `uint pad[2]`
  (`pad[0]` stores accumulated sample count up to 64).
- **Cell size & LOD**: `rt_radiance_cache_cell` (host default `10` u = `0.25 m` after
  `QR_QUAKE_UNIT_IN_METERS = 0.025f`; `VulkanDevice.cpp` fallback `0.25f`) times the LOD; the LOD
  is `2^floor(log2(1 + distance / lodDistance))`, where `rt_radiance_cache_lod` (`1024 u`)
  converts to `25.6 m` (clamped at `>= 0.25 m`).
- **Reuse window**: `rt_radiance_cache_age` (default 30 frames, capped at 600). A stored entry is
  valid while `frame - storedFrame <= age`.
- **Probing & replacement**: `q2RadianceCacheGet` probes up to 4 contiguous slots `(index + step) & mask`.
  `q2RadianceCachePut` prefers an existing slot with matching key (averaging only if non-expired,
  resetting `samples = 1` on expired match), else the first empty or expired slot, else the oldest
  live slot among the 4.
- **Lookup callers**:
  - Volume probe shading (`LightVolumeShade.hlsli`): queries `(hit.position, hit.normalGeom)`
    before shading with sun/local NEE and emission, inserting on miss.
  - Indirect first-bounce hit (`RtQ2Indirect.rgen.hlsl`): inserts outgoing `hitRadiance` keyed by
    `(hitSurf.position, hitSurf.normalGeom)`.
  - Indirect second diffuse bounce (`processSecondDiffuseBounce`): queries
    `(hitSurf.position, hitSurf.normalGeom)` and multiplies the hit outgoing radiance by
    `bounceBrdf = evalBRDFLambertian(1.0) * max(dot(surf.normal, bounceDir), 0.0)` and
    `oneOverPdf` (`M_PI / cos`). On cache miss, it falls back to
    `q2EvaluateLightVolumeIrradiance` (`volumeIrradiance * evalBRDFLambertian(1.0)`) when inside a
    volume, or sun NEE outside volumes, and does not insert partial lighting back into the cache.
- **Host switch**: `rt_radiance_cache` (default 0). The cache is allocated eagerly with the pass so
  set 6 always has a valid descriptor; the shader gate keeps it off.

The hash, normal quantization, linear probing, LOD and lookup math live in
`renderer/Source/RadianceCache.h/.cpp` and `renderer/Source/Shaders/RadianceCache.hlsli`;
`qray_radiance_cache_tests` covers the CPU twin.

## Known limitations

- **Writes are not atomic**: colliding or concurrent GPU invocations writing to the same chosen slot
  still race (the 4-slot probe reduces spatial/normal bucket collisions and keeps opposite walls
  separated, but does not lock entries).
- **View-dependent local shading**: reusing stored outgoing diffuse+emissive hit radiance across rays
  remains an approximation (specular/diffuse separation at cache level is not stored separately).
- **No explicit clear on map change**: entries carry world cell + quantized normal + age, so stale
  entries from an old level expire naturally or miss.
- The non-split volume path remains the default unless `rt_radiance_cache_split` is set.

## Runtime evidence: split path (Proton 11.0, RADV RX 9070 XT)

Build `ci-windows-42589617713`, `e1m1`, `rt_light_volume 1`:

- `rt_radiance_cache_split 0`: mean RGB `[64.1, 41.7, 35.9]`.
- `rt_radiance_cache_split 1`: `[64.0, 41.6, 35.9]`, mean absolute channel difference 2.3,
  `cvarlist` confirms `1`, `NVRHI::ERROR` count 0.
- The 20.9 MB hit buffer, the second raygen and the UAV transition pair run without a validation
  error; the split image matches the combined raygen within the run-to-run noise.

## Runtime counters and the volume-probe hit issue

Build `ci-windows-42596608531`, `e1m1`, `rt_radiance_cache 1`, `rt_radiance_cache_split 1`,
`rt_stats_panels 1`, with eight dumps over consecutive frames: the new rows are nonzero on frames
that reach geometry (`gpu.rays cache hit 9`, `cache miss 3`, `cache active 3`), and zero on frames
whose cascade has no hits. A single dump can land on one of the latter, which is why the first
one-shot runs read zero.

The volume pass adds exactly 4096 x 16 = 65,536 `INDIRECT` counts when `rt_light_volume 1`
(475,456 vs 409,920 off), so its `rayStatsAdd` calls reach the host and the counter wiring is
verified end to end. The cache lookups were rare because the probe rays inherited the primary
pass's front-face cull and missed the single-sided geometry; `fix/idtech8-volume-probe-hits`
removes that cull for the probe trace (`docs/idtech8-volumes.md`).

## Multi-frame reuse

A revisited cell now accumulates its radiance with a running average instead of overwriting it:
`pad.x` is the sample count, capped at 64; the new sample blends with `1 / samples`, so the first
sample is stored exactly and later visits converge to the cell average. The shader and the CPU
twin share the rule (`q2RadianceCachePut` / `RadianceCache::Put`).

Runtime hot-swap on the master build, e1m1, `rt_radiance_cache 1`, `rt_light_volume 1`, split:
baseline mean `[63.75, 36.91, 29.62]` (24,696 hits) vs accumulation `[69.81, 43.31, 35.22]`
(15,459 hits), mean absolute image difference 6.64, 0 `NVRHI::ERROR`. The hit-count difference is
run-state; the image shift is the accumulated cell radiance.

## Indirect feed and second-bounce reuse (historical 2026-11-11 baseline vs 2026-11-17 fix)

On the merged baseline (`ci-windows-43060164059`), the indirect raygen fed the cache from first
bounce, second bounce and final gather (`diffuse + specular`), while `processSecondDiffuseBounce`
returned `cachedRadiance * oneOverPdf` without `bounceBrdf`. Runtime e1m1, `rt_gi_level 2`, split,
30 dumps (historical pre-fix build):

| cache | mean RGB | hits | misses | active |
|---|---|---:|---:|---:|
| off | `[69.41, 42.47, 34.52]` | 0 | 0 | 0 |
| on | `[65.78, 36.93, 29.29]` | 11,337,693 | 20 | 876 |

Mean absolute image difference 6.3, 0 `NVRHI::ERROR`.

In `fix/idtech8-correctness` (`965f027` + `b80703c`, 2026-11-17):
- Final-gather `q2RadianceCachePut(surf.position, diffuse + specular)` was removed (the cache now
  stores outgoing radiance from full-NEE bounce hits and volume probe hits only).
- Second-bounce `q2RadianceCachePut` was removed so indoor second bounces without local-light NEE
  do not overwrite cache cells with zero/sun-only estimates; misses inside a volume fall back to
  `q2EvaluateLightVolumeIrradiance`.
- `processSecondDiffuseBounce` multiplies returned outgoing radiance by
  `evalBRDFLambertian(1.0) * max(dot(surf.normal, bounceDir), 0.0)` so `oneOverPdf` cancels cleanly.

## Tests

`qray_radiance_cache_tests` (62 checks locally):

- 32-byte entry layout and the 2^18 power-of-two size;
- hash determinism, zero input and one-bit spread;
- stable lookup for the same position/camera and a different key in the next cell;
- metric 0.25 m base cells (`rt_radiance_cache_cell 10`) and 25.6 m LOD threshold;
- normal quantization separation (opposite/orthogonal normals stay isolated, degenerate normal
  falls back to `+Z`, unnormalized vectors match unit vectors);
- 4-slot linear probing: coexistence of 4 colliding keys, in-place averaging at non-zero probe step,
  expired-slot reclamation resetting `samples = 1`, and oldest-slot eviction on a 5th live key;
- put/get round-trip, max-age expiry, clear, overwrite and counter tracking.

## Follow-up

1. Re-verify `e1m1` / `ad_swampy` runtime images and hit/miss/active counters on Windows Debug after
   `965f027` (`0.25 m` cell / `25.6 m` LOD) and `b80703c` (normal hash + probing + `bounceBrdf`).
2. Multi-bounce reuse beyond bounce 2 or atomic entry updates if profiling shows slot races matter.
