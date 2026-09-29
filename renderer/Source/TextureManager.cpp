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

#include "TextureManager.h"

#include <numeric>
#include <algorithm>
#include <cmath>

#include "Const.h"
#include "Utils.h"
#include "TextureOverrides.h"
#include "Generated/ShaderCommonC.h"
#include "QrException.h"
#include "RHI/RhiTextureTable.h"


using namespace qray;


namespace
{
    static_assert(TEXTURES_PER_MATERIAL_COUNT == sizeof(QrTextureSet) / sizeof(const void *), "TEXTURES_PER_MATERIAL_COUNT must be same as in QrTextureSet");

    constexpr MaterialTextures EmptyMaterialTextures = { EMPTY_TEXTURE_INDEX, EMPTY_TEXTURE_INDEX, EMPTY_TEXTURE_INDEX };

    constexpr QrSamplerFilter DefaultDynamicSamplerFilter = QR_SAMPLER_FILTER_LINEAR;

    template <typename T>
    constexpr const T *DefaultIfNull(const T *pData, const T *pDefault)
    {
        return pData != nullptr ? pData : pDefault;
    }

    TextureOverrides::Loader GetLoader(const std::shared_ptr<ImageLoader> &defaultLoader, const std::shared_ptr<ImageLoaderDev> devLoader)
    {
        return devLoader ? TextureOverrides::Loader(devLoader.get()) : TextureOverrides::Loader(defaultLoader.get());
    }

    bool IsTextureSetEmpty(const QrTextureSet &textures)
    {
        return textures.pDataAlbedoAlpha == nullptr &&
               textures.pDataRoughnessMetallicEmission == nullptr &&
               textures.pDataNormal == nullptr;
    }

    constexpr uint32_t TalCdfGridMaxSize = TAL_CDF_GRID_MAX_SIZE;

    bool GetTalCdfPixelLayout(VkFormat format, uint32_t *pBytesPerPixel, uint32_t *pEmissiveOffset)
    {
        switch (format)
        {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
                *pBytesPerPixel = 4;
                *pEmissiveOffset = 2;
                return true;

            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                *pBytesPerPixel = 4;
                *pEmissiveOffset = 0;
                return true;

            case VK_FORMAT_R8G8B8_UNORM:
            case VK_FORMAT_R8G8B8_SRGB:
                *pBytesPerPixel = 3;
                *pEmissiveOffset = 2;
                return true;

            case VK_FORMAT_B8G8R8_UNORM:
            case VK_FORMAT_B8G8R8_SRGB:
                *pBytesPerPixel = 3;
                *pEmissiveOffset = 0;
                return true;

            default:
                return false;
        }
    }

    uint32_t BuildTalCdfEntries(const uint8_t *pPixels, uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint32_t emissiveOffset,
                                uint32_t *pEntries, uint32_t maxEntries)
    {
        if (pPixels == nullptr || width == 0 || height == 0 || maxEntries == 0)
        {
            return 0;
        }

        const uint32_t gridWidth = std::min(width, TalCdfGridMaxSize);
        const uint32_t gridHeight = std::min(height, TalCdfGridMaxSize);

        std::vector<uint32_t> grid(static_cast<size_t>(gridWidth) * gridHeight, 0);

        for (uint32_t gy = 0; gy < gridHeight; gy++)
        {
            const uint32_t rowBegin = gy * height / gridHeight;
            const uint32_t rowEnd = std::max((gy + 1) * height / gridHeight, rowBegin + 1);

            for (uint32_t gx = 0; gx < gridWidth; gx++)
            {
                const uint32_t columnBegin = gx * width / gridWidth;
                const uint32_t columnEnd = std::max((gx + 1) * width / gridWidth, columnBegin + 1);

                uint32_t cellSum = 0;

                for (uint32_t y = rowBegin; y < rowEnd; y++)
                {
                    const uint8_t *pRow = pPixels + uint64_t(y) * width * bytesPerPixel;

                    for (uint32_t x = columnBegin; x < columnEnd; x++)
                    {
                        cellSum += pRow[x * bytesPerPixel + emissiveOffset];
                    }
                }

                grid[static_cast<size_t>(gy) * gridWidth + gx] = cellSum;
            }
        }

        uint64_t totalSum = 0;

        for (const uint32_t cellSum : grid)
        {
            totalSum += cellSum;
        }

        if (totalSum == 0)
        {
            return 0;
        }

        uint32_t cellIndex = 0;
        uint64_t cumulativeSum = 0;

        for (uint32_t entryIndex = 0; entryIndex < maxEntries; entryIndex++)
        {
            const uint64_t target = totalSum * (2 * uint64_t(entryIndex) + 1) / (2 * uint64_t(maxEntries));

            while (cumulativeSum < target && cellIndex < grid.size())
            {
                cumulativeSum += grid[cellIndex];
                cellIndex++;
            }

            const uint32_t cell = cellIndex > 0 ? cellIndex - 1 : 0;

            const uint32_t s = uint32_t((float(cell % gridWidth) + 0.5f) / float(gridWidth) * 65535.0f);
            const uint32_t t = uint32_t((float(cell / gridWidth) + 0.5f) / float(gridHeight) * 65535.0f);

            pEntries[entryIndex] = s | (t << 16);
        }

        return maxEntries;
    }

