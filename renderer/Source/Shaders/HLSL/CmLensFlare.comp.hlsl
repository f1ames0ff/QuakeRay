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

#define DESC_SET_TONEMAPPING 2
#include "PostEffects.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> flareSource;
[[vk::binding(1, 0)]] SamplerState flareSource_Sampler;
[[vk::binding(0, 1)]] Texture2D<float4> flareDepth;
[[vk::binding(1, 1)]] Texture2D<float4> flareHighlights;
[[vk::binding(0, 3), vk::image_format("rgba16f")]] RWTexture2D<float4> flareDest;

struct LensFlarePush_BT
{
    uint passMode;
    float threshold;
    float knee;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<LensFlarePush_BT> push;

static const uint FLARE_PASS_BRIGHT = 0;
static const uint FLARE_PASS_COMPOSITE = 4;
static const uint FLARE_PASS_APERTURE_SMALL = 5;
static const uint FLARE_PASS_APERTURE_LARGE = 6;

static const float FLARE_GAIN = 50.1;
static const float FLARE_LIMIT = 256.0;
static const float FLARE_EDGE_TEXELS = 2.0;

static const uint FLARE_APERTURE_TAPS = 96;
static const float FLARE_APERTURE_GOLDEN_ANGLE = 2.39996323;
static const float FLARE_APERTURE_SMALL_SCALE = 0.0095;
static const float FLARE_APERTURE_LARGE_SCALE = 0.130;
static const float FLARE_APERTURE_SOFTNESS = 0.1;
static const float3 FLARE_APERTURE_RADII = float3(0.955, 1.0, 1.045);

static const uint FLARE_GHOST_COUNT = 5;
static const float FLARE_GHOST_SCALES[5] = { -0.40, 0.62, -0.95, 1.45, -2.10 };
static const float FLARE_GHOST_GAINS[5] = { 0.030, 0.0225, 0.015, 0.0105, 0.006 };

static const float FLARE_CLAMP_MAX = 300.0;
static const float FLARE_DIST_REF = 256.0;
static const float FLARE_SKY_DEPTH = MAX_RAY_LENGTH;
static const float FLARE_RESPONSE_HALF = 32.0;
static const float FLARE_VIEW_FLOOR = 0.25;
static const float FLARE_VIEW_START = 0.55;

float3 flareFetchClamped(float2 uv, float exposure)
{
    float3 color = postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, uv, 0).rgb) * exposure;

    const float maximum = max(max(color.r, color.g), color.b);
    return color * min(1.0, FLARE_CLAMP_MAX / max(maximum, 1e-6));
}

float3 flareDownsampleBright(float2 uv, float2 sourceTexelSize, float exposure)
{
    const float3 tl = flareFetchClamped(uv + sourceTexelSize * float2(-2.0, -2.0), exposure);
    const float3 tc = flareFetchClamped(uv + sourceTexelSize * float2(0.0, -2.0), exposure);
    const float3 tr = flareFetchClamped(uv + sourceTexelSize * float2(2.0, -2.0), exposure);
    const float3 ml = flareFetchClamped(uv + sourceTexelSize * float2(-2.0, 0.0), exposure);
    const float3 mc = flareFetchClamped(uv, exposure);
    const float3 mr = flareFetchClamped(uv + sourceTexelSize * float2(2.0, 0.0), exposure);
    const float3 bl = flareFetchClamped(uv + sourceTexelSize * float2(-2.0, 2.0), exposure);
    const float3 bc = flareFetchClamped(uv + sourceTexelSize * float2(0.0, 2.0), exposure);
    const float3 br = flareFetchClamped(uv + sourceTexelSize * float2(2.0, 2.0), exposure);
    const float3 q0 = flareFetchClamped(uv + sourceTexelSize * float2(-1.0, -1.0), exposure);
    const float3 q1 = flareFetchClamped(uv + sourceTexelSize * float2(1.0, -1.0), exposure);
    const float3 q2 = flareFetchClamped(uv + sourceTexelSize * float2(-1.0, 1.0), exposure);
    const float3 q3 = flareFetchClamped(uv + sourceTexelSize * float2(1.0, 1.0), exposure);

    const float3 g0 = (tl + tc + ml + mc) * 0.25;
    const float3 g1 = (tc + tr + mc + mr) * 0.25;
    const float3 g2 = (ml + mc + bl + bc) * 0.25;
    const float3 g3 = (mc + mr + bc + br) * 0.25;
    const float3 g4 = (q0 + q1 + q2 + q3) * 0.25;

    const float w0 = 0.125 / (1.0 + getLuminance(g0));
    const float w1 = 0.125 / (1.0 + getLuminance(g1));
    const float w2 = 0.125 / (1.0 + getLuminance(g2));
    const float w3 = 0.125 / (1.0 + getLuminance(g3));
    const float w4 = 0.5 / (1.0 + getLuminance(g4));

    float3 result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);

    const float luminance = getLuminance(result);
    const float above = max(luminance - push.threshold, 0.0);
    const float ratio = above / max(push.threshold, POST_EFFECTS_EPSILON);

    result *= above / max(luminance, POST_EFFECTS_EPSILON);
    result *= ratio / (1.0 + ratio / FLARE_RESPONSE_HALF);

    const float2 centered = abs(uv - 0.5) * 2.0;
    const float edge = max(centered.x, centered.y);
    result *= lerp(1.0, FLARE_VIEW_FLOOR, smoothstep(FLARE_VIEW_START, 1.0, edge));

    uint depthWidth;
    uint depthHeight;
    flareDepth.GetDimensions(depthWidth, depthHeight);

    const int2 regularPix = clamp(int2(uv * float2(depthWidth, depthHeight)),
                                  int2(0, 0), int2(depthWidth - 1, depthHeight - 1));
    const int separator = int(depthWidth) / 2;
    const int odd = (regularPix.x + regularPix.y % 2) % 2;
    const int2 checkerboardPix = int2(odd * separator + regularPix.x / 2, regularPix.y);

    const float depth = flareDepth.Load(int3(checkerboardPix, 0)).r;
    if (depth <= FLARE_SKY_DEPTH)
    {
        const float square = FLARE_DIST_REF * FLARE_DIST_REF;
        result *= square / (depth * depth + square);
    }

    return postEffectsSanitize(result / exposure);
}

