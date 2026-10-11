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
    [[vk::location(1)]] uint   packedColor     : COLOR0,
    [[vk::location(2)]] float  size            : TEXCOORD0,
    [[vk::location(3)]] uint   particleCluster : TEXCOORD3,
    uint vertexId : SV_VertexID,

    [[vk::location(0)]] out float4 outColor    : COLOR0,
    [[vk::location(1)]] out float2 outTexCoord : TEXCOORD1,
    [[vk::location(2)]] out float3 outLit      : TEXCOORD2,
    [[vk::location(3)]] out float  outViewDepth : TEXCOORD3,
    out float4 outPosition : SV_Position)
{
    const float3 right = getColumn(globalUniform.invView, 0).xyz;
    const float3 up    = getColumn(globalUniform.invView, 1).xyz;

    const uint corner = vertexId % 3u;

    float3 cornerOffset   = (float3)0.0;
    float2 cornerTexCoord = (float2)0.0;

    if (corner == 1u)
    {
        cornerOffset   = up * (size * 1.5);
        cornerTexCoord = float2(1.0, 0.0);
    }
    else if (corner == 2u)
    {
        cornerOffset   = right * (size * 1.5);
        cornerTexCoord = float2(0.0, 1.0);
    }

    const float3 worldPos = position + cornerOffset;

    outColor    = unpackLittleEndianUintColor(packedColor);
    outTexCoord = cornerTexCoord;
    outLit      = smokeLightAt(worldPos, particleCluster, 0.0, rasterizerVertInfo.particleLook[1], 0.0);

    outPosition = mul(rasterizerVertInfo.viewProj, float4(worldPos, 1.0));
    outViewDepth = outPosition.w;
}
