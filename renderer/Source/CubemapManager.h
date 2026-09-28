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

#include <vector>
#include <string>

#include "qray/qray.h"
#include "Common.h"
#include "Material.h"
#include "MemoryAllocator.h"
#include "SamplerManager.h"
#include "TextureDescriptors.h"
#include "CubemapUploader.h"
#include "CommandBufferManager.h"
#include "ImageLoader.h"
#include "LibraryConfig.h"

namespace qray
{

class CubemapManager
{
public:
    CubemapManager(
        VkDevice device,
        std::shared_ptr<MemoryAllocator> allocator,
        std::shared_ptr<SamplerManager> samplerManager,
        const std::shared_ptr<CommandBufferManager> &cmdManager,
        std::shared_ptr<UserFileLoad> userFileLoad,
        const QrInstanceCreateInfo &info,
        const LibraryConfig::Config &config);
    ~CubemapManager();

    CubemapManager(const CubemapManager &other) = delete;
    CubemapManager(CubemapManager &&other) noexcept = delete;
    CubemapManager &operator=(const CubemapManager &other) = delete;
    CubemapManager &operator=(CubemapManager &&other) noexcept = delete;

    uint32_t CreateCubemap(VkCommandBuffer cmd, uint32_t frameIndex, const QrCubemapCreateInfo &info);
    void DestroyCubemap(uint32_t frameIndex, uint32_t cubemapIndex);

    VkDescriptorSetLayout GetDescSetLayout() const;
    VkDescriptorSet GetDescSet(uint32_t frameIndex) const;

    void PrepareForFrame(uint32_t frameIndex);
    void SubmitDescriptors(uint32_t frameIndex);

    bool IsCubemapValid(uint32_t cubemapIndex) const;

private:
    void CreateEmptyCubemap(VkCommandBuffer cmd);

    void MarkDescDirty(uint32_t cubemapIndex);
    void MarkAllDescDirty();

private:
    VkDevice device;

    std::shared_ptr<MemoryAllocator>    allocator;
    std::shared_ptr<ImageLoader>        imageLoader;
    std::shared_ptr<SamplerManager>     samplerManager;
    std::shared_ptr<TextureDescriptors> cubemapDesc;
    std::shared_ptr<CubemapUploader>    cubemapUploader;

    std::vector<Texture>    cubemaps;
    std::vector<Texture>    cubemapsToDestroy[MAX_FRAMES_IN_FLIGHT];

    std::vector<uint32_t>   cubemapsToUpdateDesc[MAX_FRAMES_IN_FLIGHT];

    std::vector<uint8_t>    cubemapsToUpdateDescMarked[MAX_FRAMES_IN_FLIGHT];

    std::string defaultTexturesPath;
    std::string overridenTexturePostfix;
};

}
