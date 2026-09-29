// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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



groupshared float s_Shared[HISTOGRAM_BINS];


float computeSharedSum(float val, const uint linear_idx)
{
    s_Shared[linear_idx] = val;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 64; k >= 1; k /= 2)
    {
        if (linear_idx < k)
        {
            s_Shared[linear_idx] += s_Shared[linear_idx + k];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    val = s_Shared[0];
    GroupMemoryBarrierWithGroupSync();
    return val;
}

float computeSharedMax(float val, const uint linear_idx)
{
    s_Shared[linear_idx] = val;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 64; k >= 1; k /= 2)
    {
        if (linear_idx < k)
        {
            s_Shared[linear_idx] = max(s_Shared[linear_idx], s_Shared[linear_idx + k]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    val = s_Shared[0];
    GroupMemoryBarrierWithGroupSync();
    return val;
}

float computePrefixSum(float val, const uint linear_idx)
{
    s_Shared[linear_idx] = val;
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 1; k < 128; k *= 2)
    {
        const uint block_idx = linear_idx / k;
        if ((block_idx % 2) == 1)
        {
            s_Shared[linear_idx] += s_Shared[k * block_idx - 1];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    val = s_Shared[linear_idx];
    GroupMemoryBarrierWithGroupSync();
    return val;
}

[numthreads(COMPUTE_LUM_HISTOGRAM_BIN_COUNT, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint localInvocationIndex : SV_GroupIndex)
{
    const int2 ipos = int2(dispatchThreadID.xy);
    if (any(ipos >= int2(globalUniform.renderWidth, globalUniform.renderHeight)))
        return;

    const int linear_idx = (int)localInvocationIndex;

    float original_hist = 1.0 + (float)tonemapping[0].histogram[linear_idx] / FIXED_POINT_FRAC_MULTIPLIER;
    const float hist_sum = computeSharedSum(original_hist, linear_idx);
    const float hist_max = computeSharedMax(original_hist, linear_idx);

    tonemapping[0].normalized[linear_idx] = original_hist / hist_max;

    original_hist /= hist_sum;

    const float bin_log_luminance = ((float)linear_idx / (float)HISTOGRAM_BINS) * (max_log_luminance - min_log_luminance) + min_log_luminance;

    const float histogram_cdf = computePrefixSum(original_hist, linear_idx);
    const float histogram_cdf_prev = histogram_cdf - original_hist;

    const float lower_limit = tonemapping[0].tmLowPercentile * 0.01;
    const float upper_limit = tonemapping[0].tmHighPercentile * 0.01;

    float weight_sum = 0.0;
    float bin_sum = 0.0;

    if ((lower_limit <= histogram_cdf) && (histogram_cdf_prev <= upper_limit))
    {
        weight_sum = bin_log_luminance * original_hist;
        bin_sum = original_hist;
    }

    weight_sum = computeSharedSum(weight_sum, linear_idx);
    bin_sum = computeSharedSum(bin_sum, linear_idx);

    float log_target_lum = weight_sum / max(0.0001, bin_sum);
    log_target_lum = clamp(log_target_lum, log2(tonemapping[0].tmMinLuminance), log2(tonemapping[0].tmMaxLuminance));

    if (tonemapping[0].resetCurve == 0)
    {
        float log_old_lum = tonemapping[0].adaptedLuminance;
        if (log_old_lum > 0.0)
        {
            log_old_lum = log2(log_old_lum);
        }

        const float speed = (log_old_lum < log_target_lum) ? tonemapping[0].tmExposureSpeedUp : tonemapping[0].tmExposureSpeedDown;
        log_target_lum = lerp(log_target_lum, log_old_lum, exp(-tonemapping[0].frameTime * speed));
    }

    if (linear_idx == 0)
    {
        const float adapted_luminance = exp2(log_target_lum);
        tonemapping[0].adaptedLuminance = adapted_luminance;
        tonemapping[0].avgLuminance = adapted_luminance;
    }


    if (bin_log_luminance < tonemapping[0].tmNoiseStops)
        original_hist = 0;

    const float r = tonemapping[0].tmDynRangeStops;
    const float delta = (float)(max_log_luminance - min_log_luminance) / HISTOGRAM_BINS;
    const float r_over_delta = r / delta;

    float rcp_hist = (original_hist > 0.0 ? 1.0 / original_hist : 0.0);
    float thresh = 1e-16;
    float sum_recip;
    float len_omega;
    float thresh_passed;
    for (uint i = 0; i < 16; ++i)
    {
        thresh_passed = step(thresh, original_hist);
        len_omega = computeSharedSum(thresh_passed, linear_idx);
        sum_recip = computeSharedSum(rcp_hist * thresh_passed, linear_idx);
        thresh = (len_omega - r_over_delta) / sum_recip;
    }

    thresh_passed = step(thresh, original_hist);
    len_omega = computeSharedSum(thresh_passed, linear_idx);
    sum_recip = computeSharedSum(rcp_hist * thresh_passed, linear_idx);
    float my_slope = (1.0 + rcp_hist * (r_over_delta - len_omega) / sum_recip) * thresh_passed;

    float gaussian_sum = 0.0;
    float weights[14];
    for (int i = 0; i < 14; i++)
    {
        const float kernel_value = exp(-(float)(i * i) / (2.0 * tonemapping[0].tmSlopeBlurSigma * tonemapping[0].tmSlopeBlurSigma));
        gaussian_sum += kernel_value * (i == 0 ? 1.0 : 2.0);
        weights[i] = kernel_value;
    }
    for (int i = 0; i < 14; i++)
    {
        weights[i] /= gaussian_sum;
    }

    s_Shared[linear_idx] = my_slope;
    GroupMemoryBarrierWithGroupSync();
    my_slope *= weights[0];
    for (int dx = -13; dx <= 13; dx++)
    {
        if (dx != 0)
        {
            my_slope += weights[abs(dx)] * s_Shared[clamp(linear_idx + dx, 0, HISTOGRAM_BINS - 1)];
        }
    }

    float my_tonecurve = computePrefixSum(my_slope, linear_idx);
    my_tonecurve = (my_tonecurve - my_slope) * delta - r;

    const float noise_stop_bin = clamp((tonemapping[0].tmNoiseStops * log_luminance_scale + log_luminance_bias) * HISTOGRAM_BINS, 0.0, HISTOGRAM_BINS - 1.0);
    if (linear_idx < noise_stop_bin)
    {
        const float my_tonecurve_at_ns = s_Shared[(int)noise_stop_bin - 1] * delta - r;
        const float bin_log_luminance_at_ns = ((float)(noise_stop_bin - 1) / (float)HISTOGRAM_BINS) * (max_log_luminance - min_log_luminance) + min_log_luminance;
        const float fudge = -(my_tonecurve_at_ns - bin_log_luminance_at_ns) / log_target_lum;

        const float tone_curve_ae = bin_log_luminance - log_target_lum * fudge;
        my_tonecurve = lerp(tone_curve_ae, my_tonecurve, lerp(smoothstep(0.5 * noise_stop_bin, noise_stop_bin, (float)linear_idx), 1.0, tonemapping[0].tmNoiseBlend));
    }

    if (tonemapping[0].resetCurve == 0)
    {
        const float my_old_tonecurve = tonemapping[0].curve[linear_idx];

        const float blend_speed = (my_old_tonecurve < my_tonecurve) ? tonemapping[0].tmExposureSpeedUp : tonemapping[0].tmExposureSpeedDown;

        my_tonecurve = lerp(my_tonecurve, my_old_tonecurve, exp(-tonemapping[0].frameTime * blend_speed));
    }

    tonemapping[0].curve[linear_idx] = my_tonecurve;

    tonemapping[0].histogram[linear_idx] = 0;
}
