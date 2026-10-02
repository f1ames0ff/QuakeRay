// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>

// HLSL counterpart of GLSL/RsSmoke.frag, the smoke billboard fragment stage. The body is a
// statement by statement port into the already ported headers -- ShaderCommonHLSLFunc.hlsli
// (getCheckerboardPix, globalUniform, the framebuffers), Exposure.hlsli, Q2Asvgf.hlsli (Q2SH,
// Q2_GRAD_DWN, Q2_STORAGE_SCALE_LF, q2SHToIrradiance), Smoke.hlsli (SmokeLook, SmokeField,
// smokeSampleField, smokeFieldDisplacement) and SmokeShade.hlsli (smokeErosion, smokeDepthFade,
// smokeColor) -- plus the generated accessor layer those headers pull in. The golden's two
// headers are included in its order.
//
// Spellings that had to change:
//   * the inputs and outputs move into the entry point signature, with [[vk::location(n)]]
//     pinning the golden's numbers: the six ins stay at 0..5, outColor is SV_Target0 at 0 and
//     outScreenEmission SV_Target1 at 1. gl_FragCoord becomes the fragCoord SV_Position input
//     parameter, and ivec2(gl_FragCoord.xy) becomes int2(fragCoord.xy): both truncate towards
//     zero, as RsSmoke's fragment coordinates are non-negative.
//   * the push constant block keeps the golden's name and its member offsets: dxc packs a
//     ConstantBuffer on its own rules, and the block has to start where the vertex stage's
//     RasterizerVert_BT ends, so smokeNoise sits at 88 and smokeLook at 104 through
//     [[vk::offset]], each a four-element float array.
//   * SmokeLook(...) on eight values becomes the brace initializer of the same eight values in
//     declaration order: HLSL has no struct constructor (dxc rejects the call), and aggregate
//     initialization is the spelling HLSL has.
//   * mix -> lerp; vec3(0.0) -> (float3)0.0; the multi-argument vector constructors
//     int2(globalUniform.renderWidth, globalUniform.renderHeight) keep their shape, and the
//     single-scalar ivec2(0) / ivec2(1) become (int2)0 / (int2)1, the cast rule of this base.
//   * the three texelFetch(sampler2D(texture, sampler), pix, 0) reads become
//     texture.Load(int3(pix, 0)), the substitution every port of the base makes. The golden
//     builds the combined samplers only because GLSL's texelFetch demands one; the HLSL fetch
//     reads the image and never the sampler state, so the three framebuffer samplers stay
//     declared and unread by the HLSL blob. RhiRasterOverlayPass.cpp:723 relies on exactly that
//     when it serves the images with its linear/clamp sampler. The three descriptor differences
//     are allow-listed, see ShaderPropertiesAllowList.txt.
//   * the #include order is the golden's: ShaderCommonGLSLFunc.h -> ShaderCommonHLSLFunc.hlsli,
//     Exposure.h -> Exposure.hlsli, Q2Asvgf.h -> Q2Asvgf.hlsli, Smoke.h -> Smoke.hlsli,
//     SmokeShade.h -> SmokeShade.hlsli, and the same DESC_SET_* macros are defined before them.
//
// What did not change: the whole body of main with its order of statements -- the age clamp, the
// look built from the push constant, the field sample from inWorldPos / inParams.z /
// globalUniform.time, the radial falloff pow(1.0 - radius, look.edgePower) * look.edgeGain, the
// erosion times lerp(1.0, 0.25 + 0.75 * field.mixed, SMOKE_EDGE_FIELD), the two alpha factors
// (inLook.x * inLook.y and pow(1.0 - age01, SMOKE_LIFE_POWER)), the depth fade, the checked
// checkerboard pixel and the clamp to the LF texture bounds, the SH/COCG loads and their
// / Q2_STORAGE_SCALE_LF, the ambient q2SHToIrradiance(lf, vec3(0.0)) * SMOKE_AMBIENT_GAIN, and
// the two outputs, smokeColor(inColor.rgb, inLit + ambient, alpha) and the zero emission.

