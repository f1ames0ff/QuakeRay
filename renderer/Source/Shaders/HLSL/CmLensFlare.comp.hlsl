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
[[vk::binding(2, 0)]] Texture2D<float4> flareDepth;
[[vk::binding(0, 3)]] RWTexture2D<float4> flareDest;

struct LensFlarePush_BT
{
    uint passMode;
    float threshold;
    float knee;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<LensFlarePush_BT> push;

static const uint FLARE_PASS_BRIGHT = 0;
static const uint FLARE_PASS_FLARE = 1;
static const uint FLARE_PASS_BOKEH = 2;

static const uint FLARE_GHOST_COUNT = 8;
static const float FLARE_GHOST_SCALES[8] = { -0.8, -1.5, 0.6, 1.3, -2.2, 0.35, 2.0, -0.45 };
static const float FLARE_GHOST_CHROMAS[8] = { 0.012, 0.022, 0.010, 0.030, 0.038, 0.016, 0.034, 0.026 };
static const float3 FLARE_GHOST_TINTS[8] =
{
    float3(0.45, 0.55, 1.00),
    float3(1.00, 0.60, 0.28),
    float3(0.95, 0.35, 0.85),
    float3(0.55, 0.70, 1.00),
    float3(1.00, 0.70, 0.35),
    float3(0.90, 0.40, 0.95),
    float3(0.40, 0.65, 1.00),
    float3(1.00, 0.55, 0.30),
};

static const float FLARE_DISTORTION = 0.2;
static const float FLARE_HALO_SCALE = -1.4;
static const float FLARE_HALO_SPREAD = 1.15;
static const float FLARE_HALO_CHROMA = 0.03;
static const float FLARE_HALO_INTENSITY = 2.0;
static const float FLARE_BRIGHTNESS_MAX = 4.0;
static const float FLARE_CLAMP_MAX = 300.0;
static const float FLARE_GAIN = 0.1;
static const float FLARE_DIST_REF = 512.0;
static const float FLARE_DIST_MAX = 4096.0;
static const float FLARE_SKY_DEPTH = 10000.0;

static const uint FLARE_BOKEH_TAPS = 40;
static const float FLARE_GOLDEN_ANGLE = 2.399963229728653;
static const float FLARE_BOKEH_RADIUS_TEXELS = 7.0;
static const float FLARE_BOKEH_GAUSS = 3.0;

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
    result *= above / max(luminance, POST_EFFECTS_EPSILON);
    result *= clamp(above / max(push.threshold, POST_EFFECTS_EPSILON), 0.0, FLARE_BRIGHTNESS_MAX);

    const float depth = flareDepth.SampleLevel(flareSource_Sampler, uv, 0).r;
    if (depth < FLARE_SKY_DEPTH)
    {
        const float square = FLARE_DIST_REF * FLARE_DIST_REF;
        result *= (square / (depth * depth + square)) * saturate(1.0 - depth / FLARE_DIST_MAX);
    }

    return postEffectsSanitize(result / exposure);
}

float3 flareSampleBorder(float2 uv)
{
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return (float3)0.0;
    }

    return postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, uv, 0).rgb);
}

float2 flareDistort(float2 uv, float aspect)
{
    float2 centered = uv - 0.5;
    centered.x *= aspect;
    centered *= 1.0 + FLARE_DISTORTION * dot(centered, centered);
    return 0.5 + float2(centered.x / aspect, centered.y);
}

float2 flareToUV(float2 offset, float aspect)
{
    return 0.5 + float2(offset.x / aspect, offset.y);
}

float flareSourceMask(float2 offset, float power)
{
    const float radius = length(offset) * 1.41421356;
    return pow(saturate(1.0 - radius), power);
}

float3 flareGhostLayer(float2 uv, float aspect, float scale, float chroma)
{
    const float2 position = flareDistort(uv, aspect) - 0.5;
    const float2 aspectPosition = float2(position.x * aspect, position.y);
    const float2 samplePosition = aspectPosition / scale;

    float3 color;
    color.r = flareSampleBorder(flareToUV(samplePosition * (1.0 - chroma), aspect)).r;
    color.g = flareSampleBorder(flareToUV(samplePosition, aspect)).g;
    color.b = flareSampleBorder(flareToUV(samplePosition * (1.0 + chroma), aspect)).b;

    const float mask = flareSourceMask(samplePosition, 3.0);
    return color * mask / max(scale * scale, 0.05);
}

float3 flareHaloLayer(float2 uv, float aspect)
{
    const float3 inner = flareGhostLayer(uv, aspect, FLARE_HALO_SCALE, FLARE_HALO_CHROMA);
    const float3 outer = flareGhostLayer(uv, aspect, FLARE_HALO_SCALE * FLARE_HALO_SPREAD, FLARE_HALO_CHROMA);

    return max((float3)0.0, inner - outer) * FLARE_HALO_INTENSITY;
}

float3 flareBokehBlur(float2 uv, float2 sourceTexelSize)
{
    float3 sum = (float3)0.0;

    float weightSum = 0.0;

    [unroll]
    for (uint tap = 0; tap < FLARE_BOKEH_TAPS; tap++)
    {
        const float radius = sqrt(((float)tap + 0.5) / (float)FLARE_BOKEH_TAPS);
        const float angle = (float)tap * FLARE_GOLDEN_ANGLE;
        const float2 offset = radius * float2(cos(angle), sin(angle)) * FLARE_BOKEH_RADIUS_TEXELS * sourceTexelSize;
        const float weight = exp(-FLARE_BOKEH_GAUSS * radius * radius);
        sum += flareSource.SampleLevel(flareSource_Sampler, uv + offset, 0).rgb * weight;
        weightSum += weight;
    }

    return postEffectsSanitize(sum / max(weightSum, 1e-4));
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

    if (push.passMode == FLARE_PASS_FLARE)
    {
        const float aspect = (float)width / (float)height;

        float3 sum = (float3)0.0;

        [unroll]
        for (uint ghost = 0; ghost < FLARE_GHOST_COUNT; ghost++)
        {
            sum += flareGhostLayer(uv, aspect, FLARE_GHOST_SCALES[ghost], FLARE_GHOST_CHROMAS[ghost]) * FLARE_GHOST_TINTS[ghost];
        }

        sum += flareHaloLayer(uv, aspect);
        flareDest[pixel] = float4(postEffectsSanitize(sum * FLARE_GAIN), 1.0);
        return;
    }

    flareDest[pixel] = float4(flareBokehBlur(uv, sourceTexelSize), 1.0);
}
