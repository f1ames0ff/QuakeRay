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
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
// HLSL counterpart of GlobalLightSampling.h, kept beside it as the legacy golden: the global-light
// RIS path the direct pass can select instead of the per-cluster light lists. For every NEE light
// sample it draws up to Q2_RESTIR_MAX_CANDIDATES uniform candidates from the whole light array,
// weights each by a target - the existing selection mass, floored by a solid-angle proxy so a
// light the mass cannot see is still samplable - and picks one by a reservoir draw, so the pdf of
// the pick is selectedWeight / (lightCount * mean target).
//
// Spellings that had to change:
//   * vec3 -> float3 and the C-style scalar casts become the HLSL truncating casts (uint(x) ->
//     (uint)x, int(x) -> (int)x, float(x) -> (float)x), as in every other twin.
//   * the out parameters take HLSL's out.
//
// What did not change: Q2_RNG_RESTIR_BASE and its salt arithmetic (two salts per candidate, 128
// per NEE sample, disjoint between the two NEE samples), Q2_RESTIR_MAX_CANDIDATES,
// kGlobalLightTargetFloor, the proxy per light kind (triangle and textured area get a sphere
// around their centres, everything else reads data_0), the target (directional lights excluded,
// the mass floored by proxy * floor, times luminance, times meanEmiss for a textured area), the
// reservoir loop and the pdf formula.

#ifndef GLOBAL_LIGHT_SAMPLING_HLSLI_
#define GLOBAL_LIGHT_SAMPLING_HLSLI_

// Two salts per candidate and 128 per NEE sample: the base plus sampleIndex * 128 stays
// disjoint between the samples for up to Q2_RESTIR_MAX_CANDIDATES candidates.
#define Q2_RNG_RESTIR_BASE 300
#define Q2_RESTIR_MAX_CANDIDATES 64u

static const float kGlobalLightTargetFloor = 0.02;

float globalLightSolidAngleProxy(const ShLightEncoded encoded, const float3 p)
{
    if (encoded.lightType == LIGHT_TYPE_TRIANGLE)
    {
        const TriangleLight l = decodeAsTriangleLight(encoded);
        const float3 center = (l.position[0] + l.position[1] + l.position[2]) / 3.0;
        const float radius = (length(l.position[0] - center) +
                              length(l.position[1] - center) +
                              length(l.position[2] - center)) / 3.0;
        const float r = max(radius, 1e-4);

        return calcSolidAngleForSphere(r, max(distance(center, p), r));
    }
    else if (encoded.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        const TexturedAreaLight l = decodeAsTexturedAreaLight(encoded);
        const float3 center = getTexturedAreaLightCenter(l);
        float radius = 0.0;

        for (int i = 0; i < l.numVerts; i++)
        {
            radius = max(radius, length(texturedAreaLightWorldPos(l, l.uvVerts[i]) - center));
        }

        const float r = max(radius, 1e-4);

        return calcSolidAngleForSphere(r, max(distance(center, p), r));
    }

    return calcSolidAngleForSphere(encoded.data_0.w, max(length(encoded.data_0.xyz - p), encoded.data_0.w));
}

float globalLightTarget(const ShLightEncoded encoded, const float3 p, const float3 n, const float3 V,
                        const float phongExp, const float phongScale, const float phongWeight)
{
    if (encoded.lightType == LIGHT_TYPE_DIRECTIONAL)
    {
        return 0.0;
    }

    const float mass  = q2LightSelectionMass(encoded, p, n, V, phongExp, phongScale, phongWeight);
    const float proxy = globalLightSolidAngleProxy(encoded, p);
    const float scale = (encoded.lightType == LIGHT_TYPE_TEXTURED_AREA)
        ? max(decodeAsTexturedAreaLight(encoded).meanEmiss, 0.0) : 1.0;

    return max(mass, proxy * kGlobalLightTargetFloor) * max(getLuminance(encoded.color), 0.0) * scale;
}

void q2SampleGlobalLightsRIS(const uint seed, const uint sampleIndex,
                             const float3 p, const float3 n, const float3 V,
                             const float phongExp, const float phongScale, const float phongWeight,
                             out uint lightIndex, out float lightPdf)
{
    lightIndex = LIGHT_INDEX_NONE;
    lightPdf = 0.0;

    const uint count = globalUniform.lightCount;

    if (count == 0u)
    {
        return;
    }

    const uint candidates = min(min(max(globalUniform.restirParams.y, 1u), count), Q2_RESTIR_MAX_CANDIDATES);
    const uint saltBase = (uint)Q2_RNG_RESTIR_BASE + sampleIndex * 128u;

    float weightSum = 0.0;
    float selectedWeight = 0.0;

    for (uint j = 0u; j < candidates; j++)
    {
        const uint li = (uint)LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET +
            min((uint)(rnd16(seed, saltBase + j * 2u) * float(count)), count - 1u);
        const float target = globalLightTarget(lightSources[li], p, n, V, phongExp, phongScale, phongWeight);

        weightSum += target;

        if (rnd16(seed, saltBase + j * 2u + 1u) * weightSum < target)
        {
            lightIndex = li;
            selectedWeight = target;
        }
    }

    if (lightIndex == LIGHT_INDEX_NONE || selectedWeight <= 0.0 || weightSum <= 0.0)
    {
        lightIndex = LIGHT_INDEX_NONE;
        lightPdf = 0.0;
        return;
    }

    lightPdf = selectedWeight / (float(count) * (weightSum / float(candidates)));
}

#endif
