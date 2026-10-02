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

#include "Buffer.h"

using namespace qray;

Buffer::Buffer()
    : device(VK_NULL_HANDLE)
    , buffer(VK_NULL_HANDLE)
    , memory(VK_NULL_HANDLE)
    , address(0)
    , size(0)
    , isMapped(false)
{}

Buffer::~Buffer()
{
    Destroy();
}

void Buffer::Init(
    const std::shared_ptr<MemoryAllocator> &allocator,
    VkDeviceSize newSize, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties, const char *debugName)
{
    if (newSize == 0)
    {
        assert(0);
        return;
    }

    device = allocator->GetDevice();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newSize;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECKERROR(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    memory = allocator->AllocDedicated(memRequirements, properties, MemoryAllocator::AllocType::WITH_ADDRESS_QUERY, debugName);

    VK_CHECKERROR(vkBindBufferMemory(device, buffer, memory, 0));

    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        VkBufferDeviceAddressInfoKHR addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = buffer;
        address = vkGetBufferDeviceAddress(device, &addressInfo);
    }

    SET_DEBUG_NAME(device, buffer, VK_OBJECT_TYPE_BUFFER, debugName);

    size = newSize;
}

void Buffer::Destroy()
{
    assert(!isMapped);

    if (device == VK_NULL_HANDLE)
    {
        return;
    }

    if (memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }

    if (buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }

    address = 0;
    size = 0;
}

void *Buffer::Map()
{
    assert(device != VK_NULL_HANDLE);
    assert(!isMapped);
    assert(memory != VK_NULL_HANDLE && size > 0);

    isMapped = true;

    void *mapped = nullptr;
    VK_CHECKERROR(vkMapMemory(device, memory, 0, size, 0, &mapped));

    return mapped;
}

void Buffer::Unmap()
{
    assert(device != VK_NULL_HANDLE);
    assert(isMapped);

    isMapped = false;
    vkUnmapMemory(device, memory);
}

bool Buffer::TryUnmap()
{
    assert(device != VK_NULL_HANDLE);

    if (!isMapped)
    {
        return false;
    }

    Unmap();
    return true;
}

VkBuffer Buffer::GetBuffer() const
{
    assert(buffer != VK_NULL_HANDLE);
    return buffer;
}

VkDeviceMemory Buffer::GetMemory() const
{
    assert(memory != VK_NULL_HANDLE);
    return memory;
}

VkDeviceAddress Buffer::GetAddress() const
{
    assert(address != 0);
    return address;
}

VkDeviceSize Buffer::GetSize() const
{
    assert((buffer != VK_NULL_HANDLE && size != 0) || (buffer == VK_NULL_HANDLE && size == 0));
    return size;
}

bool Buffer::IsMapped() const
{
    return isMapped;
}

bool Buffer::IsInitted() const
{
    return buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE;
}
