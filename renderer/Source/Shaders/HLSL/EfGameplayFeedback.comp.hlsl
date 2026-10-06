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
#include "ColorCompositing.hlsli"

float gameplayFeedbackEdgeMask(float2 uv)
{
    const float distanceFromCenter = length((uv - 0.5) * 2.0);
    const float inner = 0.85;
    const float softness = 3.0;

    const float t = saturate((distanceFromCenter - inner) / (1.0 - inner));
    return pow(t, softness);
}

float gameplayFeedbackPulse()
{
    const float TWO_PI = 6.28318530718;
    const float HZ = 1.0;
    const float MIN_BRIGHTNESS = 0.5;

    return lerp(MIN_BRIGHTNESS, 1.0, 0.5 + 0.5 * sin(globalUniform.time * TWO_PI * HZ));
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

    color = colorApplyTint(color, float3(1.0, 0.15, 0.10),
                           0.12 * push.custom.damage * edgeMask * gameplayFeedbackPulse());

    const float pickupMask = smoothstep(1.0 - push.custom.pickupHeight, 1.0, uv.y);
    const float amount = saturate(push.custom.pickup * pickupMask);
    const float3 pickup = float3(push.custom.pickupColorR,
                                 push.custom.pickupColorG,
                                 push.custom.pickupColorB);
    color = colorApplyTint(color, pickup, amount);

    effect_storeToTarget(colorLimitPreserveHue(color, 1.0), pix);
}
