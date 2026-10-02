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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"

float3 blendEmissionLayer( const float3 hdr, const float3 layer, const uint mode )
{
    if( mode == 0u )
    {
        return hdr;
    }
    if( mode == 2u )
    {
        return hdr + layer;
    }

    const float3 base     = clamp( hdr, (float3)0.0, (float3)1.0 );
    const float3 emiss    = clamp( layer, (float3)0.0, (float3)1.0 );
    const float  coverage = clamp( max( max( layer.x, layer.y ), layer.z ), 0.0, 1.0 );

    float3 blended = emiss;
    if( mode == 3u )
    {
        blended = lerp( 2.0 * base * emiss, 1.0 - 2.0 * ( 1.0 - base ) * ( 1.0 - emiss ), step( (float3)0.5, base ) );
    }
    else if( mode == 4u )
    {
        blended = lerp( 2.0 * base * emiss, 1.0 - 2.0 * ( 1.0 - base ) * ( 1.0 - emiss ), step( (float3)0.5, emiss ) );
    }
    else if( mode == 5u )
    {
        blended = clamp( base / max( (float3)1.0 - emiss, (float3)1e-3 ), (float3)0.0, (float3)1.0 );
    }

    return hdr + ( blended - base ) * coverage + max( layer - 1.0, 0.0 );
}

uint decodeEmissionBlendMode( const uint code )
{
    if( code < 1u || code > 6u )
    {
        return globalUniform.emissionBlendMode;
    }
    return min( code - 1u, 5u );
}

[numthreads(COMPUTE_COMPOSE_GROUP_SIZE_X, COMPUTE_COMPOSE_GROUP_SIZE_Y, 1)]
void main( uint3 dispatchThreadID : SV_DispatchThreadID )
{
    const int2 pix = int2( dispatchThreadID.x, dispatchThreadID.y );
    if( (uint)pix.x >= (uint)globalUniform.renderWidth || (uint)pix.y >= (uint)globalUniform.renderHeight )
    {
        return;
    }

    float3 hdr = framebufFinal.Load( pix ).rgb;
    const float3 screenEmis = framebufScreenEmission_Sampled.Load(int3( pix, 0 )).rgb;
    const uint emisBlendMode = decodeEmissionBlendMode(
        framebufPrimaryToReflRefr_Sampled.Load(int3( getCheckerboardPix( pix ), 0 )).a );

    if (globalUniform.coreQ2RTX != 0)
    {
        hdr += framebufGodRaysFiltered_Sampled.Load(int3( pix, 0 )).rgb;
    }
    else
    {
        const float q2SplitFlag = framebufThroughput_Sampled.Load(int3( getCheckerboardPix(pix), 0 )).a;
        if (q2SplitFlag == 0.0)
        {
            hdr += framebufGodRaysFiltered_Sampled.Load(int3( pix, 0 )).rgb;
        }
    }

    const float strength = clamp( globalUniform.emissionBlendStrength, 0.0, 1.0 );
    const float3 layer   = screenEmis * globalUniform.emissionMaxScreenColor * strength;
    const float3 input   = blendEmissionLayer( hdr, layer, emisBlendMode );

    framebufBloomInput[pix] = float4( input, 0.0 );
}
