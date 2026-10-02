// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#ifndef POST_EFFECTS_HLSLI_
#define POST_EFFECTS_HLSLI_

#include "ShaderCommonHLSLFunc.hlsli"
#include "Exposure.hlsli"

#define POST_EFFECTS_EPSILON 1e-4
#define POST_EFFECTS_MAX_RADIANCE 60000.0

float postEffectsExtractExposure()
{
    return clamp(0.18 / max(tonemapping[0].avgLuminance, 1e-4), 1e-4, 1e4);
}

float3 postEffectsSanitize(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
    {
        return (float3)0.0;
    }

    return clamp(color, (float3)0.0, (float3)POST_EFFECTS_MAX_RADIANCE);
}

float postEffectsSoftKneeWeight(float luminance, float threshold, float knee)
{
    if (knee <= 0.0)
    {
        return max(luminance - threshold, 0.0) / max(luminance, POST_EFFECTS_EPSILON);
    }

    const float k = max(threshold * knee, POST_EFFECTS_EPSILON);
    const float q = clamp(luminance - threshold + k, 0.0, 2.0 * k);
    const float soft = q * q / (4.0 * k + POST_EFFECTS_EPSILON);
    return max(luminance - threshold, soft) / max(luminance, POST_EFFECTS_EPSILON);
}

float3 postEffectsExtract(float3 color, float threshold, float knee)
{
    const float luminance = getLuminance(color);
    const float weight = postEffectsSoftKneeWeight(luminance, threshold, knee);
    return color * weight;
}

float3 postEffectsExtractWithExposure(float3 color, float threshold, float knee)
{
    const float exposure = postEffectsExtractExposure();
    const float3 bright = postEffectsExtract(postEffectsSanitize(color) * exposure, threshold, knee);
    return postEffectsSanitize(bright / exposure);
}

float3 postEffectsDownsampleBox4(Texture2D<float4> source, SamplerState sourceSampler, float2 uv, float2 destinationTexelSize)
{
    const float2 offset = destinationTexelSize * 0.5;

    float3 sum = source.SampleLevel(sourceSampler, uv + float2(-offset.x, -offset.y), 0).rgb * 0.25;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, -offset.y), 0).rgb * 0.25;
    sum += source.SampleLevel(sourceSampler, uv + float2(-offset.x, offset.y), 0).rgb * 0.25;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, offset.y), 0).rgb * 0.25;

    return postEffectsSanitize(sum);
}

float3 postEffectsDownsample13(Texture2D<float4> source, SamplerState sourceSampler, float2 uv, float2 sourceTexelSize)
{
    const float4 offset = sourceTexelSize.xyxy * float4(-1.0, -1.0, 1.0, 1.0);

    float3 sum = source.SampleLevel(sourceSampler, uv + offset.xy, 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + offset.zy, 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + offset.xw, 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + offset.zw, 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, 0.0), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.z, 0.0), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(0.0, offset.y), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(0.0, offset.w), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv, 0).rgb * 0.25;

    return postEffectsSanitize(sum);
}

float3 postEffectsDownsample13Karis(Texture2D<float4> source, SamplerState sourceSampler, float2 uv, float2 sourceTexelSize)
{
    const float3 corner0 = postEffectsSanitize(source.SampleLevel(sourceSampler, uv + float2(-sourceTexelSize.x, -sourceTexelSize.y), 0).rgb);
    const float3 corner1 = postEffectsSanitize(source.SampleLevel(sourceSampler, uv + float2(sourceTexelSize.x, -sourceTexelSize.y), 0).rgb);
    const float3 corner2 = postEffectsSanitize(source.SampleLevel(sourceSampler, uv + float2(-sourceTexelSize.x, sourceTexelSize.y), 0).rgb);
    const float3 corner3 = postEffectsSanitize(source.SampleLevel(sourceSampler, uv + float2(sourceTexelSize.x, sourceTexelSize.y), 0).rgb);
    const float3 center = postEffectsSanitize(source.SampleLevel(sourceSampler, uv, 0).rgb);

    const float weight0 = 1.0 / (1.0 + getLuminance(corner0));
    const float weight1 = 1.0 / (1.0 + getLuminance(corner1));
    const float weight2 = 1.0 / (1.0 + getLuminance(corner2));
    const float weight3 = 1.0 / (1.0 + getLuminance(corner3));
    const float weight4 = 1.0 / (1.0 + getLuminance(center));

    const float denominator = 0.125 * (weight0 + weight1 + weight2 + weight3) + 0.25 * weight4;
    const float3 numerator = 0.125 * (corner0 * weight0 + corner1 * weight1 + corner2 * weight2 + corner3 * weight3) + 0.25 * center * weight4;

    return postEffectsSanitize(numerator / denominator);
}

float3 postEffectsTent9(Texture2D<float4> source, SamplerState sourceSampler, float2 uv, float2 sourceTexelSize)
{
    const float2 offset = sourceTexelSize;

    float3 sum = source.SampleLevel(sourceSampler, uv + float2(-offset.x, -offset.y), 0).rgb * 0.0625;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, -offset.y), 0).rgb * 0.0625;
    sum += source.SampleLevel(sourceSampler, uv + float2(-offset.x, offset.y), 0).rgb * 0.0625;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, offset.y), 0).rgb * 0.0625;
    sum += source.SampleLevel(sourceSampler, uv + float2(-offset.x, 0.0), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(offset.x, 0.0), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(0.0, -offset.y), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv + float2(0.0, offset.y), 0).rgb * 0.125;
    sum += source.SampleLevel(sourceSampler, uv, 0).rgb * 0.25;

    return postEffectsSanitize(sum);
}

#endif
