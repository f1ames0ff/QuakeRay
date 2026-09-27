// Copyright (c) 2026 QuakeRay contributors

// HLSL counterpart of SmokeShade.h. Like the golden it includes nothing itself: the shader has to
// pull in ShaderCommonHLSLFunc.hlsli first (getCheckerboardPix, framebufDepthWorld_Sampled and
// globalUniform) and Smoke.hlsli (SmokeField, SmokeLook and the SMOKE_ERODE_* / SMOKE_DEPTH_FADE
// constants of the two functions).
//
// Spellings that had to change:
//   * smokeDepthFade takes the fragment coordinate as a parameter: gl_FragCoord is a GLSL global,
//     while in HLSL the fragment position is an SV_Position entry point input that a function
//     cannot read. Its caller (RsSmoke.frag.hlsl) threads its fragCoord through, and the body
//     spells the golden's ivec2(gl_FragCoord.xy) as int2(fragCoord.xy) -- the same truncation,
//     the coordinates of a fragment being non-negative on both sides.
//   * texelFetch(sampler2D(framebufDepthWorld_Sampled, framebufDepthWorld_Sampler), cbPix, 0)
//     becomes framebufDepthWorld_Sampled.Load(int3(cbPix, 0)), the substitution every port of the
//     base makes for a texelFetch (ShaderCommonHLSLFunc.hlsli). The golden builds a combined
//     sampler because GLSL's texelFetch demands one, but the fetch reads the image and not the
//     sampler state -- which is why RhiRasterOverlayPass.cpp can serve the three smoke images
//     with its linear/clamp sampler -- so on this side the separate framebufDepthWorld_Sampler
//     stays declared and unreferenced by the HLSL blob, exactly as the golden's other fetches
//     leave their samplers (CmPrepareFinal, CmQ2Fog, CmGodRays). That descriptor difference of
//     RsSmoke.frag is allow-listed, see ShaderPropertiesAllowList.txt.
//   * vec3/vec4 -> float3/float4; saturate, smoothstep, clamp and length are the same intrinsics
//     under the same names.
//
// What did not change: the names, parameter order and arithmetic of the three functions -- the
// shift = age01 * look.breakup of smokeErosion with its smoothstep(SMOKE_ERODE_LOW + shift,
// SMOKE_ERODE_HIGH + shift, f.mixed), the saturate((sceneDist - dist) / SMOKE_DEPTH_FADE) of
// smokeDepthFade with dist = length(worldPos - globalUniform.cameraPosition.xyz), and the
// vec4(tint * lit, 1.0) * alpha of smokeColor, whose alpha lane the blend discards.

#ifndef SMOKE_SHADE_HLSLI_
#define SMOKE_SHADE_HLSLI_

float smokeErosion(const SmokeField f, const float age01, const SmokeLook look)
{
    const float shift = age01 * look.breakup;

    return smoothstep(SMOKE_ERODE_LOW + shift, SMOKE_ERODE_HIGH + shift, f.mixed);
}

float smokeDepthFade(const float3 worldPos, const float4 fragCoord)
{
    const int2 cbPix     = getCheckerboardPix(int2(fragCoord.xy));
    const float sceneDist = framebufDepthWorld_Sampled.Load(int3(cbPix, 0)).r;
    const float dist      = length(worldPos - globalUniform.cameraPosition.xyz);

    return saturate((sceneDist - dist) / SMOKE_DEPTH_FADE);
}

float4 smokeColor(const float3 tint, const float3 lit, const float alpha)
{
    return float4(tint * lit, 1.0) * alpha;
}

#endif // SMOKE_SHADE_HLSLI_
