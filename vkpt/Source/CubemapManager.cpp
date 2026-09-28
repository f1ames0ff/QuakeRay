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

#include "CubemapManager.h"

#include "Generated/ShaderCommonC.h"
#include "Const.h"
#include "TextureOverrides.h"
#include "RgException.h"

namespace
{
    constexpr uint32_t MAX_CUBEMAP_COUNT = 32;

    constexpr uint32_t MATERIAL_COLOR_TEXTURE_INDEX = 0;
    static_assert(MATERIAL_COLOR_TEXTURE_INDEX < vkpt::TEXTURES_PER_MATERIAL_COUNT);

    template <typename T>
    constexpr const T *DefaultIfNull(const T *pData, const T *pDefault)
    {
        return pData != nullptr ? pData : pDefault;
    }
}


vkpt::CubemapManager::CubemapManager(
    VkDevice _device,
    std::shared_ptr<MemoryAllocator> _allocator,
    std::shared_ptr<SamplerManager> _samplerManager,
    const std::shared_ptr<CommandBufferManager> &_cmdManager,
    std::shared_ptr<UserFileLoad> _userFileLoad,
    const RgInstanceCreateInfo &_info,
    const LibraryConfig::Config &_config
)
    : device(_device)
    , allocator(std::move(_allocator))
    , samplerManager(std::move(_samplerManager))
    , cubemaps(MAX_CUBEMAP_COUNT)
    , defaultTexturesPath(
        DefaultIfNull(_info.pOverridenTexturesFolderPath, DEFAULT_TEXTURES_PATH)
        )
    , overridenTexturePostfix(
        DefaultIfNull(_info.pOverridenAlbedoAlphaTexturePostfix, DEFAULT_TEXTURE_POSTFIX_ALBEDO_ALPHA)
        )
{
    if (_config.developerMode && _info.pOverridenTexturesFolderPathDeveloper != nullptr)
    {
        defaultTexturesPath = _info.pOverridenTexturesFolderPathDeveloper;
    }

    imageLoader = std::make_shared<ImageLoader>(std::move(_userFileLoad));
    cubemapDesc = std::make_shared<TextureDescriptors>(device, samplerManager, MAX_CUBEMAP_COUNT, BINDING_CUBEMAPS, BINDING_CUBEMAPS_SAMPLER);
    cubemapUploader = std::make_shared<CubemapUploader>(device, allocator);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        cubemapsToUpdateDescMarked[i].assign(MAX_CUBEMAP_COUNT, 0);
    }

    MarkAllDescDirty();

    VkCommandBuffer cmd = _cmdManager->StartGraphicsCmd();
    CreateEmptyCubemap(cmd);
    _cmdManager->Submit(cmd);
    _cmdManager->WaitGraphicsIdle();
}

void vkpt::CubemapManager::CreateEmptyCubemap(VkCommandBuffer cmd)
{
    const uint32_t whitePixel = 0xFFFFFFFF;

    RgCubemapCreateInfo info = {};
    info.sideSize = 1;
    info.useMipmaps = 0;
    info.filter = RG_SAMPLER_FILTER_NEAREST;

    for (uint32_t i = 0; i < 6; i++)
    {
        info.pData[i] = &whitePixel;
    }

    const uint32_t index = CreateCubemap(cmd, 0, info);
    assert(index == RG_EMPTY_CUBEMAP);

    cubemapDesc->SetEmptyTextureInfo(cubemaps[RG_EMPTY_CUBEMAP].view);
}

vkpt::CubemapManager::~CubemapManager()
{
    const auto destroyTextures = [this](std::vector<Texture> &textures)
    {
        for (auto &texture : textures)
        {
            assert((texture.image == VK_NULL_HANDLE && texture.view == VK_NULL_HANDLE) ||
                   (texture.image != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE));

            if (texture.image != VK_NULL_HANDLE)
            {
                cubemapUploader->DestroyImage(texture.image, texture.view);
            }
        }
    };

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        destroyTextures(cubemapsToDestroy[i]);
    }

    destroyTextures(cubemaps);
}

