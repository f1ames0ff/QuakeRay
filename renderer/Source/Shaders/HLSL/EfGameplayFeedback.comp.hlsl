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

    const int2 size = effect_getFramebufSize();
    const float2 uv = effect_getFramebufUV(pix);
    const float aspect = float(size.x) / float(size.y);
    const float2 centered = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
    const float2 direction = normalize(centered + float2(1e-5, 1e-5));

    const float edgeMask = gameplayFeedbackEdgeMask(uv);
    const float heightScale = float(size.y) / 1080.0;
    const float master = push.custom.aberration / 0.3;

    float splitPixels = (2.0 * push.custom.damage + 0.75 * push.custom.liquid) * master * heightScale;
    splitPixels = min(splitPixels, 2.75 * heightScale);

    const float2 offset = direction * (splitPixels * edgeMask);
    const int2 redPix = int2(round(float2(pix) + offset));
    const int2 greenPix = int2(round(float2(pix) + offset * 0.5));

    float3 color;
    color.r = effect_loadFromSource(redPix).r;
    color.g = effect_loadFromSource(greenPix).g;
    color.b = effect_loadFromSource(pix).b;

    color += float3(0.12, 0.0, 0.0) * (push.custom.damage * edgeMask);

    const float pickupMask = smoothstep(1.0 - push.custom.pickupHeight, 1.0, uv.y);
    const float3 pickupColor = float3(push.custom.pickupColorR,
                                      push.custom.pickupColorG,
                                      push.custom.pickupColorB);
    color = lerp(color, pickupColor, saturate(push.custom.pickup * pickupMask));

    effect_storeToTarget(saturate(color), pix);
}
