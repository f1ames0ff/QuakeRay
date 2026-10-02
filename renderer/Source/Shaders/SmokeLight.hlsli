// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

// HLSL counterpart of SmokeLight.h. Like the golden it includes nothing itself: the shader has to
// pull in ShaderCommonHLSLFunc.hlsli first (globalUniform, lightSources and the INSTANCE_MASK_*
// constants of the generated layer), then Smoke.hlsli (smokeHash and the SMOKE_* constants),
// Light.hlsli (ShLightEncoded, DirectionalLight, LightSample, sampleLight,
// sampleDirectionalLight) and Q2ClusterLights.hlsli (q2ClusterSeesSky), in the golden's order.
//
// Spellings that had to change:
//   * the golden's ray query becomes the HLSL RayQuery object. rayQueryInitializeEXT(query, tlas,
//     flags, mask, origin, tmin, dir, tmax) becomes the flags of the RayQuery template plus
//     query.TraceRayInline(tlas, RAY_FLAG_NONE, mask, rayDesc); the four ray fields keep their
//     golden values (SMOKE_SHADOW_RAY_EPS as TMin and max(0.0, maxDistance -
//     SMOKE_SHADOW_RAY_EPS) as TMax), rayQueryProceedEXT(query) becomes query.Proceed() in the
//     same empty loop, and the committed-intersection test
//     rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT
//     becomes query.CommittedStatus() == COMMITTED_NOTHING.
//
//     The three init flags map bit for bit: gl_RayFlagsOpaqueEXT (0x1) is RAY_FLAG_FORCE_OPAQUE,
//     gl_RayFlagsTerminateOnFirstHitEXT (0x4) is RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH and
//     gl_RayFlagsSkipClosestHitShaderEXT (0x8) is RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, so the flags
//     the query is initialized with stay 0xD. The DXR spec (RayQuery, "Ray flags") treats the
//     template parameter as the compile-time part of the flags and ORs it with the dynamic flags
//     of TraceRayInline; dxc lowers that OR into the RayFlags operand of
//     OpRayQueryInitializeKHR (measured: the template expression below with RAY_FLAG_NONE as the
//     dynamic argument compiles to %uint_13, the operand the glslc golden of RsSmoke.vert
//     carries). The skip-closest-hit flag is meaningless for a query that has no closest hit
//     shader, exactly as it is for the golden, and is kept so that the operand matches.
//   * accelerationStructureEXT at the golden's set and binding becomes
//     RaytracingAccelerationStructure; the descriptor is the same TLAS the GLSL half binds.
//   * vec3/vec2 -> float3/float2, and vec3(scalar) -> (float3)scalar at the two places that build
//     a vector from one scalar, the light accumulator and the min-light floor; int(e.data_2.w)
//     keeps its spelling -- HLSL truncates the same way.
//   * The uint(...) constructors and the uint comparisons of the golden keep their spelling (HLSL
//     accepts them for the scalar types, see Q2LightLists.hlsli): uint(LIGHT_INDEX_NONE),
//     uint(Q2_MAX_CLUSTERS), uint(SMOKE_CLUSTER_SCAN) and the two light-array offsets.
//   * the for loop over the cluster's candidates and the one over the textured-area uvs keep
//     their bounds, their order and their break/early-out conditions; vec2 uvs[8] becomes
//     float2 uvs[8] with the same eight element assignments in the same order.
//   * the #ifndef SMOKE_LIGHT_H_ guard -> #ifndef SMOKE_LIGHT_HLSLI_, as everywhere in this base.
//
// What did not change: the member set of ShLightEncoded that the header reads (lightType,
// data_0..data_3, data_1.w, color), every SMOKE_* constant and its value, the two visibility
// tests with their rnd samples, the weighting of smokeLightWeight (its center, radius and
// intensity rules, the textured-area data_1.w factor included), the luminance read and the
// best-weight selection over the cluster's first SMOKE_CLUSTER_SCAN entries, the regular-light
// range check, the second sampleLight visibility test, the phase functions of both contributions
// and the final max(light * SMOKE_LIGHT_STRENGTH, vec3(SMOKE_MIN_LIGHT)).

#ifndef SMOKE_LIGHT_HLSLI_
#define SMOKE_LIGHT_HLSLI_

#define SMOKE_TLAS_SET 5

[[vk::binding(0, SMOKE_TLAS_SET)]] RaytracingAccelerationStructure topLevelAS;

#define SMOKE_SHADOW_RAY_EPS 0.01

#define SMOKE_CLUSTER_SCAN 16

