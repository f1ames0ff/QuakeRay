// Copyright (c) 2026 QuakeRay contributors
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

#ifndef RAYGEN_COMMON_HLSLI_
#define RAYGEN_COMMON_HLSLI_


#include "ShaderCommonHLSLFunc.hlsli"



#if !defined(DESC_SET_TLAS) || \
    !defined(DESC_SET_GLOBAL_UNIFORM) || \
    !defined(DESC_SET_VERTEX_DATA) || \
    !defined(DESC_SET_TEXTURES) || \
    !defined(DESC_SET_RANDOM) || \
    !defined(DESC_SET_LIGHT_SOURCES)
        #error Descriptor set indices must be set!
#endif

#define LIGHT_SAMPLE_METHOD_NONE 0
#define LIGHT_SAMPLE_METHOD_DIRECT 1
#define LIGHT_SAMPLE_METHOD_INDIR 2
#define LIGHT_SAMPLE_METHOD_GRADIENTS 3
#define LIGHT_SAMPLE_METHOD_INITIAL 4
#define LIGHT_SAMPLE_METHOD_VOLUME 5
#if !defined(LIGHT_SAMPLE_METHOD)
    #error Light sampling method must be defined
#endif



#include "Surface.hlsli"
#include "Light.hlsli"
#include "Media.hlsli"
#include "RayCone.hlsli"
#include "TurbWarp.hlsli"

#define HITINFO_INL_PRIM
    #include "HitInfo.hlsli"
#undef HITINFO_INL_PRIM

#define HITINFO_INL_RFL
    #include "HitInfo.hlsli"
#undef HITINFO_INL_RFL

#define HITINFO_INL_INDIR
    #include "HitInfo.hlsli"
#undef HITINFO_INL_INDIR



[[vk::binding(BINDING_ACCELERATION_STRUCTURE_MAIN, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;

#ifdef DESC_SET_CUBEMAPS
[[vk::binding(BINDING_CUBEMAPS, DESC_SET_CUBEMAPS)]] TextureCube globalCubemaps[];
[[vk::binding(BINDING_CUBEMAPS_SAMPLER, DESC_SET_CUBEMAPS)]] SamplerState globalCubemaps_Sampler[];
#endif

#ifdef DESC_SET_RENDER_CUBEMAP
[[vk::binding(BINDING_RENDER_CUBEMAP, DESC_SET_RENDER_CUBEMAP)]] TextureCube renderCubemap;
[[vk::binding(BINDING_RENDER_CUBEMAP_SAMPLER, DESC_SET_RENDER_CUBEMAP)]] SamplerState renderCubemap_Sampler;
[[vk::binding(BINDING_RENDER_CUBEMAP_ENV, DESC_SET_RENDER_CUBEMAP)]] TextureCube renderCubemapEnv;
[[vk::binding(BINDING_RENDER_CUBEMAP_ENV_SAMPLER, DESC_SET_RENDER_CUBEMAP)]] SamplerState renderCubemapEnv_Sampler;
#endif

#ifdef DESC_SET_PORTALS
struct PortalInstances_BT
{
    ShPortalInstance g_portals[PORTAL_MAX_COUNT];
};
[[vk::binding(BINDING_PORTAL_INSTANCES, DESC_SET_PORTALS)]] ConstantBuffer<PortalInstances_BT> portalInstances;
#endif





uint getPrimaryVisibilityCullMask()
{
    return globalUniform.rayCullMaskWorld | INSTANCE_MASK_REFRACT | INSTANCE_MASK_FIRST_PERSON;
}

uint getReflectionRefractionCullMask(uint surfInstCustomIndex, uint geometryInstanceFlags, bool isRefraction)
{
    uint world = globalUniform.rayCullMaskWorld | INSTANCE_MASK_REFRACT;

    if( ( geometryInstanceFlags & GEOM_INST_FLAG_IGNORE_REFRACT_AFTER ) != 0 )
    {
        world = world & ( ~INSTANCE_MASK_REFRACT );

        if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON) != 0)
        {
            return world;
        }
    }

    if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON) != 0)
    {
        return world | INSTANCE_MASK_FIRST_PERSON;
    }

    return isRefraction ?
        world | INSTANCE_MASK_FIRST_PERSON :
        world | INSTANCE_MASK_FIRST_PERSON_VIEWER;
}

