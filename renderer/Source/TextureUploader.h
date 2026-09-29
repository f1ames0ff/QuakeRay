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

#pragma once

#include <optional>
#include <vector>

#include "Common.h"
#include "MemoryAllocator.h"
#include "qray/qray.h"

namespace qray
{

class TextureUploader
{
public:
    struct UploadResult
    {
        bool                wasUploaded;
        VkImage             image;
        VkImageView         view;
    };

    struct UploadInfo
    {
        VkCommandBuffer cmd;
        uint32_t        frameIndex;
        const void*     pData;
        uint32_t        dataSize;
        struct
        {
            const void* pFaces[ 6 ];
        } cubemap;
        QrExtent2D baseSize;
        VkFormat   format;
        bool       useMipmaps;
        uint32_t                            pregeneratedLevelCount;
        const uint32_t*                     pLevelDataOffsets;
        const uint32_t*                     pLevelDataSizes;
        bool                                isUpdateable;
        const char*                         pDebugName;
        bool                                isCubemap;
        std::optional< QrTextureSwizzling > swizzling;
    };

public:
    TextureUploader(VkDevice device, std::shared_ptr<MemoryAllocator> memAllocator);
    virtual ~TextureUploader();

    TextureUploader(const TextureUploader &other) = delete;
    TextureUploader(TextureUploader &&other) noexcept = delete;
    TextureUploader &operator=(const TextureUploader &other) = delete;
    TextureUploader &operator=(TextureUploader &&other) noexcept = delete;

    void ClearStaging(uint32_t frameIndex);

    virtual UploadResult UploadImage(const UploadInfo &info);
    void UpdateImage(VkCommandBuffer cmd, VkImage targetImage, const void *data);
    void DestroyImage(VkImage image, VkImageView view);

    bool CanUpdateImageFromHostData(VkImage image) const;

protected:
    enum class ImagePrepareType
    {
        INIT,
        INIT_WITHOUT_COPYING,
        UPDATE
    };

protected:
    bool DoesFormatSupportBlit(VkFormat format) const;
    bool AreMipmapsPregenerated(const UploadInfo &info) const;
    uint32_t GetMipmapCount(const QrExtent2D &size, const UploadInfo &info) const;

    static void PrepareMipmaps(
        VkCommandBuffer cmd, VkImage image,
        uint32_t baseWidth, uint32_t baseHeight, uint32_t mipmapCount, uint32_t layerCount);

    static void CopyStagingToImage(
        VkCommandBuffer cmd, VkBuffer staging, VkImage image, const QrExtent2D &size, uint32_t baseLayer, uint32_t layerCount);
    void CopyStagingToImageMipmaps(
        VkCommandBuffer cmd, VkBuffer staging, VkImage image, uint32_t layerIndex, const UploadInfo &info);

    bool CreateImage(const UploadInfo &info, VkImage *result);
    void PrepareImage(VkImage image, VkBuffer staging[], const UploadInfo &info, ImagePrepareType prepareType);
    VkImageView CreateImageView( VkImage                             image,
                                 VkFormat                            format,
                                 bool                                isCubemap,
                                 uint32_t                            mipmapCount,
                                 std::optional< QrTextureSwizzling > swizzling );

private:
    struct UpdateableImageInfo
    {
        VkBuffer    stagingBuffer;
        void        *mappedData;
        uint32_t    dataSize;
        QrExtent2D  imageSize;
        bool        generateMipmaps;
        VkFormat    format;
        bool        pregenerated;
    };

protected:
    VkDevice device;

    std::shared_ptr<MemoryAllocator> memAllocator;

    std::vector<VkBuffer> stagingToFree[MAX_FRAMES_IN_FLIGHT];

    rgl::unordered_map<VkImage, UpdateableImageInfo> updateableImageInfos;
};

}
