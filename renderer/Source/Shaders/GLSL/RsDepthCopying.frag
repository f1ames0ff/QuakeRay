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

#version 460

layout(push_constant) uniform DepthCopyingFrag_BT
{
    uint renderWidth;
    uint renderHeight;
} depthCopyingPush;

#define CHECKERBOARD_FULL_WIDTH depthCopyingPush.renderWidth
#define CHECKERBOARD_FULL_HEIGHT depthCopyingPush.renderHeight

#define DESC_SET_FRAMEBUFFERS 0
#include "ShaderCommonGLSLFunc.h"

void main()
{
    const ivec2 pix = ivec2(gl_FragCoord.xy);
    gl_FragDepth = texelFetch(framebufDepthNdc_Sampled, pix, 0).r;
}
