#ifndef CLOUD_SHADOW_WORLD_HLSLI
#define CLOUD_SHADOW_WORLD_HLSLI

#ifdef DESC_SET_CLOUD_SHADOW
[[vk::binding(0, DESC_SET_CLOUD_SHADOW)]] Texture3D<float> cloudShadowVolume;
[[vk::binding(1, DESC_SET_CLOUD_SHADOW)]] SamplerState cloudShadowVolume_Sampler;

#include "CloudShadowMap.hlsli"

float getCloudSunTransmittance(float3 worldPos, float3 sunDir, bool extendShadow)
{
    const float4 placement = globalUniform.cloudShadowPlacement;
    if (globalUniform.skyType != SKY_TYPE_PROCEDURAL || placement.x <= 0.5)
    {
        return 1.0;
    }
    const float4 layer = globalUniform.cloudLayerMotion;
    float height = (worldPos.z - globalUniform.cameraPosition.z - layer.y) / max(layer.z, 1.0);
    float visibility = extendShadow
        ? exp(-cloudShadowTauNear(cloudShadowVolume, cloudShadowVolume_Sampler, worldPos, sunDir, placement, height))
        : cloudShadowTransmittance(cloudShadowVolume, cloudShadowVolume_Sampler, worldPos, sunDir, placement, height);
    return lerp(1.0, visibility, clamp(layer.w, 0.0, 1.0));
}
#endif

#endif
