// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Common.h"

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif


// Prints where the failing Vulkan call was made (file and line, which the assert
// dialog cannot show) and what it returned, then lets the assert stop the run.
void qray::VK_CHECKERROR_ReportAlloc(const VkDeviceSize size, const char *pDebugName)
{
    std::fprintf(stderr, "qray: out of device memory: %llu bytes for '%s'\n",
                 (unsigned long long)size, pDebugName ? pDebugName : "<no name>");
    std::fflush(stderr);

    std::FILE *log = std::fopen("vk_last_error.txt", "a");
    if (log)
    {
        std::fprintf(log, "qray: out of device memory: %llu bytes for '%s'\n",
                     (unsigned long long)size, pDebugName ? pDebugName : "<no name>");
        std::fclose(log);
    }
}

void qray::VK_CHECKERROR_Report(const VkResult r, const char *file, int line)
{
    const char *name = "VK_ERROR_*";

    switch (r)
    {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        name = "VK_ERROR_OUT_OF_HOST_MEMORY";
        break;
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        name = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        break;
    case VK_ERROR_INITIALIZATION_FAILED:
        name = "VK_ERROR_INITIALIZATION_FAILED";
        break;
    case VK_ERROR_DEVICE_LOST:
        name = "VK_ERROR_DEVICE_LOST";
        break;
    case VK_ERROR_MEMORY_MAP_FAILED:
        name = "VK_ERROR_MEMORY_MAP_FAILED";
        break;
    case VK_ERROR_LAYER_NOT_PRESENT:
        name = "VK_ERROR_LAYER_NOT_PRESENT";
        break;
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        name = "VK_ERROR_EXTENSION_NOT_PRESENT";
        break;
    case VK_ERROR_FEATURE_NOT_PRESENT:
        name = "VK_ERROR_FEATURE_NOT_PRESENT";
        break;
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        name = "VK_ERROR_INCOMPATIBLE_DRIVER";
        break;
    case VK_ERROR_TOO_MANY_OBJECTS:
        name = "VK_ERROR_TOO_MANY_OBJECTS";
        break;
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        name = "VK_ERROR_FORMAT_NOT_SUPPORTED";
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        name = "VK_ERROR_SURFACE_LOST_KHR";
        break;
    default:
        break;
    }

    std::fprintf(stderr, "qray: Vulkan call failed: %s (%d) at %s:%d\n", name, (int)r, file, line);
    std::fflush(stderr);

    std::FILE *log = std::fopen("vk_last_error.txt", "a");
    if (log)
    {
        std::fprintf(log, "qray: Vulkan call failed: %s (%d) at %s:%d\n", name, (int)r, file, line);
        std::fclose(log);
    }

#ifdef _WIN32
    // The assert dialog names this file and not the call, so say it here as well:
    // the message box is what the user copies.
    char message[1024];

    std::snprintf(message, sizeof(message),
                  "A Vulkan call failed.\n\n%s (%d)\n\nat %s:%d\n\n"
                  "(the same line is appended to vk_last_error.txt)",
                  name, (int)r, file, line);
    MessageBoxA(nullptr, message, "qray: Vulkan error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif

    assert(r == VK_SUCCESS);
}


namespace qray
{
#define VK_EXTENSION_FUNCTION(fname) PFN_##fname s##fname;
    VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST
    VK_DEVICE_FUNCTION_LIST
    VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION

    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR sVkGetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
    PFN_vkWaitForPresent2KHR sVkWaitForPresent2KHR = nullptr;
}

void qray::InitInstanceExtensionFunctions_DebugUtils(VkInstance instance)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetInstanceProcAddr(instance, #fname)); \
    assert(s##fname != nullptr);

    VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

bool qray::InitInstanceExtensionFunctions_SurfaceCapabilities2(VkInstance instance)
{
    sVkGetPhysicalDeviceSurfaceCapabilities2KHR =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    return sVkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr;
}

void qray::InitDeviceExtensionFunctions(VkDevice device)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetDeviceProcAddr(device, #fname)); \
    assert(s##fname != nullptr);

    VK_DEVICE_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

bool qray::InitDeviceExtensionFunctions_PresentWait2(VkDevice device)
{
    sVkWaitForPresent2KHR = reinterpret_cast<PFN_vkWaitForPresent2KHR>(vkGetDeviceProcAddr(device, "vkWaitForPresent2KHR"));
    return sVkWaitForPresent2KHR != nullptr;
}

void qray::InitDeviceExtensionFunctions_DebugUtils(VkDevice device)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetDeviceProcAddr(device, #fname)); \
    assert(s##fname != nullptr);

    VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

void qray::AddDebugName(VkDevice device, uint64_t obj, VkObjectType type, const char *pName)
{
    if (svkSetDebugUtilsObjectNameEXT == nullptr || pName == nullptr)
    {
        return;
    }

    const VkDebugUtilsObjectNameInfoEXT nameInfo =
    {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        type,
        obj,
        pName,
    };

    VK_CHECKERROR(svkSetDebugUtilsObjectNameEXT(device, &nameInfo));
}

void qray::BeginCmdLabel(VkCommandBuffer cmd, const char *pName, const float pColor[4])
{
    if (svkCmdBeginDebugUtilsLabelEXT == nullptr || pName == nullptr)
    {
        return;
    }

    VkDebugUtilsLabelEXT labelInfo =
    {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        nullptr,
        pName,
        {},
    };

    if (pColor != nullptr)
    {
        std::copy_n(pColor, 4, labelInfo.color);
    }

    svkCmdBeginDebugUtilsLabelEXT(cmd, &labelInfo);
}

void qray::EndCmdLabel(VkCommandBuffer cmd)
{
    if (svkCmdEndDebugUtilsLabelEXT != nullptr)
    {
        svkCmdEndDebugUtilsLabelEXT(cmd);
    }
}
