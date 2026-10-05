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

struct EffectSharpen_PushConst
{
    float strength;
};

#define EFFECT_PUSH_CONST_T EffectSharpen_PushConst
#include "EfSimple.hlsli"

float casSharpness(float strength)
{
    return -1.0 / lerp(8.0, 5.0, saturate(strength));
}

float casAmplitude(float mn, float mx)
{
    return sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    const float3 up = saturate(effect_loadFromSource(pix + int2(0, -1)));
    const float3 left = saturate(effect_loadFromSource(pix + int2(-1, 0)));
    const float3 center = saturate(effect_loadFromSource(pix));
    const float3 right = saturate(effect_loadFromSource(pix + int2(1, 0)));
    const float3 down = saturate(effect_loadFromSource(pix + int2(0, 1)));

    const float3 mn = min(min(left, center), min(right, min(up, down)));
    const float3 mx = max(max(left, center), max(right, max(up, down)));

    const float peak = casSharpness(push.custom.strength);
    const float3 amount = peak * float3(casAmplitude(mn.r, mx.r),
                                        casAmplitude(mn.g, mx.g),
                                        casAmplitude(mn.b, mx.b));
    const float3 weight = 1.0 / (1.0 + 4.0 * amount);
    const float3 sharpened = saturate((up * amount + left * amount + right * amount + down * amount + center) * weight);

    effect_storeToTarget(sharpened, pix);
}
