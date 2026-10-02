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
[[vk::binding(0, 3)]] RWTexture2D<float4> flareDest;

struct LensFlarePush_BT
{
    uint passMode;
    float threshold;
    float knee;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<LensFlarePush_BT> push;

static const float FLARE_PI = 3.14159265358979323846;
static const float FLARE_GOLDEN_ANGLE = 2.399963229728653;

static const uint FLARE_BOKEH_TAPS = 32;
static const float FLARE_APERTURE_RADIUS = 0.03;
static const float FLARE_APERTURE_BLADES = 8.0;
static const float FLARE_APERTURE_SOFTNESS = 0.4;

static const float FLARE_GHOST_SCALE[3] = { -0.35, -0.7, -1.1 };
static const float FLARE_GHOST_OPACITY[3] = { 0.16, 0.10, 0.06 };
static const float3 FLARE_GHOST_TINT[3] =
{
    float3(1.05, 0.88, 0.72),
    float3(0.72, 0.86, 1.05),
    float3(0.92, 1.00, 0.80),
};

static const float FLARE_EDGE_FADE = 0.08;
static const float FLARE_HALO_RADIUS = 0.7;
static const float FLARE_HALO_OPACITY = 0.04;

float flareEdgeFade(float2 uv)
{
    const float2 clamped = saturate(uv);
    const float2 low = smoothstep((float2)0.0, (float2)FLARE_EDGE_FADE, clamped);
    const float2 high = smoothstep((float2)0.0, (float2)FLARE_EDGE_FADE, 1.0 - clamped);
    return low.x * low.y * high.x * high.y;
}

float2 flareToAspect(float2 uv, float aspect)
{
    return (uv - 0.5) * float2(aspect, 1.0);
}

float2 flareFromAspect(float2 position, float aspect)
{
    return position / float2(aspect, 1.0) + 0.5;
}

float3 flareSampleFaded(float2 uv)
{
    const float fade = flareEdgeFade(uv);
    if (fade <= 0.0)
    {
        return (float3)0.0;
    }

    return postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, uv, 0).rgb) * fade;
}

float flareApertureWeight(float2 offset)
{
    const float radius = length(offset) / FLARE_APERTURE_RADIUS;
    if (radius >= 1.0)
    {
        return 0.0;
    }

    const float bladeAngle = 2.0 * FLARE_PI / FLARE_APERTURE_BLADES;
    const float angle = atan2(offset.y, offset.x);
    const float blade = frac(angle / bladeAngle + 0.5) * bladeAngle;
    const float edge = cos(bladeAngle * 0.5) / max(cos(blade - bladeAngle * 0.5), POST_EFFECTS_EPSILON);

    return 1.0 - smoothstep(1.0 - FLARE_APERTURE_SOFTNESS, 1.0, radius / edge);
}

float3 flareBokehGather(float2 uv, float aspect)
{
    const float2 uvOffsetScale = float2(1.0 / aspect, 1.0);

    float3 sum = (float3)0.0;
    float weightSum = 0.0;

    for (uint i = 0; i < FLARE_BOKEH_TAPS; i++)
    {
        const float tapRadius = FLARE_APERTURE_RADIUS * sqrt(((float)i + 0.5) / (float)FLARE_BOKEH_TAPS);
        const float tapAngle = (float)i * FLARE_GOLDEN_ANGLE;
        const float2 offset = float2(cos(tapAngle), sin(tapAngle)) * tapRadius;
        const float weight = flareApertureWeight(offset);

        if (weight <= 0.0)
        {
            continue;
        }

        const float2 tapUV = uv + offset * uvOffsetScale;
        sum += postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, tapUV, 0).rgb) *
               flareEdgeFade(tapUV) * weight;
        weightSum += weight;
    }

    return weightSum > 0.0 ? sum / weightSum : (float3)0.0;
}

float2 flareHaloPosition(float2 uv, float aspect)
{
    const float2 position = flareToAspect(uv, aspect);
    const float distance = length(position);
    if (distance <= POST_EFFECTS_EPSILON)
    {
        return uv;
    }

    return flareFromAspect(position - position / distance * FLARE_HALO_RADIUS, aspect);
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

    if (push.passMode == 0)
    {
        const float2 destinationTexelSize = 1.0 / float2(width, height);
        const float3 sample = postEffectsDownsampleBox4(flareSource, flareSource_Sampler, uv, destinationTexelSize);
        flareDest[pixel] = float4(postEffectsExtractWithExposure(sample, push.threshold, push.knee), 1.0);
        return;
    }

    const float aspect = (float)width / (float)height;

    if (push.passMode == 1)
    {
        flareDest[pixel] = float4(flareBokehGather(uv, aspect), 1.0);
        return;
    }

    const float2 aspectPosition = flareToAspect(uv, aspect);

    float3 result = (float3)0.0;
    for (uint i = 0; i < 3; i++)
    {
        const float2 ghostPosition = flareFromAspect(aspectPosition * FLARE_GHOST_SCALE[i], aspect);
        result += flareSampleFaded(ghostPosition) * (FLARE_GHOST_TINT[i] * FLARE_GHOST_OPACITY[i]);
    }

    const float2 haloPosition = flareHaloPosition(uv, aspect);
    result += flareSampleFaded(haloPosition) * (float3)FLARE_HALO_OPACITY;

    flareDest[pixel] = float4(postEffectsSanitize(result), 1.0);
}
