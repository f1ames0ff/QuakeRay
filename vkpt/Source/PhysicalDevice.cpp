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

#include "PhysicalDevice.h"

#include <string>
#include <vector>

#include "RgException.h"

using namespace vkpt;

PhysicalDevice::PhysicalDevice(VkInstance instance)
    : physDevice(VK_NULL_HANDLE)
    , memoryProperties{}
    , rtPipelineProperties{}
    , asProperties{}
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        throw RgException(RG_CANT_FIND_PHYSICAL_DEVICE, "Can't find physical devices");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    const VkResult enumerateResult = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    VK_CHECKERROR(enumerateResult);

    for (const VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
        rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &rtFeatures;
        vkGetPhysicalDeviceFeatures2(device, &features2);

        if (!rtFeatures.rayTracingPipeline)
        {
            continue;
        }

        physDevice = device;

        rtPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &rtPipelineProperties;
        rtPipelineProperties.pNext = &asProperties;

        vkGetPhysicalDeviceProperties2(physDevice, &properties2);
        vkGetPhysicalDeviceMemoryProperties(physDevice, &memoryProperties);

        break;
    }

    if (physDevice == VK_NULL_HANDLE)
    {
        throw RgException(RG_CANT_FIND_PHYSICAL_DEVICE, "Can't find physical device with ray tracing support");
    }
}

VkPhysicalDevice PhysicalDevice::Get() const
{
    return physDevice;
}

uint32_t PhysicalDevice::GetMemoryTypeIndex(uint32_t memoryTypeBits, VkFlags requirementsMask) const
{
    const bool wantsDeviceLocal = (requirementsMask & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    const VkMemoryPropertyFlags conflictingFlags =
        wantsDeviceLocal ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
    {
        if ((memoryTypeBits & 1u) != 0)
        {
            const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[i].propertyFlags;

            if ((flags & requirementsMask) == requirementsMask && (flags & conflictingFlags) != conflictingFlags)
            {
                return i;
            }
        }

        memoryTypeBits >>= 1u;
    }

    throw RgException(RG_GRAPHICS_API_ERROR, "Can't find memory type for given memory property flags (" + std::to_string(requirementsMask) + ")");
}

const VkPhysicalDeviceMemoryProperties &PhysicalDevice::GetMemoryProperties() const
{
    return memoryProperties;
}

const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &PhysicalDevice::GetRTPipelineProperties() const
{
    return rtPipelineProperties;
}

const VkPhysicalDeviceAccelerationStructurePropertiesKHR &PhysicalDevice::GetASProperties() const
{
    return asProperties;
}
