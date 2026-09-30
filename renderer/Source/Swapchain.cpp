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

#include "Swapchain.h"

#include <algorithm>
#include <utility>

#include "QrException.h"
#include "Utils.h"

using namespace qray;

namespace
{
    bool IsNullExtent(const VkExtent2D &extent)
    {
        return extent.width == 0 || extent.height == 0;
    }

    bool AreExtentsEqual(const VkExtent2D &a, const VkExtent2D &b)
    {
        return a.width == b.width && a.height == b.height;
    }

    VkSurfaceFormatKHR PickSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats)
    {
        for (const VkFormat wanted : { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB })
        {
            const auto match = std::find_if(
                availableFormats.rbegin(), availableFormats.rend(),
                [wanted](const VkSurfaceFormatKHR &format) { return format.format == wanted; });

            if (match != availableFormats.rend())
            {
                return *match;
            }
        }

        return {};
    }
}

Swapchain::Swapchain(
    VkDevice _device,
    VkSurfaceKHR _surface,
    VkPhysicalDevice _physDevice,
    std::shared_ptr<CommandBufferManager> _cmdManager,
    bool _presentWait2Supported)
    : device(_device)
    , surface(_surface)
    , physDevice(_physDevice)
    , cmdManager(std::move(_cmdManager))
    , surfaceFormat{}
    , presentModeVsync(VK_PRESENT_MODE_FIFO_KHR)
    , presentModeAdaptive(VK_PRESENT_MODE_FIFO_KHR)
    , presentModeMailbox(VK_PRESENT_MODE_FIFO_KHR)
    , requestedPresentMode(QR_PRESENT_MODE_MAILBOX)
    , surfaceExtent{ UINT32_MAX, UINT32_MAX }
    , isPresentMode(QR_PRESENT_MODE_MAILBOX)
    , swapchain(VK_NULL_HANDLE)
    , swapchainImages{}
    , swapchainViews{}
    , presentWait2Supported(_presentWait2Supported)
    , surfacePresentWait2Supported(false)
    , usePresentWait2(false)
    , currentPresentId(0)
    , maxFrameLatency(0)
    , currentSwapchainIndex(UINT32_MAX)
    , subscribers{}
    , cachedSurfaceCaps{}
    , cachedSurfaceCapsResult(VK_SUCCESS)
    , cachedSurfaceCapsValid(false)
    , cachedIsExtentOptimal(false)
{
    uint32_t formatCount = 0;
    VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice, surface, &formatCount, nullptr);
    VK_CHECKERROR(r);

    std::vector<VkSurfaceFormatKHR> availableFormats(formatCount);
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice, surface, &formatCount, availableFormats.data());
    VK_CHECKERROR(r);

    surfaceFormat = PickSurfaceFormat(availableFormats);

    uint32_t presentModeCount = 0;
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice, surface, &presentModeCount, nullptr);
    VK_CHECKERROR(r);

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice, surface, &presentModeCount, presentModes.data());
    VK_CHECKERROR(r);

    for (const VkPresentModeKHR mode : presentModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            presentModeMailbox = mode;
        }
        else if (mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
        {
            presentModeAdaptive = mode;
        }
    }
}

bool Swapchain::IsExtentOptimal() const
{
    if (cachedSurfaceCapsValid)
    {
        return cachedIsExtentOptimal;
    }

    VkSurfaceCapabilitiesKHR surfCapabilities;
    const VkResult r = GetSurfaceCapabilities(&surfCapabilities);

    if (r == VK_ERROR_SURFACE_LOST_KHR)
    {
        return false;
    }

    VK_CHECKERROR(r);

    return cachedIsExtentOptimal;
}

bool Swapchain::HasValidExtent() const
{
    ResetSurfaceCapabilitiesCache();
    return IsExtentOptimal();
}

