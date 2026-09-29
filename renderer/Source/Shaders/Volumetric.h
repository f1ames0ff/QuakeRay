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

#ifndef VOLUMETRIC_H_
#define VOLUMETRIC_H_

#if !defined( DESC_SET_GLOBAL_UNIFORM ) || !defined( DESC_SET_VOLUMETRIC )
    #error
#endif


#define VOLUMETRIC_DISTANCE_POW 1

vec3 volume_getCenter_T( const ivec3 cell, const mat4 viewprojInv, const vec3 origin )
{
    vec3 local =
        ( vec3( cell ) + 0.5 ) / vec3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z );

    vec4 ndc = {
        local.x * 2.0 - 1.0,
        local.y * 2.0 - 1.0,
        0.1,
        1.0,
    };

    vec4 worldpos = viewprojInv * ndc;
    worldpos.xyz *= safePositiveRcp( worldpos.w );

    vec3 worlddir = safeNormalize( worldpos.xyz - origin );

    float n = globalUniform.volumeCameraNear;
    float f = globalUniform.volumeCameraFar;

    float z    = clamp( local.z, 0.0, 1.0 );
    z          = pow( z, VOLUMETRIC_DISTANCE_POW );
    float dist = mix( n, f, z );

    return origin + worlddir * dist;
}

vec3 volume_toSamplePosition_T( const vec3 world, const mat4 viewproj, const vec3 origin )
{
    vec4 ndc = viewproj * vec4( world, 1.0 );
    ndc.xy /= ndc.w;

    float n = globalUniform.volumeCameraNear;
    float f = globalUniform.volumeCameraFar;

    float dist = length( world - origin );
    float z    = ( dist - n ) / ( f - n );
    z          = clamp( z, 0.0, 1.0 );
    z          = pow( z, 1.0 / VOLUMETRIC_DISTANCE_POW );

    return vec3(
        ndc.x * 0.5 + 0.5,
        ndc.y * 0.5 + 0.5,
        z );
}


vec3 volume_getCenter( const ivec3 cell )
{
    return volume_getCenter_T(
        cell, globalUniform.volumeViewProjInv, globalUniform.cameraPosition.xyz );
}
vec3 volume_getCenter_Prev( const ivec3 prevcell )
{
    return volume_getCenter_T(
        prevcell, globalUniform.volumeViewProjInv_Prev, globalUniform.cameraPositionPrev.xyz );
}


ivec3 volume_toCellIndex( const vec3 samplePosition )
{
    return ivec3( samplePosition *
                  vec3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z ) );
}


vec4 volume_sample( const vec3 world )
{
    vec3 sp = volume_toSamplePosition_T(
        world, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );

    return textureLod( sampler3D( g_volumetric_Sampled, g_volumetric_Sampler ), sp, 0.0 );
}

vec4 volume_sample_Prev( const ivec3 curcell )
{
    vec3 curworld = volume_getCenter( curcell );

    vec3 spPrev = volume_toSamplePosition_T(
        curworld, globalUniform.volumeViewProj_Prev, globalUniform.cameraPositionPrev.xyz );

    return textureLod( sampler3D( g_volumetric_Sampled_Prev, g_volumetric_Sampler_Prev ), spPrev, 0.0 );
}

vec4 volume_sampleDithered( const vec3 world, const vec3 rnd01, float ditherRadius )
{
    vec3 sp = volume_toSamplePosition_T(
        world, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );

#if SHIPPING_HACK
    ditherRadius = 0.0;
#endif

    sp += ditherRadius * ( rnd01 * 2 - 1 ) /
          vec3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z );

    return textureLod( sampler3D( g_volumetric_Sampled, g_volumetric_Sampler ), sp, 0.0 );
}


#endif