float3 flareSample(float2 uv)
{
    return postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, clamp(uv, (float2)0.0, (float2)1.0), 0).rgb);
}

float3 flareSampleBordered(float2 uv, float2 texelSize, float edgeTexels)
{
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return (float3)0.0;
    }

    const float border = min(uv.x, 1.0 - uv.x) / texelSize.x;
    const float fade = smoothstep(0.0, max(edgeTexels, 1e-4), border);
    return flareSample(uv) * fade;
}

float3 flareHighlightBordered(float2 uv, float2 texelSize, float edgeTexels)
{
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return (float3)0.0;
    }

    const float border = min(uv.x, 1.0 - uv.x) / texelSize.x;
    const float fade = smoothstep(0.0, max(edgeTexels, 1e-4), border);
    return postEffectsSanitize(flareHighlights.SampleLevel(flareSource_Sampler, clamp(uv, (float2)0.0, (float2)1.0), 0).rgb) * fade;
}

float3 flareAperture(float2 uv, float2 texelSize, float apothem)
{
    const float2 normal0 = float2(1.0, 0.0);
    const float2 normal1 = float2(0.5, 0.86602540378);
    const float2 normal2 = float2(0.5, -0.86602540378);
    const float circumradius = apothem / 0.86602540378;
    const float edgeTexels = FLARE_EDGE_TEXELS * apothem;

    float3 sum = (float3)0.0;
    float3 weightSum = (float3)0.0;

    [unroll]
    for (uint tap = 0; tap < FLARE_APERTURE_TAPS; tap++)
    {
        const float radius = sqrt(((float)tap + 0.5) / (float)FLARE_APERTURE_TAPS);
        const float angle = (float)tap * FLARE_APERTURE_GOLDEN_ANGLE;
        const float2 direction = float2(cos(angle), sin(angle));
        const float2 q = direction * radius;

        const float hexDistance = max(abs(dot(q, normal0)), max(abs(dot(q, normal1)), abs(dot(q, normal2))));
        const float3 mask = (float3)1.0 - smoothstep(1.0 - FLARE_APERTURE_SOFTNESS, 1.0, hexDistance / (0.86602540378 * FLARE_APERTURE_RADII));

        sum += flareSampleBordered(uv + direction * (radius * circumradius) * texelSize, texelSize, edgeTexels) * mask;
        weightSum += mask;
    }

    return sum / max(weightSum, 1e-4);
}

float3 flarePolygonGhosts(float2 uv, float2 texelSize, float aspect)
{
    const float2 centered = float2((uv.x - 0.5) * aspect, uv.y - 0.5);

    float3 sum = (float3)0.0;

    [unroll]
    for (uint ghost = 0; ghost < FLARE_GHOST_COUNT; ghost++)
    {
        const float2 offset = centered / FLARE_GHOST_SCALES[ghost];
        const float2 source = 0.5 + float2(offset.x / aspect, offset.y);
        sum += flareHighlightBordered(source, texelSize, 2.0) * FLARE_GHOST_GAINS[ghost];
    }

    return sum;
}

float3 flareComposite(float2 uv, float2 texelSize, float aspect)
{
    float3 color = flarePolygonGhosts(uv, texelSize, aspect);

    color *= FLARE_GAIN;
    color = color * FLARE_LIMIT / (color + FLARE_LIMIT);
    return postEffectsSanitize(color);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width;
    uint height;
    flareDest.GetDimensions(width, height);

    const int2 pixel = int2(dispatchThreadID.xy);
    if ((uint)pixel.x >= width || (uint)pixel.y >= height)
    {
        return;
    }

    const float2 uv = ((float2)pixel + 0.5) / float2(width, height);

    uint sourceWidth;
    uint sourceHeight;
    flareSource.GetDimensions(sourceWidth, sourceHeight);

    const float2 sourceTexelSize = 1.0 / float2(sourceWidth, sourceHeight);

    if (push.passMode == FLARE_PASS_BRIGHT)
    {
        flareDest[pixel] = float4(flareDownsampleBright(uv, sourceTexelSize, postEffectsExtractExposure()), 1.0);
        return;
    }

    if (push.passMode == FLARE_PASS_APERTURE_SMALL)
    {
        const float apothem = clamp(FLARE_APERTURE_SMALL_SCALE * (float)height, 4.0, 10.0);
        flareDest[pixel] = float4(flareAperture(uv, sourceTexelSize, apothem), 1.0);
        return;
    }

    if (push.passMode == FLARE_PASS_APERTURE_LARGE)
    {
        const float apothem = clamp(FLARE_APERTURE_LARGE_SCALE * (float)height, 8.0, 48.0);
        flareDest[pixel] = float4(flareAperture(uv, sourceTexelSize, apothem), 1.0);
        return;
    }

    flareDest[pixel] = float4(flareComposite(uv, sourceTexelSize, (float)width / (float)height), 1.0);
}
