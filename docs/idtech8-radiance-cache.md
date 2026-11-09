# id Tech 8 World Radiance Cache (first slice)

Status: first slice. Date: 2026-11-09.

Branch: `feat/idtech8-radiance-cache`, stacked on `feat/idtech8-volume-local`.

## Scope of this slice

The report separates the volume update into visibility sampling, a world-space radiance cache and
the irradiance-volume update. This slice adds the cache between shading and the volume integration:

- **Cache**: 2^18 entries x 32 B (8 MB), a 1D spatial hash over quantized hit positions.
- **Entry**: `float4 radiance` (xyz used), `uint key`, `uint frame`, two padding words.
- **Cell size**: `rt_radiance_cache_cell` (default 16 u) times the LOD; the LOD is
  `2^floor(log2(1 + distance / rt_radiance_cache_lod))`, so far hits use coarser cells.
- **Reuse window**: `rt_radiance_cache_age` (default 30 frames, capped at 600). A stored entry is
  valid while `frame - storedFrame <= age`.
- **Lookup**: the volume probe raygen checks the cache for a hit position before shading. A hit
  returns the stored radiance; a miss shades with the existing sun / grid-cluster NEE path and
  inserts the result. Collisions overwrite the slot.
- **Host switch**: `rt_radiance_cache` (default 0). The cache is allocated eagerly with the pass so
  set 6 always has a valid descriptor; the shader gate keeps it off.

The hash, LOD and lookup math live in `renderer/Source/RadianceCache.h/.cpp` and
`renderer/Source/Shaders/RadianceCache.hlsli`; `qray_radiance_cache_tests` covers the CPU twin.

## Limitations of this slice

- The split path (`Feat/idtech8-radiance-cache-split`, host cvar `rt_radiance_cache_split`) is now
  implemented: the volume raygen trace writes one 64-byte packed hit record per probe ray into a
  20.9 MB buffer (set 6 binding 21), and a second raygen (`RtLightVolumeShade.rgen`) reconstructs
  the hit from the record, runs the sun/local-light NEE and the cache lookup, and writes the probe
  SH. The visibility and shade dispatches are paired on the same list with a UAV transition
  between them. The non-split path remains the default.
- A cache entry stores the final shaded radiance, including the view-dependent local-light term.
  Reusing it across probe rays is an approximation; the report's cache is also reused across rays,
  but its shading stage has the full surface inputs.
- Writes are not atomic: colliding invocations race and the last writer wins.
- The cache is not cleared on a map change; the key includes the world cell and the age check
  drops stale entries, so entries from another level are simply never hit.

## Runtime evidence: split path (Proton 11.0, RADV RX 9070 XT)

Build `ci-windows-42589617713`, `e1m1`, `rt_light_volume 1`:

- `rt_radiance_cache_split 0`: mean RGB `[64.1, 41.7, 35.9]`.
- `rt_radiance_cache_split 1`: `[64.0, 41.6, 35.9]`, mean absolute channel difference 2.3,
  `cvarlist` confirms `1`, `NVRHI::ERROR` count 0.
- The 20.9 MB hit buffer, the second raygen and the UAV transition pair run without a validation
  error; the split image matches the combined raygen within the run-to-run noise.

## Tests

`qray_radiance_cache_tests` (23 checks locally):

- 32-byte entry layout and the 2^18 power-of-two size;
- hash determinism, zero input and one-bit spread;
- stable lookup for the same position/camera and a different key in the next cell;
- LOD growth at 0, 1 and 3 LOD distances;
- put/get round-trip, max-age expiry, clear and overwrite;
- key mismatch (a colliding or different slot is a miss).

## Next

1. Split the volume raygen into a visibility pass that writes packed hit records and a cache-shading
   pass that consumes them, so the cache is filled without shading inside the probe loop.
2. Feed the cache from the final gather / indirect raygen as well, so screen-space and world-space
   reuse share one cache.
3. The report's 25 cm cell quantization. The explicit active-cell counters are implemented: each
   cache slot carries a frame tag (1 MB of `uint` tags at set 6 binding 22); the first insert of a
   slot in a frame exchanges the tag and increments `RAY_STATS_CATEGORY_CACHE_ACTIVE`, so
   `rt_stats_dump` reports the per-frame active set. The hit/miss counters are categories 6 and 7.
   The base cell is still 16 u (about 40 cm).
4. Denoise/upscale policy after the cache and volume fields are stable.