    uint32_t GetUploadedMipLevelCount(const ImageLoader::ResultInfo &imageInfo, bool useMipmaps)
    {
        if (!useMipmaps)
        {
            return 1;
        }

        if (imageInfo.isPregenerated)
        {
            return std::min(imageInfo.levelCount, MAX_PREGENERATED_MIPMAP_LEVELS);
        }

        const auto widthLevelCount = static_cast<uint32_t>(std::log2(imageInfo.baseSize.width));
        const auto heightLevelCount = static_cast<uint32_t>(std::log2(imageInfo.baseSize.height));

        return std::min(widthLevelCount, heightLevelCount) + 1;
    }
}


TextureManager::TextureManager( VkDevice                                       _device,
                                std::shared_ptr< MemoryAllocator >             _memAllocator,
                                std::shared_ptr< SamplerManager >              _samplerMgr,
                                const std::shared_ptr< CommandBufferManager >& _cmdManager,
                                std::shared_ptr< UserFileLoad >                _userFileLoad,
                                const QrInstanceCreateInfo&                    _info,
                                const LibraryConfig::Config&                   _config )
    : device( _device )
    , pbrSwizzling( _info.pbrTextureSwizzling )
    , samplerMgr( std::move( _samplerMgr ) )
    , waterNormalTextureIndex( 0 )
    , currentDynamicSamplerFilter( DefaultDynamicSamplerFilter )
    , defaultTexturesPath(
        DefaultIfNull(_info.pOverridenTexturesFolderPath, DEFAULT_TEXTURES_PATH)
        )
    , postfixes
        {
            DefaultIfNull(_info.pOverridenAlbedoAlphaTexturePostfix, DEFAULT_TEXTURE_POSTFIX_ALBEDO_ALPHA),
            DefaultIfNull(_info.pOverridenRoughnessMetallicEmissionTexturePostfix, DEFAULT_TEXTURE_POSTFIX_ROUGNESS_METALLIC_EMISSION),
            DefaultIfNull(_info.pOverridenNormalTexturePostfix, DEFAULT_TEXTURE_POSTFIX_NORMAL),
        }
    , overridenIsSRGB
        {
            !!_info.overridenAlbedoAlphaTextureIsSRGB,
            !!_info.overridenRoughnessMetallicEmissionTextureIsSRGB,
            !!_info.overridenNormalTextureIsSRGB,
        }
    , originalIsSRGB
        {
            !!_info.originalAlbedoAlphaTextureIsSRGB,
            !!_info.originalRoughnessMetallicEmissionTextureIsSRGB,
            !!_info.originalNormalTextureIsSRGB,
        }
    , forceNormalMapFilterLinear( !!_info.textureSamplerForceNormalMapFilterLinear )
{
    const uint32_t maxTextureCount =
        std::clamp( _info.maxTextureCount, TEXTURE_COUNT_MIN, TEXTURE_COUNT_MAX );

    talCdfSources.resize( maxTextureCount );

    talCdfBuffer = std::make_shared< AutoBuffer >( device, _memAllocator );
    talCdfBuffer->Create( VkDeviceSize( maxTextureCount ) * TAL_CDF_LUT_ENTRIES * sizeof( uint32_t ),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          "TAL cdf" );

    imageLoader = std::make_shared< ImageLoader >( std::move( _userFileLoad ) );

    if( _config.developerMode )
    {
        imageLoaderDev = std::make_shared< ImageLoaderDev >( imageLoader );
        observer       = std::make_shared< TextureObserver >();

        if( _info.pOverridenTexturesFolderPathDeveloper != nullptr )
        {
            defaultTexturesPath = _info.pOverridenTexturesFolderPathDeveloper;
        }
    }


    textureDesc = std::make_shared< TextureDescriptors >(
        device, samplerMgr, maxTextureCount, BINDING_TEXTURES, BINDING_TEXTURES_SAMPLER );
    textureUploader = std::make_shared< TextureUploader >( device, std::move( _memAllocator ) );

    textures.resize( maxTextureCount );

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        texturesToUpdateDescMarked[i].assign(maxTextureCount, 0);
    }

    MarkAllDescDirty();

    VkCommandBuffer cmd = _cmdManager->StartGraphicsCmd();

    uint32_t *pTalCdfDefault = static_cast< uint32_t * >( talCdfBuffer->GetMapped( 0 ) );

    std::fill_n( pTalCdfDefault, size_t( maxTextureCount ) * TAL_CDF_LUT_ENTRIES, TAL_CDF_EMPTY_ENTRY );

    talCdfBuffer->CopyFromStaging( cmd, 0, talCdfBuffer->GetSize(), 0 );

    CreateEmptyTexture( cmd, 0 );
    CreateWaterNormalTexture( cmd, 0, _info.pWaterNormalTexturePath );
    _cmdManager->Submit( cmd );
    _cmdManager->WaitGraphicsIdle();

    if( this->waterNormalTextureIndex == EMPTY_TEXTURE_INDEX )
    {
        throw QrException( QR_WRONG_ARGUMENT,
                           "Couldn't create water normal texture with path: " +
                               std::string( _info.pWaterNormalTexturePath ) );
    }
}

