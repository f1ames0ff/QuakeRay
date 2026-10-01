#define DESC_SET_GLOBAL_UNIFORM 2
#ifndef DESC_SET_CLOUD_SHADOW
#define DESC_SET_CLOUD_SHADOW 12
#endif

#include "ShaderCommonHLSL.hlsli"
#include "CloudShadowWorld.hlsli"
#include "SkyMotion.hlsli"

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> probeOutput;
[[vk::binding(0, 8)]] TextureCube<float4> skyCube;
[[vk::binding(2, 8)]] SamplerState skySampler;

#ifdef WITH_PUSH_CONSTANTS
struct ProbePush
{
    uint index;
};
[[vk::push_constant]] ConstantBuffer<ProbePush> push;
#endif

[numthreads(1, 1, 1)]
void main()
{
    const float3 sunDir = normalize(float3(0.3, 0.4, sqrt(0.75)));
    const float3 rayDir = float3(0.0, 0.0, 1.0);
    float2 infinite = getMotionForInfinitePoint(rayDir);
    probeOutput[0] = float4(infinite, getMotionForCloudLayer(rayDir, infinite));

    float3 base = globalUniform.cameraPosition.xyz + float3(0.0, 0.0, globalUniform.cloudLayerMotion.y);
    float3 middle = base + sunDir * (0.5 * globalUniform.cloudLayerMotion.z / sunDir.z);
    float3 above = base + sunDir * (1.1 * globalUniform.cloudLayerMotion.z / sunDir.z);
    float3 outside = base + float3(globalUniform.cloudShadowPlacement.w * 10.0, 0.0, 0.0);
    probeOutput[1] = float4(getCloudSunTransmittance(base, sunDir, false),
        getCloudSunTransmittance(middle, sunDir, false),
        getCloudSunTransmittance(above, sunDir, true),
        getCloudSunTransmittance(outside, sunDir, false));
    probeOutput[2] = skyCube.SampleLevel(skySampler, rayDir, 0.0);
    probeOutput[3] = skyCube.SampleLevel(skySampler, -rayDir, 0.0);
#ifdef WITH_PUSH_CONSTANTS
    probeOutput[3].w = float(push.index);
#endif
}