#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TONEMAPPING    2
#define DESC_SET_FRAMEBUFFERS   4
#include "ShaderCommonHLSLFunc.hlsli"
#include "Exposure.hlsli"
#include "Q2Asvgf.hlsli"
#include "Smoke.hlsli"
#include "SmokeShade.hlsli"

struct RasterizerSmoke_BT
{
    [[vk::offset(88)]]  float smokeNoise[4];
    [[vk::offset(104)]] float smokeLook[4];
};

[[vk::push_constant]] ConstantBuffer<RasterizerSmoke_BT> rasterizerSmokeInfo;

struct RsSmokeFragOutput
{
    [[vk::location(0)]] float4 outColor          : SV_Target0;
    [[vk::location(1)]] float3 outScreenEmission : SV_Target1;
};

RsSmokeFragOutput main( [[vk::location(0)]] float4 inColor    : TEXCOORD0,
                        [[vk::location(1)]] float2 inCorner   : TEXCOORD1,
                        [[vk::location(2)]] float3 inWorldPos : TEXCOORD2,
                        [[vk::location(3)]] float3 inParams   : TEXCOORD3,
                        [[vk::location(4)]] float2 inLook     : TEXCOORD4,
                        [[vk::location(5)]] float3 inLit      : TEXCOORD5,
                        float4 fragCoord : SV_Position )
{
    const float age01 = clamp(inParams.x, 0.0, 1.0);

    const SmokeLook look = { rasterizerSmokeInfo.smokeNoise[0],
                             rasterizerSmokeInfo.smokeNoise[1],
                             rasterizerSmokeInfo.smokeNoise[2],
                             rasterizerSmokeInfo.smokeNoise[3],
                             rasterizerSmokeInfo.smokeLook[0],
                             rasterizerSmokeInfo.smokeLook[1],
                             rasterizerSmokeInfo.smokeLook[2],
                             rasterizerSmokeInfo.smokeLook[3] };

    const SmokeField field = smokeSampleField(inWorldPos, inParams.z, globalUniform.time, look);

    const float radius = length(inCorner) + smokeFieldDisplacement(field, look);
    const float radial = pow(saturate(1.0 - radius), look.edgePower) * look.edgeGain;

    const float density = smokeErosion(field, age01, look) *
                          lerp(1.0, 0.25 + 0.75 * field.mixed, SMOKE_EDGE_FIELD);

    float alpha = radial * density * inLook.x * inLook.y;
    alpha *= pow(1.0 - age01, SMOKE_LIFE_POWER);
    alpha *= smokeDepthFade(inWorldPos, fragCoord);
    alpha  = saturate(alpha);

    const int2 cbPix = getCheckerboardPix(int2(fragCoord.xy));
    const int2 lfPix = int2(clamp(cbPix / Q2_GRAD_DWN, (int2)0, int2(globalUniform.renderWidth, globalUniform.renderHeight) / Q2_GRAD_DWN - (int2)1));

    Q2SH lf;
    // The framebuffers are declared as a sampled image and a sampler in this port too, so the
    // fetches read the image with Load; the samplers the golden's combined texelFetch needed
    // stay unused here, as in every other port of the base.
    lf.shY = framebufQ2AtrousPingLF_SH_Sampled.Load(int3(lfPix, 0));
    lf.CoCg = framebufQ2AtrousPingLF_COCG_Sampled.Load(int3(lfPix, 0)).xy;
    lf.shY /= Q2_STORAGE_SCALE_LF;
    lf.CoCg /= Q2_STORAGE_SCALE_LF;

    const float3 ambient = q2SHToIrradiance(lf, (float3)0.0) * SMOKE_AMBIENT_GAIN;

    RsSmokeFragOutput o;
    o.outColor = smokeColor(inColor.rgb, inLit + ambient, alpha);
    o.outScreenEmission = (float3)0.0;
    return o;
}