void TextureManager::CreateEmptyTexture(VkCommandBuffer cmd, uint32_t frameIndex)
{
    assert(textures[EMPTY_TEXTURE_INDEX].image == VK_NULL_HANDLE && textures[EMPTY_TEXTURE_INDEX].view == VK_NULL_HANDLE);

    const uint32_t data[] = { 0xFFFFFFFF };
    const QrExtent2D size = { 1, 1 };

    ImageLoader::ResultInfo info = {};
    info.pData = reinterpret_cast<const uint8_t*>(data);
    info.dataSize = sizeof(data);
    info.baseSize = size;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.levelCount = 1;
    info.isPregenerated = false;
    info.levelSizes[0] = sizeof(data);

    const SamplerManager::Handle samplerHandle(QR_SAMPLER_FILTER_NEAREST, QR_SAMPLER_ADDRESS_MODE_REPEAT, QR_SAMPLER_ADDRESS_MODE_REPEAT, 0);

    const uint32_t textureIndex = PrepareTexture(cmd, frameIndex, info, samplerHandle, false, "Empty texture", false, std::nullopt);

    assert(textureIndex == EMPTY_TEXTURE_INDEX);

    const VkImage emptyImage = textures[textureIndex].image;
    const VkImageView emptyView = textures[textureIndex].view;

    assert(emptyImage != VK_NULL_HANDLE && emptyView != VK_NULL_HANDLE);

    textureDesc->SetEmptyTextureInfo(emptyView);
}

void qray::TextureManager::CreateWaterNormalTexture(VkCommandBuffer cmd, uint32_t frameIndex, const char *pFilePath)
{
    const SamplerManager::Handle samplerHandle(QR_SAMPLER_FILTER_LINEAR, QR_SAMPLER_ADDRESS_MODE_REPEAT, QR_SAMPLER_ADDRESS_MODE_REPEAT, 0);

    TextureOverrides::OverrideInfo parseInfo = {};
    parseInfo.commonFolderPath = "";

    for (uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++)
    {
        parseInfo.postfixes[i] = "";
        parseInfo.originalIsSRGB[i] = false;
        parseInfo.overridenIsSRGB[i] = false;
    }

    constexpr uint32_t defaultData[] = { 0x7F7FFFFF };
    constexpr QrExtent2D defaultSize = { 1, 1 };

    TextureOverrides ovrd(pFilePath, QrTextureSet{ .pDataAlbedoAlpha = defaultData }, defaultSize, parseInfo, imageLoader.get());

    this->waterNormalTextureIndex = PrepareTexture( cmd,
                                                    frameIndex,
                                                    ovrd.GetResult( 0 ),
                                                    samplerHandle,
                                                    true,
                                                    "Water normal",
                                                    false,
                                                    std::nullopt );
}

TextureManager::~TextureManager()
{
    const auto destroyTextures = [this](std::vector<Texture> &textureList)
    {
        for (auto &texture : textureList)
        {
            assert((texture.image == VK_NULL_HANDLE && texture.view == VK_NULL_HANDLE) ||
                   (texture.image != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE));

            if (texture.image != VK_NULL_HANDLE)
            {
                DestroyTexture(texture);
            }
        }
    };

    destroyTextures(textures);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        destroyTextures(texturesToDestroy[i]);
    }
}

void TextureManager::PrepareForFrame(uint32_t frameIndex)
{
    for (auto &texture : texturesToDestroy[frameIndex])
    {
        DestroyTexture(texture);
    }

    texturesToDestroy[frameIndex].clear();

    textureUploader->ClearStaging(frameIndex);
}

