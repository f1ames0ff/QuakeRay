// Copyright (c) 2026 QuakeRay contributors
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

#version 460

layout (location = 0) in vec4 vertColor;
layout (location = 1) in vec2 vertTexCoord;

layout (location = 0) out vec4 outColor;


#define DESC_SET_TEXTURES 0
#include "ShaderCommonGLSLFunc.h"

layout(push_constant) uniform RasterizerFrag_BT
{
    layout(offset = 64) vec4 color;
    layout(offset = 80) uint textureIndex;
    layout(offset = 84) uint emissionTextureIndex;
} rasterizerFragInfo;

layout (constant_id = 0) const uint alphaTest = 0;

#define ALPHA_THRESHOLD 0.5


void main()
{
    vec4 albedoAlpha = getTextureSample(rasterizerFragInfo.textureIndex, vertTexCoord);


    outColor = rasterizerFragInfo.color * vertColor * albedoAlpha;


    if (alphaTest != 0)
    {
        if (outColor.a < ALPHA_THRESHOLD)
        {
            discard;
        }
    }
}