VkResult Swapchain::GetSurfaceCapabilities(VkSurfaceCapabilitiesKHR *outCaps) const
{
    if (!cachedSurfaceCapsValid)
    {
        cachedSurfaceCapsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDevice, surface, &cachedSurfaceCaps);
        cachedSurfaceCapsValid = (cachedSurfaceCapsResult == VK_SUCCESS);

        if (cachedSurfaceCapsValid)
        {
            cachedIsExtentOptimal =
                !IsNullExtent(cachedSurfaceCaps.maxImageExtent) &&
                !IsNullExtent(cachedSurfaceCaps.currentExtent);
        }
    }

    *outCaps = cachedSurfaceCaps;
    return cachedSurfaceCapsResult;
}

void Swapchain::ResetSurfaceCapabilitiesCache() const
{
    cachedSurfaceCapsValid = false;
}

VkExtent2D Swapchain::GetOptimalExtent() const
{
    VkSurfaceCapabilitiesKHR surfCapabilities;
    VK_CHECKERROR(GetSurfaceCapabilities(&surfCapabilities));

    if (IsNullExtent(surfCapabilities.maxImageExtent) || IsNullExtent(surfCapabilities.currentExtent))
    {
        throw QrException(QR_WRONG_FUNCTION_CALL, "Surface has 0 extent. Prevent calling qray functions in that case");
    }

    if (surfCapabilities.currentExtent.width == UINT32_MAX || surfCapabilities.currentExtent.height == UINT32_MAX)
    {
        return surfCapabilities.maxImageExtent;
    }

    return surfCapabilities.currentExtent;
}

bool Swapchain::RequestPresentMode(QrPresentMode mode)
{
    requestedPresentMode = mode;
    return requestedPresentMode != isPresentMode;
}

void Swapchain::SetMaxFrameLatency(uint64_t frames)
{
    maxFrameLatency = frames > 8 ? 8 : frames;
}

bool Swapchain::IsPresentWaitActive() const
{
    return usePresentWait2;
}

const char *Swapchain::GetPresentModeName() const
{
    switch (isPresentMode)
    {
    case QR_PRESENT_MODE_VSYNC:     return "fifo";
    case QR_PRESENT_MODE_ADAPTIVE:  return "fifo_relaxed";
    default:                        return "mailbox";
    }
}

VkPresentModeKHR Swapchain::GetVkPresentMode(QrPresentMode mode) const
{
    switch (mode)
    {
    case QR_PRESENT_MODE_VSYNC:     return presentModeVsync;
    case QR_PRESENT_MODE_ADAPTIVE:  return presentModeAdaptive;
    default:                        return presentModeMailbox;
    }
}

void Swapchain::AcquireImage(VkSemaphore imageAvailableSemaphore)
{
    ResetSurfaceCapabilitiesCache();

    const VkExtent2D requestedExtent = GetOptimalExtent();

    const bool wantPresentWait2 = surfacePresentWait2Supported && maxFrameLatency > 0;
    if (!AreExtentsEqual(requestedExtent, surfaceExtent) || requestedPresentMode != isPresentMode || usePresentWait2 != wantPresentWait2)
    {
        TryRecreate(requestedExtent, requestedPresentMode);
    }

    if (usePresentWait2 && sVkWaitForPresent2KHR != nullptr && maxFrameLatency > 0 &&
        isPresentMode != QR_PRESENT_MODE_MAILBOX && currentPresentId + 1 > maxFrameLatency)
    {
        VkPresentWait2InfoKHR waitInfo = {};
        waitInfo.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
        waitInfo.presentId = currentPresentId + 1 - maxFrameLatency;
        waitInfo.timeout = 50ull * 1000ull * 1000ull;
        sVkWaitForPresent2KHR(device, swapchain, &waitInfo);
    }

    while (true)
    {
        const VkResult r = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX,
            imageAvailableSemaphore,
            VK_NULL_HANDLE, &currentSwapchainIndex);

        if (r == VK_SUCCESS)
        {
            return;
        }

        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            TryRecreate(requestedExtent, requestedPresentMode);
            continue;
        }

        assert(0);
    }
}