void TextureManager::MarkDescDirty(uint32_t textureIndex)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (texturesToUpdateDescMarked[i][textureIndex] == 0)
        {
            texturesToUpdateDescMarked[i][textureIndex] = 1;
            texturesToUpdateDesc[i].push_back(textureIndex);
        }
    }
}

void TextureManager::MarkAllDescDirty()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        auto &dirty = texturesToUpdateDesc[i];

        dirty.resize(textures.size());
        std::iota(dirty.begin(), dirty.end(), 0u);

        std::fill(texturesToUpdateDescMarked[i].begin(), texturesToUpdateDescMarked[i].end(), 1);
    }
}

void TextureManager::SubmitDescriptors(uint32_t frameIndex,
                                       const QrDrawFrameTexturesParams *pTexturesParams,
                                       bool forceUpdateAllDescriptors)
{
    const QrSamplerFilter newDynamicSamplerFilter = pTexturesParams != nullptr ?
        pTexturesParams->dynamicSamplerFilter : DefaultDynamicSamplerFilter;

    if (currentDynamicSamplerFilter != newDynamicSamplerFilter)
    {
        currentDynamicSamplerFilter = newDynamicSamplerFilter;
        forceUpdateAllDescriptors = true;
    }

    if (forceUpdateAllDescriptors)
    {
        textureDesc->ResetAllCache(frameIndex);

        MarkAllDescDirty();
    }

    auto &dirty = texturesToUpdateDesc[frameIndex];

    const bool hasDescWrites = !dirty.empty();

    for (uint32_t i : dirty)
    {
        Texture &texture = textures[i];

        texture.samplerHandle.SetIfHasDynamicSamplerFilter(newDynamicSamplerFilter);

        if (texture.image != VK_NULL_HANDLE)
        {
            textureDesc->UpdateTextureDesc(frameIndex, i, texture.view, texture.samplerHandle);

            if (rhiTextureTable != nullptr)
            {
                rhiTextureTable->SetSlot(i, texture.image, texture.format,
                                         texture.baseSize.width, texture.baseSize.height,
                                         texture.mipLevels, texture.samplerHandle.GetIndex());
            }
        }
        else
        {
            textureDesc->ResetTextureDesc(frameIndex, i);
        }

        texturesToUpdateDescMarked[frameIndex][i] = 0;
    }

    dirty.clear();

    if (hasDescWrites)
    {
        textureDesc->FlushDescWrites();
    }
}

uint32_t TextureManager::CreateMaterial( VkCommandBuffer             cmd,
                                         uint32_t                    frameIndex,
                                         const QrMaterialCreateInfo& createInfo )
{
    if( createInfo.pRelativePath == nullptr && IsTextureSetEmpty( createInfo.textures ) )
    {
        throw QrException(
            QR_WRONG_MATERIAL_PARAMETER,
            R"(At least one of 'pRelativePath' or 'textures' members must be not null)" );
    }

    const auto samplerHandle = SamplerManager::Handle(
        createInfo.filter, createInfo.addressModeU, createInfo.addressModeV, createInfo.flags );

    const auto normalMapSamplerHandle = SamplerManager::Handle(
        forceNormalMapFilterLinear ? QR_SAMPLER_FILTER_LINEAR : createInfo.filter,
        createInfo.addressModeU,
        createInfo.addressModeV,
        createInfo.flags & ( ~QR_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT ) );

    TextureOverrides::OverrideInfo parseInfo = {};
    parseInfo.commonFolderPath = defaultTexturesPath.c_str();
    parseInfo.postfixes[0] = postfixes[0].c_str();
    parseInfo.postfixes[1] = postfixes[1].c_str();
    parseInfo.postfixes[2] = postfixes[2].c_str();
    parseInfo.overridenIsSRGB[0] = overridenIsSRGB[0];
    parseInfo.overridenIsSRGB[1] = overridenIsSRGB[1];
    parseInfo.overridenIsSRGB[2] = overridenIsSRGB[2];
    parseInfo.originalIsSRGB[0] = originalIsSRGB[0];
    parseInfo.originalIsSRGB[1] = originalIsSRGB[1];
    parseInfo.originalIsSRGB[2] = originalIsSRGB[2];

    TextureOverrides ovrd( createInfo.pRelativePath,
                           createInfo.textures,
                           createInfo.size,
                           parseInfo,
                           GetLoader( imageLoader, imageLoaderDev ) );

    bool isUpdateable = ( createInfo.flags & QR_MATERIAL_CREATE_UPDATEABLE_BIT ) != 0;
    if( observer )
    {
        isUpdateable = true;
    }

    MaterialTextures mtextures = {};
    for( uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++ )
    {
        const auto& texSampler =
            i != MATERIAL_NORMAL_INDEX ? samplerHandle : normalMapSamplerHandle;

        mtextures.indices[ i ] = PrepareTexture(
            cmd,
            frameIndex,
            ovrd.GetResult( i ),
            texSampler,
            !( createInfo.flags & QR_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT ),
            ovrd.GetDebugName(),
            isUpdateable,
            i == MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX ? std::optional( pbrSwizzling )
                                                            : std::nullopt );
    }

    const uint32_t materialIndex = InsertMaterial( mtextures, isUpdateable );

    const uint32_t rmeIndex = mtextures.indices[ MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX ];

    if( rmeIndex != EMPTY_TEXTURE_INDEX )
    {
        const auto& rmeInfo = ovrd.GetResult( MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX );

        if( rmeInfo.has_value() )
        {
            RebuildTalCdf( cmd, frameIndex, rmeIndex, rmeInfo->pData + rmeInfo->levelOffsets[ 0 ] );
        }
    }

    if( observer )
    {
        for( uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++ )
        {
            observer->RegisterPath(
                materialIndex, ovrd.GetPathAndRemove( i ), ovrd.GetResult( i ), i );
        }
    }

    return materialIndex;
}

