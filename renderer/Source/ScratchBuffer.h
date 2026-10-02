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

#include <list>

#include "Buffer.h"

namespace qray
{

class ScratchBuffer
{
public:
    explicit ScratchBuffer(std::shared_ptr<MemoryAllocator> allocator, uint32_t alignment = 1);

    ScratchBuffer(const ScratchBuffer& other) = delete;
    ScratchBuffer(ScratchBuffer&& other) noexcept = delete;
    ScratchBuffer& operator=(const ScratchBuffer& other) = delete;
    ScratchBuffer& operator=(ScratchBuffer&& other) noexcept = delete;

    VkDeviceAddress GetScratchAddress(VkDeviceSize scratchSize);
    void Reset();

private:
    void AddChunk(VkDeviceSize size);

private:
    struct ChunkBuffer
    {
        Buffer buffer;
        uint32_t currentOffset = 0;
    };

    std::weak_ptr<MemoryAllocator> allocator;
    std::list<ChunkBuffer> chunks;
    uint32_t alignment = 1;
};

}
