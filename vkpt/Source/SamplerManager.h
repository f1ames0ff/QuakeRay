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

#include <vector>

#include "Common.h"
#include "Containers.h"
#include "vkpt/vkpt.h"

namespace vkpt::rhi
{
    class RhiTextureTable;
}

namespace vkpt
{

class SamplerManager
{
public:
    class Handle
    {
        friend class SamplerManager;

    public:
        explicit Handle();
        explicit Handle(RgSamplerFilter filter, RgSamplerAddressMode addressModeU, RgSamplerAddressMode addressModeV, RgMaterialCreateFlags flags);

        bool operator==(const Handle &other) const
        {
            return other.internalIndex == internalIndex;
        }

        uint32_t GetIndex() const { return internalIndex; }

        bool SetIfHasDynamicSamplerFilter(RgSamplerFilter newDynamicSamplerFilter);

    private:
        uint32_t internalIndex;
        bool hasDynamicSamplerFilter;
    };

public:
    SamplerManager(VkDevice device, uint32_t anisotropy, bool forceMinificationFilterLinear,
                   rhi::RhiTextureTable *pRhiTextureTable = nullptr);
    ~SamplerManager();

    SamplerManager(const SamplerManager &other) = delete;
    SamplerManager(SamplerManager &&other) noexcept = delete;
    SamplerManager &operator=(const SamplerManager &other) = delete;
    SamplerManager &operator=(SamplerManager &&other) noexcept = delete;

    void PrepareForFrame(uint32_t frameIndex);

    VkSampler GetSampler(
        RgSamplerFilter filter,
        RgSamplerAddressMode addressModeU,
        RgSamplerAddressMode addressModeV,
        bool forceLowestMip = false) const;

    VkSampler GetSampler(const Handle &handle) const;

    bool TryChangeMipLodBias(uint32_t frameIndex, float newMipLodBias);

    void SetRhiTextureTable(rhi::RhiTextureTable *pTable);

private:
    void CreateAllSamplers(uint32_t anisotropy, float mipLodBias);
    void AddAllSamplersToDestroy(uint32_t frameIndex);

    static uint32_t ToIndex(
        RgSamplerFilter filter,
        RgSamplerAddressMode addressModeU,
        RgSamplerAddressMode addressModeV,
        bool forceLowestMip);

    static uint32_t ToIndex(
        VkFilter filter,
        VkSamplerAddressMode addressModeU,
        VkSamplerAddressMode addressModeV,
        bool forceLowestMip);

private:
    VkDevice device;

    rgl::unordered_map<uint32_t, VkSampler> samplers;
    std::vector<VkSampler> samplersToDelete[MAX_FRAMES_IN_FLIGHT];
    float mipLodBias;
    uint32_t anisotropy;
    bool forceMinificationFilterLinear;

    rhi::RhiTextureTable *rhiTextureTable = nullptr;
};

}
