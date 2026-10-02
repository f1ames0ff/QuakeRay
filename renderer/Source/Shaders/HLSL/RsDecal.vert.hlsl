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



#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_DECALS 3
#include "ShaderCommonHLSLFunc.hlsli"

float4 getPosition(uint vertexIndex)
{
    const uint b = 1u << (vertexIndex % 14);

    return float4(
        float((0x287A & b) != 0) - 0.5,
        float((0x02AF & b) != 0) - 0.5,
        float((0x31E3 & b) != 0) - 0.5,
        1.0
    );
}

void main(
    uint vertexIndex : SV_VertexID,
    uint instanceIndex : SV_InstanceID,
    [[vk::location(0)]] nointerpolation out uint outInstanceIndex : TEXCOORD0,
    out float4 outPosition : SV_Position)
{
    outInstanceIndex = instanceIndex;
    outPosition = mul(mul(mul(globalUniform.projection, globalUniform.view), decalInstances[instanceIndex].transform), getPosition(vertexIndex));
}
