#include "ShaderCommonHLSL.hlsli"

struct ShLightGridSource
{
    float4 originReach;
    float  radius;
    float  power;
    uint   lightIndex;
    uint   pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> gridHeader;
[[vk::binding(1, 0)]] StructuredBuffer<ShLightGridSource> gridSources;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gridOffsets;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> gridIds;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> gridCounts;

#define GRID_ID_INVALID 0xFFFFFFFFu

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint cell = dispatchThreadId.x;

    if (cell >= uint(Q2_LIGHT_GRID_CELLS))
    {
        return;
    }

    const uint offsetBase = uint(Q2_LIGHT_GRID_HEADER_WORDS) + cell;
    const uint base = cell * uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL);

    gridOffsets[offsetBase] = base;
    gridOffsets[offsetBase + 1u] = base + uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL);
    gridCounts[cell] = 0u;

    for (uint i = 0; i < uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL); i++)
    {
        gridIds[base + i] = GRID_ID_INVALID;
    }

    if (gridHeader[0] == 0u)
    {
        return;
    }

    const uint sourceCount = min(gridHeader[34], uint(Q2_LIGHT_GRID_SOURCE_CAPACITY));

    if (sourceCount == 0u)
    {
        return;
    }

    const uint cascade = cell / uint(Q2_LIGHT_GRID_CELLS / Q2_LIGHT_GRID_CASCADES);
    const uint local = cell % uint(Q2_LIGHT_GRID_CELLS / Q2_LIGHT_GRID_CASCADES);
    const uint x = local & 15u;
    const uint y = (local >> 4u) & 15u;
    const uint z = local >> 8u;

    const float cellSize = asfloat(gridHeader[26u + cascade]);
    const float3 origin = float3(
        asfloat(gridHeader[2u + cascade * 3u + 0u]),
        asfloat(gridHeader[2u + cascade * 3u + 1u]),
        asfloat(gridHeader[2u + cascade * 3u + 2u]));
    const float3 center = origin + (float3(x, y, z) + 0.5) * cellSize;
    const float halfCell = cellSize * 0.5;

    float bestScore[Q2_LIGHT_GRID_MAX_IDS_PER_CELL];
    uint  bestIndex[Q2_LIGHT_GRID_MAX_IDS_PER_CELL];

    for (uint i = 0; i < uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL); i++)
    {
        bestScore[i] = -1.0;
        bestIndex[i] = GRID_ID_INVALID;
    }

    for (uint sourceIndex = 0u; sourceIndex < sourceCount; sourceIndex++)
    {
        const ShLightGridSource source = gridSources[sourceIndex];

        if (source.lightIndex == GRID_ID_INVALID)
        {
            continue;
        }

        const float influence = source.originReach.w + source.radius + 32.0;
        const float3 delta = max(abs(source.originReach.xyz - center) - halfCell, 0.0);
        const float dist2 = dot(delta, delta);

        if (dist2 > influence * influence)
        {
            continue;
        }

        const float scale = max(1.0, source.radius * source.radius);
        const float weight = source.power > 0.0 ? source.power : 1.0;
        const float score = weight / max(dist2, scale);

        if (!(score > bestScore[0]))
        {
            continue;
        }

        bestScore[0] = score;
        bestIndex[0] = source.lightIndex;

        uint root = 0u;

        for (;;)
        {
            uint child = root * 2u + 1u;

            if (child >= uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL))
            {
                break;
            }

            if (child + 1u < uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL) && bestScore[child + 1u] < bestScore[child])
            {
                child++;
            }

            if (bestScore[child] >= bestScore[root])
            {
                break;
            }

            const float swapScore = bestScore[root];
            const uint swapIndex = bestIndex[root];
            bestScore[root] = bestScore[child];
            bestIndex[root] = bestIndex[child];
            bestScore[child] = swapScore;
            bestIndex[child] = swapIndex;
            root = child;
        }
    }

    uint compact[Q2_LIGHT_GRID_MAX_IDS_PER_CELL];
    uint count = 0u;

    for (uint i = 0; i < uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL); i++)
    {
        if (bestIndex[i] != GRID_ID_INVALID)
        {
            compact[count] = bestIndex[i];
            count++;
        }
    }

    for (uint i = 0; i < uint(Q2_LIGHT_GRID_MAX_IDS_PER_CELL); i++)
    {
        gridIds[base + i] = i < count ? (compact[i] & 0xFFFFu) : GRID_ID_INVALID;
    }

    gridCounts[cell] = count;
}
