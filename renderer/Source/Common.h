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

#include <cassert>
#include <cstdio>
#include <memory>
#include <cstring>
#include <vulkan/vulkan.h>

namespace qray
{

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

#pragma region extension functions

// extension functions' lists
#define VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST \
	VK_EXTENSION_FUNCTION(vkCreateDebugUtilsMessengerEXT) \
	VK_EXTENSION_FUNCTION(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FUNCTION_LIST \
	VK_EXTENSION_FUNCTION(vkCmdPipelineBarrier2KHR) \
	VK_EXTENSION_FUNCTION(vkCreateAccelerationStructureKHR) \
	VK_EXTENSION_FUNCTION(vkDestroyAccelerationStructureKHR) \
	VK_EXTENSION_FUNCTION(vkGetRayTracingShaderGroupHandlesKHR) \
	VK_EXTENSION_FUNCTION(vkCreateRayTracingPipelinesKHR) \
	VK_EXTENSION_FUNCTION(vkGetAccelerationStructureDeviceAddressKHR) \
	VK_EXTENSION_FUNCTION(vkGetAccelerationStructureBuildSizesKHR) \
	VK_EXTENSION_FUNCTION(vkCmdBuildAccelerationStructuresKHR) \
	VK_EXTENSION_FUNCTION(vkCmdTraceRaysKHR)

#define VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST \
	VK_EXTENSION_FUNCTION(vkSetDebugUtilsObjectNameEXT) \
	VK_EXTENSION_FUNCTION(vkCmdBeginDebugUtilsLabelEXT) \
	VK_EXTENSION_FUNCTION(vkCmdEndDebugUtilsLabelEXT) 


// extension functions' declarations
#define VK_EXTENSION_FUNCTION(fname) extern PFN_##fname s##fname;
VK_INSTANCE_DEBUG_UTILS_FUNCTION_LIST
VK_DEVICE_FUNCTION_LIST
VK_DEVICE_DEBUG_UTILS_FUNCTION_LIST
#undef VK_EXTENSION_FUNCTION

void InitInstanceExtensionFunctions_DebugUtils(VkInstance instance);
void InitDeviceExtensionFunctions(VkDevice device);
void InitDeviceExtensionFunctions_DebugUtils(VkDevice device);

#pragma endregion


// A Vulkan call whose result cannot be ignored runs through this: a failure names
// the call site and the result in the log, and then the check itself trips -- a
// call that fails is a bug in the renderer, and which call it was is what the
// assert alone cannot say.
#define VK_CHECKERROR(r) \
    do \
    { \
        const VkResult vkCheckErrorResult = (r); \
        if (vkCheckErrorResult != VK_SUCCESS) \
        { \
            fprintf(stderr, "qray: Vulkan call at %s:%d failed: %d\n", __FILE__, __LINE__, (int)vkCheckErrorResult); \
        } \
        assert(vkCheckErrorResult == VK_SUCCESS); \
    } while (0)


#define SET_DEBUG_NAME(device, obj, type, pName) AddDebugName((device), reinterpret_cast<uint64_t>(obj), (type), (pName))


// If name is null, debug name won't be set
void AddDebugName(VkDevice device, uint64_t obj, VkObjectType type, const char *pName);
void BeginCmdLabel(VkCommandBuffer cmd, const char *pName, const float pColor[4] = nullptr);
void EndCmdLabel(VkCommandBuffer cmd);

}
