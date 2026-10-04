#ifndef COLOR_COMPOSITING_HLSLI_
#define COLOR_COMPOSITING_HLSLI_

float colorPeak(float3 color)
{
    return max(max(color.r, color.g), color.b);
}

float3 colorLimitPreserveHue(float3 color, float limit)
{
    if (any(isnan(color)) || any(isinf(color)))
    {
        return (float3)0.0;
    }

    color = max(color, (float3)0.0);
    return color * min(1.0, max(limit, 0.0) / max(colorPeak(color), 1e-6));
}

float3 colorHighlightShoulder(float3 color, float kneeStart, float kneeW, float kneeA, float kneeB)
{
    const float peak = colorPeak(color);
    if (peak <= kneeStart)
    {
        return color;
    }

    const float mappedPeak = max((kneeW * peak + kneeA) / max(peak + kneeB, 1e-6), 0.0);
    return color * (mappedPeak / max(peak, 1e-6));
}

float3 colorComposeBloom(float3 scene, float3 bloom, float bloomStrength, bool thresholdedBloom)
{
    const float3 glare = thresholdedBloom ? bloom : bloom - scene;
    return scene + glare * bloomStrength;
}

float3 colorApplyTint(float3 color, float3 tint, float amount)
{
    return color * lerp((float3)1.0, colorLimitPreserveHue(tint, 1.0), saturate(amount));
}

#endif
