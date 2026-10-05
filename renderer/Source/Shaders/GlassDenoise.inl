#ifndef GLASS_DENOISE_INL
#define GLASS_DENOISE_INL

bool isRtGlass(vec4 mask)
{
    return abs(mask.a) >= 4.0 && abs(mask.a) <= 5.01;
}

vec3 rtGlassNormal(vec4 mask)
{
    vec3 normal = vec3(mask.rg, 1.0 - abs(mask.r) - abs(mask.g));
    const float fold = clamp(-normal.z, 0.0, 1.0);
    normal.xy += vec2(normal.x >= 0.0 ? -fold : fold, normal.y >= 0.0 ? -fold : fold);
    return normalize(normal);
}

vec4 accumulateGlass(ivec2 pix, vec4 mask)
{
    const vec3 current = imageLoad(framebufBloomInput, pix).rgb;
    if (!isRtGlass(mask) || abs(mask.a) - 4.0 < 0.02)
    {
        return vec4(current, 1.0);
    }
    const ivec2 extent = ivec2(globalUniform.renderWidth, globalUniform.renderHeight);
    const vec4 guide = texelFetch(framebufQ2GlassReflection_Sampled, pix, 0);
    const vec2 previous = vec2(pix) + guide.xy * vec2(extent);
    const ivec2 base = ivec2(floor(previous));
    const vec2 fraction = fract(previous);
    const float predictedDepth = max(guide.z + guide.w, 0.001);
    const vec3 paneNormal = rtGlassNormal(mask);
    vec3 history = vec3(0.0);
    float historyLength = 0.0;
    float sum = 0.0;
    for (int y = 0; y < 2; y++)
    {
        for (int x = 0; x < 2; x++)
        {
            const ivec2 tap = base + ivec2(x, y);
            if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, extent)))
            {
                continue;
            }
            const vec4 previousMask = texelFetch(framebufQ2GlassFilter_Prev_Sampled, getCheckerboardPix(tap), 0);
            const vec4 previousGuide = texelFetch(framebufQ2GlassReflection_Prev_Sampled, tap, 0);
            if (!isRtGlass(previousMask) || abs(abs(mask.a) - abs(previousMask.a)) > 0.03 ||
                mask.b != previousMask.b || dot(paneNormal, rtGlassNormal(previousMask)) < 0.95 ||
                abs(predictedDepth - previousGuide.z) > max(0.5, predictedDepth * 0.02))
            {
                continue;
            }
            const vec4 color = texelFetch(framebufQ2GlassHistory_Prev_Sampled, tap, 0);
            if (color.a < 1.0 || any(isnan(color)) || any(isinf(color)))
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
        return vec4(current, 1.0);
    }
    history /= sum;
    const float motion = length(guide.xy * vec2(extent));
    const float historyLimit = motion > 0.5 ? 8.0 : 32.0;
    historyLength = min(historyLength / sum + 1.0, historyLimit);
    if (motion > 0.25)
    {
        vec3 lower = current;
        vec3 upper = current;
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                const ivec2 tap = clamp(pix + ivec2(x, y), ivec2(0), extent - 1);
                const vec4 tapMask = texelFetch(framebufQ2GlassFilter_Sampled, getCheckerboardPix(tap), 0);
                if (!isRtGlass(tapMask) || mask.b != tapMask.b || dot(paneNormal, rtGlassNormal(tapMask)) < 0.95)
                {
                    continue;
                }
                const vec3 color = imageLoad(framebufBloomInput, tap).rgb;
                lower = min(lower, color);
                upper = max(upper, color);
            }
        }
        const vec3 margin = max((upper - lower) * 0.25, vec3(0.02));
        history = clamp(history, lower - margin, upper + margin);
    }
    return vec4(mix(history, current, 1.0 / historyLength), historyLength);
}

vec3 spatialGlass(ivec2 pix, vec4 mask)
{
    const vec3 center = imageLoad(framebufPreFinal, pix).rgb;
    if (!isRtGlass(mask) || abs(mask.a) - 4.0 < 0.02)
    {
        return center;
    }
    const ivec2 extent = ivec2(globalUniform.renderWidth, globalUniform.renderHeight);
    const ivec2 ipos = getCheckerboardPix(pix);
    const float depth = texelFetch(framebufQ2ViewDepth_Sampled, ipos, 0).r;
    const vec3 normal = texelFetchNormalGeometry(ipos);
    const float depthScale = max(1.0, abs(depth) * 0.05);
    const vec3 paneNormal = rtGlassNormal(mask);
    vec3 sum = center * 4.0;
    float weightSum = 4.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            const ivec2 tap = pix + ivec2(x, y);
            if ((x == 0 && y == 0) || any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, extent)))
            {
                continue;
            }
            const ivec2 tpos = getCheckerboardPix(tap);
            const vec4 tapMask = texelFetch(framebufQ2GlassFilter_Sampled, tpos, 0);
            if (!isRtGlass(tapMask) || mask.b != tapMask.b || dot(paneNormal, rtGlassNormal(tapMask)) < 0.95 ||
                abs(abs(mask.a) - abs(tapMask.a)) > 0.03)
            {
                continue;
            }
            const float tapDepth = texelFetch(framebufQ2ViewDepth_Sampled, tpos, 0).r;
            const vec3 tapNormal = texelFetchNormalGeometry(tpos);
            float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            weight *= exp(-abs(depth - tapDepth) / depthScale);
            weight *= abs(depth) > MAX_RAY_LENGTH && abs(tapDepth) > MAX_RAY_LENGTH ?
                1.0 : pow(clamp(dot(normal, tapNormal), 0.0, 1.0), 8.0);
            sum += imageLoad(framebufPreFinal, tap).rgb * weight;
            weightSum += weight;
        }
    }
    return sum / weightSum;
}

#endif
