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

#include <array>
#include <list>
#include <vector>

#include "Common.h"
#include "CommandBufferManager.h"
#include "ISwapchainDependency.h"
#include "IFramebuffersDependency.h"
#include "MemoryAllocator.h"
#include "SamplerManager.h"
#include "Generated/ShaderCommonCFramebuf.h"

namespace qray
{

#define FRAMEBUFFERS_HISTORY_LENGTH 2

class Framebuffers
{
public:
    explicit Framebuffers( VkDevice                                device,
                           std::shared_ptr< MemoryAllocator >      allocator,
                           std::shared_ptr< CommandBufferManager > cmdManager,
                           const QrInstanceCreateInfo&             info );
    ~Framebuffers();

    Framebuffers(const Framebuffers &other) = delete;
    Framebuffers(Framebuffers &&other) noexcept = delete;
    Framebuffers &operator=(const Framebuffers &other) = delete;
    Framebuffers &operator=(Framebuffers &&other) noexcept = delete;

    bool PrepareForSize(ResolutionState resolutionState);

    enum class BarrierType { All, Storage, ColorAttachment, Transfer };

    template <uint32_t BARRIER_COUNT>
    void BarrierMultiple(VkCommandBuffer cmd,
                         uint32_t frameIndex,
                         const FramebufferImageIndex (&framebufImageIndices)[BARRIER_COUNT],
                         BarrierType barrierTypeFrom = BarrierType::All);

    VkImage GetImage(FramebufferImageIndex fbImageIndex, uint32_t frameIndex) const;
    std::tuple<VkImage, VkImageView, VkFormat> GetImageHandles(FramebufferImageIndex fbImageIndex, uint32_t frameIndex) const;
    std::tuple<VkImage, VkImageView, VkFormat, VkExtent2D> GetImageHandles(FramebufferImageIndex fbImageIndex, uint32_t frameIndex, const ResolutionState &resolutionState) const;

    std::tuple<VkImage, VkImageView, VkFormat> GetScreenEmissionHandles(uint32_t frameIndex) const;
    std::tuple<VkImage, VkImageView, VkFormat> GetPrimaryToReflRefrHandles(uint32_t frameIndex) const;

    void Subscribe(std::shared_ptr<IFramebuffersDependency> subscriber);
    void Unsubscribe(const IFramebuffersDependency *subscriber);

private:
    static FramebufferImageIndex FrameIndexToFBIndex(FramebufferImageIndex framebufferImageIndex, uint32_t frameIndex);

    void CreateImages(ResolutionState resolutionState);

    VkExtent2D GetFramebufSize( const ResolutionState& resolutionState,
                                FramebufferImageIndex  index ) const;

    void DestroyImages();

    void NotifySubscribersAboutResize(const ResolutionState &resolutionState);

private:
    VkDevice device;
    bool     effectWipeIsUsed;

    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;

    ResolutionState currentResolution;

    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> imageMemories;
    std::vector<VkImageView> imageViews;

    std::list<std::weak_ptr<IFramebuffersDependency>> subscribers;
};



template<uint32_t BARRIER_COUNT>
inline void Framebuffers::BarrierMultiple(VkCommandBuffer cmd, uint32_t frameIndex, const FramebufferImageIndex(&framebufImageIndices)[BARRIER_COUNT], BarrierType barrierTypeFrom)
{
    VkAccessFlags2KHR srcAccess = 0;
    VkPipelineStageFlags2KHR srcStage = 0;

    switch (barrierTypeFrom)
    {
        case BarrierType::All:
            srcAccess = VK_ACCESS_2_SHADER_WRITE_BIT_KHR | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT_KHR | VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
            srcStage =
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT_KHR |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT_KHR;
            break;
        case BarrierType::Storage:
            srcAccess = VK_ACCESS_2_SHADER_WRITE_BIT_KHR;
            srcStage =
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT_KHR |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
            break;
        case BarrierType::ColorAttachment:
            srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT_KHR;
            srcStage =
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT_KHR;
            break;
        case BarrierType::Transfer:
            srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
            srcStage =
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT_KHR;
            break;
        default: assert(0);
    }

    const VkAccessFlags2KHR dstAccess =
        VK_ACCESS_2_SHADER_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT_KHR |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT_KHR | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT_KHR |
        VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR | VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
    const VkPipelineStageFlags2KHR dstStage =
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT_KHR |
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT_KHR;

    std::array<VkImageMemoryBarrier2KHR, BARRIER_COUNT> barriers = {};

    for (uint32_t i = 0; i < BARRIER_COUNT; i++)
    {
        VkImageMemoryBarrier2KHR &barrier = barriers[i];

        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
        barrier.image = images[FrameIndexToFBIndex(framebufImageIndices[i], frameIndex)];
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.srcStageMask = srcStage;
        barrier.dstStageMask = dstStage;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkImageSubresourceRange &subresource = barrier.subresourceRange;
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource.baseMipLevel = 0;
        subresource.levelCount = 1;
        subresource.baseArrayLayer = 0;
        subresource.layerCount = 1;
    }

    VkDependencyInfoKHR dependencyInfo = {};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependencyInfo.pImageMemoryBarriers = barriers.data();

    svkCmdPipelineBarrier2KHR(cmd, &dependencyInfo);
}

}
