#pragma once

#include "RHI/NvrhiContext.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

inline void Require(bool value, const std::string &text)
{
    if (!value) throw std::runtime_error(text);
}

class VulkanTestContext
{
public:
    std::atomic_uint errors = 0;
    qray::NvrhiContext context;

    VulkanTestContext()
    {
        uint32_t count = 0;
        Check(vkEnumerateInstanceLayerProperties(&count, nullptr));
        std::vector<VkLayerProperties> layers(count);
        Check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
        std::vector<const char *> enabledLayers;
        for (const auto &layer : layers)
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                enabledLayers.push_back("VK_LAYER_KHRONOS_validation");

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "QuakeRay cloud regression";
        app.apiVersion = VK_API_VERSION_1_3;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = Debug;
        debug.pUserData = this;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pNext = &debug;
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = extensions;
        instanceInfo.enabledLayerCount = uint32_t(enabledLayers.size());
        instanceInfo.ppEnabledLayerNames = enabledLayers.data();
        Check(vkCreateInstance(&instanceInfo, nullptr, &instance));
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        Check(createMessenger(instance, &debug, nullptr, &messenger));
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        Require(count != 0, "no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical = devices[0];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        std::cout << "GPU: " << props.deviceName << "; validation=" << !enabledLayers.empty() << '\n';
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        uint32_t family = 0;
        while (family < count && (queues[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ++family;
        Require(family < count, "no graphics+compute queue");
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &features12;
        deviceInfo.pEnabledFeatures = &features.features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &vkDevice));
        VkQueue queue;
        vkGetDeviceQueue(vkDevice, family, 0, &queue);
        qray::NvrhiDeviceInfo info;
        info.instance = instance;
        info.physicalDevice = physical;
        info.device = vkDevice;
        info.graphicsQueue = queue;
        info.graphicsQueueIndex = family;
        info.instanceExtensions = extensions;
        info.instanceExtensionCount = 1;
        info.bufferDeviceAddressSupported = features12.bufferDeviceAddress;
        std::string error;
        auto print = [this](const char *s)
        {
            std::cerr << s << '\n';
            if (std::strstr(s, "ERROR")) ++errors;
        };
        Require(context.Init(info, print, error), error);
    }

    ~VulkanTestContext()
    {
        context.Shutdown();
        vkDestroyDevice(vkDevice, nullptr);
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyMessenger(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

private:
    const char *extensions[1] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static void Check(VkResult result)
    {
        Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result));
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
    {
        auto *self = static_cast<VulkanTestContext *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++self->errors;
        std::cerr << data->pMessage << '\n';
        return VK_FALSE;
    }
};
