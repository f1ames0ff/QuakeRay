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

#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_DECALS 3
#include "ShaderCommonGLSLFunc.h"

layout (location = 0) flat out uint outInstanceIndex;

vec4 getPosition()
{
    int b = 1 << (gl_VertexIndex % 14);

    return vec4(
        float((0x287A & b) != 0) - 0.5,
        float((0x02AF & b) != 0) - 0.5,
        float((0x31E3 & b) != 0) - 0.5,
        1.0
    );
}

void main()
{
    outInstanceIndex = gl_InstanceIndex;
    gl_Position = globalUniform.projection * globalUniform.view * decalInstances[gl_InstanceIndex].transform * getPosition();
}
