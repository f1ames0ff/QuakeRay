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

#include "CubemapUploader.h"

vkpt::CubemapUploader::CubemapUploader(VkDevice device, std::shared_ptr<MemoryAllocator> memAllocator)
    : TextureUploader(device, std::move(memAllocator))
{
}

vkpt::TextureUploader::UploadResult vkpt::CubemapUploader::UploadImage(const UploadInfo &info)
{
    assert(info.isCubemap);
    assert(!info.isUpdateable);

    constexpr uint32_t FaceCount = 6;

    const RgExtent2D &size = info.baseSize;

    UploadResult result{};
    result.wasUploaded = false;

    VkBuffer stagingBuffers[FaceCount] = {};
    void *mappedData[FaceCount] = {};

    const VkDeviceSize faceSize = static_cast<VkDeviceSize>(info.dataSize);

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = faceSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    for (uint32_t i = 0; i < FaceCount; i++)
    {
        stagingBuffers[i] = memAllocator->CreateStagingSrcTextureBuffer(&stagingInfo, info.pDebugName, &mappedData[i]);

        if (stagingBuffers[i] == VK_NULL_HANDLE)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffers[j]);
            }

            return result;
        }

        SET_DEBUG_NAME(device, stagingBuffers[i], VK_OBJECT_TYPE_BUFFER, info.pDebugName);
    }

    VkImage image = VK_NULL_HANDLE;
    const bool wasCreated = CreateImage(info, &image);

    if (!wasCreated)
    {
        for (uint32_t i = 0; i < FaceCount; i++)
        {
            memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffers[i]);
        }

        return result;
    }

    for (uint32_t i = 0; i < FaceCount; i++)
    {
        memcpy(mappedData[i], info.cubemap.pFaces[i], faceSize);
    }

    PrepareImage(image, stagingBuffers, info, ImagePrepareType::INIT);

    const VkImageView imageView = CreateImageView(
        image, info.format, info.isCubemap, GetMipmapCount(size, info),
        RG_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE);

    SET_DEBUG_NAME(device, imageView, VK_OBJECT_TYPE_IMAGE_VIEW, info.pDebugName);

    for (uint32_t i = 0; i < FaceCount; i++)
    {
        stagingToFree[info.frameIndex].push_back(stagingBuffers[i]);
    }

    result.wasUploaded = true;
    result.image = image;
    result.view = imageView;
    return result;
}
