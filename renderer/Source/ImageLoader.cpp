// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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
#include <cctype>
#include <fstream>

#include <ktx.h>
#include <ktxvulkan.h>

#include "Stb/stb_image.h"

using namespace qray;

namespace
{
    bool IsPng(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".png" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
    }
}

ImageLoader::ImageLoader(std::shared_ptr<UserFileLoad> _userFileLoad)
    : userFileLoad(std::move(_userFileLoad))
{
}

ImageLoader::~ImageLoader()
{
    assert(loadedImages.empty());
    assert(loadedPngs.empty());
}

bool ImageLoader::ReadFile(const std::filesystem::path &path, std::vector<uint8_t> &out) const
{
    if (userFileLoad->Exists())
    {
        auto fileHandle = userFileLoad->Open(path.string().c_str());

        if (!fileHandle.Contains())
        {
            return false;
        }

        const uint8_t *pData = static_cast<const uint8_t *>(fileHandle.pData);
        out.assign(pData, pData + fileHandle.dataSize);
        return true;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(file), {});
    return !out.empty();
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

    if (IsPng(path))
    {
        std::vector<uint8_t> fileData;

        if (!ReadFile(path, fileData))
        {
            return std::nullopt;
        }

        int x = 0;
        int y = 0;
        stbi_uc *pData = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()),
                                               &x, &y, nullptr, 4);

        if (pData == nullptr || x <= 0 || y <= 0)
        {
            return std::nullopt;
        }

        const uint32_t dataSize = static_cast<uint32_t>(x) * static_cast<uint32_t>(y) * 4;

        ResultInfo result{};
        result.levelOffsets[0] = 0;
        result.levelSizes[0] = dataSize;
        result.levelCount = 1;
        result.isPregenerated = false;
        result.pData = pData;
        result.dataSize = dataSize;
        result.baseSize = { static_cast<uint32_t>(x), static_cast<uint32_t>(y) };
        result.format = VK_FORMAT_R8G8B8A8_UNORM;

        loadedPngs.push_back(pData);
        return result;
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

    if (IsPng(path))
    {
        std::vector<uint8_t> fileData;

        if (!ReadFile(path, fileData))
        {
            return std::nullopt;
        }

        int x = 0;
        int y = 0;
        stbi_uc *pData = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()),
                                               &x, &y, nullptr, 4);

        if (pData == nullptr || x <= 0 || y <= 0 || (y % x) != 0)
        {
            if (pData != nullptr)
            {
                stbi_image_free(pData);
            }
            return std::nullopt;
        }

        const uint32_t layerSize = static_cast<uint32_t>(x) * static_cast<uint32_t>(x) * 4;
        const uint32_t layerCount = static_cast<uint32_t>(y) / static_cast<uint32_t>(x);

        LayeredResultInfo result{};
        result.dataSize = static_cast<uint32_t>(x) * static_cast<uint32_t>(y) * 4;
        result.baseSize = { static_cast<uint32_t>(x), static_cast<uint32_t>(x) };
        result.format = VK_FORMAT_R8G8B8A8_UNORM;

        for (uint32_t i = 0; i < layerCount; i++)
        {
            result.layerData.push_back(pData + static_cast<size_t>(i) * layerSize);
        }

        loadedPngs.push_back(pData);
        return result;
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

    for (void *pData : loadedPngs)
    {
        stbi_image_free(pData);
    }

    loadedPngs.clear();
}
