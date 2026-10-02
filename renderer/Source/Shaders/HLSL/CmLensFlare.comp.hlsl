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

static const uint FLARE_PASS_BRIGHT = 0;
static const uint FLARE_PASS_DOWNSAMPLE = 1;
static const uint FLARE_PASS_BOKEH = 2;
static const uint FLARE_PASS_SMOOTH = 3;
static const uint FLARE_PASS_COMPOSITE = 4;

static const uint FLARE_BOKEH_TAPS = 32;
static const float FLARE_APERTURE_BLADES = 8.0;
static const float FLARE_APERTURE_SOFTNESS = 0.4;
static const float FLARE_DISC_DIAMETER = 0.03;

static const float FLARE_GHOST_SCALES[3] = { 0.25, 0.5, 0.85 };
static const float FLARE_GHOST_OPACITIES[3] = { 0.10, 0.06, 0.035 };

static const float FLARE_EDGE_FADE = 0.08;
static const float FLARE_HALO_INVERSION = 0.12;
static const float FLARE_HALO_OPACITY = 0.02;

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

uint flareHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;

    return value;
}

float flareApertureWeight(float2 offset)
{
    const float radius = length(offset);
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

float3 flareBokehGather(float2 uv, float2 sourceTexelSize, float apertureRadiusTexels, float rotation)
{
    float3 sum = (float3)0.0;
    float weightSum = 0.0;

    for (uint i = 0; i < FLARE_BOKEH_TAPS; i++)
    {
        const float tapRadius = sqrt(((float)i + 0.5) / (float)FLARE_BOKEH_TAPS);
        const float tapAngle = (float)i * FLARE_GOLDEN_ANGLE + rotation;
        const float2 offset = float2(cos(tapAngle), sin(tapAngle)) * tapRadius;
        const float weight = flareApertureWeight(offset);

        if (weight <= 0.0)
        {
            continue;
        }

        const float2 tapUV = uv + offset * (apertureRadiusTexels * sourceTexelSize);
        sum += postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, tapUV, 0).rgb) *
               flareEdgeFade(tapUV) * weight;
        weightSum += weight;
    }

    return weightSum > 0.0 ? sum / weightSum : (float3)0.0;
}

float3 flareComposite(float2 uv, float aspect)
{
    const float2 position = flareToAspect(uv, aspect);

    float3 result = (float3)0.0;
    for (uint i = 0; i < 3; i++)
    {
        const float2 ghostPosition = flareFromAspect(-position / FLARE_GHOST_SCALES[i], aspect);
        result += flareSampleFaded(ghostPosition) * FLARE_GHOST_OPACITIES[i];
    }

    const float distance = length(position);
    if (distance > POST_EFFECTS_EPSILON)
    {
        const float sourceRadius = FLARE_HALO_INVERSION / max(distance, 1e-3);
        const float2 haloPosition = flareFromAspect(position / distance * sourceRadius, aspect);
        result += flareSampleFaded(haloPosition) * FLARE_HALO_OPACITY;
    }

    return postEffectsSanitize(result);
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

    if (push.passMode == FLARE_PASS_BRIGHT)
    {
        const float2 destinationTexelSize = 1.0 / float2(width, height);
        const float3 sample = postEffectsDownsampleBox4(flareSource, flareSource_Sampler, uv, destinationTexelSize);
        flareDest[pixel] = float4(postEffectsExtractWithExposure(sample, push.threshold, push.knee), 1.0);
        return;
    }

    if (push.passMode == FLARE_PASS_DOWNSAMPLE)
    {
        uint sourceWidth;
        uint sourceHeight;
        flareSource.GetDimensions(sourceWidth, sourceHeight);

        flareDest[pixel] = float4(postEffectsDownsample13(flareSource, flareSource_Sampler, uv,
                                                          1.0 / float2(sourceWidth, sourceHeight)), 1.0);
        return;
    }

    if (push.passMode == FLARE_PASS_BOKEH)
    {
        uint sourceWidth;
        uint sourceHeight;
        flareSource.GetDimensions(sourceWidth, sourceHeight);

        const float apertureRadiusTexels = max(FLARE_DISC_DIAMETER * 0.5 * (float)sourceHeight, 0.5);
        const uint hash = flareHash((uint)pixel.x * 1973u + (uint)pixel.y * 9277u + 26699u);
        const float rotation = (float)hash * (2.0 * FLARE_PI / 4294967296.0);

        flareDest[pixel] = float4(flareBokehGather(uv, 1.0 / float2(sourceWidth, sourceHeight),
                                                   apertureRadiusTexels, rotation), 1.0);
        return;
    }

    if (push.passMode == FLARE_PASS_SMOOTH)
    {
        uint sourceWidth;
        uint sourceHeight;
        flareSource.GetDimensions(sourceWidth, sourceHeight);

        flareDest[pixel] = float4(postEffectsTent9(flareSource, flareSource_Sampler, uv,
                                                   1.0 / float2(sourceWidth, sourceHeight)), 1.0);
        return;
    }

    const float aspect = (float)width / (float)height;
    flareDest[pixel] = float4(flareComposite(uv, aspect), 1.0);
}
