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

struct EffectFilmGrain_PushConst
{
    float intensity;
    float size;
};

#define EFFECT_PUSH_CONST_T EffectFilmGrain_PushConst
#include "EfSimple.hlsli"
#include "Random.hlsli"

#define FILM_GRAIN_AMPLITUDE 0.18
#define FILM_GRAIN_RESPONSE 0.58
#define FILM_GRAIN_HIGHLIGHT_FADE 0.35

float filmGrainHash(uint2 cell, uint salt)
{
    return rnd16((cell.x * 73856093u) ^ (cell.y * 19349663u), salt);
}

float filmGrainValueNoise(float2 p, uint salt)
{
    const float2 cell = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    const uint2 c = uint2(cell);
    const float a = filmGrainHash(c, salt);
    const float b = filmGrainHash(c + uint2(1, 0), salt);
    const float d = filmGrainHash(c + uint2(0, 1), salt);
    const float e = filmGrainHash(c + uint2(1, 1), salt);

    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    const float3 color = effect_loadFromSource(pix);
    const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));

    const float size = max(push.custom.size, 0.25);
    const float2 coord = (float2(pix) + 0.5) / size;
    const uint coarseSalt = globalUniform.frameId * 83492791u + RANDOM_SALT_POSTEFFECT;
    const uint fineSalt = globalUniform.frameId * 83492791u + 0x9e3779b9u;
    const float coarse = filmGrainValueNoise(coord, coarseSalt);
    const float fine = filmGrainValueNoise(coord * 2.0 + 13.7, fineSalt);
    const float grain = (coarse * 2.0 - 1.0) * 0.65 + (fine * 2.0 - 1.0) * 0.35;

    const float response = pow(saturate(luma), FILM_GRAIN_RESPONSE) *
        (1.0 - FILM_GRAIN_HIGHLIGHT_FADE * smoothstep(0.7, 1.0, luma));
    const float amplitude = push.custom.intensity * FILM_GRAIN_AMPLITUDE * response;

    effect_storeToTarget(max(color + grain * amplitude, 0.0), pix);
}