bool TextureManager::CanUpdateMaterialContents(uint32_t materialIndex, QrExtent2D size) const
{
    const auto it = materials.find(materialIndex);

    if (it == materials.end() || !it->second.isUpdateable)
    {
        return false;
    }

    if (size.width == 0 || size.height == 0)
    {
        return false;
    }

    bool anyTexture = false;

    for (uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++)
    {
        const uint32_t textureIndex = it->second.textures.indices[i];

        if (textureIndex == EMPTY_TEXTURE_INDEX)
        {
            continue;
        }

        const Texture &texture = textures[textureIndex];

        if (texture.image == VK_NULL_HANDLE)
        {
            continue;
        }

        if (texture.format != VK_FORMAT_R8G8B8A8_UNORM && texture.format != VK_FORMAT_R8G8B8A8_SRGB)
        {
            return false;
        }

        if (texture.baseSize.width != size.width || texture.baseSize.height != size.height)
        {
            return false;
        }

        if (!textureUploader->CanUpdateImageFromHostData(texture.image))
        {
            return false;
        }

        anyTexture = true;
    }

    return anyTexture;
}

bool TextureManager::UpdateMaterial(VkCommandBuffer cmd, uint32_t frameIndex, const QrMaterialUpdateInfo &updateInfo)
{
    const auto materialIt = materials.find(updateInfo.target);

    if (materialIt == materials.end())
    {
        throw QrException(QR_CANT_UPDATE_MATERIAL,
            "Material with ID=" + std::to_string(updateInfo.target) + " was not created");
    }

    if (!materialIt->second.isUpdateable)
    {
        throw QrException(QR_CANT_UPDATE_MATERIAL,
            "Material with ID=" + std::to_string(updateInfo.target) + " was not marked as updateable");
    }

    const void *updateData[TEXTURES_PER_MATERIAL_COUNT] =
    {
        updateInfo.textures.pDataAlbedoAlpha,
        updateInfo.textures.pDataRoughnessMetallicEmission,
        updateInfo.textures.pDataNormal,
    };

    auto &textureIndices = materialIt->second.textures.indices;
    static_assert(sizeof(textureIndices) / sizeof(textureIndices[0]) == TEXTURES_PER_MATERIAL_COUNT);

    bool wasUpdated = false;

    for (uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++)
    {
        const uint32_t textureIndex = textureIndices[i];

        if (textureIndex == EMPTY_TEXTURE_INDEX)
        {
            continue;
        }

        const VkImage image = textures[textureIndex].image;

        if (image == VK_NULL_HANDLE || updateData[i] == nullptr)
        {
            continue;
        }

        textureUploader->UpdateImage(cmd, image, updateData[i]);

        if (i == MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX)
        {
            RebuildTalCdf(cmd, frameIndex, textureIndex, static_cast<const uint8_t *>(updateData[i]));
        }

        wasUpdated = true;
    }

    return wasUpdated;
}

