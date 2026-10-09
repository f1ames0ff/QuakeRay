#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace qray
{
class SwapchainRecreateState
{
public:
    bool BeginAcquire(bool parametersChanged)
    {
        if (parametersChanged)
        {
            forcedRecreateAttempted = false;
        }

        const bool force = !parametersChanged && suboptimal && !forcedRecreateAttempted;
        if (force)
        {
            forcedRecreateAttempted = true;
        }

        suboptimal = false;
        return force;
    }

    void Acquired(VkResult result)
    {
        if (result == VK_SUBOPTIMAL_KHR)
        {
            suboptimal = true;
        }
    }

    void Presented(VkResult result)
    {
        if (result == VK_SUBOPTIMAL_KHR)
        {
            suboptimal = true;
        }
        else if (result == VK_SUCCESS && !suboptimal)
        {
            forcedRecreateAttempted = false;
        }
    }

private:
    bool suboptimal = false;
    bool forcedRecreateAttempted = false;
};

inline void AppendSurfaceMaintenanceExtensions(
    std::vector<const char *> &enabled,
    std::span<const VkExtensionProperties> supported,
    bool surfaceCapabilities2Supported)
{
    if (!surfaceCapabilities2Supported)
    {
        return;
    }

    for (const char *name : { VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                             VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME })
    {
        if (std::any_of(supported.begin(), supported.end(),
            [name](const VkExtensionProperties &ext) { return std::strcmp(ext.extensionName, name) == 0; }))
        {
            if (std::none_of(enabled.begin(), enabled.end(),
                [name](const char *ext) { return std::strcmp(ext, name) == 0; }))
            {
                enabled.push_back(name);
            }
        }
    }
}

inline const char *SelectSwapchainMaintenanceExtension(
    std::span<const VkExtensionProperties> supportedDevice,
    std::span<const std::string> enabledInstance)
{
    const char *deviceNames[] = { VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
                                 VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME };
    const char *instanceNames[] = { VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                                   VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME };

    for (size_t i = 0; i < 2; i++)
    {
        const bool deviceSupported = std::any_of(supportedDevice.begin(), supportedDevice.end(),
            [&](const VkExtensionProperties &ext) { return std::strcmp(ext.extensionName, deviceNames[i]) == 0; });
        const bool instanceEnabled = std::any_of(enabledInstance.begin(), enabledInstance.end(),
            [&](const std::string &ext) { return ext == instanceNames[i]; });

        if (deviceSupported && instanceEnabled)
        {
            return deviceNames[i];
        }
    }

    return nullptr;
}
}
