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



#define DESC_SET_TEXTURES       0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TONEMAPPING    2
#define DESC_SET_VOLUMETRIC     3
#define DESC_SET_FRAMEBUFFERS   4
#include "ShaderCommonHLSLFunc.hlsli"
#include "Exposure.hlsli"
#include "Volumetric.hlsli"

struct RasterizerFrag_BT
{
    [[vk::offset(64)]] float4 color;
    [[vk::offset(80)]] uint   textureIndex;
    [[vk::offset(84)]] uint   emissionTextureIndex;
};

[[vk::push_constant]] ConstantBuffer<RasterizerFrag_BT> rasterizerFragInfo;

[[vk::constant_id(0)]] const uint alphaTest = 0;

#define ALPHA_THRESHOLD 0.5
#define EMISSION_CHANNEL 2

struct RsWorldFragOutput
{
    [[vk::location(0)]] float4 outColor          : SV_Target0;
    [[vk::location(1)]] float3 outScreenEmission : SV_Target1;
};

RsWorldFragOutput main( [[vk::location(0)]] float4 vertColor    : TEXCOORD0,
                        [[vk::location(1)]] float2 vertTexCoord : TEXCOORD1,
                        float4 fragCoord : SV_Position )
{
    RsWorldFragOutput o;

    float4 albedoAlpha = getTextureSample( rasterizerFragInfo.textureIndex, vertTexCoord );
    o.outColor         = rasterizerFragInfo.color * vertColor * albedoAlpha;

#if ILLUMINATION_VOLUME
    float4 ndc = float4( fragCoord.xyz, 1.0 );
    ndc.xy   /= float2( globalUniform.renderWidth, globalUniform.renderHeight );
    ndc.xy = ndc.xy * 2.0 - 1.0;

    float4 worldpos = mul( mul( globalUniform.invView, globalUniform.invProjection ), ndc );
    worldpos.xyz /= worldpos.w;

    float3 sp = volume_toSamplePosition_T(
        worldpos.xyz, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );
    float3 illum = g_illuminationVolume_Sampled.SampleLevel(
        g_illuminationVolume_Sampler, sp, 0.0 ).rgb;

    o.outColor.rgb *= illum;
#else
    o.outColor.rgb *= ev100ToLuminousExposure( getCurrentEV100() );
#endif

    float emis            = 0.0;
    uint  emisBlendCode   = 0u;
    if( rasterizerFragInfo.emissionTextureIndex != MATERIAL_NO_TEXTURE )
    {
        const float4 emisSample = getTextureSample( rasterizerFragInfo.emissionTextureIndex,
                                                    vertTexCoord );
        emis          = emisSample[ EMISSION_CHANNEL ];

        emisBlendCode = uint( emisSample.a * 255.0 + 0.5 );
    }
    o.outScreenEmission = rmeEmissionToScreenEmission( emis ) * albedoAlpha.rgb;

    if( alphaTest != 0 )
    {
        if( o.outColor.a < ALPHA_THRESHOLD )
        {
            discard;
        }
    }

    if( emisBlendCode != 0u )
    {
        const int2  pix  = getCheckerboardPix( int2( fragCoord.xy ) );
        const uint4 prev = framebufPrimaryToReflRefr[ pix ];
        framebufPrimaryToReflRefr[ pix ] = uint4( prev.rgb, emisBlendCode );
    }

    return o;
}
