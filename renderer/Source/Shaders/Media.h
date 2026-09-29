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

#ifndef MEDIA_H_
#define MEDIA_H_


#ifndef DESC_SET_GLOBAL_UNIFORM
    #error DESC_SET_GLOBAL_UNIFORM must be defined
#endif

#define MEDIA_EXTINCTION_MIN_COLOR 1e-6


float getIndexOfRefraction(uint media)
{
    switch (media)
    {
        case MEDIA_TYPE_WATER:
            return globalUniform.indexOfRefractionWater;
        case MEDIA_TYPE_ACID:
            return globalUniform.indexOfRefractionWater;
        case MEDIA_TYPE_GLASS:
            return globalUniform.indexOfRefractionGlass;
        default:
            return 1.0;
    }
}

vec3 getMediaTransmittance( uint media, float distance )
{
    vec3 extinction = vec3( 0.0 );

    if( media == MEDIA_TYPE_WATER )
    {
        extinction = -log( max( globalUniform.waterColorAndDensity.rgb, vec3( MEDIA_EXTINCTION_MIN_COLOR ) ) );
    }
    else if( media == MEDIA_TYPE_ACID )
    {
        extinction = -log( max( globalUniform.acidColorAndDensity.rgb, vec3( MEDIA_EXTINCTION_MIN_COLOR ) ) );
        extinction *= max(1.0, sqrt( globalUniform.acidColorAndDensity.a ) );
    }

    return exp( -distance * extinction );
}

#if SHIPPING_HACK
vec3 getGlowingMediaFog( uint media, float distance )
{
    if( media != MEDIA_TYPE_ACID )
    {
        return vec3( 0 );
    }

    float density = 0.00005 * globalUniform.acidColorAndDensity.a;

    float fog = exp( -distance * density );
    fog       = clamp( 1.0 - fog, 0.0, 1.0 );

    return fog * globalUniform.acidColorAndDensity.rgb;
}
#endif


bool calcRefractionDirection(float n1, float n2, const vec3 I, const vec3 N, out vec3 T)
{
    float eta = n1 / n2;
    float c1 = -dot(I, N);
    float w = eta * c1;
    float c2m = (w - eta) * (w + eta);

    if (c2m < -1.0f)
    {
        return false;
    }

    T = eta * I + (w - sqrt(1.0f + c2m)) * N;
    return true;
}

uint getMediaTypeFromFlags(uint geometryInstanceFlags)
{
    if ((geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) != 0)
    {
        return MEDIA_TYPE_WATER;
    }
    else if ((geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0)
    {
        return MEDIA_TYPE_GLASS;
    }
    else if ((geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_ACID) != 0)
    {
        return MEDIA_TYPE_ACID;
    }
    else
    {
        return MEDIA_TYPE_VACUUM;
    }
}

bool isPortalFromFlags(uint geometryInstanceFlags)
{
    return (geometryInstanceFlags & GEOM_INST_FLAG_PORTAL) != 0;
}

bool isRefractFromFlags(uint geometryInstanceFlags)
{
    return
        !(globalUniform.forceNoWaterRefraction != 0 && (geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) != 0) &&
        ((geometryInstanceFlags & GEOM_INST_FLAG_REFRACT) != 0);
}

bool isReflectFromFlags(uint geometryInstanceFlags)
{
    return (geometryInstanceFlags & GEOM_INST_FLAG_REFLECT) != 0;
}

#endif
