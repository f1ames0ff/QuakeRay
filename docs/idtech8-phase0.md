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

## Proposed first grid prototype

- 2 cascades × 16³ = 8192 cells, cell 64 u and 128 u, camera-centred on a 512 u snap.
- 128 fast slots + 128 tail entries per cell, within the existing 128-entry list and 1,048,576-entry tail capacities, and the existing 16-strata + tail shader path.
- Cells outside the 2 cascades fall back to the existing cluster list.
- This covers the near field (2048 u = 52 m); the outer map keeps the cluster path until the cascade count/ABI is expanded.
- The 64-ID id Tech 8 parameter is replaced by 128 + 128; the sampling loop and per-sample ray cost are unchanged.

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

The current two-cascade CPU builder is a scaffold and a test oracle. It is not the final grid, and its 128 + 128 layout is not the target ABI.

## 2A.1 builder on the branch

The first prototype layer is implemented behind `rt_light_grid` (default `0`). It builds the two-cascade grid on the CPU, without changing shading:

- 2 cascades × 16³ = 8192 cells; cells 64 u and 128 u; camera-centred on a 512 u snap.
- 128 fast entries + 128 tail entries per cell.
- A light is inserted into every cell its influence sphere (`reach + radius + 32`) intersects.
- Fast entries are the nearest 128 candidates; the remaining candidates up to 128 form the alias tail.
- The grid is rebuilt when the light set, its reach/origin, or the 512 u camera snap changes.
- `rt_light_grid_report [cell]` prints cell counts, candidate percentiles, fast/tail entries, dropped tail candidates, build time, and the fast/tail UID lists of one cell.
- Cell indices are `cascade * 4096 + (z * 16 + y) * 16 + x`.

The builder does not feed the shaders yet; the direct-NEE path and the cluster fallback are the next step. `qray_light_grid_tests` covers the capacity split, the cascade index, empty/out-of-range sources and determinism.

## Missing measurements

- `ad_tfuma`, `ad_swampy`, and the user's save at the same settings.
- Change-frame cost at HEAD (M3; the legacy 30 ms figure predates `3e59c3da`).
- `rt_gi_level 1` vs 2 with a cvar dump (A1).
- CPU build cost on the real maps (the report prints it).
- GPU publication cost and direct-NEE cost of the 2-cascade grid.

## Next step

Publish the 2-cascade 128 + 128 grid to the renderer, route direct NEE through it behind `rt_light_grid 0/1`, keep the cluster fallback, and measure build cost, direct cost, shadow-ray rejection, and quality before considering a new 5-cascade ABI.
