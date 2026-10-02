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

struct EffectChromaticAberration_PushConst
{
    float intensity;
};

#define EFFECT_PUSH_CONST_T EffectChromaticAberration_PushConst
#include "EfSimple.hlsli"

static const float BASE_RADIUS = 0.05;
static const int MAX_ITERATIONS = 8;

float3 mix3(float3 a, float3 b, float3 c, float t)
{
    return lerp(
        lerp(a, b, clamp(t * 2, 0, 1)),
        c,
        clamp((t - 0.5) * 2, 0, 1));
}

float3 getAberrationColor(float t)
{
    return mix3(
        float3(1,0,0),
        float3(1,1,1),
        float3(0,0,1),
        t);
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    float2 c = effect_getCenteredFromPix(pix);
    float2 offset = BASE_RADIUS * getProgress() * push.custom.intensity * c * dot(c, c);

    int iterCount = clamp(int(length(effect_getFramebufSize() * offset / 2.0)), 3, MAX_ITERATIONS);
    float2 delta = offset / iterCount;

    float3 color = (float3)0.0;
    float3 weight = (float3)0.0;

    for (int i = 0; i < iterCount; i++)
    {
        float3 a = effect_loadFromSource_Centered(c - delta * i);
        float3 w = getAberrationColor((i + 0.5) / iterCount);

        color += a * w;
        weight += w;
    }

    effect_storeToTarget(color / weight, pix);
}
