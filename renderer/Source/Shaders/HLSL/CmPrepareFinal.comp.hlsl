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
#define DESC_SET_TONEMAPPING 2
#define DESC_SET_LPM_PARAMS 3
#define DESC_SET_VOLUMETRIC 4
#include "ShaderCommonHLSLFunc.hlsli"
#include "Volumetric.hlsli"
#include "Random.hlsli"
#include "Exposure.hlsli"
#include "TonemappingUtils.hlsli"
#include "LocalExposure.hlsli"
#include "ColorCompositing.hlsli"
#include "NearDof.hlsli"


struct PostEffectsControl_BT
{
    float4 opticalControl;
    float4 gameplayFeedback;
    float4 suitControl;
};

[[vk::push_constant]] ConstantBuffer<PostEffectsControl_BT> postEffectsControl;

[[vk::binding(400, DESC_SET_FRAMEBUFFERS)]] Texture2D<float4> bloomResultTexture;
[[vk::binding(402, DESC_SET_FRAMEBUFFERS)]] SamplerState opticalResultSampler;


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


float3 agxDefaultContrastApprox( const float3 x )
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;

    return + 15.5 * x4 * x2
           - 40.14 * x4 * x
           + 31.96 * x4
           - 6.868 * x2 * x
           + 0.4298 * x2
           + 0.1191 * x
           - 0.00232;
}


float3 acesFilmic( const float3 color )
{
    const float3x3 acesIn = float3x3(
        0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777);
    const float3x3 acesOut = float3x3(
         1.60475, -0.53108, -0.07367,
        -0.10208,  1.10813, -0.00605,
        -0.00327, -0.07276,  1.07602);

    const float3 v = mul( acesIn, color / 0.6 );
    const float3 a = v * ( v + 0.0245786 ) - 0.000090537;
    const float3 b = v * ( 0.983729 * v + 0.4329510 ) + 0.238081;

    return saturate( mul( acesOut, a / b ) );
}


float3 agxFilmic( const float3 color )
{
    const float3x3 agxIn = float3x3(
        0.856627153315983,  0.0951212405381588, 0.0482516061458583,
        0.137318972929847,  0.761241990602591,  0.101439036467562,
        0.11189821299995,   0.0767994186031903, 0.811302368396859);
    const float3x3 agxOut = float3x3(
         1.1271005818144368,  -0.11060664309660323,  -0.016493938717834573,
        -0.1413297634984383,   1.157823702216272,    -0.016493938717834257,
        -0.14132976349843826, -0.11060664309660294,   1.2519364065950405);
    const float3x3 rec2020FromSrgb = float3x3(
        0.6274, 0.3293, 0.0433,
        0.0691, 0.9195, 0.0113,
        0.0164, 0.0880, 0.8956);
    const float3x3 srgbFromRec2020 = float3x3(
         1.6605, -0.5876, -0.0728,
        -0.1246,  1.1329, -0.0083,
        -0.0182, -0.1006,  1.1187);

    float3 v = mul( rec2020FromSrgb, color );
    v = mul( agxIn, v );
    v = max( v, 1e-10 );
    v = log2( v );
    v = ( v + 12.47393 ) / 16.5;
    v = clamp( v, 0.0, 1.0 );
    v = agxDefaultContrastApprox( v );
    v = mul( agxOut, v );
    v = pow( max( v, 0.0 ), 2.2 );
    v = mul( srgbFromRec2020, v );

    return saturate( v );
}