uint getShadowCullMask(uint surfInstCustomIndex)
{
    const uint world = globalUniform.rayCullMaskWorld_Shadow;

    if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON) != 0)
    {
        return world | INSTANCE_MASK_FIRST_PERSON;
    }
    else if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER) != 0)
    {
        return world | INSTANCE_MASK_FIRST_PERSON_VIEWER;
    }
    else
    {
        return world | INSTANCE_MASK_FIRST_PERSON_VIEWER;
    }
}

uint getIndirectIlluminationCullMask(uint surfInstCustomIndex, uint extraMask)
{
    const uint world = globalUniform.rayCullMaskWorld | extraMask;

    if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON) != 0)
    {
        return world | INSTANCE_MASK_FIRST_PERSON;
    }
    else if ((surfInstCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER) != 0)
    {
        return world | INSTANCE_MASK_FIRST_PERSON_VIEWER;
    }
    else
    {
        return world | INSTANCE_MASK_FIRST_PERSON_VIEWER;
    }
}



uint getAdditionalRayFlags()
{
    return globalUniform.rayCullBackFaces != 0 ? RAY_FLAG_CULL_FRONT_FACING_TRIANGLES : 0;
}



bool doesPayloadContainHitInfo(const ShPayload p)
{
    if (p.instIdAndIndex == UINT32_MAX || p.geomAndPrimIndex == UINT32_MAX)
    {
        return false;
    }

    int instanceId, instanceCustomIndex;
    unpackInstanceIdAndCustomIndex(p.instIdAndIndex, instanceId, instanceCustomIndex);

    if ((instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_SKY) != 0)
    {
        return false;
    }

    return true;
}

void resetPayload(inout ShPayload g_payload)
{
    g_payload.baryCoords = (float2)0.0;
    g_payload.instIdAndIndex = UINT32_MAX;
    g_payload.geomAndPrimIndex = UINT32_MAX;
}

ShPayload tracePrimaryRay(float3 origin, float3 direction)
{
    ShPayload g_payload;
    resetPayload(g_payload);

    uint cullMask = getPrimaryVisibilityCullMask();

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.TMin = globalUniform.primaryRayMinDist;
    rayDesc.Direction = direction;
    rayDesc.TMax = globalUniform.rayLength;

    TraceRay(
        topLevelAS,
        getAdditionalRayFlags(),
        cullMask,
        0, 0,
        SBT_INDEX_MISS_DEFAULT,
        rayDesc,
        g_payload);

    return g_payload;
}

ShPayload traceReflectionRefractionRay(float3 origin, float3 direction, uint surfInstCustomIndex, uint geometryInstanceFlags, bool isRefraction)
{
    ShPayload g_payload;
    resetPayload(g_payload);

    uint cullMask = getReflectionRefractionCullMask(surfInstCustomIndex, geometryInstanceFlags, isRefraction);

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.TMin = 0.001;
    rayDesc.Direction = direction;
    rayDesc.TMax = globalUniform.rayLength;

    TraceRay(
        topLevelAS,
        getAdditionalRayFlags(),
        cullMask,
        0, 0,
        SBT_INDEX_MISS_DEFAULT,
        rayDesc,
        g_payload);

    return g_payload;
}

ShPayload traceIndirectRay(uint surfInstCustomIndex, float3 surfPosition, float3 bounceDirection, uint extraMask)
{
    ShPayload g_payload;
    resetPayload(g_payload);

    uint cullMask = getIndirectIlluminationCullMask(surfInstCustomIndex, extraMask);

    RayDesc rayDesc;
    rayDesc.Origin = surfPosition;
    rayDesc.TMin = 0.001;
    rayDesc.Direction = bounceDirection;
    rayDesc.TMax = globalUniform.rayLength;

    TraceRay(
        topLevelAS,
        getAdditionalRayFlags(),
        cullMask,
        0, 0,
        SBT_INDEX_MISS_DEFAULT,
        rayDesc,
        g_payload);

    return g_payload;
}



