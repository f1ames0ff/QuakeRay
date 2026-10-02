// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

// HLSL counterpart of GLSL/RsSmoke.vert, the smoke billboard vertex stage. It is a plain vertex
// shader with an entry point named main; the body is a statement by statement port into the
// already ported headers -- ShaderCommonHLSLFunc.hlsli (globalUniform, getColumn),
// Random.hlsli, Smoke.hlsli, Light.hlsli, Q2ClusterLights.hlsli and SmokeLight.hlsli (the
// ray-query lighting of the puff) -- plus the generated accessor layer those headers pull in.
//
// The GLSL `#extension GL_EXT_ray_query : require` has no HLSL counterpart: the query itself is
// the RayQuery object of SmokeLight.hlsli, and whether dxc may declare SPV_KHR_ray_query /
// RayQueryKHR is a build flag of the two shader tools (they allow the extension for a source
// whose translation unit uses the RayQuery type, see GenerateShaders.py).
//
// Spellings that had to change:
//   * the inputs and outputs move into the entry point signature: an HLSL parameter needs a
//     semantic beside the [[vk::location(n)]] that pins the golden's layout(location = n). The
//     input puffCluster is a vertex attribute, not a varying, so it needs no flatness qualifier;
//     every output keeps the golden's order and name.
//   * gl_Position -> the SV_Position output parameter, and the golden's
//     `rasterizerVertInfo.viewProj * vec4(worldPos, 1.0)` keeps the order of its operands as
//     mul(rasterizerVertInfo.viewProj, float4(worldPos, 1.0)): the push constant matrix is
//     declared with the golden's shape (a square matrix is its own transpose), see the matrix
//     rules in ShaderCommonHLSL.hlsli.
//   * globalUniform.invView[0].xyz / [1].xyz -> getColumn(globalUniform.invView, 0).xyz /
//     getColumn(globalUniform.invView, 1).xyz: a single index of a matrix is a column in GLSL
//     and a row in HLSL, and getColumn reads the golden's column on both.
//   * vec3/vec4 -> float3/float4, vec3(scalar) -> (float3)scalar; the uint parameter and the
//     uint(...) constructor in the body keep their names and meaning.
//
// What did not change: the push constant block RasterizerVert_BT with its single viewProj member
// at offset 0, the whole body's statement order, the billboard arithmetic
// position + (right * texCoord.x + up * texCoord.y) * puffParams.y with its operands in the
// golden's order, every output assignment, and the argument order of the smokeLightAt call.

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
};

[[vk::push_constant]] ConstantBuffer<RasterizerVert_BT> rasterizerVertInfo;

void main(
    [[vk::location(0)]] float3 position    : POSITION0,
    [[vk::location(1)]] float4 color       : COLOR0,
    [[vk::location(2)]] float2 texCoord    : TEXCOORD0,
    [[vk::location(3)]] float3 puffParams  : NORMAL0,
    [[vk::location(4)]] float2 puffLook    : TEXCOORD4,
    [[vk::location(5)]] uint   puffCluster : TEXCOORD5,

    [[vk::location(0)]] out float4 outColor    : COLOR0,
    [[vk::location(1)]] out float2 outCorner   : TEXCOORD1,
    [[vk::location(2)]] out float3 outWorldPos : TEXCOORD2,
    [[vk::location(3)]] out float3 outParams   : TEXCOORD3,
    [[vk::location(4)]] out float2 outLook     : TEXCOORD4,
    [[vk::location(5)]] out float3 outLit      : TEXCOORD5,
    out float4 outPosition : SV_Position)
{
    const float3 right = getColumn(globalUniform.invView, 0).xyz;
    const float3 up    = getColumn(globalUniform.invView, 1).xyz;

    const float3 worldPos = position + (right * texCoord.x + up * texCoord.y) * puffParams.y;

    outColor    = color;
    outCorner   = texCoord;
    outWorldPos = worldPos;
    outParams   = puffParams;
    outLook     = puffLook;
    outLit      = smokeLightAt(worldPos, puffCluster, puffParams.z);

    outPosition = mul(rasterizerVertInfo.viewProj, float4(worldPos, 1.0));
}
