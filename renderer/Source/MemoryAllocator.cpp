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

#include "MemoryAllocator.h"

#include <utility>

#include "Const.h"

using namespace qray;

MemoryAllocator::MemoryAllocator(
    VkInstance _instance,
    VkDevice _device,
    std::shared_ptr<PhysicalDevice> _physDevice)
    : device(_device)
    , physDevice(std::move(_physDevice))
    , allocator(VK_NULL_HANDLE)
    , texturesStagingPool(VK_NULL_HANDLE)
    , texturesFinalPool(VK_NULL_HANDLE)
    , isAmd(false)
{
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.instance = _instance;
    allocatorInfo.device = device;
    allocatorInfo.physicalDevice = physDevice->Get();
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorInfo.frameInUseCount = MAX_FRAMES_IN_FLIGHT;

    allocatorInfo.flags =
        VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT |
        VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;

    VK_CHECKERROR(vmaCreateAllocator(&allocatorInfo, &allocator));

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physDevice->Get(), &properties);
    isAmd = (properties.vendorID == 0x1002);

    if (!isAmd)
    {
        CreateTexturesStagingPool();
        CreateTexturesFinalPool();
    }
}

MemoryAllocator::~MemoryAllocator()
{
    assert(bufAllocs.size() == 0);

    if (texturesStagingPool != VK_NULL_HANDLE)
    {
        vmaDestroyPool(allocator, texturesStagingPool);
    }

    if (texturesFinalPool != VK_NULL_HANDLE)
    {
        vmaDestroyPool(allocator, texturesFinalPool);
    }

    vmaDestroyAllocator(allocator);
}

