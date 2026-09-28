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


namespace vkpt
{
#define VK_EXTENSION_FUNCTION(fname) PFN_##fname s##fname;
    VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST
    VK_DEVICE_FUNCTION_LIST
    VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

void vkpt::InitInstanceExtensionFunctions_DebugUtils(VkInstance instance)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetInstanceProcAddr(instance, #fname)); \
    assert(s##fname != nullptr);

    VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

void vkpt::InitDeviceExtensionFunctions(VkDevice device)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetDeviceProcAddr(device, #fname)); \
    assert(s##fname != nullptr);

    VK_DEVICE_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

void vkpt::InitDeviceExtensionFunctions_DebugUtils(VkDevice device)
{
#define VK_EXTENSION_FUNCTION(fname) \
    s##fname = reinterpret_cast<PFN_##fname>(vkGetDeviceProcAddr(device, #fname)); \
    assert(s##fname != nullptr);

    VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION
}

void vkpt::AddDebugName(VkDevice device, uint64_t obj, VkObjectType type, const char *pName)
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

void vkpt::BeginCmdLabel(VkCommandBuffer cmd, const char *pName, const float pColor[4])
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

void vkpt::EndCmdLabel(VkCommandBuffer cmd)
{
    if (svkCmdEndDebugUtilsLabelEXT != nullptr)
    {
        svkCmdEndDebugUtilsLabelEXT(cmd);
    }
}
