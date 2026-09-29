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
#define DESC_SET_RANDOM 2
#include "ShaderCommonHLSLFunc.hlsli"


[[vk::constant_id(0)]] const uint isSourcePing = 0;

#define EFFECT_SOURCE_IS_PING (isSourcePing != 0)
#include "EfCommon.hlsli"

struct WipePush_BT
{
    uint stripWidthInPixels;
    uint startFrameId;
    float beginTime;
    float endTime;
};

[[vk::push_constant]] ConstantBuffer<WipePush_BT> push;

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);
    int2 sz = effect_getFramebufSize();

    uint count = sz.x  / push.stripWidthInPixels;
    uint strip = pix.x / push.stripWidthInPixels;

    float rnd = effect_getRandomSample(int2(0, 0), push.startFrameId);

    float x = strip / float(count) + rnd * 10;

    const float maxDiff = 0.1f;
    float wave = maxDiff * sin(20 * x) * sin(10 * x) * sin(100 * x) + maxDiff;

    float progress =
        max(globalUniform.time - push.beginTime, 0.0) /
        max(push.endTime       - push.beginTime, 0.001);

    progress = clamp(progress - wave, 0.0, 1.0);
    progress *= progress;
    progress *= 2;


    int pix_offset = int(progress * sz.y);
    float3 c;

    if (pix.y - pix_offset < 0)
    {
        c = effect_loadFromSource(pix);
    }
    else
    {
        c = framebufWipeEffectSource[int2(pix.x, pix.y - pix_offset)].rgb;
    }

    effect_storeToTarget(c, pix);
}
