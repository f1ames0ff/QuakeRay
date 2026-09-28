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

#include "EfSimple.hlsli"

float hueShiftMod(float x, float y)
{
    return x - y * floor(x / y);
}

float3 hsv2rgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    float3 color = effect_loadFromSource(pix);

    const int2 rendPix = int2(effect_getFramebufUV(pix) * float2(globalUniform.renderWidth, globalUniform.renderHeight));
    const float3 albedo = framebufAlbedo_Sampled.Load(int3(rendPix, 0)).rgb;


    float bw = getLuminance(color) + getLuminance(albedo) * 0.4;
    bw = clamp(bw * 1.5, 0.0, 1.0);

    const float h_scale = 0.7;
    const float h_offset = 0.65;
    float h = hueShiftMod(h_offset + bw * h_scale, 1.0);

    float3 dst = hsv2rgb(float3(h, 1.0, clamp(sqrt(bw)+0.1, 0.0, 1.0)));

    effect_storeToTarget(lerp(color, dst, getProgress()), pix);
}
