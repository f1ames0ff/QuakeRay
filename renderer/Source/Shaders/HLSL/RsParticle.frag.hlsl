// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#define DESC_SET_TEXTURES       0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_FRAMEBUFFERS   4
#include "ShaderCommonHLSLFunc.hlsli"
#include "Q2Asvgf.hlsli"
#include "Smoke.hlsli"

struct RasterizerFrag_BT
{
    [[vk::offset(64)]]  float4 color;
    [[vk::offset(80)]]  uint   textureIndex;
    [[vk::offset(104)]] float  particleLook[4];
    [[vk::offset(120)]] uint   particleProxy;
};

[[vk::push_constant]] ConstantBuffer<RasterizerFrag_BT> rasterizerFragInfo;

struct RsParticleFragOutput
{
    [[vk::location(0)]] float4 outColor          : SV_Target0;
    [[vk::location(1)]] float3 outScreenEmission : SV_Target1;
};

RsParticleFragOutput main( [[vk::location(0)]] float4 inColor     : COLOR0,
                           [[vk::location(1)]] float2 inTexCoord  : TEXCOORD0,
                           [[vk::location(2)]] float3 inLit       : TEXCOORD1,
                           [[vk::location(3)]] float  inViewDepth : TEXCOORD3,
                           float4 fragCoord : SV_Position )
{
    const float lightGain  = rasterizerFragInfo.particleLook[2];
    const float lightFloor = rasterizerFragInfo.particleLook[3];
    const bool  debug      = rasterizerFragInfo.particleLook[0] > 0.5;

    const int2 cbPix = getCheckerboardPix(int2(fragCoord.xy));

    /* A sprite whose stand-in the reflect/refract raygen traced is not rasterized behind a pane:
       the traced copy is what the glass shows, and the raster one would be the sharp double over
       it. The mask's blue is the pane's view depth (RaygenPrimary.hlsli) and its alpha's
       magnitude above four marks the normal-map glass mode, the only mode that writes it. Draws
       the proxy list does not describe (FTE strips and the other blend modes) keep their raster
       copy - the per-draw flag the pass sets is false for them. */
    if (rasterizerFragInfo.particleProxy != 0u && globalUniform.glassParticles != 0u)
    {
        const float4 glassMask = framebufQ2GlassFilter_Sampled.Load(int3(cbPix, 0));
        if (abs(glassMask.a) >= 4.0 && inViewDepth > glassMask.z + max(0.01, glassMask.z * 1e-4))
        {
            discard;
        }
    }

    const int2 lfPix = int2(clamp(cbPix / Q2_GRAD_DWN, (int2)0, int2(globalUniform.renderWidth, globalUniform.renderHeight) / Q2_GRAD_DWN - (int2)1));

    Q2SH lf;
    lf.shY = framebufQ2AtrousPingLF_SH_Sampled.Load(int3(lfPix, 0));
    lf.CoCg = framebufQ2AtrousPingLF_COCG_Sampled.Load(int3(lfPix, 0)).xy;
    lf.shY /= Q2_STORAGE_SCALE_LF;
    lf.CoCg /= Q2_STORAGE_SCALE_LF;

    const float3 ambient = q2SHToIrradiance(lf, (float3)0.0) * SMOKE_AMBIENT_GAIN;
    const float3 factor  = lightGain * max(inLit + ambient, (float3)lightFloor);

    const float4 albedo = getTextureSample(rasterizerFragInfo.textureIndex, inTexCoord);

    RsParticleFragOutput o;
    if (debug)
    {
        o.outColor = float4(inLit.r, ambient.r, min(factor.r, 1.0), albedo.a);
        o.outScreenEmission = (float3)0.0;
        return o;
    }

    o.outColor = float4(inColor.rgb * rasterizerFragInfo.color.rgb * factor, 1.0) * albedo;
    o.outScreenEmission = (float3)0.0;
    return o;
}
