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

#include <bitset>

#include "qray/qray.h"

#include "AutoBuffer.h"

namespace qray
{
    namespace detail
    {
        constexpr size_t PORTAL_LIST_BITCOUNT = 63;
    }

    class PortalList
    {
    public:
        PortalList(VkDevice device, std::shared_ptr<MemoryAllocator> allocator);
        ~PortalList();

        PortalList(const PortalList &other) = delete;
        PortalList(PortalList &&other) noexcept = delete;
        PortalList &operator=(const PortalList &other) = delete;
        PortalList &operator=(PortalList &&other) noexcept = delete;

        void Upload(uint32_t frameIndex, const QrPortalUploadInfo &info);

        VkBuffer GetStagingBuffer(uint32_t frameIndex);
        VkBuffer GetDeviceLocalBuffer() const;
        VkDeviceSize GetBufferSize() const;

        void ResetUploads();

        VkDescriptorSet GetDescSet(uint32_t frameIndex) const;
        VkDescriptorSetLayout GetDescSetLayout() const;

    private:
        void CreateDescriptors();

    private:
        VkDevice device;
        std::shared_ptr<AutoBuffer> buffer;

        VkDescriptorPool        descPool;
        VkDescriptorSetLayout   descSetLayout;
        VkDescriptorSet         descSet;

        std::bitset<detail::PORTAL_LIST_BITCOUNT> uploadedIndices;
    };
}