void Swapchain::Present(const std::shared_ptr<Queues> &queues, VkSemaphore renderFinishedSemaphore)
{
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &currentSwapchainIndex;

    const uint64_t nextPresentId = currentPresentId + 1;
    VkPresentId2KHR presentIdInfo{};
    if (usePresentWait2)
    {
        presentIdInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
        presentIdInfo.swapchainCount = 1;
        presentIdInfo.pPresentIds = &nextPresentId;
        presentInfo.pNext = &presentIdInfo;
    }

    const VkResult r = vkQueuePresentKHR(queues->GetGraphics(), &presentInfo);

    if (usePresentWait2)
    {
        currentPresentId = nextPresentId;
    }

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        ResetSurfaceCapabilitiesCache();
        TryRecreate(GetOptimalExtent(), requestedPresentMode);
    }
}

bool Swapchain::TryRecreate(const VkExtent2D &newExtent, QrPresentMode mode)
{
    const bool wantPresentWait2 = surfacePresentWait2Supported && maxFrameLatency > 0;

    if (AreExtentsEqual(surfaceExtent, newExtent) && isPresentMode == mode && usePresentWait2 == wantPresentWait2)
    {
        return false;
    }

    cmdManager->WaitDeviceIdle();

    const VkSwapchainKHR oldSwapchain = DestroyWithoutSwapchain();
    Create(newExtent.width, newExtent.height, mode, oldSwapchain);

    return true;
}

