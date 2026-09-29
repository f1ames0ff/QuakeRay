// Copyright (c) 2026 QuakeRay contributors
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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TONEMAPPING 2
#define TONEMAPPING_BUFFER_WRITEABLE
#include "ShaderCommonHLSLFunc.hlsli"
#include "TonemappingUtils.hlsli"


groupshared uint s_Histogram[HISTOGRAM_BINS];

[numthreads(COMPUTE_LUM_HISTOGRAM_GROUP_SIZE_X, COMPUTE_LUM_HISTOGRAM_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint linear_idx : SV_GroupIndex)
{
    const int2 ipos = int2(dispatchThreadID.xy);
    const int2 screenSize = int2(globalUniform.renderWidth, globalUniform.renderHeight);

    const bool validThread = !any(ipos >= screenSize);

    const float3 input_color = validThread ? framebufPreFinal_Sampled.Load(int3(ipos, 0)).rgb : (float3)0.0;

    if (validThread && linear_idx < HISTOGRAM_BINS)
    {
        s_Histogram[linear_idx] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    if (validThread && getLuminance(input_color) > 0.0)
    {
        const float lum = max(getLuminance(input_color), exp2(min_log_luminance));
        const float biased_log_luminance = log2(lum) * log_luminance_scale + log_luminance_bias;
        const float histogram_bin = clamp(biased_log_luminance * HISTOGRAM_BINS, 0.0, HISTOGRAM_BINS - 1.0);

        const uint left_bin = (uint)histogram_bin;
        const uint right_bin = left_bin + 1;

        const float weight = clamp(1.0 - length((float2)ipos / (float2)screenSize - (float2)0.5) * 1.5, 0.01, 1.0);

        const float right_weight_F = frac(histogram_bin) * weight;
        const float left_weight_F = weight - right_weight_F;

        const uint right_weight_U = (uint)(right_weight_F * FIXED_POINT_FRAC_MULTIPLIER);
        const uint left_weight_U = (uint)(left_weight_F * FIXED_POINT_FRAC_MULTIPLIER);

        InterlockedAdd(s_Histogram[left_bin], left_weight_U);
        if (right_bin < HISTOGRAM_BINS)
        {
            InterlockedAdd(s_Histogram[right_bin], right_weight_U);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (validThread && linear_idx < HISTOGRAM_BINS)
    {
        const int localBinValue = (int)s_Histogram[linear_idx];
        if (localBinValue != 0)
        {
            InterlockedAdd(tonemapping[0].histogram[linear_idx], localBinValue);
        }
    }
}
