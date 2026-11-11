# id Tech 8 Phase 0 — M2 grid candidate histogram

Status: first measurement. Date: 2026-10-21.

Branch: `feat/idtech8-grid`, built from `master` after the legacy DTAL/cluster PR.

Command: `rt_grid_stats [cell] [cascades] [mode]`, CPU-only, no renderer change. `mode 0` inserts a light by its origin (center-in-cell); `mode 1` inserts it into every cell whose AABB intersects the light's influence sphere (`reach + radius + 32` Quake units).

## Maps measured

- Quake demo `the Necropolis`: 78 lights.
- Arcane Dimensions `In the beginning` (AD start): 347 lights, 95 clusters at the 128 fast-slot cap.

The AD start numbers decide the gate.

## AD start, 347 lights, mode 1 (overlap)

| cell | occupied | mean | p50 | p95 | p99 | max | over64 | over96 | over128 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 4096 | 89.72 | 91 | 128 | 135 | 141 | 3766 | 1171 | 197 |
| 128 | 3842 | 67.16 | 67 | 136 | 160 | 177 | 1991 | 1171 | 353 |
| 256 | 2060 | 50.70 | 39 | 137 | 164 | 185 | 686 | 403 | 178 |
| 512 | 439 | 56.00 | 47 | 153 | 184 | 203 | 150 | 91 | 52 |
| 1024 | 84 | 70.86 | 52 | 170 | 268 | 268 | 35 | 26 | 14 |

At reach 25 m, **64 IDs/cell is not viable**: p95 = 128 at 64 u, and 92% of occupied 64 u cells exceed 64. A 128-ID cell still overflows in 8.6% of 128 u cells and 4.8% of 64 u cells. Increasing the cell does not reduce the candidate count because the 25 m influence sphere covers many cells.

## Reach sweep (mode 1, AD start)

| reach | cell | p95 | max | over64 | over128 |
|---:|---:|---:|---:|---:|---:|
| 15 m | 64 | 77 | 86 | 1148 | 0 |
| 15 m | 128 | 82 | 125 | 486 | 0 |
| 15 m | 256 | 85 | 130 | 169 | 1 |
| 10 m | 64 | 65 | 74 | 202 | 0 |
| 10 m | 128 | 61 | 87 | 109 | 0 |
| 10 m | 256 | 59 | 110 | 45 | 0 |

Reach is the dominant variable. At 10 m, a 64-ID cell is nearly viable; at 15 m, 128 IDs are needed for the tail. At the shipped 25 m, 128 IDs are insufficient and 256 IDs are needed for the near-field cascades.

## Mode 0 (center-in-cell) is not a valid culling test

At reach 25 m, 64 u center mode reports p95 = 8 and max = 8, while overlap reports p95 = 128 and max = 141. Origin-only insertion drops the lights whose influence covers the cell but whose origin is elsewhere. The grid must insert by influence bounds; center mode is an optimistic lower bound only.

## Gate result

- **64 IDs/cell: rejected** for the AD start at the shipped 25 m reach.
- **128 IDs/cell: insufficient** at 64/128 u; the tail overflows.
- **128 fast + 128 tail per cell: viable for the near-field 2-cascade grid.** 8192 cells × 128 fast = 1,048,576 words (the existing list buffer) and 8192 × 128 tail = 1,048,576 entries (the existing tail buffer). It covers the 64/128 u max (141/177) without a new buffer.
- **256 IDs/cell: full near-field coverage** at 64/128/256 u, but needs a new list buffer (8192 × 256 = 2,097,152 words, 8 MiB) or a larger tail.
- **Reach 10 m + 64 IDs** is viable but changes the light domain; do not make it the default without a quality A/B.

## Architecture-first target

The capacity comparison is closed. The next work is the full id Tech 8 structure, not a 64/128 parameter sweep.

Target grid, following the public id Tech 8 cascaded light grid:

- 16 x 16 x 16 cells per cascade, 8 exponential cascades = 32,768 cells.
- First cascade 32 m bounds, 2 m^3 per cell; later cascades double.
- 64 typed IDs per cell: lights 16-bit, reflection probes 8-bit, decals 16-bit.
- One ID table, typed entries, and per-type index arrays; about 30 MB for the grid.
- GPU build and publication; the CPU builder is a reference oracle, not the runtime path.
- World-space cell lookup for direct NEE, indirect NEE, particles and glass; the cluster list stays as the fallback outside the cascades.

Target volumes:

- 16 x 16 x 16 x 6 cascaded irradiance volumes, plus local 10 x 10 x 10 volumes.
- One cascade and one volume updated per frame, interleaved.
- 64 / 32 / 16 visibility rays per probe by quality.
- Probe NEE reads the same typed grid; no separate per-probe light list.

Migration order:

1. Grid ABI: 8 cascades, 32,768 cells, 64 typed IDs, new publication buffers. No tail in v1.
2. GPU grid builder and publication.
3. Direct NEE, then indirect NEE, particles and glass; keep the cluster fallback.
4. Cascaded and local irradiance/radiance volumes.
5. Only then tune capacity, reach, cell size and cascade count.

## Current runtime

The runtime path behind `rt_light_grid` (default `0`) now follows the target grid shape:

- 8 cascades x 16^3 = 32,768 cells; base cell 80 u (about 2 m); first cascade 1,280 u bounds.
- 64 IDs per cell, no tail; typed light IDs use the low 16 bits and type 0.
- The CPU reference builder is no longer run in the frame. The GPU compute builder builds every cell, ranks candidates by `power / distance^2` (unit weight for zero-power map emitters), compacts the valid IDs, and publishes a per-cell count.
- The grid is rebuilt when the light set, its origin, reach, radius or power, or the 640 u camera snap changes.
- Direct NEE and indirect NEE sample the grid; particles and glass read the same typed cell lists. The cluster lists remain the fallback outside the cascades and for empty cells.
- `rt_light_grid_report [cell]` still reports the CPU reference statistics; the runtime report is the GPU builder and does not keep a CPU cell list.
- Cell indices are `cascade * 4096 + (z * 16 + y) * 16 + x`.

`qray_light_grid_tests` covers the cascade index, capacity, empty/out-of-range sources and determinism. `qray_cluster_lists_tests` covers the cluster fallback.

## Missing measurements

- `ad_tfuma`, `ad_swampy`, and the user's save at the same settings.
- Change-frame cost at HEAD (M3; the legacy 30 ms figure predates `3e59c3da`).
- `rt_gi_level 1` vs 2 with a cvar dump (A1).
- GPU build/publication cost of the 8-cascade grid.
- Grid discontinuity and denoiser artifacts on the dense AD scene.

## Next step

Implement the cascaded irradiance and radiance volumes: 16^3 x 6 cascades plus local 10^3 volumes, one cascade and one local volume updated per frame, and probe NEE through the typed grid. Capacity, reach and cell-size tuning stay deferred until the volume path exists.
