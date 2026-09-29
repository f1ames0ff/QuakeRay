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

#pragma once

#include <vector>

#include "Common.h"
#include "SamplerManager.h"

namespace qray
{

class TextureDescriptors
{
public:
    explicit TextureDescriptors(VkDevice device, std::shared_ptr<SamplerManager> samplerManager, uint32_t maxTextureCount, uint32_t bindingIndex, uint32_t samplerBindingIndex);
    ~TextureDescriptors();

    TextureDescriptors(const TextureDescriptors &other) = delete;
    TextureDescriptors(TextureDescriptors &&other) noexcept = delete;
    TextureDescriptors &operator=(const TextureDescriptors &other) = delete;
    TextureDescriptors &operator=(TextureDescriptors &&other) noexcept = delete;

    void UpdateTextureDesc(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle);
    void ResetTextureDesc(uint32_t frameIndex, uint32_t textureIndex);
    void ResetAllCache(uint32_t frameIndex);

    void FlushDescWrites();

    VkDescriptorSet GetDescSet(uint32_t frameIndex) const;
    VkDescriptorSetLayout GetDescSetLayout() const;

    void SetEmptyTextureInfo(VkImageView view);

private:
    void CreateDescriptors(uint32_t maxTextureCount);

    bool IsCached(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle);
    void AddToCache(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle);
    void ResetCache(uint32_t frameIndex, uint32_t textureIndex);

private:
    struct UpdatedDescCache
    {
        VkImageView view;
        SamplerManager::Handle samplerHandle;
    };

private:
    VkDevice device;
    std::shared_ptr<SamplerManager> samplerManager;

    uint32_t bindingIndex;
    uint32_t samplerBindingIndex;

    VkDescriptorPool descPool;
    VkDescriptorSetLayout descLayout;
    VkDescriptorSet descSets[MAX_FRAMES_IN_FLIGHT];

    VkImageView      emptyTextureImageView;
    VkImageLayout    emptyTextureImageLayout;

    uint32_t currentWriteCount;
    std::vector<VkDescriptorImageInfo> writeImageInfos;
    std::vector<VkDescriptorImageInfo> writeSamplerInfos;
    std::vector<VkWriteDescriptorSet> writeInfos;

    std::vector<UpdatedDescCache> writeCache[MAX_FRAMES_IN_FLIGHT];
};

}