#ifdef DESC_SET_CUBEMAPS
float3 getSkyPrimary(float3 direction)
{
    uint skyType = globalUniform.skyType;

#ifdef DESC_SET_RENDER_CUBEMAP
    if (skyType == SKY_TYPE_RASTERIZED_GEOMETRY || skyType == SKY_TYPE_PROCEDURAL)
    {
        return renderCubemap.SampleLevel(renderCubemap_Sampler, direction, 0.0).rgb;
    }
#endif

    if (skyType == SKY_TYPE_CUBEMAP)
    {
        direction = mul((float3x3)globalUniform.skyCubemapRotationTransform, direction);

        return globalCubemaps[NonUniformResourceIndex(globalUniform.skyCubemapIndex)].SampleLevel(globalCubemaps_Sampler[NonUniformResourceIndex(globalUniform.skyCubemapIndex)], direction, 0.0).rgb;
    }

    return globalUniform.skyColorDefault.xyz;
}

float3 getSky(float3 direction)
{
    float3 col = getSkyPrimary(direction);
    return col * globalUniform.skyColorMultiplier;
}

#define SKY_MIP_COUNT 11.0
#define SKY_DIFFUSE_BOUNCE_LOD 6.0

float3 getSkyFiltered(float3 direction, float lod)
{
    uint skyType = globalUniform.skyType;

#ifdef DESC_SET_RENDER_CUBEMAP
    if (skyType == SKY_TYPE_RASTERIZED_GEOMETRY)
    {
        return renderCubemap.SampleLevel(renderCubemap_Sampler, direction, lod).rgb;
    }

    if (skyType == SKY_TYPE_PROCEDURAL)
    {
        return renderCubemapEnv.SampleLevel(renderCubemapEnv_Sampler, direction, lod).rgb;
    }
#endif

    if (skyType == SKY_TYPE_CUBEMAP)
    {
        direction = mul((float3x3)globalUniform.skyCubemapRotationTransform, direction);
        return globalCubemaps[NonUniformResourceIndex(globalUniform.skyCubemapIndex)].SampleLevel(globalCubemaps_Sampler[NonUniformResourceIndex(globalUniform.skyCubemapIndex)], direction, lod).rgb;
    }

    return globalUniform.skyColorDefault.xyz;
}

float3 getSkyFilteredMultiplied(float3 direction, float lod)
{
    float3 col = getSkyFiltered(direction, lod);
#ifdef DESC_SET_RENDER_CUBEMAP
    if (globalUniform.skyType == SKY_TYPE_PROCEDURAL)
    {
        return col;
    }
#endif
    return col * globalUniform.skyColorMultiplier;
}

float3 getSkyAmbientMultiplied(float3 direction, float lod)
{
    return getSkyFilteredMultiplied(direction, min(lod, max(globalUniform.skyAmbientLod, 0.0)));
}

float evalSkyNeePdf(const float3 n, const float3 direction)
{
    return clamp(dot(n, direction), 0.0, 1.0) * (1.0 / M_PI);
}
#endif



#if LIGHT_SAMPLE_METHOD != LIGHT_SAMPLE_METHOD_NONE

#define SHADOW_RAY_EPS       0.01
#define RAY_ORIGIN_LEAK_BIAS 0.01

bool traceShadowRay(uint surfInstCustomIndex, float3 start, float3 end, bool ignoreFirstPersonViewer  )
{
    ShPayloadShadow g_payloadShadow;
    g_payloadShadow.isShadowed = 1;

    uint cullMask = getShadowCullMask(surfInstCustomIndex);

    if (ignoreFirstPersonViewer)
    {
        cullMask &= ~INSTANCE_MASK_FIRST_PERSON_VIEWER;
    }

    float3 l = end - start;
    float maxDistance = length(l);
    l /= maxDistance;

    RayDesc rayDesc;
    rayDesc.Origin = start;
    rayDesc.TMin = 0.001;
    rayDesc.Direction = l;
    rayDesc.TMax = maxDistance - SHADOW_RAY_EPS;

    TraceRay(
        topLevelAS,
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | getAdditionalRayFlags(),
        cullMask,
        0, 0,
        SBT_INDEX_MISS_SHADOW,
        rayDesc,
        g_payloadShadow);

    return g_payloadShadow.isShadowed == 1;
}

