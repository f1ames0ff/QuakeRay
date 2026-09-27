// Copyright (c) 2026 QuakeRay contributors

#version 460

layout (location = 0) in vec4 inColor;
layout (location = 1) in vec2 inCorner;
layout (location = 2) in vec3 inWorldPos;
layout (location = 3) in vec3 inParams;
layout (location = 4) in vec2 inLook;
layout (location = 5) in vec3 inLit;

layout (location = 0) out vec4 outColor;
layout (location = 1) out vec3 outScreenEmission;

#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TONEMAPPING    2
#define DESC_SET_FRAMEBUFFERS   4
#include "ShaderCommonGLSLFunc.h"
#include "Exposure.h"
#include "Q2Asvgf.h"
#include "Smoke.h"
#include "SmokeShade.h"

layout(push_constant) uniform RasterizerSmoke_BT
{
    layout(offset = 88)  float smokeNoise[4];
    layout(offset = 104) float smokeLook[4];
} rasterizerSmokeInfo;

void main()
{
    const float age01 = clamp(inParams.x, 0.0, 1.0);

    const SmokeLook look = SmokeLook(rasterizerSmokeInfo.smokeNoise[0],
                                     rasterizerSmokeInfo.smokeNoise[1],
                                     rasterizerSmokeInfo.smokeNoise[2],
                                     rasterizerSmokeInfo.smokeNoise[3],
                                     rasterizerSmokeInfo.smokeLook[0],
                                     rasterizerSmokeInfo.smokeLook[1],
                                     rasterizerSmokeInfo.smokeLook[2],
                                     rasterizerSmokeInfo.smokeLook[3]);

    const SmokeField field = smokeSampleField(inWorldPos, inParams.z, globalUniform.time, look);

    const float radius = length(inCorner) + smokeFieldDisplacement(field, look);
    const float radial = pow(saturate(1.0 - radius), look.edgePower) * look.edgeGain;

    const float density = smokeErosion(field, age01, look) *
                          mix(1.0, 0.25 + 0.75 * field.mixed, SMOKE_EDGE_FIELD);

    float alpha = radial * density * inLook.x * inLook.y;
    alpha *= pow(1.0 - age01, SMOKE_LIFE_POWER);
    alpha *= smokeDepthFade(inWorldPos);
    alpha  = saturate(alpha);

    const ivec2 cbPix = getCheckerboardPix(ivec2(gl_FragCoord.xy));
    const ivec2 lfPix = ivec2(clamp(cbPix / Q2_GRAD_DWN, ivec2(0), ivec2(globalUniform.renderWidth, globalUniform.renderHeight) / Q2_GRAD_DWN - ivec2(1)));

    Q2SH lf;
    // The framebuffer samplers are declared separately here (texture + sampler, the HLSL-port
    // convention), so the combined samplers these fetches need are built explicitly.
    lf.shY = texelFetch(sampler2D(framebufQ2AtrousPingLF_SH_Sampled, framebufQ2AtrousPingLF_SH_Sampler), lfPix, 0);
    lf.CoCg = texelFetch(sampler2D(framebufQ2AtrousPingLF_COCG_Sampled, framebufQ2AtrousPingLF_COCG_Sampler), lfPix, 0).xy;
    lf.shY /= Q2_STORAGE_SCALE_LF;
    lf.CoCg /= Q2_STORAGE_SCALE_LF;

    const vec3 ambient = q2SHToIrradiance(lf, vec3(0.0)) * SMOKE_AMBIENT_GAIN;

    outColor = smokeColor(inColor.rgb, inLit + ambient, alpha);
    outScreenEmission = vec3(0.0);
}
