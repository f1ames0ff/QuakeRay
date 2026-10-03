#define DESC_SET_CAUSTICS 0

#include "Caustics.hlsli"

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] StructuredBuffer<uint4> causticsFrameCells;
[[vk::binding(2, DESC_SET_CAUSTICS)]] RWStructuredBuffer<float4> causticsHistoryCells;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const CausticsParams_BT params = causticsParams[0];
    const uint resolution = params.gridSize.x;

    if (dispatchThreadID.x >= resolution || dispatchThreadID.y >= resolution)
    {
        return;
    }

    const uint index = dispatchThreadID.y * resolution + dispatchThreadID.x;

    const uint4 frame = causticsFrameCells[index];
    const float4 decoded = float4((float3)frame.xyz / CAUSTICS_FLUX_SCALE, (float)frame.w);

    const float weight = params.accumParams.x;
    const bool reset = params.accumParams.y != 0.0;

    if (reset || weight >= 1.0)
    {
        causticsHistoryCells[index] = decoded;
        return;
    }

    const float4 history = causticsHistoryCells[index];

    causticsHistoryCells[index] = lerp(history, decoded, saturate(weight));
}
