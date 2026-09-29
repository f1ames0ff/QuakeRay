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

#pragma once

#include "Common.h"

namespace qray
{

class PhysicalDevice
{
public:
    explicit PhysicalDevice(VkInstance instance);
    ~PhysicalDevice() = default;

    PhysicalDevice(const PhysicalDevice &other) = delete;
    PhysicalDevice(PhysicalDevice &&other) noexcept = delete;
    PhysicalDevice &operator=(const PhysicalDevice &other) = delete;
    PhysicalDevice &operator=(PhysicalDevice &&other) noexcept = delete;

    VkPhysicalDevice Get() const;
    const VkPhysicalDeviceMemoryProperties &GetMemoryProperties() const;
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &GetRTPipelineProperties() const;
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR &GetASProperties() const;

    uint32_t GetMemoryTypeIndex(uint32_t memoryTypeBits, VkFlags requirementsMask) const;

private:
    VkPhysicalDevice physDevice;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProperties;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties;
};

}
