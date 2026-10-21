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

#define DESC_SET_TLAS 0
#define DESC_SET_LIGHT_SHAFT_PARAMS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_LIGHT_SHAFT_VOLUME 3

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"

#define LIGHT_SHAFT_VOLUME_SIZE_X 160
#define LIGHT_SHAFT_VOLUME_SIZE_Y 88
#define LIGHT_SHAFT_VOLUME_SIZE_Z 64

[[vk::binding(0, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] ConstantBuffer<LightShaftVolumeParams_BT> params;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

static const float PI = 3.1415926535;

float ScatterPhase_HenyeyGreenstein(float cosa, float g)
{
    const float g_sqr = g * g;
    const float num = (1.0 - abs(g));
    const float denom = sqrt(max(1.0 - 2.0 * g * cosa + g_sqr, 0.0));
    const float frac = num / denom;
    const float scale = g_sqr + (1.0 - g_sqr) / (4.0 * PI);
    return scale * (frac * frac * frac);
}

float getDensity(float3 p)
{
    float3 bounds = clamp(3.0 - 2.0 * abs((p - params.worldCenter.xyz) * params.worldHalfSizeInv.xyz),
                          (float3)0, (float3)1);
    return bounds.x * bounds.y * bounds.z;
}

float3 volumeGetCenter(int3 cell)
{
    const float3 local = ((float3)cell + 0.5) /
                         float3(LIGHT_SHAFT_VOLUME_SIZE_X, LIGHT_SHAFT_VOLUME_SIZE_Y, LIGHT_SHAFT_VOLUME_SIZE_Z);

    float4 ndc = float4(local.x * 2.0 - 1.0, local.y * 2.0 - 1.0, 0.1, 1.0);
    float4 worldPos = mul(globalUniform.volumeViewProjInv, ndc);
    worldPos.xyz /= worldPos.w;

    const float3 cameraPos = globalUniform.cameraPosition.xyz;
    const float3 direction = normalize(worldPos.xyz - cameraPos);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float distance = lerp(nearPlane, farPlane, saturate(local.z));

    return cameraPos + direction * distance;
}

float hash1(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value & 0x00ffffffu) / 16777215.0;
}

bool lightShaftVisible(float3 origin, float3 target)
{
    const float3 delta = target - origin;
    const float distance = length(delta);

    if (distance <= 0.05)
    {
        return true;
    }

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = delta / distance;
    ray.TMin = 0.05;
    ray.TMax = distance - 0.05;

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, globalUniform.rayCullMaskWorld, ray);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

[numthreads(4, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int3 cell = int3(dispatchThreadID);
    if (cell.x >= LIGHT_SHAFT_VOLUME_SIZE_X || cell.y >= LIGHT_SHAFT_VOLUME_SIZE_Y ||
        cell.z >= LIGHT_SHAFT_VOLUME_SIZE_Z)
    {
        return;
    }

    const uint shaftCount = min(params.lightShaftCount, (uint)MAX_LIGHT_SHAFTS);
    if (shaftCount == 0)
    {
        lightShaftVolume[cell] = (float4)0;
        return;
    }

    const float3 center0 = volumeGetCenter(cell);
    const float3 centerZ = volumeGetCenter(cell + int3(0, 0, 1));
    const uint seed = (uint)(cell.x * 1973 + cell.y * 9277 + (int)globalUniform.frameId * 7331);
    const float jitter = hash1(seed) - 0.5;
    const float3 center = center0 + jitter * (centerZ - center0);
    const float3 toViewer = normalize(globalUniform.cameraPosition.xyz - center);
    const float density = 0.02 * getDensity(center);
    float3 radiance = (float3)0;

    for (uint i = 0; i < shaftCount; ++i)
    {
        const LightShaft_BT shaft = params.lightShafts[i];
        const float3 source = shaft.positionRadius.xyz;
        const float radius = max(shaft.positionRadius.w, 0.001);
        float3 lightDir = shaft.direction.xyz;

        if (dot(lightDir, lightDir) <= 0.000001)
        {
            continue;
        }

        lightDir = normalize(lightDir);

        const float3 emitter = source + lightDir * max(radius, 0.5);
        const float3 fromLight = center - emitter;
        const float distToLight = length(fromLight);

        if (distToLight < 0.001)
        {
            continue;
        }

        const float cosAngle = dot(fromLight / distToLight, lightDir);
        const float cone = smoothstep(shaft.cone.y, shaft.cone.x, cosAngle);

        if (cone <= 0.0)
        {
            continue;
        }

        if (!lightShaftVisible(center, emitter))
        {
            continue;
        }

        const float attenuation = radius * radius / (distToLight * distToLight + radius * radius);
        const float phase = ScatterPhase_HenyeyGreenstein(
            dot(-fromLight / distToLight, toViewer), 0.5);

        radiance += shaft.color.rgb * (cone * attenuation * phase);
    }

    lightShaftVolume[cell] = float4(radiance * density, 0.0);
}
