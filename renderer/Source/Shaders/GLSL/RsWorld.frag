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

#version 460

layout (location = 0) in vec4 vertColor;
layout (location = 1) in vec2 vertTexCoord;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec3 outScreenEmission;

#define DESC_SET_TEXTURES       0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TONEMAPPING    2
#define DESC_SET_VOLUMETRIC     3
#define DESC_SET_FRAMEBUFFERS   4
#include "ShaderCommonGLSLFunc.h"
#include "Exposure.h"
#include "Volumetric.h"

layout(push_constant) uniform RasterizerFrag_BT
{
    layout(offset = 64) vec4 color;
    layout(offset = 80) uint textureIndex;
    layout(offset = 84) uint emissionTextureIndex;
} rasterizerFragInfo;

layout (constant_id = 0) const uint alphaTest = 0;

#define ALPHA_THRESHOLD 0.5
#define EMISSION_CHANNEL 2


void main()
{
    vec4 albedoAlpha = getTextureSample( rasterizerFragInfo.textureIndex, vertTexCoord );
    outColor         = rasterizerFragInfo.color * vertColor * albedoAlpha;

#if ILLUMINATION_VOLUME
    vec4 ndc = vec4( gl_FragCoord.xyz, 1.0 );
    ndc.xy   /= vec2( globalUniform.renderWidth, globalUniform.renderHeight );
    ndc.xy = ndc.xy * 2.0 - 1.0;

    vec4 worldpos = globalUniform.invView * globalUniform.invProjection * ndc;
    worldpos.xyz /= worldpos.w;

    vec3 sp = volume_toSamplePosition_T(
        worldpos.xyz, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );
    vec3 illum = textureLod( sampler3D( g_illuminationVolume_Sampled, g_illuminationVolume_Sampler ), sp, 0.0 ).rgb;

    outColor.rgb *= illum;
#else
    outColor.rgb *= ev100ToLuminousExposure( getCurrentEV100() );
#endif

    float emis            = 0.0;
    uint  emisBlendCode   = 0u;
    if( rasterizerFragInfo.emissionTextureIndex != MATERIAL_NO_TEXTURE )
    {
        const vec4 emisSample = getTextureSample( rasterizerFragInfo.emissionTextureIndex,
                                                  vertTexCoord );
        emis          = emisSample[ EMISSION_CHANNEL ];

        emisBlendCode = uint( emisSample.a * 255.0 + 0.5 );
    }
    outScreenEmission = rmeEmissionToScreenEmission( emis ) * albedoAlpha.rgb;

    if( alphaTest != 0 )
    {
        if( outColor.a < ALPHA_THRESHOLD )
        {
            discard;
        }
    }

    if( emisBlendCode != 0u )
    {
        const ivec2 pix  = getCheckerboardPix( ivec2( gl_FragCoord.xy ) );
        const uvec4 prev = imageLoad( framebufPrimaryToReflRefr, pix );
        imageStore( framebufPrimaryToReflRefr, pix, uvec4( prev.rgb, emisBlendCode ) );
    }
}