uint32_t vkpt::CubemapManager::CreateCubemap(VkCommandBuffer cmd, uint32_t frameIndex, const RgCubemapCreateInfo &info)
{
    using namespace std::string_literals;

    const auto slotIt = std::find_if(cubemaps.begin(), cubemaps.end(), [] (const Texture &texture)
    {
        assert((texture.image == VK_NULL_HANDLE && texture.view == VK_NULL_HANDLE) ||
               (texture.image != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE));

        return texture.image == VK_NULL_HANDLE && texture.view == VK_NULL_HANDLE;
    });

    TextureUploader::UploadInfo upload = {};
    upload.cmd = cmd;
    upload.frameIndex = frameIndex;
    upload.useMipmaps = info.useMipmaps != 0;
    upload.isUpdateable = false;
    upload.pDebugName = nullptr;
    upload.isCubemap = true;

    static_assert(MATERIAL_COLOR_TEXTURE_INDEX == 0);

    TextureOverrides::OverrideInfo parseInfo = {};
    parseInfo.commonFolderPath = defaultTexturesPath.c_str();
    parseInfo.postfixes[0] = overridenTexturePostfix.c_str();
    parseInfo.postfixes[1] = "";
    parseInfo.postfixes[2] = "";
    parseInfo.overridenIsSRGB[0] = true;
    parseInfo.overridenIsSRGB[1] = false;
    parseInfo.overridenIsSRGB[2] = false;
    parseInfo.originalIsSRGB[0] = true;
    parseInfo.originalIsSRGB[1] = false;
    parseInfo.originalIsSRGB[2] = false;

    const RgExtent2D size = { info.sideSize, info.sideSize };

    TextureOverrides ovrd0(info.pRelativePaths[0], RgTextureSet{ .pDataAlbedoAlpha = info.pData[0] }, size, parseInfo, imageLoader.get());
    TextureOverrides ovrd1(info.pRelativePaths[1], RgTextureSet{ .pDataAlbedoAlpha = info.pData[1] }, size, parseInfo, imageLoader.get());
    TextureOverrides ovrd2(info.pRelativePaths[2], RgTextureSet{ .pDataAlbedoAlpha = info.pData[2] }, size, parseInfo, imageLoader.get());
    TextureOverrides ovrd3(info.pRelativePaths[3], RgTextureSet{ .pDataAlbedoAlpha = info.pData[3] }, size, parseInfo, imageLoader.get());
    TextureOverrides ovrd4(info.pRelativePaths[4], RgTextureSet{ .pDataAlbedoAlpha = info.pData[4] }, size, parseInfo, imageLoader.get());
    TextureOverrides ovrd5(info.pRelativePaths[5], RgTextureSet{ .pDataAlbedoAlpha = info.pData[5] }, size, parseInfo, imageLoader.get());

    TextureOverrides *overrides[6] =
    {
        &ovrd0,
        &ovrd1,
        &ovrd2,
        &ovrd3,
        &ovrd4,
        &ovrd5,
    };

    bool useOverrides = true;

    RgExtent2D commonSize = {};
    VkFormat commonFormat = VK_FORMAT_UNDEFINED;

    if (const auto &firstAlbedo = overrides[0]->GetResult(MATERIAL_COLOR_TEXTURE_INDEX))
    {
        commonSize = { firstAlbedo->baseSize.width, firstAlbedo->baseSize.height };
        commonFormat = firstAlbedo->format;
    }
    else
    {
        useOverrides = false;
    }

    for (TextureOverrides *o : overrides)
    {
        if (const auto &albedo = o->GetResult(MATERIAL_COLOR_TEXTURE_INDEX))
        {
            const char *debugName = o->GetDebugName();
            assert(albedo->pData != nullptr);

            const RgExtent2D &faceSize = albedo->baseSize;

            if (albedo->format != commonFormat)
            {
                throw RgException(RG_WRONG_ARGUMENT, "Cubemap must have the same format on each face. Failed on: "s + debugName);
            }

            if (faceSize.width != faceSize.height)
            {
                throw RgException(RG_WRONG_ARGUMENT, "Cubemap must have square face size: "s + debugName + " has (" + std::to_string(faceSize.width) + ", " + std::to_string(faceSize.height) + ")");
            }

            if (faceSize.width != commonSize.width || faceSize.height != commonSize.height)
            {
                throw RgException(RG_WRONG_ARGUMENT,
                    "Cubemap faces must have the same size: "s +
                    debugName + " has (" + std::to_string(faceSize.width) + ", " + std::to_string(faceSize.height) + ")"
                    "but expected (" + std::to_string(commonSize.width) + ", " + std::to_string(commonSize.height) + ") like on " + debugName);
            }
        }
        else
        {
            useOverrides = false;
        }
    }

    if (useOverrides)
    {
        upload.pDebugName = overrides[0]->GetDebugName();

        for (uint32_t i = 0; i < 6; i++)
        {
            upload.cubemap.pFaces[i] = overrides[i]->GetResult(MATERIAL_COLOR_TEXTURE_INDEX)->pData;
        }
    }
    else
    {
        commonSize = { info.sideSize, info.sideSize };
        commonFormat = VK_FORMAT_R8G8B8A8_SRGB;

        if (info.sideSize == 0)
        {
            throw RgException(RG_WRONG_ARGUMENT, "Cubemap's side size must be non-zero");
        }

        for (uint32_t i = 0; i < 6; i++)
        {
            if (info.pData[i] == nullptr)
            {
                return RG_EMPTY_CUBEMAP;
            }

            upload.cubemap.pFaces[i] = info.pData[i];
        }
    }

    upload.format = commonFormat;

    if (commonFormat != VK_FORMAT_R8G8B8A8_SRGB && commonFormat != VK_FORMAT_R8G8B8A8_UNORM)
    {
        assert(false && "For now, cubemaps only support only R8G8B8A8 formats!");
        return RG_EMPTY_CUBEMAP;
    }

    upload.baseSize = commonSize;
    upload.dataSize = 4 * commonSize.width * commonSize.height;

    const TextureUploader::UploadResult result = cubemapUploader->UploadImage(upload);

    if (!result.wasUploaded)
    {
        return RG_EMPTY_CUBEMAP;
    }

    slotIt->image = result.image;
    slotIt->view = result.view;
    slotIt->samplerHandle = SamplerManager::Handle(info.filter, RG_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, RG_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);

    const uint32_t cubemapIndex = static_cast<uint32_t>(std::distance(cubemaps.begin(), slotIt));

    MarkDescDirty(cubemapIndex);

    return cubemapIndex;
}

