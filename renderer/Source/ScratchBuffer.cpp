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

#include "ScratchBuffer.h"

#include <algorithm>

#include "Utils.h"

using namespace qray;

constexpr VkDeviceSize SCRATCH_CHUNK_BUFFER_SIZE = (1 << 24);

ScratchBuffer::ScratchBuffer(std::shared_ptr<MemoryAllocator> _allocator, uint32_t _alignment)
:
    allocator(_allocator),
    alignment(_alignment)
{
    AddChunk(SCRATCH_CHUNK_BUFFER_SIZE);
}

VkDeviceAddress ScratchBuffer::GetScratchAddress(VkDeviceSize scratchSize)
{
    const VkDeviceSize alignedSize = Utils::Align(scratchSize, (VkDeviceSize)alignment);

    for (auto &chunk : chunks)
    {
        const VkDeviceSize remaining = chunk.buffer.GetSize() - chunk.currentOffset;

        if (alignedSize < remaining)
        {
            VkDeviceAddress address = chunk.buffer.GetAddress() + chunk.currentOffset;

            chunk.currentOffset += alignedSize;
            return address;
        }
    }

    AddChunk(std::max(SCRATCH_CHUNK_BUFFER_SIZE, alignedSize));

    return chunks.back().buffer.GetAddress();
}

void ScratchBuffer::Reset()
{
    for (auto &chunk : chunks)
    {
        chunk.currentOffset = 0;
    }
}

void ScratchBuffer::AddChunk(VkDeviceSize size)
{
    const auto alloc = allocator.lock();

    if (alloc == nullptr)
    {
        return;
    }

    chunks.emplace_back();

    ChunkBuffer &chunk = chunks.back();
    chunk.buffer.Init(
        alloc, size,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        "Scratch buffer");
}
