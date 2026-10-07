#ifndef GLASS_DENOISE_HLSLI
#define GLASS_DENOISE_HLSLI

bool isRtGlass(float4 mask)
{
    return abs(mask.a) >= 4.0 && abs(mask.a) <= 5.01;
}

float3 rtGlassNormal(float4 mask)
{
    float3 normal = float3(mask.rg, 1.0 - abs(mask.r) - abs(mask.g));
    const float fold = saturate(-normal.z);
    normal.xy += float2(normal.x >= 0.0 ? -fold : fold, normal.y >= 0.0 ? -fold : fold);
    return normalize(normal);
}

float4 accumulateGlass(int2 pix, float4 mask)
{
    const float3 current = framebufBloomInput.Load(pix).rgb;
    if (!isRtGlass(mask) || abs(mask.a) - 4.0 < 0.02)
    {
        return float4(current, 1.0);
    }
    const int2 extent = int2(globalUniform.renderWidth, globalUniform.renderHeight);
    const float4 guide = framebufQ2GlassReflection_Sampled.Load(int3(pix, 0));
    const float2 previous = float2(pix) + guide.xy * float2(extent);
    const int2 base = int2(floor(previous));
    const float2 fraction = frac(previous);
    const float predictedDepth = max(guide.z + guide.w, 0.001);
    const float3 paneNormal = rtGlassNormal(mask);
    float3 history = (float3)0.0;
    float historyLength = 0.0;
    float sum = 0.0;
    for (int y = 0; y < 2; y++)
    {
        for (int x = 0; x < 2; x++)
        {
            const int2 tap = base + int2(x, y);
            if (any(tap < 0) || any(tap >= extent))
            {
                continue;
            }
            const float4 previousMask = framebufQ2GlassFilter_Prev_Sampled.Load(int3(getCheckerboardPix(tap), 0));
            const float4 previousGuide = framebufQ2GlassReflection_Prev_Sampled.Load(int3(tap, 0));
            if (!isRtGlass(previousMask) || abs(abs(mask.a) - abs(previousMask.a)) > 0.03 ||
                mask.b != previousMask.b || dot(paneNormal, rtGlassNormal(previousMask)) < 0.95 ||
                abs(predictedDepth - previousGuide.z) > max(0.5, predictedDepth * 0.02))
            {
                continue;
            }
            const float4 color = framebufQ2GlassHistory_Prev_Sampled.Load(int3(tap, 0));
            if (color.a < 1.0 || !all(isfinite(color)))
            {
                continue;
            }
            const float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                                 (y == 0 ? 1.0 - fraction.y : fraction.y);
            history += color.rgb * weight;
            historyLength += color.a * weight;
            sum += weight;
        }
    }
    if (sum < 0.01)
    {
        return float4(current, 1.0);
    }
    history /= sum;
    const float motion = length(guide.xy * float2(extent));
    const float historyLimit = motion > 0.5 ? 8.0 : 32.0;
    historyLength = min(historyLength / sum + 1.0, historyLimit);
    if (motion > 0.25)
    {
        float3 lower = current;
        float3 upper = current;
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                const int2 tap = clamp(pix + int2(x, y), int2(0, 0), extent - 1);
                const float4 tapMask = framebufQ2GlassFilter_Sampled.Load(int3(getCheckerboardPix(tap), 0));
                if (!isRtGlass(tapMask) || mask.b != tapMask.b || dot(paneNormal, rtGlassNormal(tapMask)) < 0.95)
                {
                    continue;
                }
                const float3 color = framebufBloomInput.Load(tap).rgb;
                lower = min(lower, color);
                upper = max(upper, color);
            }
        }
        const float3 margin = max((upper - lower) * 0.25, (float3)0.02);
        history = clamp(history, lower - margin, upper + margin);
    }
    return float4(lerp(history, current, 1.0 / historyLength), historyLength);
}

float3 spatialGlass(int2 pix, float4 mask)
{
    const float3 center = framebufPreFinal.Load(pix).rgb;
    if (!isRtGlass(mask) || abs(mask.a) - 4.0 < 0.02)
    {
        return center;
    }
    const int2 extent = int2(globalUniform.renderWidth, globalUniform.renderHeight);
    const int2 ipos = getCheckerboardPix(pix);
    const float depth = framebufQ2ViewDepth_Sampled.Load(int3(ipos, 0)).r;
    const float3 normal = texelFetchNormalGeometry(ipos);
    const float depthScale = max(1.0, abs(depth) * 0.05);
    const float3 paneNormal = rtGlassNormal(mask);
    float3 sum = center * 4.0;
    float weightSum = 4.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            const int2 tap = pix + int2(x, y);
            if ((x == 0 && y == 0) || any(tap < 0) || any(tap >= extent))
            {
                continue;
            }
            const int2 tpos = getCheckerboardPix(tap);
            const float4 tapMask = framebufQ2GlassFilter_Sampled.Load(int3(tpos, 0));
            if (!isRtGlass(tapMask) || mask.b != tapMask.b || dot(paneNormal, rtGlassNormal(tapMask)) < 0.95 ||
                abs(abs(mask.a) - abs(tapMask.a)) > 0.03)
            {
                continue;
            }
            const float tapDepth = framebufQ2ViewDepth_Sampled.Load(int3(tpos, 0)).r;
            const float3 tapNormal = texelFetchNormalGeometry(tpos);
            float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            weight *= exp(-abs(depth - tapDepth) / depthScale);
            weight *= abs(depth) > MAX_RAY_LENGTH && abs(tapDepth) > MAX_RAY_LENGTH ?
                1.0 : pow(saturate(dot(normal, tapNormal)), 8.0);
            sum += framebufPreFinal.Load(tap).rgb * weight;
            weightSum += weight;
        }
    }
    return sum / weightSum;
}

#endif
