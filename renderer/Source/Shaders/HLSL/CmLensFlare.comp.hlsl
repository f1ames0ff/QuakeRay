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
[[vk::binding(0, 1)]] Texture2D<float4> flareWide;
[[vk::binding(1, 1)]] SamplerState flareWide_Sampler;
[[vk::binding(0, 3)]] RWTexture2D<float4> flareDest;

struct LensFlarePush_BT
{
    uint passMode;
    float threshold;
    float knee;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<LensFlarePush_BT> push;

static const float FLARE_EDGE_FADE = 0.08;
static const float FLARE_COMPACT_RATIO = 1.5;
static const float FLARE_HALO_RADIUS = 0.22;
static const float FLARE_HALO_OPACITY = 0.05;
static const float FLARE_GHOST_SCALE[3] = { -0.35, -0.7, -1.1 };
static const float FLARE_GHOST_OPACITY[3] = { 0.16, 0.10, 0.06 };

float flareEdgeFade(float2 uv)
{
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 0.0;
    }

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

float3 flareSampleEligible(float2 uv, float edgeFade)
{
    if (edgeFade <= 0.0)
    {
        return (float3)0.0;
    }

    const float3 bright = postEffectsSanitize(flareSource.SampleLevel(flareSource_Sampler, uv, 0).rgb);
    const float3 wide = postEffectsSanitize(flareWide.SampleLevel(flareWide_Sampler, uv, 0).rgb);

    const float brightLuminance = getLuminance(bright);
    const float wideLuminance = getLuminance(wide);
    const float weight = saturate((brightLuminance - FLARE_COMPACT_RATIO * wideLuminance) / max(brightLuminance, POST_EFFECTS_EPSILON));

    return bright * weight * edgeFade;
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

    if (push.passMode == 0)
    {
        const float2 uv = ((float2)pixel + 0.5) / float2(width, height);
        const float2 destinationTexelSize = 1.0 / float2(width, height);
        const float3 sample = postEffectsDownsampleBox4(flareSource, flareSource_Sampler, uv, destinationTexelSize);
        flareDest[pixel] = float4(postEffectsExtractWithExposure(sample, push.threshold, push.knee), 1.0);
        return;
    }

    if (push.passMode == 1)
    {
        uint sourceWidth;
        uint sourceHeight;
        flareSource.GetDimensions(sourceWidth, sourceHeight);

        const int2 base = pixel * 4;
        float3 sum = (float3)0.0;
        for (int y = 0; y < 4; y++)
        {
            for (int x = 0; x < 4; x++)
            {
                const int2 samplePixel = clamp(base + int2(x, y), int2(0, 0), int2((int)sourceWidth - 1, (int)sourceHeight - 1));
                sum += flareSource.Load(int3(samplePixel, 0)).rgb;
            }
        }

        flareDest[pixel] = float4(postEffectsSanitize(sum * 0.0625), 1.0);
        return;
    }

    const float aspect = (float)width / (float)height;
    const float2 uv = ((float2)pixel + 0.5) / float2(width, height);
    const float2 aspectPosition = flareToAspect(uv, aspect);

    float3 result = (float3)0.0;
    for (uint i = 0; i < 3; i++)
    {
        const float2 ghostPosition = flareFromAspect(aspectPosition * FLARE_GHOST_SCALE[i], aspect);
        result += flareSampleEligible(ghostPosition, flareEdgeFade(ghostPosition)) * FLARE_GHOST_OPACITY[i];
    }

    float3 halo = (float3)0.0;
    for (uint i = 0; i < 8; i++)
    {
        const float angle = (float)i * 0.78539816339744831;
        const float2 offset = float2(cos(angle), sin(angle)) * FLARE_HALO_RADIUS / float2(aspect, 1.0);
        const float2 haloPosition = uv + offset;
        halo += flareSampleEligible(haloPosition, flareEdgeFade(haloPosition));
    }

    result += halo * (FLARE_HALO_OPACITY * 0.125);

    flareDest[pixel] = float4(postEffectsSanitize(result), 1.0);
}
