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

#pragma once

#include <array>

#include "Common.h"

namespace vkpt
{

// The engine's GPU pass timings, a Vulkan timestamp query pool around the legacy renderer's pass
// sequence.
//
// Deferral, recorded with the A5 "RHI diagnostics" increment: the only BeginFrame/Mark call sites are
// inside the legacy VulkanDevice::Render (VulkanDevice.cpp:720-1231), so a frame recorded through the
// RHI layer marks nothing and this class collects nothing. GetFrameStatsEx reports gpuTimingValid = 0
// for such a frame (VulkanDevice.cpp:1804-1813) instead of presenting the untouched, smoothed zeroes
// as a measured 0 ms. Real RHI-pass timings are deliberately not attempted here: NVRHI owns the
// query lifecycle on that path (IDevice::createTimerQuery, ICommandList::beginTimerQuery /
// endTimerQuery, IDevice::pollTimerQuery / getTimerQueryTime), and the mark sites would be the frame
// skeleton's pass sequence, not the legacy one. Nothing in this class changes for the legacy path.

enum GpuPassIndex : uint32_t
{
    GPU_PASS_SETUP = 0,
    GPU_PASS_LIGHTS,
    GPU_PASS_PRIMARY_DECALS,
    GPU_PASS_GODRAYS,
    GPU_PASS_REFL_REFR,
    GPU_PASS_REFL_GODRAYS,
    GPU_PASS_GRADIENT,
    GPU_PASS_DIRECT,
    GPU_PASS_INDIRECT,
    GPU_PASS_DENOISE,
    GPU_PASS_EXPOSURE,
    GPU_PASS_COMPOSITE_RASTER,
    GPU_PASS_UPSCALE,
    GPU_PASS_UPSCALE_BLIT,
    GPU_PASS_SHARPEN,
    GPU_PASS_POST,
    GPU_PASS_PRESENT,
    GPU_PASS_SWAPBLIT,

    GPU_PASS_COUNT
};

constexpr uint32_t GPU_MARK_COUNT = GPU_PASS_COUNT + 1;

class PassTimings
{
public:
    PassTimings(VkDevice device, VkPhysicalDevice physDevice, uint32_t queueFamilyIndex);
    ~PassTimings();

    PassTimings(const PassTimings &other) = delete;
    PassTimings(PassTimings &&other) noexcept = delete;
    PassTimings &operator=(const PassTimings &other) = delete;
    PassTimings &operator=(PassTimings &&other) noexcept = delete;

    bool IsSupported() const;

    void BeginFrame(VkCommandBuffer cmd, uint32_t frameIndex, bool enabled);
    void Mark(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t markIndex);

    float GetPassMs(uint32_t passIndex) const;
    float GetTotalMs() const;

    // True once Mark() has written at least one timestamp since the last successful Fetch (or since
    // construction). False means GetPassMs/GetTotalMs were never fed - VulkanDevice::GetFrameStatsEx
    // reports gpuTimingValid = 0 then, so a frame whose timings were not collected is distinguishable
    // from a measured 0 ms. The counter is consumed by the fetch that reads the recorded marks.
    bool HasRecordedMarks() const;

private:
    void Fetch(uint32_t frameIndex);

private:
    VkDevice device = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;

    bool supported = false;
    bool recording[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t marksWritten[MAX_FRAMES_IN_FLIGHT] = {};

    // Timestamps written since the last successful Fetch; see HasRecordedMarks(). The per-slot
    // marksWritten counters are reset by BeginFrame, so they cannot answer the question across
    // slots.
    uint32_t marksSinceFetch = 0;

    float timestampPeriodNs = 0.0f;
    uint64_t timestampMask = ~uint64_t(0);

    std::array<float, GPU_PASS_COUNT> passMs = {};
    float totalMs = 0.0f;
};

const char *GetGpuPassName(uint32_t passIndex);

}
