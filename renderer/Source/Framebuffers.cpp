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

#include "Framebuffers.h"

using namespace qray;

#include "Utils.h"

static_assert(MAX_FRAMES_IN_FLIGHT == FRAMEBUFFERS_HISTORY_LENGTH, "Framebuffers class logic must be changed if history length is not equal to max frames in flight");

namespace
{
    int GetDownscaleFactor(FramebufferImageFlags flags)
    {
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_2)  { return 2; }
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_3)  { return 3; }
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_4)  { return 4; }
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_8)  { return 8; }
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_16) { return 16; }
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_32) { return 32; }

        return 1;
    }
}

FramebufferImageIndex Framebuffers::FrameIndexToFBIndex(FramebufferImageIndex framebufferImageIndex, uint32_t frameIndex)
{
    assert(frameIndex < FRAMEBUFFERS_HISTORY_LENGTH);
    assert(framebufferImageIndex >= 0 && framebufferImageIndex < ShFramebuffers_Count);

    const bool isSwapped =
        ShFramebuffers_Bindings[framebufferImageIndex] != ShFramebuffers_BindingsSwapped[framebufferImageIndex];

    if (!isSwapped)
    {
        return framebufferImageIndex;
    }

    return frameIndex == 0
        ? static_cast<FramebufferImageIndex>(ShFramebuffers_Bindings[framebufferImageIndex])
        : static_cast<FramebufferImageIndex>(ShFramebuffers_BindingsSwapped[framebufferImageIndex]);
}

Framebuffers::Framebuffers( VkDevice                                _device,
                            std::shared_ptr< MemoryAllocator >      _allocator,
                            std::shared_ptr< CommandBufferManager > _cmdManager,
                            const QrInstanceCreateInfo&             info )
    : device( _device )
    , effectWipeIsUsed( info.effectWipeIsUsed )
    , allocator( std::move( _allocator ) )
    , cmdManager( std::move( _cmdManager ) )
    , currentResolution{}
{
    images.resize( ShFramebuffers_Count );
    imageMemories.resize( ShFramebuffers_Count );
    imageViews.resize( ShFramebuffers_Count );
}

Framebuffers::~Framebuffers()
{
    DestroyImages();
}

bool Framebuffers::PrepareForSize(ResolutionState resolutionState)
{
    if (currentResolution == resolutionState)
    {
        return false;
    }

    vkDeviceWaitIdle(device);

    DestroyImages();
    CreateImages(resolutionState);

    assert(currentResolution == resolutionState);
    return true;
}

VkImage Framebuffers::GetImage(FramebufferImageIndex fbImageIndex, uint32_t frameIndex) const
{
    fbImageIndex = FrameIndexToFBIndex(fbImageIndex, frameIndex);
    return images[fbImageIndex];
}

std::tuple<VkImage, VkImageView, VkFormat> Framebuffers::GetImageHandles(FramebufferImageIndex fbImageIndex, uint32_t frameIndex) const
{
    fbImageIndex = FrameIndexToFBIndex(fbImageIndex, frameIndex);

    return std::make_tuple(images[fbImageIndex], imageViews[fbImageIndex], ShFramebuffers_Formats[fbImageIndex]);
}

std::tuple<VkImage, VkImageView, VkFormat, VkExtent2D> Framebuffers::GetImageHandles(
    FramebufferImageIndex fbImageIndex, uint32_t frameIndex, const ResolutionState &resolutionState) const
{
    auto [image, view, format] = GetImageHandles(fbImageIndex, frameIndex);

    return std::make_tuple( image, view, format, GetFramebufSize( resolutionState, fbImageIndex ) );
}

std::tuple<VkImage, VkImageView, VkFormat> Framebuffers::GetScreenEmissionHandles(uint32_t frameIndex) const
{
    return GetImageHandles(FB_IMAGE_INDEX_SCREEN_EMISSION, frameIndex);
}

std::tuple<VkImage, VkImageView, VkFormat> Framebuffers::GetPrimaryToReflRefrHandles(uint32_t frameIndex) const
{
    return GetImageHandles(FB_IMAGE_INDEX_PRIMARY_TO_REFL_REFR, frameIndex);
}

VkExtent2D Framebuffers::GetFramebufSize( const ResolutionState& resolutionState,
                                          FramebufferImageIndex  index ) const
{
    if (index == FramebufferImageIndex::FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE && !effectWipeIsUsed)
    {
        return { 1, 1 };
    }

    const FramebufferImageFlags flags = ShFramebuffers_Flags[ index ];

    if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_UPSCALED_SIZE)
    {
        return { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
    }

    if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_SINGLE_PIXEL_SIZE)
    {
        return { 1, 1 };
    }

    const int downscale = GetDownscaleFactor(flags);

    if (downscale == 1)
    {
        return { resolutionState.renderWidth, resolutionState.renderHeight };
    }

    return {
        std::max(1u, (resolutionState.renderWidth + 1) / downscale),
        std::max(1u, (resolutionState.renderHeight + 1) / downscale)
    };
}

void Framebuffers::CreateImages(ResolutionState resolutionState)
{
    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        const VkFormat format = ShFramebuffers_Formats[i];
        const FramebufferImageFlags flags = ShFramebuffers_Flags[i];

        const VkExtent2D extent =
            GetFramebufSize( resolutionState, static_cast< FramebufferImageIndex >( i ) );

        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_IS_ATTACHMENT)
        {
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }

        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_USAGE_TRANSFER)
        {
            imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }

        VkResult r = vkCreateImage(device, &imageInfo, nullptr, &images[i]);
        VK_CHECKERROR(r);

        VkMemoryRequirements memoryRequirements;
        vkGetImageMemoryRequirements(device, images[i], &memoryRequirements);

        imageMemories[i] = allocator->AllocDedicated(memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, MemoryAllocator::AllocType::DEFAULT, ShFramebuffers_DebugNames[i]);

        r = vkBindImageMemory(device, images[i], imageMemories[i], 0);
        VK_CHECKERROR(r);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {};
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        viewInfo.image = images[i];

        r = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, images[i], VK_OBJECT_TYPE_IMAGE, ShFramebuffers_DebugNames[i]);
        SET_DEBUG_NAME(device, imageViews[i], VK_OBJECT_TYPE_IMAGE_VIEW, ShFramebuffers_DebugNames[i]);

        Utils::BarrierImage(
            cmd, images[i],
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }

    cmdManager->Submit(cmd);
    cmdManager->WaitGraphicsIdle();

    currentResolution = resolutionState;

    NotifySubscribersAboutResize(resolutionState);
}

void Framebuffers::DestroyImages()
{
    for (VkImage &image : images)
    {
        if (image != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
    }

    for (VkDeviceMemory &memory : imageMemories)
    {
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }

    for (VkImageView &view : imageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
}

void Framebuffers::NotifySubscribersAboutResize(const ResolutionState &resolutionState)
{
    for (auto &weakSubscriber : subscribers)
    {
        if (auto subscriber = weakSubscriber.lock())
        {
            subscriber->OnFramebuffersSizeChange(resolutionState);
        }
    }
}

void Framebuffers::Subscribe(std::shared_ptr<IFramebuffersDependency> subscriber)
{
    subscribers.emplace_back(subscriber);
}

void Framebuffers::Unsubscribe(const IFramebuffersDependency *subscriber)
{
    subscribers.remove_if([subscriber](const std::weak_ptr<IFramebuffersDependency> &weakSubscriber)
    {
        if (const auto sharedSubscriber = weakSubscriber.lock())
        {
            return sharedSubscriber.get() == subscriber;
        }

        return true;
    });
}
