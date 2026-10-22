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

// Independent implementation of standard froxel volumetric techniques; no reference
// renderer source is copied.

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

float shaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55 * asymmetry - 0.55 * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denominator * denominator);
}

float shaftDensity(float3 p)
{
    const float3 distance = abs((p - params.worldCenter.xyz) * params.worldHalfSizeInv.xyz);
    const float edge = max(distance.x, max(distance.y, distance.z));
    return 1.0 - smoothstep(0.75, 1.5, edge);
}

float3 volumeRayDirection(float2 cellUv)
{
    const float2 ndc = cellUv * 2.0 - 1.0;
    float4 farPoint = mul(globalUniform.volumeViewProjInv, float4(ndc, 1.0, 1.0));
    farPoint.xyz /= farPoint.w;
    return normalize(farPoint.xyz - globalUniform.cameraPosition.xyz);
}

float3 volumeGetCenter(int3 cell)
{
    const float2 cellUv = ((float2)cell.xy + 0.5) /
                          float2(LIGHT_SHAFT_VOLUME_SIZE_X, LIGHT_SHAFT_VOLUME_SIZE_Y);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float distance = nearPlane + (farPlane - nearPlane) * saturate((cell.z + 0.5) /
                                                                          (float)LIGHT_SHAFT_VOLUME_SIZE_Z);
    return globalUniform.cameraPosition.xyz + volumeRayDirection(cellUv) * distance;
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
    const float density = 0.02 * shaftDensity(center);
    if (density <= 0.0)
    {
        lightShaftVolume[cell] = (float4)0;
        return;
    }
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

        const bool isArea = abs(shaft.tangentU.w) + abs(shaft.tangentV.w) > 0.0001;
        float3 sourcePoint = source;

        if (isArea)
        {
            const float3 sourceDelta = center - source;
            const float u = clamp(dot(sourceDelta, shaft.tangentU.xyz) / max(abs(shaft.tangentU.w), 0.0001),
                                  -1.0, 1.0);
            const float v = clamp(dot(sourceDelta, shaft.tangentV.xyz) / max(abs(shaft.tangentV.w), 0.0001),
                                  -1.0, 1.0);
            sourcePoint += shaft.tangentU.xyz * (u * shaft.tangentU.w) +
                           shaft.tangentV.xyz * (v * shaft.tangentV.w);
        }

        const float3 emitter = sourcePoint + lightDir * max(radius, 0.5);
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
        const float phase = shaftPhase(
            dot(-fromLight / distToLight, toViewer), 0.5);

        radiance += shaft.color.rgb * (cone * attenuation * phase);
    }

    lightShaftVolume[cell] = float4(radiance * density, 0.0);
}
