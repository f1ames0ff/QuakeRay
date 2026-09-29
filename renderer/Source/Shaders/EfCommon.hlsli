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



#if !defined(EFFECT_SOURCE_IS_PING) && !defined(EFFECT_SOURCE_IS_PONG)
    #error Define EFFECT_SOURCE_IS_PING or EFFECT_SOURCE_IS_PONG to boolean value
#endif


int2 effect_getFramebufSize()
{
    uint w, h;
    framebufUpscaledPing.GetDimensions(w, h);
    return int2(w, h);
}


float2 effect_getInverseFramebufSize()
{
    int2 sz = effect_getFramebufSize();
    return float2(1.0 / float(sz.x), 1.0 / float(sz.y));
}


int2 effect_clampPix(int2 pix)
{
    return clamp(pix, int2(0, 0), effect_getFramebufSize() - 1);
}


bool effect_isPixValid(int2 pix)
{
    return all(pix == effect_clampPix(pix));
}


float2 effect_getFramebufUV(int2 pix)
{
    return (float2(pix) + 0.5) * effect_getInverseFramebufSize();
}


float2 effect_getCenteredFromPix(int2 pix)
{
    return effect_getFramebufUV(pix) * 2.0 - 1.0;
}


int2 effect_getPixFromCentered(float2 centered)
{
    return int2((centered * 0.5 + 0.5) * float2(effect_getFramebufSize()));
}


float3 effect_loadFromSource(int2 pix)
{
    pix = effect_clampPix(pix);

    if (EFFECT_SOURCE_IS_PING)
    {
        return framebufUpscaledPing[pix].rgb;
    }
    else
    {
        return framebufUpscaledPong[pix].rgb;
    }
}


void effect_storeToTarget(const float3 value, int2 pix)
{
    pix = effect_clampPix(pix);

    if (EFFECT_SOURCE_IS_PING)
    {
        framebufUpscaledPong[pix] = float4(value, 0.0);
    }
    else
    {
        framebufUpscaledPing[pix] = float4(value, 0.0);
    }
}


void effect_storeUnmodifiedToTarget(int2 pix)
{
    effect_storeToTarget(effect_loadFromSource(pix), pix);
}


float3 effect_loadFromSource_Centered(float2 centered)
{
    return effect_loadFromSource(effect_getPixFromCentered(centered));
}


#ifdef DESC_SET_RANDOM
#include "Random.hlsli"
float effect_getRandomSample(int2 pix, uint frameIndex)
{
    return rnd16(getRandomSeed(pix, frameIndex), RANDOM_SALT_POSTEFFECT);
}
#endif

#define I_LIMIT 0.6
#define Q_LIMIT 0.55
float3 encodeYiqForStorage(float3 yiq)
{
    float i = clamp(yiq.y, -I_LIMIT, I_LIMIT);
    float q = clamp(yiq.z, -Q_LIMIT, Q_LIMIT);

    i += I_LIMIT;
    q += Q_LIMIT;

    i /= I_LIMIT * 2;
    q /= Q_LIMIT * 2;

    return float3(yiq.x, i, q);
}
float3 decodeYiqFromStorage(float3 yiqFromStorage)
{
    float i = clamp(yiqFromStorage.y, 0.0, 1.0);
    float q = clamp(yiqFromStorage.z, 0.0, 1.0);

    i *= I_LIMIT * 2;
    q *= Q_LIMIT * 2;

    i -= I_LIMIT;
    q -= Q_LIMIT;

    return float3(yiqFromStorage.x, i, q);
}
#undef I_LIMIT
#undef Q_LIMIT
