#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TEXTURES       0
#define DESC_SET_LIGHT_SOURCES  6
#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "Smoke.hlsli"
#include "Light.hlsli"
#include "Q2ClusterLights.hlsli"
#include "SmokeLight.hlsli"

#define PROBE_DESC_SET 8

[[vk::binding(0, PROBE_DESC_SET)]] RWStructuredBuffer<float> probeOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const float3 worldPos = (float3)dispatchThreadID * 0.25;
    const uint   cluster  = dispatchThreadID.x;

    const float3 lit = smokeLightAt(worldPos, cluster, (float)dispatchThreadID.y);

    probeOutput[dispatchThreadID.x] = lit.x + lit.y + lit.z;
}
