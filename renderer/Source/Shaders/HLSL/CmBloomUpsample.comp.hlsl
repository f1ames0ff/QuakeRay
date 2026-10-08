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

[[vk::binding(0, 0)]] Texture2D<float4> bloomSource;
[[vk::binding(1, 0)]] SamplerState bloomSource_Sampler;
[[vk::binding(0, 1)]] Texture2D<float4> bloomCoarse;
[[vk::binding(1, 1)]] SamplerState bloomCoarse_Sampler;
[[vk::binding(0, 3), vk::image_format("rgba16f")]] RWTexture2D<float4> bloomDest;

struct BloomUpsamplePush_BT
{
    float scatter;
    uint hasSource;
    float normalization;
    float padding1;
};

[[vk::push_constant]] ConstantBuffer<BloomUpsamplePush_BT> push;

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width;
    uint height;
    bloomDest.GetDimensions(width, height);

    const int2 pixel = int2(dispatchThreadID.xy);
    if ((uint)pixel.x >= width || (uint)pixel.y >= height)
    {
        return;
    }

    const float2 uv = ((float2)pixel + 0.5) / float2(width, height);

    uint coarseWidth;
    uint coarseHeight;
    bloomCoarse.GetDimensions(coarseWidth, coarseHeight);
    const float2 coarseTexelSize = 1.0 / float2(coarseWidth, coarseHeight);

    float3 result = postEffectsTent9(bloomCoarse, bloomCoarse_Sampler, uv, coarseTexelSize);
    if (push.hasSource != 0)
    {
        const float3 source = postEffectsSanitize(bloomSource.SampleLevel(bloomSource_Sampler, uv, 0).rgb);
        result = result * saturate(push.scatter) + source;
    }
    result *= max(push.normalization, 0.0);

    bloomDest[pixel] = float4(postEffectsSanitize(result), 1.0);
}
