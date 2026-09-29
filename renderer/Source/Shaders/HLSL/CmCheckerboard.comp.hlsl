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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"

bool isLeftOutOfBounds(int checkerboardPix_X)
{
    return
        checkerboardPix_X == 0 ||
        checkerboardPix_X == getCheckerboardSeparatorX();
}

bool isRightOutOfBounds(int checkerboardPix_X)
{
    return
        checkerboardPix_X == getCheckerboardSeparatorX() ||
        checkerboardPix_X == (int)globalUniform.renderWidth;
}

float3 resolveCheckerboard( Texture2D<float4> src, const int2 regularPix, const int2 checkerboardPix, const float3 center )
{
    float3 crossColor =
        0.25 * src.Load(int3(regularPix + int2( -1,  0 ), 0)).rgb * (float)( !isLeftOutOfBounds( checkerboardPix.x ) ) +
        0.25 * src.Load(int3(regularPix + int2(  1,  0 ), 0)).rgb * (float)( !isRightOutOfBounds( checkerboardPix.x ) ) +
        0.25 * src.Load(int3(regularPix + int2(  0,  1 ), 0)).rgb +
        0.25 * src.Load(int3(regularPix + int2(  0, -1 ), 0)).rgb;

    return lerp( center, crossColor, 0.5 );
}

[numthreads(COMPUTE_COMPOSE_GROUP_SIZE_X, COMPUTE_COMPOSE_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix             = int2( dispatchThreadID.x, dispatchThreadID.y );
    const int2 checkerboardPix = getCheckerboardPix( pix );

    if( (uint)pix.x >= (uint)globalUniform.renderWidth || (uint)pix.y >= (uint)globalUniform.renderHeight )
    {
        return;
    }

    float3 hdr  = framebufPreFinal_Sampled.Load(int3( pix, 0 )).rgb;
    float3 emis = framebufScreenEmisRT_Sampled.Load(int3( pix, 0 )).rgb;
    float3 fog  = framebufAcidFogRT_Sampled.Load(int3( pix, 0 )).rgb;

    if( needResolveCheckerboard( checkerboardPix ) )
    {
        emis = resolveCheckerboard( framebufScreenEmisRT_Sampled, pix, checkerboardPix, emis );
        fog  = resolveCheckerboard( framebufAcidFogRT_Sampled, pix, checkerboardPix, fog );
    }

    framebufFinal[pix] = float4( hdr, 0 );
    framebufScreenEmission[pix] = float4( emis, 0 );
    framebufAcidFog[pix] = float4( fog, 0 );
}