float3 finalizeColor( const float3 input_color )
{
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

    const float white_point          = tonemapping[0].tmWhitePoint;
    const float white_point_squared  = white_point * white_point;
    const float exposure_scale       = exp2( tonemapping[0].tmExposureBias - 2.0 ) / tonemapping[0].adaptedLuminance;
    const float3 exposed_color       = input_color * exposure_scale;

    float3 mapped_color;
    const uint tonemap_type = tonemapping[0].tonemapType;

    if( tonemap_type == 0u )
    {
        mapped_color = exposed_color;
    }
    else if( tonemap_type == 2u )
    {
        const float scaled_luminance = max( getLuminance( exposed_color ), 1e-6 );
        const float mapped_luminance = ( scaled_luminance * ( 1.0 + scaled_luminance / white_point_squared ) ) /
                                       ( 1.0 + scaled_luminance );
        mapped_color = exposed_color * ( mapped_luminance / scaled_luminance );
    }
    else if( tonemap_type == 3u )
    {
        mapped_color = acesFilmic( exposed_color );
    }
    else if( tonemap_type == 4u )
    {
        mapped_color = agxFilmic( exposed_color );
    }
    else
    {
        mapped_color = input_color * out_luminance / lum;

        mapped_color = colorHighlightShoulder( mapped_color, tonemapping[0].tmKneeStart,
                                               tonemapping[0].kneeW, tonemapping[0].kneeA, tonemapping[0].kneeB );

        const float adapted_luminance    = tonemapping[0].adaptedLuminance;
        const float scaled_luminance     = exp2( tonemapping[0].tmExposureBias - 2.0 ) * lum / adapted_luminance;
        const float mapped_luminance     = ( scaled_luminance * ( 1.0 + scaled_luminance / white_point_squared ) ) /
                                           ( 1.0 + scaled_luminance );
        const float3 ae_mapped_color     = input_color * mapped_luminance / lum;

        mapped_color = lerp( mapped_color, ae_mapped_color, tonemapping[0].tmReinhard );
    }

    return colorLimitPreserveHue( mapped_color, 1.0 );
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


float3 sampleScene( const int2 pix )
{
    if( postEffectsControl.suitControl.y <= 0.0 )
    {
        return framebufBloomInput_Sampled.Load( int3( pix, 0 ) ).rgb;
    }
    const float displayHeight = globalUniform.upscaledRenderHeight > 0.0 ?
        globalUniform.upscaledRenderHeight : globalUniform.renderHeight;
    const float radiusScale = globalUniform.renderHeight / max( displayHeight, 1.0 );
    return nearDofFilter( framebufBloomInput_Sampled, framebufDepthWorld_Sampled,
                          framebufSurfacePosition_Sampled, pix, getViewAxisFactor( pix ),
                          postEffectsControl.suitControl.y, postEffectsControl.suitControl.z,
                          postEffectsControl.suitControl.w * radiusScale );
}

float3 sampleSceneUV( const float2 uv )
{
    if( postEffectsControl.suitControl.y <= 0.0 )
    {
        return framebufBloomInput_Sampled.SampleLevel( opticalResultSampler, uv, 0.0 ).rgb;
    }
    const int2 pix = clamp( int2( uv * float2( globalUniform.renderWidth, globalUniform.renderHeight ) ),
                            int2( 0, 0 ), int2( globalUniform.renderWidth - 1, globalUniform.renderHeight - 1 ) );
    if( !nearDofIsWeapon( framebufSurfacePosition_Sampled, pix, int( globalUniform.renderWidth ) ) )
    {
        return framebufBloomInput_Sampled.SampleLevel( opticalResultSampler, uv, 0.0 ).rgb;
    }
    return sampleScene( pix );
}

float3 applyChromaticAberration( const int2 pix )
{
    const float3 scene = sampleScene( pix );

    const float damage     = postEffectsControl.gameplayFeedback.x;
    const float liquid     = postEffectsControl.gameplayFeedback.y;
    const float aberration = postEffectsControl.gameplayFeedback.z;
    const float suit       = postEffectsControl.suitControl.x;

    if( damage <= 0.0 && liquid <= 0.0 && suit <= 0.0 )
    {
        return scene;
    }

    const float2 uv = ( float2( pix ) + 0.5 ) /
                      float2( globalUniform.renderWidth, globalUniform.renderHeight );

    const float2 outside = saturate( ( abs( uv - 0.5 ) - 0.15 ) / 0.35 );
    const float edgeMask = smoothstep( 0.0, 1.0, length( outside ) );

    const float damageDistance = length( ( uv - 0.5 ) * 2.0 );
    const float damageMask = smoothstep( 0.8, 1.0, damageDistance );

    const float aspect = globalUniform.renderWidth / globalUniform.renderHeight;
    const float2 centered = float2( ( uv.x - 0.5 ) * aspect, uv.y - 0.5 );
    const float2 direction = normalize( centered + float2( 1e-5, 1e-5 ) );

    float displayHeight = globalUniform.upscaledRenderHeight;
    if( displayHeight <= 0.0 )
    {
        displayHeight = globalUniform.renderHeight;
    }

    const float heightScale = displayHeight / 1080.0;
    const float damageAmount = 270.0 * damage * damageMask;
    const float liquidAmount = ( 180.0 * liquid + 4.0 * suit ) * edgeMask;
    const float splitPixels = min( ( damageAmount + liquidAmount ) * ( aberration / 0.3 ), 200.0 ) * heightScale;
    const float2 offset = direction * ( splitPixels / displayHeight );

    const int tapCount = 7;

    float3 color  = (float3)0.0;
    float3 weight = (float3)0.0;

    for( int i = 0; i < tapCount; i++ )
    {
        const float t = ( i + 0.5 ) / (float)tapCount;
        const float3 w = float3( t, 1.0 - abs( 2.0 * t - 1.0 ), 1.0 - t );
        const float2 sampleUV = clamp( uv + offset * ( t - 0.5 ), (float2)0.0, (float2)1.0 );

        color  += sampleSceneUV( sampleUV ) * w;
        weight += w;
    }

    color /= weight;

    const float redness = saturate( damage * 2.0 ) * damageMask * 0.5;
    color = colorApplyTint( color, float3( 1.0, 0.15, 0.10 ), redness );

    return color;
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

    const float3 scene = applyChromaticAberration( pix );
    float3 hdr = scene;

    if( postEffectsControl.opticalControl.z != 0.0 )
    {
        const float2 opticalUV = (float2( pix ) + 0.5) /
                                 float2( globalUniform.renderWidth, globalUniform.renderHeight );

        const float3 bloom = bloomResultTexture.SampleLevel( opticalResultSampler, opticalUV, 0.0 ).rgb;
        hdr = colorComposeBloom( scene, bloom, postEffectsControl.opticalControl.x,
                                 postEffectsControl.opticalControl.w > 0.0 );
    }

    if (postEffectsControl.gameplayFeedback.w > 0.0)
    {
        const float2 inverseSize = 1.0 / float2(globalUniform.renderWidth, globalUniform.renderHeight);
        const float2 uv = (float2(pix) + 0.5) * inverseSize;
        const float correction = localExposureCorrection(framebufBloomInput_Sampled, opticalResultSampler,
                                                           uv, inverseSize, tonemapping[0].adaptedLuminance);
        hdr *= exp2(correction * postEffectsControl.gameplayFeedback.w);
    }

    float3 color = finalizeColor( hdr );

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

    color = colorLimitPreserveHue( color, 1.0 );
    color = clamp( color + dither * OUTPUT_DITHER_CODES * outputCodeStepLinear( color ), (float3)0.0, (float3)1.0 );

    framebufFinal[pix] = float4( color, 0 );
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
