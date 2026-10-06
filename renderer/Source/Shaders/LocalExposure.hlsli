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

#ifndef LOCAL_EXPOSURE_HLSLI_
#define LOCAL_EXPOSURE_HLSLI_

float localExposureLogLuminance(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
    {
        return log2(1e-4);
    }
    return log2(max(dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722)), 1e-4));
}

float localExposureCorrection(Texture2D<float4> source, SamplerState sourceSampler,
                              float2 uv, float2 inverseSize, float adaptedLuminance)
{
    const float center = localExposureLogLuminance(source.SampleLevel(sourceSampler, uv, 0.0).rgb);
    const float radius = max(1.0, 0.012 / inverseSize.y);
    float sum = 0.0;
    float totalWeight = 0.0;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            const float2 offset = float2(x, y) * inverseSize * radius;
            const float value = localExposureLogLuminance(source.SampleLevel(sourceSampler, uv + offset, 0.0).rgb);
            const float difference = value - center;
            const float spatial = exp(-0.5 * float(x * x + y * y));
            const float range = exp(-0.5 * difference * difference / (1.5 * 1.5));
            const float weight = spatial * range;
            sum += value * weight;
            totalWeight += weight;
        }
    }

    const float localLuminance = sum / max(totalWeight, 1e-4);
    const float target = log2(max(adaptedLuminance, 1e-4));
    return clamp(target - localLuminance, -1.0, 1.0);
}

#endif
