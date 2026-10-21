// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#ifndef Q2_LIGHT_GRID_HLSLI_
#define Q2_LIGHT_GRID_HLSLI_

#define Q2_LIGHT_GRID_ID_INDEX_MASK 0xFFFFu
#define Q2_LIGHT_GRID_ID_TYPE_SHIFT 30u
#define Q2_LIGHT_GRID_ID_TYPE_MASK 3u
#define Q2_LIGHT_GRID_ID_TYPE_LIGHT 0u

int q2GetLightGridCell(const float3 p)
{
    if (q2LightGridOffsets[0] == 0u)
    {
        return -1;
    }

    const uint cascades = min(q2LightGridOffsets[1], uint(Q2_LIGHT_GRID_CASCADES));

    for (uint cascade = 0; cascade < cascades; cascade++)
    {
        const float3 origin = float3(
            asfloat(q2LightGridOffsets[2u + cascade * 3u + 0u]),
            asfloat(q2LightGridOffsets[2u + cascade * 3u + 1u]),
            asfloat(q2LightGridOffsets[2u + cascade * 3u + 2u]));
        const float cell = asfloat(q2LightGridOffsets[2u + uint(Q2_LIGHT_GRID_CASCADES) * 3u + cascade]);

        if (!(cell > 0.0))
        {
            continue;
        }

        const float3 local = (p - origin) / cell;

        if (local.x < 0.0 || local.y < 0.0 || local.z < 0.0 ||
            local.x >= float(Q2_LIGHT_GRID_DIM) || local.y >= float(Q2_LIGHT_GRID_DIM) ||
            local.z >= float(Q2_LIGHT_GRID_DIM))
        {
            continue;
        }

        const uint x = uint(local.x);
        const uint y = uint(local.y);
        const uint z = uint(local.z);
        const uint cellsPerCascade = uint(Q2_LIGHT_GRID_DIM) * uint(Q2_LIGHT_GRID_DIM) * uint(Q2_LIGHT_GRID_DIM);

        return int(cascade * cellsPerCascade + (z * uint(Q2_LIGHT_GRID_DIM) + y) * uint(Q2_LIGHT_GRID_DIM) + x);
    }

    return -1;
}

uint q2GetLightGridCount(const int gridCell)
{
    if (gridCell < 0 || gridCell >= int(Q2_LIGHT_GRID_CELLS))
    {
        return 0u;
    }

    return min(q2LightGridCounts[uint(gridCell)], uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL));
}

uint q2GetLightGridLight(const int gridCell, const uint slot)
{
    const uint count = q2GetLightGridCount(gridCell);

    if (slot >= count)
    {
        return 0xFFFFFFFFu;
    }

    const uint cell = uint(gridCell);
    const uint base = q2LightGridOffsets[uint(Q2_LIGHT_GRID_HEADER_WORDS) + cell];
    const uint id = q2LightGridIds[base + slot];

    if (((id >> Q2_LIGHT_GRID_ID_TYPE_SHIFT) & Q2_LIGHT_GRID_ID_TYPE_MASK) != Q2_LIGHT_GRID_ID_TYPE_LIGHT)
    {
        return 0xFFFFFFFFu;
    }

    return id & Q2_LIGHT_GRID_ID_INDEX_MASK;
}

#endif
