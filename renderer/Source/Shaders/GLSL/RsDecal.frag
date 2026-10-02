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

#version 460

#define FRAMEBUF_IGNORE_ATTACHMENTS
#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_TEXTURES 2
#define DESC_SET_DECALS 3
#include "ShaderCommonGLSLFunc.h"

layout (location = 0) flat in uint instanceIndex;

layout (location = 0) out vec4 outAlbedo;

void main()
{
    const ivec2 pix = getCheckerboardPix(ivec2(gl_FragCoord.xy));

    const ShDecalInstance decal = decalInstances[instanceIndex];

    const vec3 worldPosition = texelFetch(framebufSurfacePosition_Sampled, pix, 0).xyz;
    const mat4 worldToLocal = inverse(decal.transform);
    const vec4 localPosition = worldToLocal * vec4(worldPosition, 1.0);

    if (any(greaterThan(abs(localPosition.xyz), vec3(0.5))))
    {
        discard;
    }

    const vec2 texCoord = localPosition.xy + 0.5;

    const vec4 decalAlbedo = getTextureSample(decal.textureAlbedoAlpha, texCoord);

    outAlbedo = decalAlbedo;
}
