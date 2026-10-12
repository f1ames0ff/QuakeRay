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
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_DIRECT)
#include "RaygenCommon.hlsli"
#include "Q2Asvgf.hlsli"
#include "Q2LightLists.hlsli"
#include "GlobalLightSampling.hlsli"
#include "Q2DirectReuse.hlsli"

#define Q2_RNG_CELL_SELECT 2000
#define Q2_RNG_LIGHT_POINT 2100
#define Q2_RNG_LIGHT_MEMBER 2200
#define Q2_RNG_SUN_DISK 212

#define Q2_NEE_MAX_SAMPLES 8

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

    const int numNeeSamples = clamp((int)globalUniform.neeLightSamples, 1, Q2_NEE_MAX_SAMPLES);

    const bool reuseEnabled = (globalUniform.restirParams[3] & 1u) != 0u && !useGlobalRestir && numNeeSamples == 1;

    if (reuseEnabled)
    {
        const int  width = (int)globalUniform.renderWidth;
        const int  height = (int)globalUniform.renderHeight;
        const uint seedBase = isGradient ? wellonsLowBias32(seed ^ 0x9E3779B9u) : seed;
        const float3 selRng = float3(
            rnd24(seedBase, Q2_REUSE_RNG_SELECT),
            rnd24(seedBase, Q2_REUSE_RNG_SELECT + 1u),
            rnd24(seedBase, Q2_REUSE_RNG_SELECT + 2u));

        uint  lightIndex = LIGHT_INDEX_NONE;
        uint  lightSlot = 0u;
        float lightPdf = 0.0;

        q2SampleClusterLights(cluster, surf.position, surf.normal, surf.toViewerDir,
                              phongExp, phongScale, phongWeight, isGradient, selRng,
                              lightIndex, lightSlot, lightPdf);

        float  wSum = 0.0;
        uint   mSum = 0u;
        uint   selLight = LIGHT_INDEX_NONE;
        uint   selFlags = 0u;
        uint   selCluster = cluster;
        uint   selSlot = 0xFFu;
        float2 selPointRnd = (float2)0.0;
        float  selMemberRnd = 0.0;
        float  selTarget = Q2_REUSE_TARGET_EPS;

        if (lightIndex != LIGHT_INDEX_NONE && lightPdf > 0.0)
        {
            const float2 pointRnd = rnd16_2(seedBase, Q2_REUSE_RNG_POINT);
            const float2 memberRnd = rnd16_2(seedBase, Q2_REUSE_RNG_MEMBER);
            float memberPdf = 1.0;
            float2 storedPointRnd = pointRnd;
            LightSample candLight;

            if (lightSources[lightIndex].lightType == LIGHT_TYPE_SPHERE)
            {
                const SphereLight sl = decodeAsSphereLight(lightSources[lightIndex]);
                const DirectionAndLength toLightCenter = calcDirectionAndLength(surf.position, sl.center);
                float oneOverPdf;
                const float3 lightNormal = sampleOrientedHemisphere(-toLightCenter.dir, pointRnd.x, pointRnd.y, oneOverPdf);

                candLight.position = sl.center + lightNormal * sl.radius;
                candLight.color = sl.color;
                candLight.dw = calcSolidAngleForSphere(sl.radius, toLightCenter.len);

                const uint2 stored = q2ReuseEncodeDir(lightNormal);
                storedPointRnd = float2(q2ReuseUnpack16(stored.x), q2ReuseUnpack16(stored.y));
            }
            else
            {
                candLight = sampleLightNee(lightSources[lightIndex], surf.position, pointRnd, memberRnd, memberPdf);
            }

            float3 dCand, sCand;
            shade(surf, candLight, 1.0, dCand, sCand);

            const float target = max(getLuminance(dCand + sCand), Q2_REUSE_TARGET_EPS);

            wSum = target / max(lightPdf * memberPdf, 1e-9);
            mSum = 1u;
            selLight = lightIndex;
            selPointRnd = storedPointRnd;
            selMemberRnd = memberRnd.x;
            selTarget = target;
            selCluster = cluster;
            selSlot = (lightSlot == (uint)Q2_LIGHT_LIST_INVALID_SLOT) ? 0xFFu : lightSlot;
        }

        const uint reuseCfg = globalUniform.restirParams[3];
        const float neighborRadius = (float)((reuseCfg >> 8u) & 0xFFu);
        uint mCapCfg = (reuseCfg >> 16u) & 0xFFu;
        if (mCapCfg == 0u)
        {
            mCapCfg = Q2_REUSE_M_CAP;
        }

        bool histValid = false;
        uint histHash = 0u;

        if (globalUniform.restirParams.z == 0u)
        {
            const float4 motion = framebufMotion_Sampled.Load(int3(pix, 0));
            const float2 posPrev = ((float2(q2CheckerToFlat(pix, width)) + (float2)0.5) *
                                    float2(1.0 / globalUniform.renderWidth, 1.0 / globalUniform.renderHeight) + motion.xy) *
                                   float2(width, height);
            const float2 nbrRnd = rnd16_2(seedBase, Q2_REUSE_RNG_NEIGHBOR);
            const float2 nbrOff = (nbrRnd * 2.0 - 1.0) * neighborRadius;

            const float  depthCurr = framebufQ2ViewDepth_Sampled.Load(int3(pix, 0)).r;
            const float3 geoNormalCurr = texelFetchNormalGeometry(pix);

            for (int attempt = 0; attempt < 2; attempt++)
            {
                const float2 off = (attempt == 0) ? (float2)0.0 : nbrOff;
                const int2 prevPix = q2FlatToChecker((int2)floor(posPrev - (float2)0.5 + off), width);

                if (prevPix.x < 0 || prevPix.y < 0 || prevPix.x >= width || prevPix.y >= height)
                {
                    continue;
                }

                const float  depthPrev = framebufQ2ViewDepth_Prev_Sampled.Load(int3(prevPix, 0)).r;
                const float3 geoNormalPrev = texelFetchNormalGeometry_Prev(prevPix);
                const float  motionZ = (depthCurr < 0.0) ? -motion.z : motion.z;
                float        distDepth = abs(depthCurr - depthPrev + motionZ) / max(abs(depthCurr), 1e-6);

                if (depthCurr < 0.0)
                {
                    distDepth *= 0.25;
                }

                Q2ReuseReservoir hist;
                if (!(distDepth < 0.1 && dot(geoNormalCurr, geoNormalPrev) > 0.9) ||
                    !q2ReuseLoadPrev(prevPix, hist) ||
                    hist.lightIndex < (uint)LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET ||
                    hist.lightIndex >= (uint)LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET + globalUniform.lightCount ||
                    hist.fingerprint != q2ReuseFingerprint(lightSources[hist.lightIndex]) ||
                    (attempt == 1 && q2ReuseIsReceiverDependent(lightSources[hist.lightIndex])))
                {
                    continue;
                }

                histValid = true;
                histHash = hist.candidateHash;

                float memberPdfHist = 1.0;
                const LightSample histLight = q2ReuseSampleStored(hist.lightIndex, surf.position, hist.pointRnd, hist.memberRnd, memberPdfHist);

                float3 dHist, sHist;
                shade(surf, histLight, 1.0, dHist, sHist);

                const float targetHist = max(getLuminance(dHist + sHist), Q2_REUSE_TARGET_EPS);
                const float wHist = targetHist * hist.ucw * (float)hist.m;
                const float mergeRnd = rnd16(seedBase, Q2_REUSE_RNG_MERGE + (uint)attempt);

                wSum += wHist;
                mSum += hist.m;

                if (mergeRnd * wSum < wHist)
                {
                    selLight = hist.lightIndex;
                    selPointRnd = hist.pointRnd;
                    selMemberRnd = hist.memberRnd;
                    selTarget = targetHist;
                    selCluster = hist.cluster;
                    selSlot = hist.slot;
                    selFlags = hist.flags & 0x3u;
                }
            }

            uint mCap = mCapCfg;
            if (histValid)
            {
                const int2 dupOffFlat[8] = { int2(7, 0), int2(-7, 0), int2(0, 7), int2(0, -7), int2(7, 7), int2(-7, 7), int2(7, -7), int2(-7, -7) };
                uint dupCount = 0u;

                for (int i = 0; i < 8; i++)
                {
                    const int2 p = q2FlatToChecker((int2)floor(posPrev - (float2)0.5 + (float2)dupOffFlat[i]), width);
                    if (p.x < 0 || p.y < 0 || p.x >= width || p.y >= height)
                    {
                        continue;
                    }

                    const uint4 metaNbr = framebufQ2ReuseReservoirMeta_Prev_Sampled.Load(int3(p, 0));
                    if (((metaNbr.x >> 16u) & 0xFFFFu) == histHash)
                    {
                        dupCount += 1u;
                    }
                }

                const float dupScore = (float)dupCount / 8.0;
                mCap = max(Q2_REUSE_M_CAP_MIN, (uint)round(lerp((float)mCapCfg, (float)Q2_REUSE_M_CAP_MIN, pow(dupScore, 0.1))));
            }

            if (mSum > mCap)
            {
                const float meanW = wSum / (float)mSum;
                mSum = mCap;
                wSum = meanW * (float)mSum;
            }
        }

        Q2ReuseReservoir outReservoir;
        outReservoir.lightIndex = selLight;
        outReservoir.m = mSum;
        outReservoir.pointRnd = selPointRnd;
        outReservoir.memberRnd = selMemberRnd;
        outReservoir.targetOwn = min(selTarget, Q2_REUSE_TARGET_MAX);
        outReservoir.ucw = (selLight != LIGHT_INDEX_NONE) ? q2ReuseCapUcw(wSum / ((float)mSum * selTarget), reuseCfg) : 0.0;
        outReservoir.fingerprint = (selLight != LIGHT_INDEX_NONE) ? q2ReuseFingerprint(lightSources[selLight]) : 0u;
        outReservoir.candidateHash = q2ReuseSampleHash(selLight, selPointRnd, selMemberRnd);
        outReservoir.cluster = selCluster;
        outReservoir.slot = selSlot;
        outReservoir.flags = (selLight != LIGHT_INDEX_NONE)
                                 ? (Q2_REUSE_FLAG_VALID | selFlags | ((globalUniform.frameId & 0x3Fu) << 2u))
                                 : 0u;
        q2ReuseStore(pix, outReservoir);

        if (selLight != LIGHT_INDEX_NONE)
        {
            float memberPdfSel = 1.0;
            const LightSample selLightSample = q2ReuseSampleStored(selLight, surf.position, selPointRnd, selMemberRnd, memberPdfSel);

            bool lightTraced;
            const float3 vis = traceLightVisibility(surf, selLightSample, selLight, lightTraced);

            if (lightTraced)
            {
                rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_DIRECT, 1);
            }

            const uint selType = lightSources[selLight].lightType;

            if ((selType == LIGHT_TYPE_TRIANGLE ||
                 selType == LIGHT_TYPE_TEXTURED_AREA ||
                 selType == LIGHT_TYPE_DTAL_GROUP) &&
                selSlot != 0xFFu && selCluster < (uint)Q2_MAX_CLUSTERS)
            {
                q2AccumulateLightStats(selCluster, selSlot, surf.normal, getLuminance(vis), 0u);
            }

            float3 d, s;
            shade(surf, selLightSample, 1.0, d, s);

            const float ucw = q2ReuseCapUcw(wSum / ((float)mSum * selTarget), reuseCfg);
            directDiffuse += d * vis * ucw;
            directSpecular += s * vis * ucw;
        }
    }
    else
    {
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
                rnd24(seed, saltBase),
                rnd24(seed, saltBase + 1u),
                rnd24(seed, saltBase + 2u));

            q2SampleClusterLights(cluster, surf.position, surf.normal, surf.toViewerDir,
                                  phongExp, phongScale, phongWeight, isGradient, rng,
                                  lightIndex, lightSlot, lightPdf);
        }

        if (lightIndex != LIGHT_INDEX_NONE && lightPdf > 0.0)
        {
            const float2 pointRnd = rnd16_2(seed, (uint)Q2_RNG_LIGHT_POINT + (uint)s * 2u);
            const float2 memberRnd = float2(
                rnd24(seed, (uint)Q2_RNG_LIGHT_MEMBER + (uint)s * 2u),
                rnd24(seed, (uint)Q2_RNG_LIGHT_MEMBER + (uint)s * 2u + 1u));
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
                const float3 vis = traceLightVisibility(surf, light, lightIndex, lightTraced);

                if (lightTraced)
                {
                    rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_DIRECT, 1);
                }

                if (!useGlobalRestir &&
                    (lightSources[lightIndex].lightType == LIGHT_TYPE_TRIANGLE ||
                     lightSources[lightIndex].lightType == LIGHT_TYPE_TEXTURED_AREA ||
                     lightSources[lightIndex].lightType == LIGHT_TYPE_DTAL_GROUP))
                {
                    q2AccumulateLightStats(cluster, lightSlot, surf.normal, getLuminance(vis), (uint)s);
                }

                float3 d, s;
                shade(surf, light, 1.0 / max(lightPdf * memberPdf, 1e-9), d, s);
                directDiffuse += d * vis;
                directSpecular += s * vis;
            }
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
        const float3 sunVis = traceSunVisibility(surf, sunLight, sunTraced);

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
