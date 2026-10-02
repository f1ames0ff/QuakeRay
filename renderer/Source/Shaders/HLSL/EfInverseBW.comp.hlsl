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

#include "EfSimple.hlsli"

float getBW(float3 color)
{
    return max(max(color.r, color.g), color.b);
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

    float bw = max(getBW(color), getBW(albedo));
    bw = sqrt(bw);

    const int L = 32;
    bw = clamp(int(bw * L), 0, L) / float(L);

    effect_storeToTarget(lerp(color, (float3)(1 - bw), getProgress()), pix);
}
