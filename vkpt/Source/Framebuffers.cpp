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

#include "Framebuffers.h"

using namespace vkpt;

#include "Utils.h"

static_assert(MAX_FRAMES_IN_FLIGHT == FRAMEBUFFERS_HISTORY_LENGTH, "Framebuffers class logic must be changed if history length is not equal to max frames in flight");

FramebufferImageIndex Framebuffers::FrameIndexToFBIndex(FramebufferImageIndex framebufferImageIndex, uint32_t frameIndex)
{
    assert(frameIndex < FRAMEBUFFERS_HISTORY_LENGTH);
    assert(framebufferImageIndex >= 0 && framebufferImageIndex < ShFramebuffers_Count);

    // if framebuffer with given index can be swapped,
    // use one that is currently in use
    if (ShFramebuffers_Bindings[framebufferImageIndex] != ShFramebuffers_BindingsSwapped[framebufferImageIndex])
    {
        // Apply the actual swap permutation instead of a linear increment.
        // `+frameIndex` is only correct for the first (current) index of each
        // swapped pair; for the `_Prev` (second) index it pointed at the next,
        // unrelated framebuffer (e.g. Q2_VIEW_DEPTH_PREV + 1 == Q2_BASE_COLOR),
        // so barriers and image-handle lookups hit the wrong image.
        return frameIndex == 0
            ? (FramebufferImageIndex)ShFramebuffers_Bindings[framebufferImageIndex]
            : (FramebufferImageIndex)ShFramebuffers_BindingsSwapped[framebufferImageIndex];
    }

    return framebufferImageIndex;
}

Framebuffers::Framebuffers( VkDevice                                _device,
                            std::shared_ptr< MemoryAllocator >      _allocator,
                            std::shared_ptr< CommandBufferManager > _cmdManager,
                            const RgInstanceCreateInfo&             info )
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

bool vkpt::Framebuffers::PrepareForSize(ResolutionState resolutionState)
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

std::tuple<VkImage, VkImageView, VkFormat> vkpt::Framebuffers::GetImageHandles(FramebufferImageIndex fbImageIndex, uint32_t frameIndex) const
{
    fbImageIndex = FrameIndexToFBIndex(fbImageIndex, frameIndex);

    return std::make_tuple(images[fbImageIndex], imageViews[fbImageIndex], ShFramebuffers_Formats[fbImageIndex]);
}

std::tuple<VkImage, VkImageView, VkFormat, VkExtent2D> Framebuffers::GetImageHandles(
    FramebufferImageIndex fbImageIndex, uint32_t frameIndex, const ResolutionState&resolutionState) const
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

VkExtent2D vkpt::Framebuffers::GetFramebufSize( const ResolutionState& resolutionState,
                                                 FramebufferImageIndex  index ) const
{
    if( index == FramebufferImageIndex::FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE )
    {
        if( !effectWipeIsUsed )
        {
            return { 1, 1 };
        }
    }


    FramebufferImageFlags flags = ShFramebuffers_Flags[ index ];

    if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_UPSCALED_SIZE)
    {
        return { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
    }

    if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_SINGLE_PIXEL_SIZE)
    {
        return { 1,1 };
    }

    int downscale = 1;

    if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_2)
    {
        downscale = 2;
    }
    else if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_3)
    {
        downscale = 3;
    }
    else if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_4)
    {
        downscale = 4;
    }
    else if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_8)
    {
        downscale = 8;
    }
    else if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_16)
    {
        downscale = 16;
    }
    else if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_1_32)
    {
        downscale = 32;
    }
    else
    {
        return { resolutionState.renderWidth, resolutionState.renderHeight };
    }

    VkExtent2D extent;

    extent.width  = (resolutionState.renderWidth  + 1) / downscale;
    extent.height = (resolutionState.renderHeight + 1) / downscale;

    extent.width  = std::max(1u, extent.width);
    extent.height = std::max(1u, extent.height);

    return extent;
}

void Framebuffers::CreateImages(ResolutionState resolutionState)
{
    VkResult r;

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkFormat format = ShFramebuffers_Formats[i];
        FramebufferImageFlags flags = ShFramebuffers_Flags[i];

        const VkExtent2D extent =
            GetFramebufSize( resolutionState, static_cast< FramebufferImageIndex >( i ) );

        // create image
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1};
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

        r = vkCreateImage(device, &imageInfo, nullptr, &images[i]);
        VK_CHECKERROR(r);

        // allocate dedicated memory
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, images[i], &memReqs);

        imageMemories[i] = allocator->AllocDedicated(memReqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, MemoryAllocator::AllocType::DEFAULT, ShFramebuffers_DebugNames[i]);

        r = vkBindImageMemory(device, images[i], imageMemories[i], 0);
        VK_CHECKERROR(r);

        // create image view
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

        // to general layout
        Utils::BarrierImage(
            cmd, images[i],
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }

    // image creation happens rarely
    cmdManager->Submit(cmd);
    cmdManager->WaitGraphicsIdle();

    currentResolution = resolutionState;



    NotifySubscribersAboutResize(resolutionState);
}

void Framebuffers::DestroyImages()
{
    for (auto &i : images)
    {
        if (i != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, i, nullptr);
            i = VK_NULL_HANDLE;
        }
    }

    for (auto &m : imageMemories)
    {
        if (m != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
    }

    for (auto &v : imageViews)
    {
        if (v != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
}

void Framebuffers::NotifySubscribersAboutResize(const ResolutionState &resolutionState)
{
    for (auto &ws : subscribers)
    {
        if (auto s = ws.lock())
        {
            s->OnFramebuffersSizeChange(resolutionState);
        }
    }
}

void Framebuffers::Subscribe(std::shared_ptr<IFramebuffersDependency> subscriber)
{
    subscribers.emplace_back(subscriber);
}

void Framebuffers::Unsubscribe(const IFramebuffersDependency *subscriber)
{
    subscribers.remove_if([subscriber] (const std::weak_ptr<IFramebuffersDependency> &ws)
    {
        if (const auto s = ws.lock())
        {
            return s.get() == subscriber;
        }

        return true;
    });
}
