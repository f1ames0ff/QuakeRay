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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonGLSLFunc.h"

layout(local_size_x = COMPUTE_EFFECT_GROUP_SIZE_X, local_size_y = COMPUTE_EFFECT_GROUP_SIZE_Y, local_size_z = 1) in;

layout(constant_id = 0) const uint isSourcePing = 0;

#define EFFECT_SOURCE_IS_PING (isSourcePing != 0)
#include "EfCommon.inl"

layout(push_constant) uniform EffectSimplePush_BT
{
    uint transitionType;
    float transitionBeginTime;
    float transitionDuration;
#ifdef EFFECT_PUSH_CONST_T
    EFFECT_PUSH_CONST_T custom;
#endif
} push;

float getProgress()
{
    float progress =
        max(globalUniform.time - push.transitionBeginTime, 0.001) /
        max(push.transitionDuration, 0.001);

    progress = clamp(progress, 0, 1);

    if (push.transitionType == 1)
    {
        return 1.0 - progress;
    }
    else
    {
        return progress;
    }
}