uint32_t TextureManager::PrepareTexture(
    VkCommandBuffer                                 cmd,
    uint32_t                                        frameIndex,
    const std::optional< ImageLoader::ResultInfo >& optImageInfo,
    SamplerManager::Handle                          samplerHandle,
    bool                                            useMipmaps,
    const char*                                     debugName,
    bool                                            isUpdateable,
    std::optional< QrTextureSwizzling >             swizzling )
{
    if( !optImageInfo.has_value() )
    {
        return EMPTY_TEXTURE_INDEX;
    }

    const auto& imageInfo = optImageInfo.value();

    if( imageInfo.baseSize.width == 0 || imageInfo.baseSize.height == 0 )
    {
        using namespace std::string_literals;

        throw QrException( QR_WRONG_MATERIAL_PARAMETER,
                           "Incorrect size (" + std::to_string( imageInfo.baseSize.width ) + ", " +
                               std::to_string( imageInfo.baseSize.height ) +
                               ") of one of images in a material" +
                               ( debugName != nullptr ? " with name: "s + debugName : ""s ) );
    }

    assert( imageInfo.dataSize > 0 );
    assert( imageInfo.levelCount > 0 && imageInfo.levelSizes[ 0 ] > 0 );

    TextureUploader::UploadInfo info = {};
    info.cmd                    = cmd;
    info.frameIndex             = frameIndex;
    info.pData                  = imageInfo.pData;
    info.dataSize               = imageInfo.dataSize;
    info.baseSize               = imageInfo.baseSize;
    info.format                 = imageInfo.format;
    info.useMipmaps             = useMipmaps;
    info.pregeneratedLevelCount = imageInfo.isPregenerated ? imageInfo.levelCount : 0;
    info.pLevelDataOffsets      = imageInfo.levelOffsets;
    info.pLevelDataSizes        = imageInfo.levelSizes;
    info.isUpdateable           = isUpdateable;
    info.pDebugName             = debugName;
    info.isCubemap              = false;
    info.swizzling              = swizzling;

    auto [ wasUploaded, image, view ] = textureUploader->UploadImage( info );

    if( !wasUploaded )
    {
        return EMPTY_TEXTURE_INDEX;
    }

    const uint32_t mipLevels = GetUploadedMipLevelCount( imageInfo, useMipmaps );

    const uint32_t textureIndex = InsertTexture( frameIndex,
                                                 image,
                                                 view,
                                                 samplerHandle,
                                                 imageInfo.format,
                                                 VkExtent2D{ imageInfo.baseSize.width, imageInfo.baseSize.height },
                                                 mipLevels );

    talCdfSources[ textureIndex ] = TalCdfSource{ .baseSize = imageInfo.baseSize,
                                                  .format = imageInfo.format,
                                                  .level0Size = imageInfo.levelSizes[ 0 ] };

    return textureIndex;
}

void TextureManager::RebuildTalCdf(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t textureIndex, const uint8_t *pData)
{
    if (textureIndex >= talCdfSources.size() || pData == nullptr)
    {
        return;
    }

    const TalCdfSource &source = talCdfSources[textureIndex];

    const bool isEmissiveChannelBlue = pbrSwizzling == QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE ||
                                       pbrSwizzling == QR_TEXTURE_SWIZZLING_METALLIC_ROUGHNESS_EMISSIVE;

    uint32_t bytesPerPixel = 0;
    uint32_t emissiveOffset = 0;

    const bool layoutMatches = isEmissiveChannelBlue &&
                               GetTalCdfPixelLayout(source.format, &bytesPerPixel, &emissiveOffset) &&
                               uint64_t(source.baseSize.width) * source.baseSize.height * bytesPerPixel == source.level0Size;

    std::vector<uint32_t> entries(TAL_CDF_LUT_ENTRIES, TAL_CDF_EMPTY_ENTRY);

    if (layoutMatches)
    {
        BuildTalCdfEntries(pData,
                           source.baseSize.width,
                           source.baseSize.height,
                           bytesPerPixel,
                           emissiveOffset,
                           entries.data(),
                           uint32_t(TAL_CDF_LUT_ENTRIES));
    }

    const VkDeviceSize lutSize = VkDeviceSize(TAL_CDF_LUT_ENTRIES) * sizeof(uint32_t);
    const VkDeviceSize offset = VkDeviceSize(textureIndex) * lutSize;

    uint32_t *pStaging = static_cast<uint32_t *>(talCdfBuffer->GetMapped(frameIndex)) +
                         static_cast<size_t>(textureIndex) * TAL_CDF_LUT_ENTRIES;

    std::memcpy(pStaging, entries.data(), size_t(lutSize));

    talCdfBuffer->CopyFromStaging(cmd, frameIndex, lutSize, offset);
}

uint32_t TextureManager::CreateAnimatedMaterial(VkCommandBuffer cmd, uint32_t frameIndex, const QrAnimatedMaterialCreateInfo &createInfo)
{
    if (createInfo.frameCount == 0)
    {
        return QR_NO_MATERIAL;
    }

    std::vector<uint32_t> materialIndices;
    materialIndices.reserve(createInfo.frameCount);

    for (uint32_t i = 0; i < createInfo.frameCount; i++)
    {
        materialIndices.push_back(CreateMaterial(cmd, frameIndex, createInfo.pFrames[i]));
    }

    return InsertAnimatedMaterial(materialIndices);
}

