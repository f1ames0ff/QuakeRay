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



#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSL.hlsli"


[[vk::constant_id(0)]] const uint isSourcePing = 0;

#define EFFECT_SOURCE_IS_PING (isSourcePing != 0)
#include "EfCommon.hlsli"

struct EffectSimplePush_BT
{
    uint transitionType;
    float transitionBeginTime;
    float transitionDuration;
#ifdef EFFECT_PUSH_CONST_T
    EFFECT_PUSH_CONST_T custom;
#endif
};

[[vk::push_constant]] ConstantBuffer<EffectSimplePush_BT> push;

float getProgress()
{
    float progress =
        max(globalUniform.time - push.transitionBeginTime, 0.001) /
        max(push.transitionDuration, 0.001);

    progress = clamp(progress, 0.0, 1.0);

    if (push.transitionType == 1)
    {
        return 1.0 - progress;
    }
    else
    {
        return progress;
    }
}
