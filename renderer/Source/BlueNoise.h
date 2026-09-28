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

#include "Common.h"
#include "CommandBufferManager.h"
#include "MemoryAllocator.h"
#include "UserFunction.h"

namespace qray
{

class BlueNoise
{
public:
    explicit BlueNoise(
        VkDevice device,
        const char *blueNoiseFilePath,
        std::shared_ptr<MemoryAllocator> allocator,
        const std::shared_ptr<CommandBufferManager> &cmdManager,
        std::shared_ptr<UserFileLoad> userFileLoad);
    ~BlueNoise();

    BlueNoise(const BlueNoise &other) = delete;
    BlueNoise(BlueNoise &&other) noexcept = delete;
    BlueNoise &operator=(const BlueNoise &other) = delete;
    BlueNoise &operator=(BlueNoise &&other) noexcept = delete;

    VkDescriptorSetLayout GetDescSetLayout() const;
    VkDescriptorSet GetDescSet() const;

    VkImage GetImage() const;
    VkImageView GetImageView() const;

    VkFormat GetFormat() const;
    VkExtent2D GetExtent() const;
    uint32_t GetLayerCount() const;

private:
    void CreateDescriptors();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;

    VkImage blueNoiseImages;
    VkImageView blueNoiseImagesView;

    VkDescriptorSetLayout descSetLayout;
    VkDescriptorPool descPool;
    VkDescriptorSet descSet;
};

}
