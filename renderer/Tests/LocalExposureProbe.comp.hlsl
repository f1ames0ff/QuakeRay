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

#include "LocalExposure.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::binding(1, 0)]] SamplerState sourceSampler;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> output;

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
    const float2 inverseSize = 1.0 / float2(width, height);
    const float2 uv = (float2(id.xy) + 0.5) * inverseSize;
    const float correction = localExposureCorrection(source, sourceSampler, uv, inverseSize, 0.5);
    output[id.xy] = float4(correction, correction, correction, 1.0);
}
