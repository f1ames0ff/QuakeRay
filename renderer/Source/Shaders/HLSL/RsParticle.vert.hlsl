// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#define DESC_SET_FRAMEBUFFERS 4
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TEXTURES       0
#define DESC_SET_LIGHT_SOURCES  6
#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "Smoke.hlsli"
#include "Light.hlsli"
#include "Q2ClusterLights.hlsli"
#include "SmokeLight.hlsli"

struct RasterizerVert_BT
{
    float4x4 viewProj;
    [[vk::offset(104)]] float particleLook[4];
};

[[vk::push_constant]] ConstantBuffer<RasterizerVert_BT> rasterizerVertInfo;

void main(
    [[vk::location(0)]] float3 position        : POSITION0,
    [[vk::location(1)]] float4 color           : COLOR0,
    [[vk::location(2)]] float2 texCoord        : TEXCOORD0,
    [[vk::location(3)]] uint   particleCluster : TEXCOORD3,

    [[vk::location(0)]] out float4 outColor    : COLOR0,
    [[vk::location(1)]] out float2 outTexCoord : TEXCOORD1,
    [[vk::location(2)]] out float3 outLit      : TEXCOORD2,
    out float4 outPosition : SV_Position)
{
    outColor    = color;
    outTexCoord = texCoord;
    outLit      = smokeLightAt(position, particleCluster, 0.0, rasterizerVertInfo.particleLook[1], 0.0);

    outPosition = mul(rasterizerVertInfo.viewProj, float4(position, 1.0));
    {
        const float2 ndc = outPosition.xy / max(outPosition.w, 1e-6);
        const int2 pix = int2((ndc * float2(0.5, -0.5) + 0.5) * float2(globalUniform.renderWidth, globalUniform.renderHeight));
        const int2 cbPix = getCheckerboardPix(pix);
        const float4 glassMask = framebufQ2GlassFilter_Sampled.Load(int3(cbPix, 0));
        if (abs(glassMask.a) >= 4.0)
        {
            const float paneDepth = framebufDepthWorld_Sampled.Load(int3(cbPix, 0)).r;
            if (outPosition.w > paneDepth)
            {
                const float2 e = glassMask.xy;
                float3 paneN = float3(e, 1.0 - abs(e.x) - abs(e.y));
                if (paneN.z < 0.0)
                {
                    paneN.xy = (1.0 - abs(paneN.yx)) * float2(paneN.x >= 0.0 ? 1.0 : -1.0, paneN.y >= 0.0 ? 1.0 : -1.0);
                }
                paneN = normalize(paneN);
                const float3 viewN = mul(globalUniform.view, float4(paneN, 0.0)).xyz;
                const float projScale = globalUniform.projection[1][1] * globalUniform.renderHeight * 0.5;
                float2 offsetPx = viewN.xy * (clamp(glassMask.z, 0.0, 64.0) * 0.35) * projScale / max(outPosition.w, 1.0);
                offsetPx = clamp(offsetPx, -96.0, 96.0);
                outPosition.xy += offsetPx * float2(2.0 / globalUniform.renderWidth, -2.0 / globalUniform.renderHeight) * outPosition.w;
            }
        }
    }
}
