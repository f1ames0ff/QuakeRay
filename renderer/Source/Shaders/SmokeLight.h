// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#ifndef SMOKE_LIGHT_H_
#define SMOKE_LIGHT_H_

#define SMOKE_TLAS_SET 5

layout(set = SMOKE_TLAS_SET, binding = 0) uniform accelerationStructureEXT topLevelAS;

#define SMOKE_SHADOW_RAY_EPS 0.01

#define SMOKE_CLUSTER_SCAN 16

bool smokeVisible(const vec3 start, const vec3 dir, const float maxDistance)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query,
                          topLevelAS,
                          gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
                          globalUniform.rayCullMaskWorld | INSTANCE_MASK_REFRACT,
                          start,
                          SMOKE_SHADOW_RAY_EPS,
                          dir,
                          max(0.0, maxDistance - SMOKE_SHADOW_RAY_EPS));

    while (rayQueryProceedEXT(query))
    {
    }

    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

float smokeSafeSolidAngle(float a)
{
    return (a > 0.0 && !isnan(a) && !isinf(a)) ? clamp(a, 0.0, 4.0 * M_PI) : 0.0;
}

vec3 smokeLightCenter(const ShLightEncoded e, out float radius)
{
    radius = 1.0;

    if (e.lightType == LIGHT_TYPE_SPHERE || e.lightType == LIGHT_TYPE_SPOT)
    {
        radius = max(e.data_0.w, 1.0);
        return e.data_0.xyz;
    }

    if (e.lightType == LIGHT_TYPE_TRIANGLE)
    {
        const vec3 center = (e.data_0.xyz + e.data_1.xyz + e.data_2.xyz) / 3.0;
        radius = max(max(length(e.data_0.xyz - center), length(e.data_1.xyz - center)),
                     length(e.data_2.xyz - center));
        return center;
    }

    if (e.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        const int numVerts = clamp(int(e.data_2.w), 0, 8);

        vec2 uvs[8];
        uvs[0] = e.data_3.xy;  uvs[1] = e.data_3.zw;
        uvs[2] = e.data_4.xy;  uvs[3] = e.data_4.zw;
        uvs[4] = e.data_5.xy;  uvs[5] = e.data_5.zw;
        uvs[6] = e.data_6.xy;  uvs[7] = e.data_6.zw;

        vec2 uvCenter = vec2(0.0);
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

float smokeLightWeight(const ShLightEncoded e, const vec3 p)
{
    if (e.lightType == LIGHT_TYPE_DIRECTIONAL || e.lightType == LIGHT_TYPE_NONE)
    {
        return 0.0;
    }

    float radius;
    const vec3 center = smokeLightCenter(e, radius);

    const float dist2   = dot(center - p, center - p);
    const float radius2 = radius * radius;

    float intensity = getLuminance(e.color);
    if (e.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        intensity *= max(e.data_1.w, 0.0);
    }

    return intensity * radius2 / (dist2 + radius2 + 1.0);
}

vec3 smokeLightAt(const vec3 worldPos, const uint cluster, const float seed)
{
    const vec3 toViewer = normalize(globalUniform.cameraPosition.xyz - worldPos);
    const vec2 rnd      = vec2(smokeHash(vec3(seed, 1.7, 3.1)), smokeHash(vec3(seed, 5.3, 7.9)));

    vec3 light = vec3(0.0);

    if (globalUniform.directionalLightExists != 0 && q2ClusterSeesSky(cluster))
    {
        const DirectionalLight sun = decodeAsDirectionalLight(lightSources[LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET]);
        const LightSample sunSample = sampleDirectionalLight(sun, worldPos, rnd);

        const vec3  toSun    = normalize(sunSample.position - worldPos);
        const float distance = length(sunSample.position - worldPos);

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
            const vec3  toLight  = normalize(lightSample.position - worldPos);
            const float distance = length(lightSample.position - worldPos);

            if (smokeVisible(worldPos, toLight, distance))
            {
                light += lightSample.color * lightSample.dw * smokePhase(toLight, toViewer);
            }
        }
    }

    return max(light * SMOKE_LIGHT_STRENGTH, vec3(SMOKE_MIN_LIGHT));
}

#endif // SMOKE_LIGHT_H_
