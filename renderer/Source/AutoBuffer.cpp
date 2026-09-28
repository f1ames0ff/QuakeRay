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

#include "AutoBuffer.h"

#include <utility>

using namespace qray;

namespace
{
    void BarrierAfterCopy(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
    {
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = offset;
        barrier.size = size;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);
    }
}

AutoBuffer::AutoBuffer(std::shared_ptr<MemoryAllocator> allocator)
    : allocator(std::move(allocator))
    , staging{}
    , deviceLocal{}
    , mapped{}
{}

AutoBuffer::AutoBuffer(VkDevice device, std::shared_ptr<MemoryAllocator> allocator)
    : AutoBuffer(std::move(allocator))
{
    (void)device;
}

AutoBuffer::~AutoBuffer()
{
    Destroy();
}

void AutoBuffer::Create(VkDeviceSize size, VkBufferUsageFlags usage, const std::string &debugName, uint32_t frameCount)
{
    assert(frameCount > 0 && frameCount <= MAX_FRAMES_IN_FLIGHT);

    const VkBufferUsageFlags stagingUsage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) |
        (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT));

    const std::string stagingDebugName = debugName + " - staging";

    for (uint32_t i = 0; i < frameCount; i++)
    {
        assert(!staging[i].IsInitted());

        staging[i].Init(
            allocator, size, stagingUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingDebugName.c_str());

        mapped[i] = staging[i].Map();
    }

    assert(!deviceLocal.IsInitted());

    deviceLocal.Init(
        allocator, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        debugName.c_str());
}

void AutoBuffer::Destroy()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (staging[i].IsInitted())
        {
            staging[i].TryUnmap();
            staging[i].Destroy();
        }

        mapped[i] = nullptr;
    }

    deviceLocal.Destroy();
}

void AutoBuffer::CopyFromStaging(VkCommandBuffer cmd, uint32_t frameIndex, VkDeviceSize size, VkDeviceSize offset)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    assert(staging[frameIndex].GetSize() == deviceLocal.GetSize());

    if (size == VK_WHOLE_SIZE)
    {
        size = deviceLocal.GetSize();
    }
    else
    {
        assert(offset + size <= staging[frameIndex].GetSize());
        assert(offset + size <= deviceLocal.GetSize());
    }

    if (size == 0)
    {
        return;
    }

    VkBufferCopy copyInfo{};
    copyInfo.srcOffset = offset;
    copyInfo.dstOffset = offset;
    copyInfo.size = size;

    vkCmdCopyBuffer(
        cmd,
        staging[frameIndex].GetBuffer(), deviceLocal.GetBuffer(),
        1, &copyInfo);

    BarrierAfterCopy(cmd, deviceLocal.GetBuffer(), offset, size);
}

void AutoBuffer::CopyFromStaging(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const VkBufferCopy *copyInfos, uint32_t copyInfosCount)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    assert(staging[frameIndex].GetSize() == deviceLocal.GetSize());

    if (copyInfosCount == 0)
    {
        return;
    }

    vkCmdCopyBuffer(
        cmd,
        staging[frameIndex].GetBuffer(), deviceLocal.GetBuffer(),
        copyInfosCount, copyInfos);

    for (uint32_t i = 0; i < copyInfosCount; i++)
    {
        BarrierAfterCopy(cmd, deviceLocal.GetBuffer(), copyInfos[i].dstOffset, copyInfos[i].size);
    }
}

void *AutoBuffer::GetMapped(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    assert(staging[frameIndex].IsMapped());
    return mapped[frameIndex];
}

VkBuffer AutoBuffer::GetStaging(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    assert(staging[frameIndex].IsInitted());
    return staging[frameIndex].GetBuffer();
}

VkBuffer AutoBuffer::GetDeviceLocal()
{
    assert(deviceLocal.IsInitted());
    return deviceLocal.GetBuffer();
}

VkDeviceAddress AutoBuffer::GetDeviceAddress()
{
    return deviceLocal.GetAddress();
}

VkDeviceSize AutoBuffer::GetSize() const
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        assert(deviceLocal.GetSize() == staging[i].GetSize());
    }

    return deviceLocal.GetSize();
}
