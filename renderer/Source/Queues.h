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

namespace qray
{

class Queues
{
public:
    explicit Queues(VkPhysicalDevice physDevice, VkSurfaceKHR surface);
    ~Queues();

    Queues(const Queues &other) = delete;
    Queues(Queues &&other) noexcept = delete;
    Queues &operator=(const Queues &other) = delete;
    Queues &operator=(Queues &&other) noexcept = delete;

    void SetDevice(VkDevice device);

    void GetDeviceQueueCreateInfos(std::vector<VkDeviceQueueCreateInfo> &outInfos) const;

    uint32_t GetIndexGraphics() const;
    uint32_t GetIndexCompute() const;
    uint32_t GetIndexTransfer() const;

    VkQueue GetGraphics() const;
    VkQueue GetCompute() const;
    VkQueue GetTransfer() const;

private:
    std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    float defaultQueuePriority;

    uint32_t indexGraphics;
    uint32_t indexCompute;
    uint32_t indexTransfer;

    VkQueue graphics;
    VkQueue compute;
    VkQueue transfer;
};

}
