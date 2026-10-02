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

struct EffectGameplayFeedback_PushConst
{
    float damage;
    float liquid;
    float pickup;
    float pickupHeight;
    float aberration;
    float pickupColorR;
    float pickupColorG;
    float pickupColorB;
};

#define EFFECT_PUSH_CONST_T EffectGameplayFeedback_PushConst
#include "EfSimple.hlsli"

float gameplayFeedbackEdgeMask(float2 uv)
{
    const float2 outside = saturate((abs(uv - 0.5) - 0.25) / 0.25);
    return smoothstep(0.0, 1.0, length(outside));
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    const float2 uv = effect_getFramebufUV(pix);
    const float edgeMask = gameplayFeedbackEdgeMask(uv);

    float3 color = effect_loadFromSource(pix);

    color += float3(0.18, 0.0, 0.0) * (push.custom.damage * edgeMask);

    const float pickupMask = smoothstep(1.0 - push.custom.pickupHeight, 1.0, uv.y);
    const float amount = saturate(push.custom.pickup * pickupMask);
    const float3 pickup = float3(push.custom.pickupColorR,
                                 push.custom.pickupColorG,
                                 push.custom.pickupColorB) * amount;
    color = 1.0 - (1.0 - saturate(color)) * (1.0 - saturate(pickup));

    effect_storeToTarget(saturate(color), pix);
}
