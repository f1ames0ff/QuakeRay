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


ivec2 effect_getFramebufSize()
{
    return imageSize(framebufUpscaledPing);
}


vec2 effect_getInverseFramebufSize()
{
    ivec2 sz = effect_getFramebufSize();
    return vec2(1.0 / float(sz.x), 1.0 / float(sz.y));
}


ivec2 effect_clampPix(ivec2 pix)
{
    return clamp(pix, ivec2(0), effect_getFramebufSize() - 1);
}


bool effect_isPixValid(ivec2 pix)
{
    return pix == effect_clampPix(pix);
}


vec2 effect_getFramebufUV(ivec2 pix)
{
    return (vec2(pix) + 0.5) * effect_getInverseFramebufSize();
}


vec2 effect_getCenteredFromPix(ivec2 pix)
{
    return effect_getFramebufUV(pix) * 2.0 - 1.0;
}


ivec2 effect_getPixFromCentered(vec2 centered)
{
    return ivec2((centered * 0.5 + 0.5) * effect_getFramebufSize());
}


vec3 effect_loadFromSource(ivec2 pix)
{
    pix = effect_clampPix(pix);

    if (EFFECT_SOURCE_IS_PING)
    {
        return imageLoad(framebufUpscaledPing, pix).rgb;
    }
    else
    {
        return imageLoad(framebufUpscaledPong, pix).rgb;
    }
}


void effect_storeToTarget(const vec3 value, ivec2 pix)
{
    pix = effect_clampPix(pix);

    if (EFFECT_SOURCE_IS_PING)
    {
        imageStore(framebufUpscaledPong, pix, vec4(value, 0.0));
    }
    else
    {
        imageStore(framebufUpscaledPing, pix, vec4(value, 0.0));
    }
}


void effect_storeUnmodifiedToTarget(ivec2 pix)
{
    effect_storeToTarget(effect_loadFromSource(pix), pix);
}


vec3 effect_loadFromSource_Centered(vec2 centered)
{
    return effect_loadFromSource(effect_getPixFromCentered(centered));
}


#ifdef DESC_SET_RANDOM
#include "Random.h"
float effect_getRandomSample(ivec2 pix, uint frameIndex)
{
    return rnd16(getRandomSeed(pix, frameIndex), RANDOM_SALT_POSTEFFECT);
}
#endif


#define I_LIMIT 0.6
#define Q_LIMIT 0.55
vec3 encodeYiqForStorage(vec3 yiq)
{
    float i = clamp(yiq.y, -I_LIMIT, I_LIMIT);
    float q = clamp(yiq.z, -Q_LIMIT, Q_LIMIT);

    i += I_LIMIT;
    q += Q_LIMIT;

    i /= I_LIMIT * 2;
    q /= Q_LIMIT * 2;

    return vec3(yiq.x, i, q);
}
vec3 decodeYiqFromStorage(vec3 yiqFromStorage)
{
    float i = clamp(yiqFromStorage.y, 0, 1);
    float q = clamp(yiqFromStorage.z, 0, 1);

    i *= I_LIMIT * 2;
    q *= Q_LIMIT * 2;

    i -= I_LIMIT;
    q -= Q_LIMIT;

    return vec3(yiqFromStorage.x, i, q);
}
#undef I_LIMIT
#undef Q_LIMIT