bool TextureManager::ChangeAnimatedMaterialFrame(uint32_t animMaterial, uint32_t materialFrame)
{
    const auto animIt = animatedMaterials.find(animMaterial);

    if (animIt == animatedMaterials.end())
    {
        throw QrException(QR_CANT_UPDATE_ANIMATED_MATERIAL, "Material with ID=" + std::to_string(animMaterial) + " is not animated");
    }

    AnimatedMaterial &anim = animIt->second;

    {
        const auto maxFrameCount = static_cast<uint32_t>(anim.materialIndices.size());

        if (materialFrame >= maxFrameCount)
        {
            throw QrException(QR_CANT_UPDATE_ANIMATED_MATERIAL,
                "Animated material with ID=" + std::to_string(animMaterial) + " has only " +
                std::to_string(maxFrameCount) + " frames, but frame with index "
                + std::to_string(materialFrame) + " was requested");
        }
    }

    anim.currentFrame = materialFrame;

    for (auto &weakSubscriber : subscribers)
    {
        if (auto subscriber = weakSubscriber.lock())
        {
            const uint32_t frameMatIndex = anim.materialIndices[anim.currentFrame];

            const auto materialIt = materials.find(frameMatIndex);

            if (materialIt != materials.end())
            {
                subscriber->OnMaterialChange(animMaterial, materialIt->second.textures);
            }
        }
    }

    return true;
}

uint32_t TextureManager::GenerateMaterialIndex(const MaterialTextures &materialTextures)
{
    uint32_t materialIndex = std::accumulate(materialTextures.indices, materialTextures.indices + TEXTURES_PER_MATERIAL_COUNT, 0u);

    while (materials.count(materialIndex) != 0)
    {
        materialIndex++;
    }

    return materialIndex;
}

uint32_t TextureManager::GenerateMaterialIndex(const std::vector<uint32_t> &materialIndices)
{
    uint32_t materialIndex = std::accumulate(materialIndices.begin(), materialIndices.end(), 0u);

    while (materials.count(materialIndex) != 0)
    {
        materialIndex++;
    }

    return materialIndex;
}

uint32_t TextureManager::InsertMaterial(const MaterialTextures &materialTextures, bool isUpdateable)
{
    const bool isEmpty = std::none_of(materialTextures.indices, materialTextures.indices + TEXTURES_PER_MATERIAL_COUNT,
                                      [] (uint32_t textureIndex) { return textureIndex != EMPTY_TEXTURE_INDEX; });

    if (isEmpty)
    {
        return QR_NO_MATERIAL;
    }

    const uint32_t materialIndex = GenerateMaterialIndex(materialTextures);

    materials[materialIndex] = Material
    {
        .textures = materialTextures,
        .isUpdateable = isUpdateable,
    };

    return materialIndex;
}

uint32_t TextureManager::InsertAnimatedMaterial(std::vector<uint32_t> &materialIndices)
{
    const bool isEmpty = std::none_of(materialIndices.begin(), materialIndices.end(),
                                      [] (uint32_t materialIndex) { return materialIndex != QR_NO_MATERIAL; });

    if (isEmpty)
    {
        return QR_NO_MATERIAL;
    }

    const uint32_t animMaterialIndex = GenerateMaterialIndex(materialIndices);

    animatedMaterials[animMaterialIndex] = AnimatedMaterial
    {
        .materialIndices = std::move(materialIndices),
        .currentFrame = 0,
    };

    return animMaterialIndex;
}

void TextureManager::DestroyMaterialTextures(uint32_t frameIndex, uint32_t materialIndex)
{
    const auto materialIt = materials.find(materialIndex);

    if (materialIt != materials.end())
    {
        DestroyMaterialTextures(frameIndex, materialIt->second);
    }
}

void TextureManager::DestroyMaterialTextures(uint32_t frameIndex, const Material &material)
{
    for (uint32_t textureIndex : material.textures.indices)
    {
        if (textureIndex == EMPTY_TEXTURE_INDEX)
        {
            continue;
        }

        Texture &texture = textures[textureIndex];

        AddToBeDestroyed(frameIndex, texture);

        if (rhiTextureTable != nullptr)
        {
            rhiTextureTable->ResetSlot(textureIndex);
        }

        texture.image = VK_NULL_HANDLE;
        texture.view = VK_NULL_HANDLE;
        texture.samplerHandle = SamplerManager::Handle();

        MarkDescDirty(textureIndex);
    }
}

