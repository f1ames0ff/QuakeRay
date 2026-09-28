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

#include "ImageLoader.h"

#include <algorithm>
#include <cassert>

#include <ktx.h>
#include <ktxvulkan.h>

using namespace vkpt;

ImageLoader::ImageLoader(std::shared_ptr<UserFileLoad> _userFileLoad)
    : userFileLoad(std::move(_userFileLoad))
{
}

ImageLoader::~ImageLoader()
{
    assert(loadedImages.empty());
}

bool ImageLoader::LoadTextureFile(const std::filesystem::path &path, ktxTexture **ppTexture)
{
    KTX_error_code result;

    if (userFileLoad->Exists())
    {
        auto fileHandle = userFileLoad->Open(path.string().c_str());

        if (!fileHandle.Contains())
        {
            return false;
        }

        result = ktxTexture_CreateFromMemory(
            static_cast<const uint8_t *>(fileHandle.pData), fileHandle.dataSize,
            KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
            ppTexture);
    }
    else
    {
        result = ktxTexture_CreateFromNamedFile(
            path.string().c_str(),
            KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
            ppTexture);
    }

    return result == KTX_SUCCESS;
}

std::optional<ImageLoader::ResultInfo> ImageLoader::Load(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return std::nullopt;
    }

    ktxTexture *pTexture = nullptr;

    if (!LoadTextureFile(path, &pTexture))
    {
        return std::nullopt;
    }

    assert(pTexture->numDimensions == 2);
    assert(pTexture->numLevels <= MAX_PREGENERATED_MIPMAP_LEVELS);
    assert(pTexture->numLayers == 1);
    assert(pTexture->numFaces == 1);

    ResultInfo result{};
    result.levelCount = std::min(pTexture->numLevels, MAX_PREGENERATED_MIPMAP_LEVELS);
    result.isPregenerated = true;
    result.pData = ktxTexture_GetData(pTexture);
    result.dataSize = static_cast<uint32_t>(ktxTexture_GetDataSize(pTexture));
    result.baseSize = { pTexture->baseWidth, pTexture->baseHeight };
    result.format = ktxTexture_GetVkFormat(pTexture);

    for (uint32_t level = 0; level < result.levelCount; level++)
    {
        ktx_size_t offset = 0;
        const KTX_error_code offsetResult = ktxTexture_GetImageOffset(pTexture, level, 0, 0, &offset);
        const ktx_size_t levelSize = ktxTexture_GetImageSize(pTexture, level);

        if (offsetResult != KTX_SUCCESS || levelSize == 0)
        {
            result.levelCount = level + 1;
            break;
        }

        result.levelOffsets[level] = static_cast<uint32_t>(offset);
        result.levelSizes[level] = static_cast<uint32_t>(levelSize);
    }

    loadedImages.push_back(pTexture);
    return result;
}

std::optional<ImageLoader::LayeredResultInfo> ImageLoader::LoadLayered(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return std::nullopt;
    }

    ktxTexture *pTexture = nullptr;

    if (!LoadTextureFile(path, &pTexture))
    {
        return std::nullopt;
    }

    assert(pTexture->numDimensions == 2);
    assert(pTexture->numLevels == 1);
    assert(pTexture->numFaces == 1);

    LayeredResultInfo result{};
    result.dataSize = static_cast<uint32_t>(ktxTexture_GetDataSize(pTexture));
    result.baseSize = { pTexture->baseWidth, pTexture->baseHeight };
    result.format = ktxTexture_GetVkFormat(pTexture);

    uint8_t *pData = ktxTexture_GetData(pTexture);

    for (uint32_t i = 0; i < pTexture->numLayers; i++)
    {
        ktx_size_t offset = 0;
        const KTX_error_code offsetResult = ktxTexture_GetImageOffset(pTexture, 0, i, 0, &offset);

        if (offsetResult != KTX_SUCCESS)
        {
            continue;
        }

        result.layerData.push_back(pData + offset);
    }

    loadedImages.push_back(pTexture);
    return result;
}

void ImageLoader::FreeLoaded()
{
    for (ktxTexture *pTexture : loadedImages)
    {
        ktxTexture_Destroy(pTexture);
    }

    loadedImages.clear();
}
