// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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



#define DESC_SET_TEXTURES 0
#include "ShaderCommonHLSLFunc.hlsli"

struct RasterizerFrag_BT
{
    [[vk::offset(64)]] float4 color;
    [[vk::offset(80)]] uint   textureIndex;
    [[vk::offset(84)]] uint   emissionTextureIndex;
};

[[vk::push_constant]] ConstantBuffer<RasterizerFrag_BT> rasterizerFragInfo;

[[vk::constant_id(0)]] const uint alphaTest = 0;

#define ALPHA_THRESHOLD 0.5

struct RasterizerFragInput
{
    [[vk::location(0)]] float4 vertColor    : COLOR0;
    [[vk::location(1)]] float2 vertTexCoord : TEXCOORD0;
};

struct RasterizerFragOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

void main(RasterizerFragInput input, out RasterizerFragOutput output)
{
    float4 albedoAlpha = getTextureSample(rasterizerFragInfo.textureIndex, input.vertTexCoord);


    output.outColor = rasterizerFragInfo.color * input.vertColor * albedoAlpha;


    if (alphaTest != 0)
    {
        if (output.outColor.a < ALPHA_THRESHOLD)
        {
            discard;
        }
    }
}