float3 traceSunWaterFactor(const Surface surf, const float3 lightPosition, const uint receiverMedia)
{
    if (receiverMedia != MEDIA_TYPE_WATER && receiverMedia != MEDIA_TYPE_ACID)
    {
        return (float3)1.0;
    }

    const float3 toLight = lightPosition - surf.position;
    const float distance = length(toLight);

    if (distance <= 0.0)
    {
        return (float3)1.0;
    }

    const float3 I = toLight / distance;

    // The closest water/acid interface between the receiver and the light. The query keeps the
    // closest-hit semantics: traversal order is not distance order, and the flat-interface
    // correction needs the nearest boundary, not an arbitrary committed one.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc rayDesc;
    rayDesc.Origin    = surf.position + surf.toViewerDir * RAY_ORIGIN_LEAK_BIAS;
    rayDesc.Direction = I;
    rayDesc.TMin      = SHADOW_RAY_EPS;
    rayDesc.TMax      = distance;

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, INSTANCE_MASK_REFRACT, rayDesc);

    while (query.Proceed())
    {
    }

    // A glass surface or no boundary at all leaves the analytic path unresolved; the conservative
    // fallback is full transmission rather than an invented absorption or Fresnel term.
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return (float3)1.0;
    }

    const ShTriangle hitTriangle = getTriangle((int)query.CommittedInstanceIndex(),
                                               (int)query.CommittedInstanceID(),
                                               (int)query.CommittedGeometryIndex(),
                                               (int)query.CommittedPrimitiveIndex());

    const uint hitMedia = getMediaTypeFromFlags(hitTriangle.geometryInstanceFlags);

    if (hitMedia != MEDIA_TYPE_WATER && hitMedia != MEDIA_TYPE_ACID)
    {
        return (float3)1.0;
    }

    const float2 bary = query.CommittedTriangleBarycentrics();
    const float3 baryWeights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    const float3 hitPosition = mul(hitTriangle.positions, baryWeights);

    float3 normal = normalize(mul(hitTriangle.normals, baryWeights));
    if (dot(normal, I) > 0.0)
    {
        normal = -normal;
    }

    // Air-to-water Snell refraction of the receiver-to-light direction. The refracted vector is
    // the reverse of the measured in-water travel direction: it points from the interface back
    // toward the receiver, so the receiver-to-interface segment runs along +Iw.
    const float hitIor = getIndexOfRefraction(hitMedia);
    const float3 Iw = refract(I, normal, 1.0 / hitIor);

    if (dot(Iw, Iw) <= 0.0)
    {
        return (float3)1.0;
    }

    const float denominator = dot(Iw, normal);
    if (denominator == 0.0)
    {
        return (float3)1.0;
    }

    const float dWater = dot(hitPosition - surf.position, normal) / denominator;

    if (!(dWater > 0.0) || isinf(dWater) || isnan(dWater))
    {
        return (float3)1.0;
    }

    // The air->water Fresnel pair plus the receiver medium's extinction over the in-water
    // segment. dot(Iw, Iw) > 0 already proved the refraction is not a total internal reflection,
    // so the transmitted fraction is a physical one.
    const float fresnel = getFresnelSchlick(1.0, hitIor, I, normal);
    const float3 transmittance = getMediaTransmittance(receiverMedia, dWater);

    return transmittance * (1.0 - fresnel);
}

float traceVisibility(const Surface surf, const float3 lightPosition, uint lightIndex)
{
    const float3 start = surf.position + surf.toViewerDir * RAY_ORIGIN_LEAK_BIAS;
    const float3 end = lightPosition;

    const bool ignoreFirstPersonViewer = (globalUniform.lightIndexIgnoreFPVShadows == lightIndex);

    const bool isShadowed = traceShadowRay(surf.instCustomIndex, start, end, ignoreFirstPersonViewer);
    return float(!isShadowed);
}

