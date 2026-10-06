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

#pragma once

#include "Common.h"
#include "Containers.h"
#include "PhysicalDevice.h"
#include "Vma/vk_mem_alloc.h"

namespace qray
{

class MemoryAllocator
{
public:
    enum class AllocType
    {
        DEFAULT,
        WITH_ADDRESS_QUERY
    };

public:
    explicit MemoryAllocator(
        VkInstance instance,
        VkDevice device,
        std::shared_ptr<PhysicalDevice> physDevice);
    ~MemoryAllocator();

    MemoryAllocator(const MemoryAllocator &other) = delete;
    MemoryAllocator(MemoryAllocator &&other) noexcept = delete;
    MemoryAllocator &operator=(const MemoryAllocator &other) = delete;
    MemoryAllocator &operator=(MemoryAllocator &&other) noexcept = delete;

    VkDevice GetDevice();

    VkDeviceMemory AllocDedicated(const VkMemoryRequirements &memReqs, VkMemoryPropertyFlags properties, AllocType allocType, const char *pDebugName = nullptr) const;
    VkDeviceMemory AllocDedicated(const VkMemoryRequirements2 &memReqs2, VkMemoryPropertyFlags properties, AllocType allocType, const char *pDebugName = nullptr) const;
    static void FreeDedicated(VkDevice device, VkDeviceMemory memory);

    VkBuffer CreateStagingSrcTextureBuffer(
        const VkBufferCreateInfo *info, const char *pDebugName,
        void **pOutMappedData, VkDeviceMemory *outMemory = nullptr);
    VkImage CreateDstTextureImage(
        const VkImageCreateInfo *info, const char *pDebugName,
        VkDeviceMemory *outMemory = nullptr);

    void DestroyStagingSrcTextureBuffer(VkBuffer buffer);
    void DestroyTextureImage(VkImage image);

private:
    void CreateTexturesStagingPool();
    void CreateTexturesFinalPool();

private:
    VkDevice device;
    std::shared_ptr<PhysicalDevice> physDevice;

    VmaAllocator allocator;

    VmaPool texturesStagingPool;
    VmaPool texturesFinalPool;

    bool isAmd;

    rgl::unordered_map<VkBuffer, VmaAllocation> bufAllocs;
    rgl::unordered_map<VkImage, VmaAllocation> imgAllocs;
};

}