void vkpt::CubemapManager::DestroyCubemap(uint32_t frameIndex, uint32_t cubemapIndex)
{
    if (cubemapIndex >= MAX_CUBEMAP_COUNT)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Wrong cubemap ID=" + std::to_string(cubemapIndex));
    }

    Texture &texture = cubemaps[cubemapIndex];

    if (texture.image == VK_NULL_HANDLE)
    {
        return;
    }

    cubemapsToDestroy[frameIndex].push_back(texture);

    texture.image = VK_NULL_HANDLE;
    texture.view = VK_NULL_HANDLE;
    texture.samplerHandle = SamplerManager::Handle();

    MarkDescDirty(cubemapIndex);
}

VkDescriptorSetLayout vkpt::CubemapManager::GetDescSetLayout() const
{
    return cubemapDesc->GetDescSetLayout();
}

VkDescriptorSet vkpt::CubemapManager::GetDescSet(uint32_t frameIndex) const
{
    return cubemapDesc->GetDescSet(frameIndex);
}

void vkpt::CubemapManager::PrepareForFrame(uint32_t frameIndex)
{
    for (auto &texture : cubemapsToDestroy[frameIndex])
    {
        vkDestroyImage(device, texture.image, nullptr);
        vkDestroyImageView(device, texture.view, nullptr);
    }

    cubemapsToDestroy[frameIndex].clear();

    cubemapUploader->ClearStaging(frameIndex);
}

void vkpt::CubemapManager::MarkDescDirty(uint32_t cubemapIndex)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (cubemapsToUpdateDescMarked[i][cubemapIndex] == 0)
        {
            cubemapsToUpdateDescMarked[i][cubemapIndex] = 1;
            cubemapsToUpdateDesc[i].push_back(cubemapIndex);
        }
    }
}

void vkpt::CubemapManager::MarkAllDescDirty()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        auto &dirty = cubemapsToUpdateDesc[i];

        dirty.clear();
        dirty.reserve(cubemaps.size());

        for (uint32_t c = 0; c < cubemaps.size(); c++)
        {
            dirty.push_back(c);
        }

        std::fill(cubemapsToUpdateDescMarked[i].begin(), cubemapsToUpdateDescMarked[i].end(), 1);
    }
}

void vkpt::CubemapManager::SubmitDescriptors(uint32_t frameIndex)
{
    auto &dirty = cubemapsToUpdateDesc[frameIndex];

    const bool hasDescWrites = !dirty.empty();

    for (uint32_t i : dirty)
    {
        if (cubemaps[i].image != VK_NULL_HANDLE)
        {
            cubemapDesc->UpdateTextureDesc(frameIndex, i, cubemaps[i].view, cubemaps[i].samplerHandle);
        }
        else
        {
            cubemapDesc->ResetTextureDesc(frameIndex, i);
        }

        cubemapsToUpdateDescMarked[frameIndex][i] = 0;
    }

    dirty.clear();

    if (hasDescWrites)
    {
        cubemapDesc->FlushDescWrites();
    }
}

bool vkpt::CubemapManager::IsCubemapValid(uint32_t cubemapIndex) const
{
    return cubemapIndex < MAX_CUBEMAP_COUNT;
}