void Swapchain::Create(uint32_t newWidth, uint32_t newHeight, QrPresentMode mode, VkSwapchainKHR oldSwapchain)
{
    isPresentMode = mode;
    surfaceExtent = { newWidth, newHeight };
    currentPresentId = 0;

    ResetSurfaceCapabilitiesCache();

    VkSurfaceCapabilitiesKHR surfCapabilities;
    VkResult r = GetSurfaceCapabilities(&surfCapabilities);
    VK_CHECKERROR(r);

    if (surfCapabilities.currentExtent.width != UINT32_MAX && surfCapabilities.currentExtent.height != UINT32_MAX)
    {
        assert(AreExtentsEqual(surfaceExtent, surfCapabilities.currentExtent));
    }
    else
    {
        assert(surfCapabilities.minImageExtent.width <= surfaceExtent.width && surfaceExtent.width <= surfCapabilities.maxImageExtent.width);
        assert(surfCapabilities.minImageExtent.height <= surfaceExtent.height && surfaceExtent.height <= surfCapabilities.maxImageExtent.height);
    }

    assert(swapchain == VK_NULL_HANDLE);
    assert(swapchainImages.empty());
    assert(swapchainViews.empty());

    uint32_t imageCount = std::max(3u, surfCapabilities.minImageCount);
    if (surfCapabilities.maxImageCount > 0)
    {
        imageCount = std::min(imageCount, surfCapabilities.maxImageCount);
    }

    surfacePresentWait2Supported = false;
    if (presentWait2Supported && sVkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr)
    {
        VkSurfaceCapabilitiesPresentId2KHR presentId2Capabilities = {};
        presentId2Capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;

        VkSurfaceCapabilitiesPresentWait2KHR presentWait2Capabilities = {};
        presentWait2Capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
        presentWait2Capabilities.pNext = &presentId2Capabilities;

        VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
        surfaceInfo.surface = surface;

        VkSurfaceCapabilities2KHR surfaceCapabilities2 = {};
        surfaceCapabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
        surfaceCapabilities2.pNext = &presentWait2Capabilities;

        if (sVkGetPhysicalDeviceSurfaceCapabilities2KHR(physDevice, &surfaceInfo, &surfaceCapabilities2) == VK_SUCCESS)
        {
            surfacePresentWait2Supported = presentId2Capabilities.presentId2Supported && presentWait2Capabilities.presentWait2Supported;
        }
    }

    usePresentWait2 = surfacePresentWait2Supported && maxFrameLatency > 0;

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = surfaceExtent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = surfCapabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = GetVkPresentMode(mode);
    if (usePresentWait2)
    {
        swapchainInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    }
    swapchainInfo.clipped = VK_FALSE;
    swapchainInfo.oldSwapchain = oldSwapchain;

    r = vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain);
    VK_CHECKERROR(r);

    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    }

    r = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    VK_CHECKERROR(r);

    swapchainImages.resize(imageCount);
    swapchainViews.resize(imageCount);

    r = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
    VK_CHECKERROR(r);

    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat.format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        r = vkCreateImageView(device, &viewInfo, nullptr, &swapchainViews[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, swapchainImages[i], VK_OBJECT_TYPE_IMAGE, "Swapchain image");
        SET_DEBUG_NAME(device, swapchainViews[i], VK_OBJECT_TYPE_IMAGE_VIEW, "Swapchain image view");
    }

    const VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    for (uint32_t i = 0; i < imageCount; i++)
    {
        Utils::BarrierImage(
            cmd, swapchainImages[i],
            0, 0,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    cmdManager->Submit(cmd);
    cmdManager->WaitGraphicsIdle();

    CallCreateSubscribers();
}

void Swapchain::Destroy()
{
    const VkSwapchainKHR oldSwapchain = DestroyWithoutSwapchain();
    vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
}

VkSwapchainKHR Swapchain::DestroyWithoutSwapchain()
{
    vkDeviceWaitIdle(device);

    if (swapchain != VK_NULL_HANDLE)
    {
        CallDestroySubscribers();
    }

    for (const VkImageView view : swapchainViews)
    {
        vkDestroyImageView(device, view, nullptr);
    }

    swapchainViews.clear();
    swapchainImages.clear();

    const VkSwapchainKHR oldSwapchain = swapchain;
    swapchain = VK_NULL_HANDLE;

    return oldSwapchain;
}

void Swapchain::CallCreateSubscribers()
{
    for (const std::weak_ptr<ISwapchainDependency> &subscriber : subscribers)
    {
        if (const std::shared_ptr<ISwapchainDependency> locked = subscriber.lock())
        {
            locked->OnSwapchainCreate(this);
        }
    }
}

void Swapchain::CallDestroySubscribers()
{
    for (const std::weak_ptr<ISwapchainDependency> &subscriber : subscribers)
    {
        if (const std::shared_ptr<ISwapchainDependency> locked = subscriber.lock())
        {
            locked->OnSwapchainDestroy();
        }
    }
}

Swapchain::~Swapchain()
{
    Destroy();
}

void Swapchain::Subscribe(std::shared_ptr<ISwapchainDependency> subscriber)
{
    subscribers.push_back(std::move(subscriber));
}

void Swapchain::Unsubscribe(const ISwapchainDependency *subscriber)
{
    subscribers.remove_if([subscriber](const std::weak_ptr<ISwapchainDependency> &weak)
    {
        const std::shared_ptr<ISwapchainDependency> locked = weak.lock();

        if (!locked)
        {
            return true;
        }

        return locked.get() == subscriber;
    });
}

VkFormat Swapchain::GetSurfaceFormat() const
{
    return surfaceFormat.format;
}

uint32_t Swapchain::GetWidth() const
{
    return surfaceExtent.width;
}

uint32_t Swapchain::GetHeight() const
{
    return surfaceExtent.height;
}

uint32_t Swapchain::GetCurrentImageIndex() const
{
    return currentSwapchainIndex;
}

uint32_t Swapchain::GetImageCount() const
{
    assert(swapchainViews.size() == swapchainImages.size());
    return static_cast<uint32_t>(swapchainViews.size());
}

VkImageView Swapchain::GetImageView(uint32_t index) const
{
    assert(index < swapchainViews.size());
    return swapchainViews[index];
}

VkImage Swapchain::GetImage(uint32_t index) const
{
    assert(index < swapchainImages.size());
    return swapchainImages[index];
}

const VkImageView *Swapchain::GetImageViews() const
{
    if (swapchainViews.empty())
    {
        return nullptr;
    }

    return swapchainViews.data();
}
