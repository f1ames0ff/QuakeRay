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

#include "ColorCompositing.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> scene;
[[vk::binding(1, 0)]] Texture2D<float4> bloom;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> output;

struct ColorProbePush
{
    uint mode;
    uint thresholdedBloom;
    float bloomStrength;
    float padding;
};

[[vk::push_constant]] ConstantBuffer<ColorProbePush> push;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    const float3 input = scene.Load(int3(id.xy, 0)).rgb;
    float3 color;
    if (push.mode == 0)
    {
        color = colorLimitPreserveHue(input, 1.0);
    }
    else if (push.mode == 1)
    {
        color = colorLimitPreserveHue(input, 256.0);
    }
    else if (push.mode == 2)
    {
        const float kneeStart = 0.6;
        const float kneeW = (kneeStart * (kneeStart - 2.0) + 10.0) / 9.0;
        color = colorHighlightShoulder(input, kneeStart, kneeW, -kneeStart * kneeStart,
                                       kneeW - 2.0 * kneeStart);
        color = colorLimitPreserveHue(color, 1.0);
    }
    else if (push.mode == 3)
    {
        color = colorComposeBloom(input, bloom.Load(int3(id.xy, 0)).rgb,
                                  push.bloomStrength, push.thresholdedBloom != 0);
    }
    else if (push.mode == 4)
    {
        color = colorLimitPreserveHue(input, 60000.0);
    }
    else
    {
        color = colorApplyTint(input, bloom.Load(int3(id.xy, 0)).rgb, push.bloomStrength);
    }

    output[id.xy] = float4(color, 1.0);
}
