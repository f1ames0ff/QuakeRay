// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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



#ifndef VOLUMETRIC_HLSLI_
#define VOLUMETRIC_HLSLI_
#if !defined( DESC_SET_GLOBAL_UNIFORM ) || !defined( DESC_SET_VOLUMETRIC )
    #error
#endif


#define VOLUMETRIC_DISTANCE_POW 1

float3 volume_getCenter_T( const int3 cell, const float4x4 viewprojInv, const float3 origin )
{
    float3 local =
        ( (float3)cell + 0.5 ) / float3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z );

    float4 ndc = {
        local.x * 2.0 - 1.0,
        local.y * 2.0 - 1.0,
        0.1,
        1.0,
    };

    float4 worldpos = mul( viewprojInv, ndc );
    worldpos.xyz *= safePositiveRcp( worldpos.w );

    float3 worlddir = safeNormalize( worldpos.xyz - origin );

    float n = globalUniform.volumeCameraNear;
    float f = globalUniform.volumeCameraFar;

    float z    = clamp( local.z, 0.0, 1.0 );
    z          = pow( z, VOLUMETRIC_DISTANCE_POW );
    float dist = lerp( n, f, z );

    return origin + worlddir * dist;
}

float3 volume_toSamplePosition_T( const float3 world, const float4x4 viewproj, const float3 origin )
{
    float4 ndc = mul( viewproj, float4( world, 1.0 ) );
    ndc.xy /= ndc.w;

    float n = globalUniform.volumeCameraNear;
    float f = globalUniform.volumeCameraFar;

    float dist = length( world - origin );
    float z    = ( dist - n ) / ( f - n );
    z          = clamp( z, 0.0, 1.0 );
    z          = pow( z, 1.0 / VOLUMETRIC_DISTANCE_POW );

    return float3(
        ndc.x * 0.5 + 0.5,
        ndc.y * 0.5 + 0.5,
        z );
}


float3 volume_getCenter( const int3 cell )
{
    return volume_getCenter_T(
        cell, globalUniform.volumeViewProjInv, globalUniform.cameraPosition.xyz );
}
float3 volume_getCenter_Prev( const int3 prevcell )
{
    return volume_getCenter_T(
        prevcell, globalUniform.volumeViewProjInv_Prev, globalUniform.cameraPositionPrev.xyz );
}


int3 volume_toCellIndex( const float3 samplePosition )
{
    return int3( samplePosition *
                 float3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z ) );
}


float4 volume_sample( const float3 world )
{
    float3 sp = volume_toSamplePosition_T(
        world, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );

    return g_volumetric_Sampled.SampleLevel( g_volumetric_Sampler, sp, 0.0 );
}

float4 volume_sample_Prev( const int3 curcell )
{
    float3 curworld = volume_getCenter( curcell );

    float3 spPrev = volume_toSamplePosition_T(
        curworld, globalUniform.volumeViewProj_Prev, globalUniform.cameraPositionPrev.xyz );

    return g_volumetric_Sampled_Prev.SampleLevel( g_volumetric_Sampler_Prev, spPrev, 0.0 );
}

float4 volume_sampleDithered( const float3 world, const float3 rnd01, float ditherRadius )
{
    float3 sp = volume_toSamplePosition_T(
        world, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );

#if SHIPPING_HACK
    ditherRadius = 0.0;
#endif

    sp += ditherRadius * ( rnd01 * 2 - 1 ) /
          float3( VOLUMETRIC_SIZE_X, VOLUMETRIC_SIZE_Y, VOLUMETRIC_SIZE_Z );

    return g_volumetric_Sampled.SampleLevel( g_volumetric_Sampler, sp, 0.0 );
}

#endif
