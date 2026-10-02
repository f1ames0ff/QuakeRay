// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#define MATERIAL_MAX_ALBEDO_LAYERS 0

#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_RAY_STATS 11
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_DIRECT)
#include "RaygenCommon.hlsli"
#include "Q2Asvgf.hlsli"
#include "Q2LightLists.hlsli"
#include "GlobalLightSampling.hlsli"

#define Q2_RNG_CELL_SELECT 200
#define Q2_RNG_LIGHT_POINT 208
#define Q2_RNG_LIGHT_MEMBER 216
#define Q2_RNG_SUN_DISK 212

#define Q2_DIRECT_MAX_SPHERE_SOLID_ANGLE (2.0 * M_PI)

[shader("raygeneration")]
void main()
{
    const int2 pix = (int2)DispatchRaysIndex().xy;

    const uint seed = framebufQ2RngSeed_Sampled.Load(int3(pix, 0)).r;

    const Surface surf = fetchGbufferSurface(pix);

    if (surf.isSky)
    {
        framebufUnfilteredDirect[pix] = (uint4)0;
        framebufUnfilteredSpecular[pix] = (uint4)0;
        return;
    }

    const bool useGlobalRestir = globalUniform.restirParams.x != 0u;
    const bool isGradient = useGlobalRestir ? false : q2GetIsGradient(pix);
    const uint cluster = framebufQ2Cluster_Sampled.Load(int3(pix, 0)).r;

    const float alpha = square(surf.roughness);
    const float phongExp = q2RoughnessSquareToSpecPower(alpha);
    const float phongScale = min(100.0, 1.0 / (M_PI * max(alpha * alpha, 1e-4)));
    const float phongWeight = clamp(
        getLuminance(surf.specularColor) / (getLuminance(surf.specularColor) + getLuminance(surf.albedo)),
        0.0, 0.9);

    float3 directDiffuse = (float3)0.0;
    float3 directSpecular = (float3)0.0;

    const int numNeeSamples = clamp((int)globalUniform.neeLightSamples, 1, 2);

    for (int s = 0; s < numNeeSamples; s++)
    {
        uint lightIndex = LIGHT_INDEX_NONE;
        uint lightSlot = 0u;
        float lightPdf = 0.0;

        if (useGlobalRestir)
        {
            q2SampleGlobalLightsRIS(seed, (uint)s, surf.position, surf.normal, surf.toViewerDir,
                                    phongExp, phongScale, phongWeight, lightIndex, lightPdf);
        }
        else
        {
            const uint saltBase = (uint)Q2_RNG_CELL_SELECT + (uint)s * 4u;
            const float3 rng = float3(
                rnd16(seed, saltBase),
                rnd16(seed, saltBase + 1u),
                rnd16(seed, saltBase + 2u));

            q2SampleClusterLights(cluster, surf.position, surf.normal, surf.toViewerDir,
                                  phongExp, phongScale, phongWeight, isGradient, rng,
                                  lightIndex, lightSlot, lightPdf);
        }

        if (lightIndex != LIGHT_INDEX_NONE && lightPdf > 0.0)
        {
            const float2 pointRnd = rnd16_2(seed, (uint)Q2_RNG_LIGHT_POINT + (uint)s * 2u);
            const float2 memberRnd = rnd16_2(seed, (uint)Q2_RNG_LIGHT_MEMBER + (uint)s * 2u);
            float memberPdf = 1.0;
            LightSample light = sampleLightNee(lightSources[lightIndex], surf.position, pointRnd, memberRnd, memberPdf);

            if (lightSources[lightIndex].lightType == LIGHT_TYPE_SPHERE ||
                lightSources[lightIndex].lightType == LIGHT_TYPE_SPOT)
            {
                light.dw = min(light.dw, Q2_DIRECT_MAX_SPHERE_SOLID_ANGLE);
            }

            if (getLuminance(light.color) > 0.0)
            {
                bool lightTraced;
                const float vis = traceLightVisibility(surf, light, lightIndex, lightTraced);

                if (lightTraced)
                {
                    rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_DIRECT, 1);
                }

                if (!useGlobalRestir &&
                    (lightSources[lightIndex].lightType == LIGHT_TYPE_TRIANGLE ||
                     lightSources[lightIndex].lightType == LIGHT_TYPE_TEXTURED_AREA ||
                     lightSources[lightIndex].lightType == LIGHT_TYPE_DTAL_GROUP))
                {
                    q2AccumulateLightStats(cluster, lightSlot, surf.normal, vis, (uint)s);
                }

                float3 d, s;
                shade(surf, light, 1.0 / max(lightPdf * memberPdf, 1e-9), d, s);
                directDiffuse += d * vis;
                directSpecular += s * vis;
            }
        }
    }
    directDiffuse  *= (1.0 / (float)numNeeSamples);
    directSpecular *= (1.0 / (float)numNeeSamples);

    if (globalUniform.directionalLightExists != 0)
    {
        const DirectionalLight sun = decodeAsDirectionalLight(lightSources[LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET]);
        const float2 sunRnd = rnd16_2(seed, Q2_RNG_SUN_DISK) * 0.99;
        const LightSample sunLight = sampleDirectionalLight(sun, surf.position, sunRnd);

        bool sunTraced;
        const float sunVis = traceSunVisibility(surf, sunLight, sunTraced);

        if (sunTraced)
        {
            rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_DIRECT, 1);
        }

        float3 d, s;
        shade(surf, sunLight, 1.0, d, s);
        directDiffuse += d * sunVis;
        directSpecular += s * sunVis;
    }

    imageStoreUnfilteredDirect(pix, directDiffuse);
    imageStoreUnfilteredSpecular(pix, demodulateSpecular(directSpecular, surf.specularColor));
    framebufViewDirection[pix] = float4(-surf.toViewerDir, MAX_RAY_LENGTH);
}
