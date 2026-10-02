// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

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
