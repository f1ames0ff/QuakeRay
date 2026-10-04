#ifndef NEAR_DOF_HLSLI_
#define NEAR_DOF_HLSLI_

#include "../Generated/ShaderCommonHLSL.hlsli"

int2 nearDofCheckerboardPix(int2 pix, int width)
{
    return int2(((pix.x + pix.y % 2) % 2) * (width / 2) + pix.x / 2, pix.y);
}

bool nearDofIsWeapon(Texture2D<float4> surface, int2 pix, int width)
{
    const uint flags = asuint(surface.Load(int3(nearDofCheckerboardPix(pix, width), 0)).w);
    return (flags & INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON) != 0u;
}

float nearDofRadius(float depth, float strength, float focusDistance, float maxRadius)
{
    if (!isfinite(depth) || depth <= 0.0 || focusDistance <= 0.0)
    {
        return 0.0;
    }

    return saturate(strength) * max(maxRadius, 0.0) * saturate(1.0 - depth / focusDistance);
}

float3 nearDofFilter(Texture2D<float4> source, Texture2D<float4> depthTexture,
                     Texture2D<float4> surface, int2 pix, float axisFactor,
                     float strength, float focusDistance, float maxRadius)
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    pix = clamp(pix, int2(0, 0), int2(width - 1, height - 1));
    const float3 center = source.Load(int3(pix, 0)).rgb;
    if (strength <= 0.0 || !nearDofIsWeapon(surface, pix, int(width)))
    {
        return center;
    }

    const float depth = depthTexture.Load(int3(nearDofCheckerboardPix(pix, int(width)), 0)).r;
    const float radius = nearDofRadius(depth * axisFactor, strength, focusDistance, maxRadius);
    if (radius <= 1e-4)
    {
        return center;
    }

    float3 sum = center;
    float weightSum = 1.0;
    for (uint tap = 0; tap < 16; tap++)
    {
        const float radial = sqrt((float(tap) + 0.5) / 16.0);
        const float angle = float(tap) * 2.39996323;
        const float2 samplePosition = float2(pix) + float2(cos(angle), sin(angle)) * radial * radius;
        const int2 basePix = int2(floor(samplePosition));
        const float2 fraction = frac(samplePosition);
        for (int y = 0; y < 2; y++)
        {
            for (int x = 0; x < 2; x++)
            {
                const int2 samplePix = basePix + int2(x, y);
                if (any(samplePix < 0) || any(samplePix >= int2(width, height)) ||
                    !nearDofIsWeapon(surface, samplePix, int(width)))
                {
                    continue;
                }

                const float sampleDepth = depthTexture.Load(int3(nearDofCheckerboardPix(samplePix, int(width)), 0)).r;
                if (!isfinite(sampleDepth) || sampleDepth <= 0.0 || sampleDepth * axisFactor >= focusDistance)
                {
                    continue;
                }

                const float weight = exp(-2.0 * radial * radial) *
                    (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
                sum += source.Load(int3(samplePix, 0)).rgb * weight;
                weightSum += weight;
            }
        }
    }

    return sum / weightSum;
}

#endif
