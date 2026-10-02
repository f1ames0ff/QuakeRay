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

struct CmCloudShadowParams_BT
{
    float4 sunDirection;
    float4 cloudLayer;
    float4 cloudMarch;
    float4 mapProjection;
};

[[vk::binding(0, 0), vk::image_format("r16f")]] RWTexture3D<float> cloudShadowOut;
[[vk::binding(1, 0)]] ConstantBuffer<CmCloudShadowParams_BT> params;

#include "CloudLayer.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 ipos = (int2)dispatchThreadID.xy;
    int size = (int)(params.mapProjection.w + 0.5);

    if (ipos.x >= size || ipos.y >= size)
    {
        return;
    }

    CloudLayer layer;
    layer.altitude  = params.sunDirection.w;
    layer.thickness = max(params.cloudLayer.x, 1.0);
    layer.coverage  = clamp(params.cloudLayer.y, 0.0, 0.99);
    layer.density   = max(params.cloudLayer.z, 0.0);
    layer.detail    = clamp(params.cloudLayer.w, 0.0, 0.9);
    layer.time      = params.cloudMarch.x;
    layer.speed     = params.cloudMarch.y;

    int steps = CLOUD_SHADOW_STEPS;

    float extent = max(params.mapProjection.z, 1.0);
    float2 flatPos = params.mapProjection.xy + ((float2)ipos + 0.5) / (float)size * extent;

    float3 sunDir = normalize(params.sunDirection.xyz);
    float3 origin = float3(flatPos.x, flatPos.y, layer.altitude);
    origin.xy += sunDir.xy * (params.cloudMarch.w / max(sunDir.z, 1.0e-3));

    float dt = layer.thickness / max(sunDir.z, 1.0e-3) / (float)steps;
    float stepColumn[CLOUD_SHADOW_STEPS];

    for (int i = 0; i < steps; i++)
    {
        float at = ((float)i + 0.5) * dt;
        stepColumn[i] = cloudDensity(layer, cloudSunSample(origin, sunDir, at, dt), false) * dt;
    }

    uint sizeX, sizeY, slices;
    cloudShadowOut.GetDimensions(sizeX, sizeY, slices);

    for (int k = 0; k < (int)slices; k++)
    {
        float height = (float)k / (float)max((int)slices - 1, 1);
        float above = 0.0;

        for (int i = 0; i < steps; i++)
        {
            float fraction = clamp((float)(i + 1) - height * (float)steps, 0.0, 1.0);
            above += stepColumn[i] * fraction;
        }

        cloudShadowOut[int3(ipos, k)] = cloudOpticalDepth(layer, above);
    }
}
