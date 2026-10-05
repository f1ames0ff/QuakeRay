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

#include "NearDof.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::binding(1, 0)]] Texture2D<float4> depthTexture;
[[vk::binding(2, 0)]] Texture2D<float4> surface;
[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> output;

struct NearDofPush
{
    float strength;
    float focusDistance;
    float maxRadius;
    float axisFactor;
};

[[vk::push_constant]] ConstantBuffer<NearDofPush> push;

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
    output[id.xy] = float4(nearDofFilter(source, depthTexture, surface, int2(id.xy), push.axisFactor,
                                        push.strength, push.focusDistance, push.maxRadius), 1.0);
}