void TextureManager::DestroyMaterial(uint32_t currentFrameIndex, uint32_t materialIndex)
{
    if (materialIndex == QR_NO_MATERIAL)
    {
        return;
    }

    const auto animIt = animatedMaterials.find(materialIndex);

    if (animIt != animatedMaterials.end())
    {
        AnimatedMaterial &anim = animIt->second;

        for (auto &material : anim.materialIndices)
        {
            DestroyMaterialTextures(currentFrameIndex, material);
        }

        animatedMaterials.erase(animIt);
    }
    else
    {
        const auto materialIt = materials.find(materialIndex);

        if (materialIt != materials.end())
        {
            DestroyMaterialTextures(currentFrameIndex, materialIt->second);
            materials.erase(materialIt);
        }
    }

    if (observer)
    {
        observer->Remove(materialIndex);
    }

    for (auto &weakSubscriber : subscribers)
    {
        if (auto subscriber = weakSubscriber.lock())
        {
            subscriber->OnMaterialChange(materialIndex, EmptyMaterialTextures);
        }
    }
}

void TextureManager::CheckForHotReload(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (observer && imageLoaderDev)
    {
        observer->CheckPathsAndReupload(cmd, frameIndex, *this, imageLoaderDev.get());
    }
}

uint32_t TextureManager::InsertTexture(uint32_t frameIndex, VkImage image, VkImageView view,
                                       SamplerManager::Handle samplerHandle, VkFormat format,
                                       VkExtent2D baseSize, uint32_t mipLevels)
{
    const auto textureIt = std::find_if(textures.begin(), textures.end(), [] (const Texture &texture)
    {
        return texture.image == VK_NULL_HANDLE && texture.view == VK_NULL_HANDLE;
    });

    if (textureIt == textures.end())
    {
        Texture pending = {};
        pending.image = image;
        pending.view = view;
        AddToBeDestroyed(frameIndex, pending);

        assert(false && "Too many textures");

        return EMPTY_TEXTURE_INDEX;
    }

    *textureIt = Texture
    {
        .image = image,
        .view = view,
        .samplerHandle = samplerHandle,
        .format = format,
        .baseSize = baseSize,
        .mipLevels = mipLevels,
    };

    const uint32_t textureIndex = static_cast<uint32_t>(std::distance(textures.begin(), textureIt));

    MarkDescDirty(textureIndex);

    return textureIndex;
}

void TextureManager::DestroyTexture(const Texture &texture)
{
    assert(texture.image != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE);
    textureUploader->DestroyImage(texture.image, texture.view);
}

void TextureManager::AddToBeDestroyed(uint32_t frameIndex, const Texture &texture)
{
    assert(texture.image != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE);

    texturesToDestroy[frameIndex].push_back(texture);
}

MaterialTextures TextureManager::GetMaterialTextures(uint32_t materialIndex) const
{
    if (materialIndex == QR_NO_MATERIAL)
    {
        return EmptyMaterialTextures;
    }

    const auto animIt = animatedMaterials.find(materialIndex);

    if (animIt != animatedMaterials.end())
    {
        const AnimatedMaterial &anim = animIt->second;

        return GetMaterialTextures(anim.materialIndices[anim.currentFrame]);
    }

    const auto materialIt = materials.find(materialIndex);

    if (materialIt == materials.end())
    {
        return EmptyMaterialTextures;
    }

    return materialIt->second.textures;
}

VkBuffer TextureManager::GetTalCdfBuffer() const
{
    if (!talCdfBuffer)
    {
        return VK_NULL_HANDLE;
    }

    return talCdfBuffer->GetDeviceLocal();
}

VkDescriptorSet TextureManager::GetDescSet(uint32_t frameIndex) const
{
    return textureDesc->GetDescSet(frameIndex);
}

VkDescriptorSetLayout TextureManager::GetDescSetLayout() const
{
    return textureDesc->GetDescSetLayout();
}

void TextureManager::Subscribe(std::shared_ptr<IMaterialDependency> subscriber)
{
    subscribers.push_back(subscriber);
}

void TextureManager::Unsubscribe(const IMaterialDependency *subscriber)
{
    subscribers.remove_if([subscriber] (const std::weak_ptr<IMaterialDependency> &weakSubscriber)
    {
        if (const auto lockedSubscriber = weakSubscriber.lock())
        {
            return lockedSubscriber.get() == subscriber;
        }

        return true;
    });
}

uint32_t TextureManager::GetWaterNormalTextureIndex() const
{
    return waterNormalTextureIndex;
}

void TextureManager::SetRhiTextureTable(rhi::RhiTextureTable *pTable)
{
    if (rhiTextureTable == pTable)
    {
        return;
    }

    rhiTextureTable = pTable;

    if (rhiTextureTable != nullptr)
    {
        MarkAllDescDirty();
    }
}
