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



struct RasterizerVert_BT
{
    float4x4 viewProj;
};

[[vk::push_constant]] ConstantBuffer<RasterizerVert_BT> rasterizerVertInfo;

[[vk::constant_id(0)]] const uint applyVertexColorGamma = 0;

void main(
    [[vk::location(0)]] float3 position : POSITION0,
    [[vk::location(1)]] float4 color : COLOR0,
    [[vk::location(2)]] float2 texCoord : TEXCOORD0,
    [[vk::location(0)]] out float4 outColor : COLOR0,
    [[vk::location(1)]] out float2 outTexCoord : TEXCOORD1,
    out float4 outPosition : SV_Position)
{
    if (applyVertexColorGamma != 0)
    {
        outColor = float4(pow(color.rgb, (float3)2.2), color.a);
    }
    else
    {
        outColor = color;
    }

    outTexCoord = texCoord;
    outPosition = mul(rasterizerVertInfo.viewProj, float4(position, 1.0));
}