VkBuffer MemoryAllocator::CreateStagingSrcTextureBuffer(const VkBufferCreateInfo *info, const char *pDebugName, void **pOutMappedData, VkDeviceMemory *outMemory)
{
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    allocInfo.pUserData = const_cast<char *>(pDebugName);

    if (isAmd)
    {
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    }
    else
    {
        allocInfo.pool = texturesStagingPool;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};

    const VkResult r = vmaCreateBuffer(allocator, info, &allocInfo, &buffer, &allocation, &allocationInfo);
    VK_CHECKERROR(r);

    if (r != VK_SUCCESS || buffer == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    bufAllocs[buffer] = allocation;

    if (outMemory != nullptr)
    {
        *outMemory = allocationInfo.deviceMemory;
    }

    *pOutMappedData = allocationInfo.pMappedData;
    return buffer;
}

VkImage MemoryAllocator::CreateDstTextureImage(const VkImageCreateInfo *info, const char *pDebugName, VkDeviceMemory *outMemory)
{
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    allocInfo.pUserData = const_cast<char *>(pDebugName);

    if (isAmd)
    {
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    }
    else
    {
        allocInfo.pool = texturesFinalPool;
    }

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};

    const VkResult r = vmaCreateImage(allocator, info, &allocInfo, &image, &allocation, &allocationInfo);
    VK_CHECKERROR(r);

    if (r != VK_SUCCESS || image == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    imgAllocs[image] = allocation;

    if (outMemory != nullptr)
    {
        *outMemory = allocationInfo.deviceMemory;
    }

    return image;
}

void MemoryAllocator::DestroyStagingSrcTextureBuffer(VkBuffer buffer)
{
    const auto it = bufAllocs.find(buffer);

    if (it == bufAllocs.end())
    {
        assert(0);
        return;
    }

    vmaDestroyBuffer(allocator, buffer, it->second);
    bufAllocs.erase(it);
}

void MemoryAllocator::DestroyTextureImage(VkImage image)
{
    const auto it = imgAllocs.find(image);

    if (it == imgAllocs.end())
    {
        assert(0);
        return;
    }

    vmaDestroyImage(allocator, image, it->second);
    imgAllocs.erase(it);
}

void MemoryAllocator::CreateTexturesStagingPool()
{
    VkBufferCreateInfo prototypeInfo{};
    prototypeInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    prototypeInfo.size = 64;
    prototypeInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo prototypeAllocInfo{};
    prototypeAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    prototypeAllocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    prototypeAllocInfo.pUserData = const_cast<char *>("VMA Image staing pool prototype");

    uint32_t memoryTypeIndex = 0;
    VkResult r = vmaFindMemoryTypeIndexForBufferInfo(allocator, &prototypeInfo, &prototypeAllocInfo, &memoryTypeIndex);
    VK_CHECKERROR(r);

    VmaPoolCreateInfo poolInfo{};
    poolInfo.frameInUseCount = MAX_FRAMES_IN_FLIGHT;
    poolInfo.memoryTypeIndex = memoryTypeIndex;
    poolInfo.blockSize = ALLOCATOR_BLOCK_SIZE_STAGING_TEXTURES;
    poolInfo.flags = VMA_POOL_CREATE_BUDDY_ALGORITHM_BIT;

    r = vmaCreatePool(allocator, &poolInfo, &texturesStagingPool);
    VK_CHECKERROR(r);
}

void MemoryAllocator::CreateTexturesFinalPool()
{
    VkImageCreateInfo prototypeInfo{};
    prototypeInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    prototypeInfo.imageType = VK_IMAGE_TYPE_2D;
    prototypeInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    prototypeInfo.extent = { 1, 1, 1 };
    prototypeInfo.mipLevels = 1;
    prototypeInfo.arrayLayers = 1;
    prototypeInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    prototypeInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    prototypeInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

    VmaAllocationCreateInfo prototypeAllocInfo{};
    prototypeAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    prototypeAllocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    prototypeAllocInfo.pUserData = const_cast<char *>("VMA Image pool prototype");

    uint32_t memoryTypeIndex = 0;
    VkResult r = vmaFindMemoryTypeIndexForImageInfo(allocator, &prototypeInfo, &prototypeAllocInfo, &memoryTypeIndex);
    VK_CHECKERROR(r);

    VmaPoolCreateInfo poolInfo{};
    poolInfo.frameInUseCount = MAX_FRAMES_IN_FLIGHT;
    poolInfo.memoryTypeIndex = memoryTypeIndex;
    poolInfo.blockSize = ALLOCATOR_BLOCK_SIZE_TEXTURES;
    poolInfo.flags = VMA_POOL_CREATE_BUDDY_ALGORITHM_BIT;

    r = vmaCreatePool(allocator, &poolInfo, &texturesFinalPool);
    VK_CHECKERROR(r);
}

VkDevice MemoryAllocator::GetDevice()
{
    return device;
}

VkDeviceMemory MemoryAllocator::AllocDedicated(const VkMemoryRequirements &memReqs, VkMemoryPropertyFlags properties,
                                               AllocType allocType, const char *pDebugName) const
{
    VkMemoryAllocateFlagsInfo allocFlagsInfo{};
    allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = physDevice->GetMemoryTypeIndex(memReqs.memoryTypeBits, properties);

    if (allocType == AllocType::WITH_ADDRESS_QUERY)
    {
        allocInfo.pNext = &allocFlagsInfo;
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VK_CHECKERROR(vkAllocateMemory(device, &allocInfo, nullptr, &memory));

    SET_DEBUG_NAME(device, memory, VK_OBJECT_TYPE_DEVICE_MEMORY, pDebugName);

    return memory;
}

VkDeviceMemory MemoryAllocator::AllocDedicated(const VkMemoryRequirements2 &memReqs2, VkMemoryPropertyFlags properties,
                                               AllocType allocType, const char *pDebugName) const
{
    return AllocDedicated(memReqs2.memoryRequirements, properties, allocType, pDebugName);
}

void MemoryAllocator::FreeDedicated(VkDevice device, VkDeviceMemory memory)
{
    vkFreeMemory(device, memory, nullptr);
}
