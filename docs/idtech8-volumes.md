# id Tech 8 Volumes — ABI and CPU reference

Status: first slice. Date: 2026-11-07.

Branch: `feat/idtech8-volumes`, stacked on `feat/idtech8-restir`.

## Scope of this slice

This commit adds the volume ABI and the CPU sampling oracle, the same way the grid work started
with a CPU candidate histogram before the GPU builder:

- 6 nested cascades x 16^3 probes, first cascade 1,280 u (32 m) and every next one doubling.
- 10^3 hand-placed local volumes, up to 128 slots.
- Probe position = cell centre; cascade centre snapped to 640 u (the grid's rebuild snap).
- 4-coefficient RGB SH per probe (L0, L1x, L1y, L1z), 48 B per probe.
- Interleaved update schedule: one cascade and one local volume per frame.
- Trilinear lookup with local volumes taking precedence over cascades; the smallest containing
  local volume wins.
- SH helpers: `AccumulateRadiance`, `NormalizeSh` (4*pi / sampleCount) and the Lambertian
  convolution `EvaluateIrradiance` (0.886227 for L0, 1.023328 for L1).

The CPU reference is not wired into the frame. The next commits add the GPU probe atlas, the
per-frame visibility/ray pass, the irradiance update and the final-gather sampling.

## Constants

| | value |
|---|---:|
| cascades | 6 |
| cascade probes | 16^3 = 4,096 |
| first cascade bounds | 1,280 u |
| cascade 5 bounds | 40,960 u |
| local volume probes | 10^3 = 1,000 |
| local volume slots | 128 |
| total probe slots | 152,576 |
| probe record | 48 B (SH4 RGB) |

Cascade `c` has `bounds = 1280 * 2^c`, `cell = bounds / 16`. The camera origin is snapped to
640 u on every axis, so a moving camera crosses a rebuild boundary at most once per 640 u.

## Update schedule

`GetUpdateSchedule(frameIndex, cascade, localVolume)` returns `cascade = frameIndex % 6` and the
`(frameIndex % activeLocalVolumes)`-th enabled local volume (or `-1`). One cascade and one local
volume are updated per frame, matching the report's interleaved update.

## Lookup

- `FindCascade` returns the finest cascade that contains the point (`-1` outside all).
- `FindLocalVolume` returns the smallest enabled local volume that contains the point, or `-1`.
- `GetCascadeLookup` / `GetLocalLookup` return the `(0,0,0)` probe and the eight weights indexed
  `[z][y][x]`, matching the probe layout `(z * dim + y) * dim + x`.
- `SampleIrradiance` prefers a local volume, then the cascade, and trilinearly blends the eight
  probes with `EvaluateIrradiance`. It returns false outside all volumes; the final gather keeps
  the existing screen-space SH as the fallback.

## Tests

`qray_light_volume_tests` (85 checks locally) covers:

- cascade bounds and the finest-cascade choice, including the inclusive edge and the outside case;
- probe positions for the first, last and centre probe of cascade 0, and cascade 5's cell size;
- trilinear weight sums and the probe-centre delta;
- local-volume containment, precedence, base corner and probe layout;
- the corner mapping — a probe-centre sample reads exactly that probe, and halfway samples blend
  the correct two probes on each axis;
- constant-environment SH (pi * radiance), a directional lobe, and zero-sample normalization;
- the interleaved update schedule with zero, one and two active local volumes.

## Next

1. GPU probe atlas and the volume layout buffer (the 48 B record is already fixed).
2. One compute dispatch per frame for the scheduled cascade/local volume: 64/32/16 visibility
   rays per probe, packed hit records, world radiance cache.
3. Irradiance update into the atlas and a final-gather lookup in the indirect pass.
4. The 4-coefficient SH can grow to 9 coefficients only after the atlas layout and sampling are
   stable; the report's 2-band SH matches the current choice.
