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

#include <list>
#include <string>

#include "Common.h"
#include "CommandBufferManager.h"
#include "Material.h"
#include "AutoBuffer.h"
#include "ImageLoader.h"
#include "ImageLoaderDev.h"
#include "IMaterialDependency.h"
#include "MemoryAllocator.h"
#include "SamplerManager.h"
#include "TextureDescriptors.h"
#include "TextureUploader.h"
#include "LibraryConfig.h"
#include "TextureObserver.h"

namespace qray
{

namespace rhi
{
class RhiTextureTable;
}

class TextureManager
{
public:
    explicit TextureManager(
        VkDevice device,
        std::shared_ptr<MemoryAllocator> memAllocator,
        std::shared_ptr<SamplerManager> samplerManager,
        const std::shared_ptr<CommandBufferManager> &cmdManager,
        std::shared_ptr<UserFileLoad> userFileLoad,
        const QrInstanceCreateInfo &info,
        const LibraryConfig::Config &config);
    ~TextureManager();

    TextureManager(const TextureManager &other) = delete;
    TextureManager(TextureManager &&other) noexcept = delete;
    TextureManager &operator=(const TextureManager &other) = delete;
    TextureManager &operator=(TextureManager &&other) noexcept = delete;

    void PrepareForFrame(uint32_t frameIndex);
    void SubmitDescriptors(uint32_t frameIndex,
                           const QrDrawFrameTexturesParams *pTexturesParams,
                           bool forceUpdateAllDescriptors = false);

    uint32_t CreateMaterial(VkCommandBuffer cmd, uint32_t frameIndex, const QrMaterialCreateInfo &createInfo);
    uint32_t CreateAnimatedMaterial(VkCommandBuffer cmd, uint32_t frameIndex, const QrAnimatedMaterialCreateInfo &createInfo);
    bool ChangeAnimatedMaterialFrame(uint32_t animMaterial, uint32_t materialFrame);
    bool UpdateMaterial(VkCommandBuffer cmd, uint32_t frameIndex, const QrMaterialUpdateInfo &updateInfo);
    bool CanUpdateMaterialContents(uint32_t materialIndex, QrExtent2D size) const;
    void DestroyMaterial(uint32_t currentFrameIndex, uint32_t materialIndex);

    void CheckForHotReload(VkCommandBuffer cmd, uint32_t frameIndex);

    MaterialTextures GetMaterialTextures(uint32_t materialIndex) const;

    VkBuffer GetTalCdfBuffer() const;

    static constexpr uint32_t GetEmptyTextureIndex();
    uint32_t GetWaterNormalTextureIndex() const;

    void SetRhiTextureTable(rhi::RhiTextureTable *pTable);

    VkDescriptorSet GetDescSet(uint32_t frameIndex) const;
    VkDescriptorSetLayout GetDescSetLayout() const;

    void Subscribe(std::shared_ptr<IMaterialDependency> subscriber);
    void Unsubscribe(const IMaterialDependency *subscriber);

private:
    void CreateEmptyTexture(VkCommandBuffer cmd, uint32_t frameIndex);
    void CreateWaterNormalTexture(VkCommandBuffer cmd, uint32_t frameIndex, const char *pFilePath);

    uint32_t PrepareTexture( VkCommandBuffer                                 cmd,
                             uint32_t                                        frameIndex,
                             const std::optional< ImageLoader::ResultInfo >& info,
                             SamplerManager::Handle                          samplerHandle,
                             bool                                            useMipmaps,
                             const char*                                     debugName,
                             bool                                            isUpdateable,
                             std::optional< QrTextureSwizzling >             swizzling );

    uint32_t InsertTexture(uint32_t frameIndex, VkImage image, VkImageView view,
                           SamplerManager::Handle samplerHandle, VkFormat format,
                           VkExtent2D baseSize, uint32_t mipLevels);
    void DestroyTexture(const Texture &texture);
    void AddToBeDestroyed(uint32_t frameIndex, const Texture &texture);

    void MarkDescDirty(uint32_t textureIndex);
    void MarkAllDescDirty();

    void RebuildTalCdf(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t textureIndex, const uint8_t *pData);

    uint32_t GenerateMaterialIndex(const MaterialTextures &materialTextures);
    uint32_t GenerateMaterialIndex(const std::vector<uint32_t> &materialIndices);

    uint32_t InsertMaterial(const MaterialTextures &materialTextures, bool isUpdateable);
    uint32_t InsertAnimatedMaterial(std::vector<uint32_t> &materialIndices);

    void DestroyMaterialTextures(uint32_t frameIndex, uint32_t materialIndex);
    void DestroyMaterialTextures(uint32_t frameIndex, const Material &material);

private:
    struct TalCdfSource
    {
        QrExtent2D  baseSize = {};
        VkFormat    format = VK_FORMAT_UNDEFINED;
        uint32_t    level0Size = 0;
    };

    VkDevice device;
    QrTextureSwizzling pbrSwizzling;

    std::shared_ptr<ImageLoader> imageLoader;

    std::shared_ptr<ImageLoaderDev> imageLoaderDev;
    std::shared_ptr<TextureObserver> observer;

    std::shared_ptr<SamplerManager> samplerMgr;
    std::shared_ptr<TextureDescriptors> textureDesc;
    std::shared_ptr<TextureUploader> textureUploader;

    std::shared_ptr<AutoBuffer> talCdfBuffer;
    std::vector<TalCdfSource> talCdfSources;

    std::vector<Texture> textures;
    rhi::RhiTextureTable *rhiTextureTable = nullptr;
    std::vector<Texture> texturesToDestroy[MAX_FRAMES_IN_FLIGHT];

    std::vector<uint32_t> texturesToUpdateDesc[MAX_FRAMES_IN_FLIGHT];

    std::vector<uint8_t> texturesToUpdateDescMarked[MAX_FRAMES_IN_FLIGHT];

    rgl::unordered_map<uint32_t, AnimatedMaterial> animatedMaterials;
    rgl::unordered_map<uint32_t, Material> materials;

    uint32_t waterNormalTextureIndex;

    QrSamplerFilter currentDynamicSamplerFilter;

    std::string defaultTexturesPath;
    std::string postfixes[TEXTURES_PER_MATERIAL_COUNT];
    bool overridenIsSRGB[TEXTURES_PER_MATERIAL_COUNT];
    bool originalIsSRGB[TEXTURES_PER_MATERIAL_COUNT];

    bool forceNormalMapFilterLinear;

    std::list<std::weak_ptr<IMaterialDependency>> subscribers;
};

inline constexpr uint32_t TextureManager::GetEmptyTextureIndex()
{
    return EMPTY_TEXTURE_INDEX;
}

}
