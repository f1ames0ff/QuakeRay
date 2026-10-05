#ifndef GLASS_BLUR_HLSLI
#define GLASS_BLUR_HLSLI

float3 loadGlassBlurSource(int2 pix, uint axis)
{
    if (axis == 0u)
    {
        return framebufBloomInput.Load(pix).rgb;
    }
    return framebufPreFinal.Load(pix).rgb;
}

float3 blurGlassAxis(int2 pix, float roughness, uint axis)
{
    const float3 center = loadGlassBlurSource(pix, axis);
    const float sigma = min(roughness * 0.008 * float(globalUniform.renderHeight), 64.0 / 3.0);
    if (sigma < 0.25)
    {
        return center;
    }

    const int radius = min(64, int(ceil(3.0 * sigma)));
    const int2 extent = int2(globalUniform.renderWidth, globalUniform.renderHeight) - 1;
    const int2 direction = axis == 0u ? int2(1, 0) : int2(0, 1);
    const float ratioStep = exp(-1.0 / (sigma * sigma));
    float ratio = sqrt(ratioStep);
    float weight = 1.0;
    float3 sum = center;
    float weightSum = 1.0;
    for (int offset = 1; offset <= radius; offset++)
    {
        weight *= ratio;
        ratio *= ratioStep;
        for (int side = -1; side <= 1; side += 2)
        {
            const int2 tap = clamp(pix + direction * offset * side, int2(0, 0), extent);
            const float4 tapGlass = framebufQ2GlassFilter_Sampled.Load(int3(getCheckerboardPix(tap), 0));
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
