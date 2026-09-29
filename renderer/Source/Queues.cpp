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

#include "Queues.h"

using namespace qray;

namespace
{
    bool IsFullQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
               (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
               (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    }

    bool IsComputeOnlyQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
               (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
               (flags & VK_QUEUE_TRANSFER_BIT) == 0;
    }

    bool IsTransferOnlyQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
               (flags & VK_QUEUE_COMPUTE_BIT) == 0 &&
               (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    }
}

Queues::Queues(VkPhysicalDevice physDevice, VkSurfaceKHR surface)
    : queueFamilyProperties{}
    , defaultQueuePriority(0.0f)
    , indexGraphics(UINT32_MAX)
    , indexCompute(UINT32_MAX)
    , indexTransfer(UINT32_MAX)
    , graphics(VK_NULL_HANDLE)
    , compute(VK_NULL_HANDLE)
    , transfer(VK_NULL_HANDLE)
{
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &familyCount, nullptr);
    assert(familyCount > 0);

    queueFamilyProperties.resize(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &familyCount, queueFamilyProperties.data());

    for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++)
    {
        VkBool32 presentSupported = VK_FALSE;
        VK_CHECKERROR(vkGetPhysicalDeviceSurfaceSupportKHR(physDevice, i, surface, &presentSupported));

        const VkQueueFlags flags = queueFamilyProperties[i].queueFlags;

        if (presentSupported && IsFullQueue(flags))
        {
            indexGraphics = i;
        }

        if (IsComputeOnlyQueue(flags))
        {
            indexCompute = i;
        }

        if (IsTransferOnlyQueue(flags))
        {
            indexTransfer = i;
        }
    }

    assert(indexGraphics != UINT32_MAX);

    if (indexCompute == UINT32_MAX)
    {
        indexCompute = indexGraphics;
    }

    if (indexTransfer == UINT32_MAX)
    {
        indexTransfer = indexGraphics;
    }
}

Queues::~Queues()
{}

void Queues::SetDevice(VkDevice device)
{
    vkGetDeviceQueue(device, indexGraphics, 0, &graphics);
    vkGetDeviceQueue(device, indexCompute, 0, &compute);
    vkGetDeviceQueue(device, indexTransfer, 0, &transfer);
}

void Queues::GetDeviceQueueCreateInfos(std::vector<VkDeviceQueueCreateInfo> &outInfos) const
{
    const auto appendQueueInfo = [this, &outInfos](uint32_t familyIndex)
    {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = familyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &defaultQueuePriority;
        outInfos.push_back(queueInfo);
    };

    appendQueueInfo(indexGraphics);

    if (indexCompute != indexGraphics)
    {
        appendQueueInfo(indexCompute);
    }

    if (indexTransfer != indexGraphics && indexTransfer != indexCompute)
    {
        appendQueueInfo(indexTransfer);
    }
}

uint32_t Queues::GetIndexGraphics() const
{
    return indexGraphics;
}

uint32_t Queues::GetIndexCompute() const
{
    return indexCompute;
}

uint32_t Queues::GetIndexTransfer() const
{
    return indexTransfer;
}

VkQueue Queues::GetGraphics() const
{
    return graphics;
}

VkQueue Queues::GetCompute() const
{
    return compute;
}

VkQueue Queues::GetTransfer() const
{
    return transfer;
}
