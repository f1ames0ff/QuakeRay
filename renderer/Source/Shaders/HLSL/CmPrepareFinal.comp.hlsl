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
#define DESC_SET_TONEMAPPING 2
#define DESC_SET_LPM_PARAMS 3
#define DESC_SET_VOLUMETRIC 4
#include "ShaderCommonHLSLFunc.hlsli"
#include "Volumetric.hlsli"
#include "Random.hlsli"
#include "Exposure.hlsli"
#include "TonemappingUtils.hlsli"


#define DEBUG_LPM 0

#define A_GPU 1
#define A_HLSL 1
#include "LPM/ffx_a.h"

struct LpmParams_BT
{
    AU4 g_lpmParams[24];
};

[[vk::binding(BINDING_LPM_PARAMS, DESC_SET_LPM_PARAMS)]] ConstantBuffer<LpmParams_BT> lpmParams;

#if DEBUG_LPM
    AU4 debug_lpmParams[24];
    void LpmSetupOut(AU1 i, inAU4 v) { debug_lpmParams[i] = v; }
    AU4  LpmFilterCtl(AU1 i)         { return debug_lpmParams[i]; }
#else
    #define LPM_NO_SETUP 1
    AU4  LpmFilterCtl(AU1 i)         { return lpmParams.g_lpmParams[i]; }
#endif