bool smokeVisible(const float3 start, const float3 dir, const float maxDistance)
{
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc rayDesc;
    rayDesc.Origin    = start;
    rayDesc.Direction = dir;
    rayDesc.TMin      = SMOKE_SHADOW_RAY_EPS;
    rayDesc.TMax      = max(0.0, maxDistance - SMOKE_SHADOW_RAY_EPS);

    query.TraceRayInline(topLevelAS,
                         RAY_FLAG_NONE,
                         globalUniform.rayCullMaskWorld | INSTANCE_MASK_REFRACT,
                         rayDesc);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

float smokeSafeSolidAngle(float a)
{
    return (a > 0.0 && !isnan(a) && !isinf(a)) ? clamp(a, 0.0, 4.0 * M_PI) : 0.0;
}

float3 smokeLightCenter(const ShLightEncoded e, out float radius)
{
    radius = 1.0;

    if (e.lightType == LIGHT_TYPE_SPHERE || e.lightType == LIGHT_TYPE_SPOT)
    {
        radius = max(e.data_0.w, 1.0);
        return e.data_0.xyz;
    }

    if (e.lightType == LIGHT_TYPE_TRIANGLE)
    {
        const float3 center = (e.data_0.xyz + e.data_1.xyz + e.data_2.xyz) / 3.0;
        radius = max(max(length(e.data_0.xyz - center), length(e.data_1.xyz - center)),
                     length(e.data_2.xyz - center));
        return center;
    }

    if (e.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        const int numVerts = clamp(int(e.data_2.w), 0, 8);

        float2 uvs[8];
        uvs[0] = e.data_3.xy;  uvs[1] = e.data_3.zw;
        uvs[2] = e.data_4.xy;  uvs[3] = e.data_4.zw;
        uvs[4] = e.data_5.xy;  uvs[5] = e.data_5.zw;
        uvs[6] = e.data_6.xy;  uvs[7] = e.data_6.zw;

        float2 uvCenter = (float2)0.0;
        for (int i = 0; i < numVerts; i++)
        {
            uvCenter += uvs[i];
        }
        uvCenter /= max(float(numVerts), 1.0);

        radius = max(1.0, 0.5 * (length(e.data_0.xyz) + length(e.data_1.xyz)));
        return e.data_2.xyz + e.data_0.xyz * uvCenter.x + e.data_1.xyz * uvCenter.y;
    }

    return e.data_0.xyz;
}

float smokeLightWeight(const ShLightEncoded e, const float3 p)
{
    if (e.lightType == LIGHT_TYPE_DIRECTIONAL || e.lightType == LIGHT_TYPE_NONE)
    {
        return 0.0;
    }

    float radius;
    const float3 center = smokeLightCenter(e, radius);

    const float dist2   = dot(center - p, center - p);
    const float radius2 = radius * radius;

    float intensity = getLuminance(e.color);
    if (e.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        intensity *= max(e.data_1.w, 0.0);
    }

    return intensity * radius2 / (dist2 + radius2 + 1.0);
}

float3 smokeLightAt(const float3 worldPos, const uint cluster, const float seed)
{
    const float3 toViewer = normalize(globalUniform.cameraPosition.xyz - worldPos);
    const float2 rnd      = float2(smokeHash(float3(seed, 1.7, 3.1)), smokeHash(float3(seed, 5.3, 7.9)));

    float3 light = (float3)0.0;

    if (globalUniform.directionalLightExists != 0 && q2ClusterSeesSky(cluster))
    {
        const DirectionalLight sun = decodeAsDirectionalLight(lightSources[LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET]);
        const LightSample sunSample = sampleDirectionalLight(sun, worldPos, rnd);

        const float3 toSun    = normalize(sunSample.position - worldPos);
        const float  distance = length(sunSample.position - worldPos);

        if (smokeVisible(worldPos, toSun, distance))
        {
            light += sunSample.color * sunSample.dw * smokePhase(toSun, toViewer);
        }
    }

    uint     bestLight  = uint(LIGHT_INDEX_NONE);
    float    bestWeight = 0.0;

    if (cluster < uint(Q2_MAX_CLUSTERS))
    {
        const uint count = min(q2GetClusterLightCount(cluster), uint(SMOKE_CLUSTER_SCAN));

        for (uint i = 0u; i < count; i++)
        {
            const uint li = q2GetClusterLight(cluster, i);

            if (li < uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) ||
                li >= uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) + globalUniform.lightCount)
            {
                continue;
            }

            const float w = smokeLightWeight(lightSources[li], worldPos);

            if (w > bestWeight)
            {
                bestWeight = w;
                bestLight  = li;
            }
        }
    }

    if (bestLight != uint(LIGHT_INDEX_NONE))
    {
        const LightSample lightSample = sampleLight(lightSources[bestLight], worldPos, rnd);

        if (lightSample.dw > 0.0)
        {
            const float3 toLight  = normalize(lightSample.position - worldPos);
            const float  distance = length(lightSample.position - worldPos);

            if (smokeVisible(worldPos, toLight, distance))
            {
                light += lightSample.color * lightSample.dw * smokePhase(toLight, toViewer);
            }
        }
    }

    return max(light * SMOKE_LIGHT_STRENGTH, (float3)SMOKE_MIN_LIGHT);
}

#endif // SMOKE_LIGHT_HLSLI_
