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
[[vk::binding(0, 3)]] RWTexture2D<float4> bloomDest;

struct BloomDownsamplePush_BT
{
    uint extract;
    float threshold;
    float knee;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<BloomDownsamplePush_BT> push;

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

    uint sourceWidth;
    uint sourceHeight;
    bloomSource.GetDimensions(sourceWidth, sourceHeight);

    const float2 uv = ((float2)pixel + 0.5) / float2(width, height);
    const float2 sourceTexelSize = 1.0 / float2(sourceWidth, sourceHeight);

    float3 result;
    if (push.extract != 0)
    {
        result = postEffectsDownsample13Karis(bloomSource, bloomSource_Sampler, uv, sourceTexelSize);
    }
    else
    {
        result = postEffectsDownsample13(bloomSource, bloomSource_Sampler, uv, sourceTexelSize);
    }

    bloomDest[pixel] = float4(result, 1.0);
}