float3 uncharted2TonemapOp(const float3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;

    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

float3 uncharted2Tonemap(const float3 color, float whitePoint)
{
    return uncharted2TonemapOp(2.0 * color) / uncharted2TonemapOp((float3)whitePoint);
}


float3 reinhard(const float3 c)
{
    const float3 c1 = clamp(c, exp2(globalUniform.minLogLuminance), exp2(globalUniform.maxLogLuminance));
    const float w2 = globalUniform.luminanceWhitePoint * globalUniform.luminanceWhitePoint;
    return c * (1.0 + c1 / w2) / (1.0 + c1);
}


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


float3 finalizeColor( const float3 hdr, const float3 screenEmis, const uint emisBlendMode )
{
    const float strength = clamp( globalUniform.emissionBlendStrength, 0.0, 1.0 );
    const float3 layer   = screenEmis * globalUniform.emissionMaxScreenColor * strength;

    float3 input_color = blendEmissionLayer( hdr, layer, emisBlendMode );

    const float lum = max( getLuminance( input_color ), exp2( min_log_luminance ) );

    const float biased_log_luminance = log2( lum ) * log_luminance_scale + log_luminance_bias;
    const float histogram_bin = clamp( biased_log_luminance * HISTOGRAM_BINS, 0.0, (float)( HISTOGRAM_BINS - 1 ) );
    const uint  left_bin       = (uint)histogram_bin;
    const uint  right_bin      = min( left_bin + 1, (uint)( HISTOGRAM_BINS - 1 ) );
    const float right_weight_F = frac( histogram_bin );
    const float left_weight_F  = 1.0 - right_weight_F;

    const float out_log_luminance = left_weight_F * tonemapping[0].curve[left_bin] +
                                    right_weight_F * tonemapping[0].curve[right_bin];
    const float out_luminance = exp2( out_log_luminance + tonemapping[0].tmExposureBias );

    float3 mapped_color = input_color * out_luminance / lum;

    const float3 step_value = step( tonemapping[0].tmKneeStart, mapped_color );
    mapped_color = lerp( mapped_color,
                         ( tonemapping[0].kneeW * mapped_color + tonemapping[0].kneeA ) / max( (float3)1e-6, mapped_color + tonemapping[0].kneeB ),
                         step_value );

    const float adapted_luminance    = tonemapping[0].adaptedLuminance;
    const float scaled_luminance     = exp2( tonemapping[0].tmExposureBias - 2.0 ) * lum / adapted_luminance;
    const float white_point          = tonemapping[0].tmWhitePoint;
    const float white_point_squared  = white_point * white_point;
    const float mapped_luminance     = ( scaled_luminance * ( 1.0 + scaled_luminance / white_point_squared ) ) / ( 1.0 + scaled_luminance );
    const float3 ae_mapped_color     = input_color * mapped_luminance / lum;

    mapped_color = lerp( mapped_color, ae_mapped_color, tonemapping[0].tmReinhard );

    return clamp( mapped_color, (float3)0, (float3)1 );
}


float3 getBloomInput( const float3 hdr, const float3 screenEmis, const int2 pix )
{
    const float power = getLuminance( screenEmis );

    const float albedoLum = getLuminance( framebufAlbedo_Sampled.Load(int3( pix, 0 )).rgb );
    const float3 emis     = power > 0.001 ? screenEmis / max( albedoLum, power ) : (float3)0;

    float ec = power * globalUniform.bloomEmissionMultiplier;

    return hdr * ev100ToLuminance( getCurrentEV100() ) +
           emis * ev100ToLuminance( getCurrentEV100() + ec );
}


float3 applyVolumetrics( const int2 pix, const float3 color )
{
    if( globalUniform.volumeEnableType == VOLUME_ENABLE_NONE )
    {
        return color;
    }

    if( globalUniform.coreQ2RTX != 0 )
    {
        return color;
    }

    uint seed = getRandomSeed( pix, globalUniform.frameId );
    float3 rnd  = rnd8_4( seed, 0 ).xyz;

    const float2 inUV         = getPixelUVWithJitter( pix );
    const float3 cameraRayDir = getRayDir( inUV );

    float virtualdepth = framebufDepthWorld_Sampled.Load(int3( getCheckerboardPix( pix ), 0 )).r;
    bool  isSky        = virtualdepth > MAX_RAY_LENGTH;

    if( globalUniform.volumeEnableType == VOLUME_ENABLE_VOLUMETRIC )
    {
        float3 position = globalUniform.cameraPosition.xyz + cameraRayDir * virtualdepth;

#ifdef DEBUG_VOLUME_ILLUMINATION
        float3 sp = volume_toSamplePosition_T(
            position.xyz, globalUniform.volumeViewProj, globalUniform.cameraPosition.xyz );
        float3 illum = g_illuminationVolume_Sampled.SampleLevel( g_illuminationVolume_Sampler, sp, 0.0 ).rgb;
        return color * illum;
#endif

        float4 v = volume_sampleDithered( position, rnd, 2.0 );
        return color * v.a + v.rgb;
    }
    else
    {
        float density = globalUniform.volumeScattering * 0.001;

        if( !isSky )
        {
            float f = exp( -virtualdepth * density );
            return lerp( globalUniform.volumeAmbient.rgb, color, f );
        }
        else
        {
            return color;
        }
    }
}


float getViewAxisFactor( const int2 pix )
{
    const float2 inUV = getPixelUVWithJitter( pix ) * 2.0 - 1.0;

    const float4 target   = mul( globalUniform.invProjection, float4( inUV.x, inUV.y, 1.0, 1.0 ) );
    const float3 localDir = abs( target.w ) < 0.001 ? target.xyz : target.xyz / target.w;

    return clamp( -normalize( localDir ).z, 0.0, 1.0 );
}


float3 applyLevelFog( const int2 pix, const float3 color )
{
    const float density  = globalUniform.levelFogColorDensity.w;
    const float skyBlend = globalUniform.levelFogSkyBlend.x;

    if( density <= 0.0 )
    {
        return color;
    }

    const float depth = framebufDepthWorld_Sampled.Load(int3( getCheckerboardPix( pix ), 0 )).r;

    float fog;
    if( depth > MAX_RAY_LENGTH )
    {
        fog = 1.0 - skyBlend;
    }
    else
    {
        const float d = density * max( depth, 0.0 ) * getViewAxisFactor( pix );
        fog = exp( -d * d );
    }

    return lerp( globalUniform.levelFogColorDensity.rgb, color, fog );
}


float3 processDebug( const int2 pix, const float3 fallback );


#define OUTPUT_DITHER_CODES 1.0

float3 outputCodeStepLinear( const float3 linearColor )
{
    float3 l = clamp( linearColor, (float3)1e-5, (float3)1.0 );

    float3 slope = lerp( (float3)12.92,
                         ( 1.055 / 2.4 ) * pow( l, (float3)( 1.0 / 2.4 - 1.0 ) ),
                         l > (float3)0.0031308 );

    return (float3)( 1.0 / 255.0 ) / slope;
}

float interleavedGradientNoise( const float2 p )
{
    const float3 magic = float3( 0.06711056, 0.00583715, 52.9829189 );
    return frac( magic.z * frac( dot( p, magic.xy ) ) );
}

float outputDither( const int2 pix, const uint channel )
{
    const float structured = interleavedGradientNoise( (float2)pix + 0.5 + (float)channel * 31.7 );
    const float white = (float)( murmurHash33( uint3( (uint)pix.x, (uint)pix.y, channel ) ).x & 0xFFFFu ) / (float)UINT16_MAX;

    return structured + white - 1.0;
}


[numthreads(COMPUTE_COMPOSE_GROUP_SIZE_X, COMPUTE_COMPOSE_GROUP_SIZE_Y, 1)]
void main( uint3 dispatchThreadID : SV_DispatchThreadID )
{
    const int2 pix = int2( dispatchThreadID.x, dispatchThreadID.y );
    if( (uint)pix.x >= (uint)globalUniform.renderWidth || (uint)pix.y >= (uint)globalUniform.renderHeight )
    {
        return;
    }

    float3 hdr        = framebufFinal.Load( pix ).rgb;
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

    float3 color = finalizeColor( hdr, screenEmis, emisBlendMode );
    float3 bloom = getBloomInput( hdr, screenEmis, pix );

    color = applyVolumetrics( pix, color );
#if SHIPPING_HACK
    color += framebufAcidFog_Sampled.Load(int3( pix, 0 )).rgb;
#endif

    color = applyLevelFog( pix, color );


    if( globalUniform.debugShowFlags != 0 )
    {
        color = processDebug( pix, color );
    }

    const float3 dither = float3(
        outputDither( pix, 0u ),
        outputDither( pix, 1u ),
        outputDither( pix, 2u ) );

    color = clamp( color + dither * OUTPUT_DITHER_CODES * outputCodeStepLinear( color ), (float3)0.0, (float3)1.0 );

    framebufFinal[pix] = float4( color, 0 );
    framebufBloomInput[pix] = float4( bloom, 0.0 );
}


float3 processDebug(const int2 pix, const float3 fallback)
{
    if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_GOD_RAYS) != 0)
    {
        return framebufGodRaysFiltered_Sampled.Load(int3(pix, 0)).rgb * 4.0;
    }
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_MOTION_VECTORS) != 0)
    {
        const float2 m = framebufMotion_Sampled.Load(int3(getCheckerboardPix(pix), 0)).rg;
        return float3(abs(m.r), abs(m.g), 0);
    }
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_UNFILTERED_DIFFUSE) != 0)
    {
        return texelFetchUnfilteredDirect(getCheckerboardPix(pix));
    }
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR) != 0)
    {
        return texelFetchUnfilteredSpecular(getCheckerboardPix(pix));
    }
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT) != 0)
    {
        const SH sh = imageLoadUnfilteredIndirectSH(getCheckerboardPix(pix));
        const float3 normal = texelFetchNormal(getCheckerboardPix(pix));
        return SHToIrradiance(sh, normal);
    }
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_ALBEDO_WHITE) != 0)
    {
        return framebufAlbedo_Sampled.Load(int3(pix, 0)).rgb;
    }
