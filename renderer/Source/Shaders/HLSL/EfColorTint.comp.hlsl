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

struct EffectColorTint_PushConst
{
    float intensity;
    float r;
    float g;
    float b;
};

#define EFFECT_PUSH_CONST_T EffectColorTint_PushConst
#include "EfSimple.hlsli"

float3 applyTint(float3 color)
{
    float3 tint = float3(push.custom.r, push.custom.g, push.custom.b);

    float t = push.custom.intensity * clamp(getLuminance(color), 0.05, 1.0) * getProgress();
    return lerp(color, tint, t);
}

#define APPLY_RADIAL_OFFSET 1

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

#if APPLY_RADIAL_OFFSET
    float2 c = effect_getCenteredFromPix(pix);
    c *= lerp(1, 0.985, getProgress());

    float3 rgb = lerp(effect_loadFromSource(pix), applyTint(effect_loadFromSource_Centered(c)), 0.5 * dot(c, c));
#else
    float3 rgb = applyTint(effect_loadFromSource(pix));
#endif

    effect_storeToTarget(rgb, pix);
}
