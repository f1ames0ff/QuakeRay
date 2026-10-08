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
    [[vk::location(3)]] out float  outViewDepth : TEXCOORD3,
    out float4 outPosition : SV_Position)
{
    outColor    = color;
    outTexCoord = texCoord;
    outLit      = smokeLightAt(position, particleCluster, 0.0, rasterizerVertInfo.particleLook[1], 0.0);

    outPosition = mul(rasterizerVertInfo.viewProj, float4(position, 1.0));
    /* The pane-depth test of the fragment needs the sprite's own view depth: the clip-space w is
       the axis depth the glass mask stores (RaygenPrimary.hlsli writes the pane's -view.z), and
       the perspective-correct varying interpolation keeps them comparable per fragment. */
    outViewDepth = outPosition.w;
}
