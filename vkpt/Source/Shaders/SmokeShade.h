// Copyright (c) 2026 QuakeRay contributors

#ifndef SMOKE_SHADE_H_
#define SMOKE_SHADE_H_

float smokeErosion(const SmokeField f, const float age01, const SmokeLook look)
{
    const float shift = age01 * look.breakup;

    return smoothstep(SMOKE_ERODE_LOW + shift, SMOKE_ERODE_HIGH + shift, f.mixed);
}

float smokeDepthFade(const vec3 worldPos)
{
    const ivec2 cbPix     = getCheckerboardPix(ivec2(gl_FragCoord.xy));
    const float sceneDist = texelFetch(framebufDepthWorld_Sampler, cbPix, 0).r;
    const float dist      = length(worldPos - globalUniform.cameraPosition.xyz);

    return saturate((sceneDist - dist) / SMOKE_DEPTH_FADE);
}

vec4 smokeColor(const vec3 tint, const vec3 lit, const float alpha)
{
    return vec4(tint * lit, 1.0) * alpha;
}

#endif // SMOKE_SHADE_H_
