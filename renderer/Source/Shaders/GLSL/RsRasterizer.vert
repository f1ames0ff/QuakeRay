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

layout (location = 0) in vec3 position;
layout (location = 1) in vec4 color;
layout (location = 2) in vec2 texCoord;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec2 outTexCoord;

layout(push_constant) uniform RasterizerVert_BT
{
    layout(offset = 0) mat4 viewProj;
} rasterizerVertInfo;

layout (constant_id = 0) const uint applyVertexColorGamma = 0;

void main()
{
    if (applyVertexColorGamma != 0)
    {
        outColor = vec4(pow(color.rgb, vec3(2.2)), color.a);
    }
    else
    {
        outColor = color;
    }

    outTexCoord = texCoord;
    gl_Position = rasterizerVertInfo.viewProj * vec4(position, 1.0);
}
