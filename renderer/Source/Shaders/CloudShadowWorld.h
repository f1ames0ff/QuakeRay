#ifndef CLOUD_SHADOW_WORLD_H
#define CLOUD_SHADOW_WORLD_H

#ifdef DESC_SET_CLOUD_SHADOW
layout(set = DESC_SET_CLOUD_SHADOW, binding = 0) uniform texture3D cloudShadowVolume;
layout(set = DESC_SET_CLOUD_SHADOW, binding = 1) uniform sampler cloudShadowVolume_Sampler;

#include "CloudShadowMap.h"

float getCloudSunTransmittance(vec3 worldPos, vec3 sunDir, bool extendShadow)
{
    const vec4 placement = globalUniform.cloudShadowPlacement;
    if (globalUniform.skyType != SKY_TYPE_PROCEDURAL || placement.x <= 0.5)
    {
        return 1.0;
    }
    const vec4 layer = globalUniform.cloudLayerMotion;
    float height = (worldPos.z - globalUniform.cameraPosition.z - layer.y) / max(layer.z, 1.0);
    float visibility = extendShadow
        ? exp(-cloudShadowTauNear(cloudShadowVolume, cloudShadowVolume_Sampler, worldPos, sunDir, placement, height))
        : cloudShadowTransmittance(cloudShadowVolume, cloudShadowVolume_Sampler, worldPos, sunDir, placement, height);
    return mix(1.0, visibility, clamp(layer.w, 0.0, 1.0));
}
#endif

#endif
