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

#define MATERIAL_MAX_ALBEDO_LAYERS 0

#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_RAY_STATS 11
#define DESC_SET_CLOUD_SHADOW 12
#define DESC_SET_CAUSTICS 13
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_DIRECT)
#include "RaygenCommon.hlsli"
#include "Caustics.hlsli"
#include "Q2Asvgf.hlsli"
#include "Q2LightLists.hlsli"
#include "GlobalLightSampling.hlsli"

#define Q2_RNG_CELL_SELECT 200
#define Q2_RNG_LIGHT_POINT 208
#define Q2_RNG_SUN_DISK 212

[[vk::binding(0, DESC_SET_CAUSTICS)]] StructuredBuffer<CausticsParams_BT> causticsParams;
[[vk::binding(1, DESC_SET_CAUSTICS)]] StructuredBuffer<float4> causticsCells;

// The receiver medium of the packed pixel, decoded from the normalized byte the primary G-buffer
// stores in MetallicRoughness.z (media / 3.0, contract of the RGBA8 metadata route).
uint loadReceiverMedia(const int2 pix)
{
    return (uint)round(framebufMetallicRoughness_Sampled.Load(int3(pix, 0)).z * 3.0);
}

// Bilinear reconstruction of the photon cell buffer at the receiver's world XY. The cell-centered
// sample coordinate is `gridCoords - 0.5`; taps outside the loaded domain are dropped and their
// weights redistributed over the remaining taps. Returns false when no tap is inside, which is the
// documented analytic fallback. The result is the receiver irradiance: no extra NdotL, albedo or
// camera throughput, because those belong to the composition chain.
bool gatherCausticsIrradiance(const Surface surf, const CausticsParams_BT params, out float3 irradiance,
                              out float coverage)
{
    irradiance = (float3)0.0;
    coverage = 0.0;

    const uint resolution = params.gridSize.x;
    if (resolution == 0u || !(params.gridMinAndTexel.z > 0.0))
    {
        return false;
    }

    const float2 gridCoords =
        (surf.position.xy - params.gridMinAndTexel.xy) / params.gridMinAndTexel.z - 0.5;

    const float2 base = floor(gridCoords);
    const float2 cellFrac = gridCoords - base;
    const int2 baseCell = (int2)base;

    const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
    const float tapWeights[4] =
    {
        (1.0 - cellFrac.x) * (1.0 - cellFrac.y),
        cellFrac.x * (1.0 - cellFrac.y),
        (1.0 - cellFrac.x) * cellFrac.y,
        cellFrac.x * cellFrac.y
    };

    float3 sum = (float3)0.0;
    float countSum = 0.0;
    float weightSum = 0.0;

    for (int i = 0; i < 4; i++)
    {
        const int2 cell = baseCell + offsets[i];
        if (cell.x < 0 || cell.y < 0 || cell.x >= (int)resolution || cell.y >= (int)resolution)
        {
            continue;
        }

        const float4 tap = causticsCells[(uint)cell.y * resolution + (uint)cell.x];
        sum += tap.xyz * tapWeights[i];
        countSum += tap.w * tapWeights[i];
        weightSum += tapWeights[i];
    }

    if (weightSum <= 0.0)
    {
        return false;
    }

    irradiance = sum / weightSum;
    coverage = saturate(countSum / weightSum);
    return true;
}

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
            const float2 pointRnd = rnd16_2(seed, (uint)Q2_RNG_LIGHT_POINT + (uint)s * 2u) * 0.99;
            LightSample light = sampleLight(lightSources[lightIndex], surf.position, pointRnd);

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
                     lightSources[lightIndex].lightType == LIGHT_TYPE_TEXTURED_AREA))
                {
                    q2AccumulateLightStats(cluster, lightSlot, surf.normal, vis, (uint)s);
                }

                float3 d, s;
                shade(surf, light, 1.0 / lightPdf, d, s);
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

        const uint receiverMedia =
            globalUniform.waterLightPath != 0u ? loadReceiverMedia(pix) : (uint)MEDIA_TYPE_VACUUM;
        const bool underwaterReceiver =
            receiverMedia == MEDIA_TYPE_WATER || receiverMedia == MEDIA_TYPE_ACID;

        if (underwaterReceiver)
        {
            // The corrected analytic fallback describes the transmitted sun for the specular term
            // and for receivers outside the photon domain. It is computed once and reused.
            const float3 waterFactor = traceSunWaterFactor(surf, sunLight.position, receiverMedia);
            directSpecular += s * sunVis * waterFactor;

            const CausticsParams_BT caustics = causticsParams[0];
            const bool traceValid = (caustics.gridSize.z & CAUSTICS_FLAG_TRACE_VALID) != 0u;

            float3 causticIrradiance = (float3)0.0;
            float causticCoverage = 0.0;
            if (traceValid && gatherCausticsIrradiance(surf, caustics, causticIrradiance, causticCoverage))
            {
                // The photon estimate blends with the analytic transmitted sun by the accumulated
                // photon coverage: a cell that received photons keeps the network, a cell that
                // received none falls back to the analytic term instead of turning black. The
                // analytic term carries the straight sun visibility, so the player's shadow stays
                // pixel-precise where the photon field is empty. The intensity scales the photon
                // side exactly once.
                const float3 analyticDiffuse = d * sunVis * waterFactor;
                const float3 photonDiffuse =
                    boostChroma(causticIrradiance) * (1.0 / M_PI) * caustics.sunDirection.w;
                directDiffuse += lerp(analyticDiffuse, photonDiffuse, causticCoverage);
            }
            else
            {
                directDiffuse += d * sunVis * waterFactor;
            }
        }
        else
        {
            directDiffuse += d * sunVis;
            directSpecular += s * sunVis;
        }
    }

    imageStoreUnfilteredDirect(pix, directDiffuse);
    imageStoreUnfilteredSpecular(pix, demodulateSpecular(directSpecular, surf.specularColor));
    framebufViewDirection[pix] = float4(-surf.toViewerDir, MAX_RAY_LENGTH);
}