float traceLightVisibility(const Surface surf, const LightSample light, uint lightIndex, out bool traced)
{
    const float3 l = safeNormalize(light.position - surf.position);
    traced = dot(surf.normal, l) > 0.0 && dot(surf.normalGeom, l) > 0.0;

    if (!traced)
    {
        return 0.0;
    }

    return traceVisibility(surf, light.position, lightIndex);
}

float traceSunVisibility(const Surface surf, const LightSample sunLight, out bool traced)
{
    const float3 l = safeNormalize(sunLight.position - surf.position);
    traced = dot(surf.normal, l) > 0.0 && dot(surf.normalGeom, l) > 0.0;

    if (!traced)
    {
        return 0.0;
    }


    const uint sunCluster = surf.cluster;
    if (sunCluster < uint(Q2_MAX_CLUSTERS) &&
        (q2ClusterSkyVis[sunCluster >> 5] & (1u << (sunCluster & 31u))) == 0u)
    {
        traced = false;
        return 0.0;
    }

    return traceVisibility(surf, sunLight.position, LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET);
}

float traceSkyVisibility(const Surface surf, const float3 skyDirection)
{
    const float3 start = surf.position + surf.normalGeom * 0.01;
    const float3 end   = start + skyDirection * globalUniform.rayLength;

    const bool isShadowed = traceShadowRay(surf.instCustomIndex, start, end, false);
    return float(!isShadowed);
}
#endif



#define LIGHT_CHROMA_BOOST 1.3
float3 boostChroma(const float3 c)
{
    const float lum = getLuminance(c);
    return max(lerp((float3)lum, c, LIGHT_CHROMA_BOOST), (float3)0.0);
}

#define EMISSION_CHROMA_BOOST 8.0
float3 boostEmissionChroma(const float3 c)
{
    const float lum = getLuminance(c);
    return max(lerp((float3)lum, c, EMISSION_CHROMA_BOOST), (float3)0.0);
}

void shade(const Surface surf, const LightSample light, float oneOverPdf, out float3 diffuse, out float3 specular)
{
    float3 l = safeNormalize(light.position - surf.position);
    float nl = dot(surf.normal, l);
    float ngl = dot(surf.normalGeom, l);

    if (nl <= 0 || ngl <= 0)
    {
        diffuse = specular = (float3)0;
        return;
    }

    const float3 color = boostChroma(light.color);
    diffuse  = light.dw * nl * color * evalBRDFLambertian(1.0);
    specular = light.dw * nl * color * evalBRDFSmithGGX(surf.normal, surf.toViewerDir, l, surf.roughness, surf.specularColor);

    diffuse  *= oneOverPdf;
    specular *= oneOverPdf;
}

void shadeDiffuse(const Surface surf, const LightSample light, float oneOverPdf, out float3 diffuse)
{
    float3 l = safeNormalize(light.position - surf.position);
    float nl = dot(surf.normal, l);
    float ngl = dot(surf.normalGeom, l);

    if (nl <= 0 || ngl <= 0)
    {
        diffuse = (float3)0;
        return;
    }

    const float3 color = boostChroma(light.color);
    diffuse = light.dw * nl * color * evalBRDFLambertian(1.0);

    diffuse *= oneOverPdf;
}


#define RAY_STATS_CATEGORY_PRIMARY            0
#define RAY_STATS_CATEGORY_REFLECTION_REFRACTION 1
#define RAY_STATS_CATEGORY_INDIRECT           2
#define RAY_STATS_CATEGORY_SHADOW_DIRECT      3
#define RAY_STATS_CATEGORY_SHADOW_INDIRECT    4

struct RtRayStats
{
    uint counts[RAY_STATS_CATEGORY_COUNT];
};

[[vk::binding(0, DESC_SET_RAY_STATS)]] RWStructuredBuffer<RtRayStats> rtStats;

void rayStatsAdd(const uint category, const uint count)
{
    if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_RAY_STATS) != 0)
    {
        InterlockedAdd(rtStats[0].counts[category], count);
    }
}

#endif
