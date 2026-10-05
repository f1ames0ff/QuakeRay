#ifndef GLASS_BLUR_INL
#define GLASS_BLUR_INL

vec3 loadGlassBlurSource(ivec2 pix, uint axis)
{
    if (axis == 0u)
    {
        return imageLoad(framebufBloomInput, pix).rgb;
    }
    return imageLoad(framebufPreFinal, pix).rgb;
}

vec3 blurGlassAxis(ivec2 pix, float roughness, uint axis)
{
    const vec3 center = loadGlassBlurSource(pix, axis);
    const float sigma = min(roughness * 0.008 * float(globalUniform.renderHeight), 64.0 / 3.0);
    if (sigma < 0.25)
    {
        return center;
    }

    const int radius = min(64, int(ceil(3.0 * sigma)));
    const ivec2 extent = ivec2(globalUniform.renderWidth, globalUniform.renderHeight) - 1;
    const ivec2 direction = axis == 0u ? ivec2(1, 0) : ivec2(0, 1);
    const float ratioStep = exp(-1.0 / (sigma * sigma));
    float ratio = sqrt(ratioStep);
    float weight = 1.0;
    vec3 sum = center;
    float weightSum = 1.0;
    for (int offset = 1; offset <= radius; offset++)
    {
        for (int side = -1; side <= 1; side += 2)
        {
            const ivec2 tap = clamp(pix + direction * offset * side, ivec2(0), extent);
            const vec4 tapGlass = texelFetch(framebufQ2GlassFilter_Sampled, getCheckerboardPix(tap), 0);
            if (abs(tapGlass.a) < 1.0)
            {
                continue;
            }
            sum += loadGlassBlurSource(tap, axis) * weight;
            weightSum += weight;
        }
    }
    return sum / weightSum;
}

#endif
