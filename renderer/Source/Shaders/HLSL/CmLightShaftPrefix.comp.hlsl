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

// Independent implementation of the standard front-to-back prefix integral;
// no reference renderer source is copied.

#define DESC_SET_LIGHT_SHAFT_VOLUME 0
#define DESC_SET_GLOBAL_UNIFORM 1

#include "ShaderCommonHLSLFunc.hlsli"

#define LIGHT_SHAFT_VOLUME_SIZE_X 160
#define LIGHT_SHAFT_VOLUME_SIZE_Y 88
#define LIGHT_SHAFT_VOLUME_SIZE_Z 64

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 column = int2(dispatchThreadID.xy);
    if (column.x >= LIGHT_SHAFT_VOLUME_SIZE_X || column.y >= LIGHT_SHAFT_VOLUME_SIZE_Y)
    {
        return;
    }

    const float stepLength = max(globalUniform.volumeCameraFar - globalUniform.volumeCameraNear, 0.001) /
                             (float)LIGHT_SHAFT_VOLUME_SIZE_Z;
    float3 accumulated = (float3)0;

    for (int z = 0; z < LIGHT_SHAFT_VOLUME_SIZE_Z; z++)
    {
        const int3 cell = int3(column, z);
        const float4 current = lightShaftVolume[cell];
        accumulated += current.rgb * stepLength;
        lightShaftVolume[cell] = float4(accumulated, current.a);
    }
}