#if GRADIENT_ESTIMATION_ENABLED
    else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_GRADIENTS) != 0)
    {
        return framebufDISPingGradient_Sampled.Load(int3(getCheckerboardPix(pix) / COMPUTE_ASVGF_STRATA_SIZE, 0)).xyz;
    }
#endif
#ifdef DEBUG_SHOW_SH
    const int2 checkSHRange = int2(800, 400);

    if (pix.x < checkSHRange.x && pix.y < checkSHRange.y)
    {
        float2 uv = float2(pix.x / (float)checkSHRange.x, pix.y / (float)checkSHRange.y);

        float theta = uv.x * 2.0 * M_PI;
        float phi = uv.y * M_PI;
        float3 normal = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));

        int2 centerPix = int2(globalUniform.renderWidth * 0.5, globalUniform.renderHeight * 0.5);
        SH indirSH = texelFetchSH(
            framebufIndirPongSH_R_Sampled, framebufIndirPongSH_G_Sampled, framebufIndirPongSH_B_Sampled,
            getCheckerboardPix(centerPix));

        return SHToIrradiance(indirSH, normal);
    }
#endif

    if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_LUMA) != 0)
    {
        const float3 luma = framebufScreenEmission_Sampled.Load(int3(pix, 0)).rgb;
        const float3 albedo = framebufAlbedo_Sampled.Load(int3(pix, 0)).rgb;
        return luma * 8.0 + albedo * 0.15;
    }

    return fallback;
}
