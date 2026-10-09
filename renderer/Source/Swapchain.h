// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#include <list>
#include <vector>

#include <qray/qray.h>

#include "Common.h"
#include "PhysicalDevice.h"
#include "CommandBufferManager.h"
#include "ISwapchainDependency.h"
#include "SwapchainPolicy.h"

namespace qray
{

class Swapchain
{
public:
    Swapchain(
        VkDevice device,
        VkSurfaceKHR surface,
        VkPhysicalDevice physDevice,
        std::shared_ptr<CommandBufferManager> cmdManager,
        bool presentWait2Supported,
        bool swapchainMaintenance1Supported);
    ~Swapchain();

    Swapchain(const Swapchain &other) = delete;
    Swapchain(Swapchain &&other) noexcept = delete;
    Swapchain &operator=(const Swapchain &other) = delete;
    Swapchain &operator=(Swapchain &&other) noexcept = delete;

    bool RequestPresentMode(QrPresentMode mode);
    void SetMaxFrameLatency(uint64_t frames);
    bool IsPresentWaitActive() const;

    void AcquireImage(VkSemaphore imageAvailableSemaphore);
    void Present(const std::shared_ptr<Queues> &queues, VkSemaphore renderFinishedSemaphore);

    void Subscribe(std::shared_ptr<ISwapchainDependency> subscriber);
    void Unsubscribe(const ISwapchainDependency *subscriber);

    VkFormat GetSurfaceFormat() const;
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetCurrentImageIndex() const;
    uint32_t GetImageCount() const;
    VkImageView GetImageView(uint32_t index) const;
    VkImage GetImage(uint32_t index) const;
    const VkImageView *GetImageViews() const;

    // The semaphore paired with one swapchain image: the render-finished semaphore the submit
    // signals before the present of that image. One per image, not one per frame in flight,
    // because a binary semaphore may only be re-signaled after the present that waited on it
    // finished - which acquiring the image again guarantees (the validation layer's own advice
    // for VUID-vkQueueSubmit-pSignalSemaphores-00067).
    VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const;

    bool IsExtentOptimal() const;
    bool HasValidExtent() const;
    const char *GetPresentModeName() const;

private:
    VkExtent2D GetOptimalExtent() const;
    VkResult GetSurfaceCapabilities(VkSurfaceCapabilitiesKHR *outCaps) const;
    void ResetSurfaceCapabilitiesCache() const;
    VkPresentModeKHR GetVkPresentMode(QrPresentMode mode) const;
    bool IsWaitablePresentMode(QrPresentMode mode) const;

    bool TryRecreate(const VkExtent2D &newExtent, QrPresentMode mode, bool force = false);

    void Create(uint32_t newWidth, uint32_t newHeight, QrPresentMode mode, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
    void Destroy();
    VkSwapchainKHR DestroyWithoutSwapchain();

    void WaitPresentFences();

    void CallCreateSubscribers();
    void CallDestroySubscribers();

private:
    VkDevice device;
    VkSurfaceKHR surface;
    VkPhysicalDevice physDevice;
    std::shared_ptr<CommandBufferManager> cmdManager;

    VkSurfaceFormatKHR surfaceFormat;
    VkPresentModeKHR presentModeVsync;
    VkPresentModeKHR presentModeAdaptive;
    VkPresentModeKHR presentModeMailbox;

    QrPresentMode requestedPresentMode;
    VkExtent2D surfaceExtent;
    QrPresentMode isPresentMode;

    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkSemaphore> renderFinishedSemaphores;

    bool usePresentFences;
    std::vector<VkFence> presentFences;
    std::vector<uint8_t> presentFencePending;

    bool presentWait2Supported;
    bool surfacePresentWait2Supported;
    bool usePresentWait2;
    uint64_t currentPresentId;
    uint64_t waitablePresentId;
    uint64_t maxFrameLatency;

    SwapchainRecreateState recreateState;

    uint32_t currentSwapchainIndex;

    std::list<std::weak_ptr<ISwapchainDependency>> subscribers;

    mutable VkSurfaceCapabilitiesKHR cachedSurfaceCaps;
    mutable VkResult cachedSurfaceCapsResult;
    mutable bool cachedSurfaceCapsValid;
    mutable bool cachedIsExtentOptimal;
};

}
