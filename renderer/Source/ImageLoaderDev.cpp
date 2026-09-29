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

#include "ImageLoaderDev.h"

#include <cassert>

#include "Stb/stb_image.h"

using namespace qray;

ImageLoaderDev::ImageLoaderDev(std::shared_ptr<ImageLoader> _fallback)
    : fallback(std::move(_fallback))
{
}

ImageLoaderDev::~ImageLoaderDev()
{
    assert(loadedImages.empty());
}

std::optional<ImageLoader::ResultInfo> ImageLoaderDev::Load(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return std::nullopt;
    }

    int x = 0;
    int y = 0;
    constexpr uint32_t Channels = 4;

    stbi_uc *pData = stbi_load(path.string().c_str(), &x, &y, nullptr, Channels);

    if (pData == nullptr)
    {
        return fallback->Load(path);
    }

    const uint32_t width = x;
    const uint32_t height = y;

    assert(width > 0 && height > 0);

    const uint32_t dataSize = width * height * Channels;

    ImageLoader::ResultInfo result{};
    result.levelOffsets[0] = 0;
    result.levelSizes[0] = dataSize;
    result.levelCount = 1;
    result.isPregenerated = false;
    result.pData = pData;
    result.dataSize = dataSize;
    result.baseSize = { width, height };
    result.format = VK_FORMAT_R8G8B8A8_SRGB;

    loadedImages.push_back(static_cast<void *>(pData));
    return result;
}

void ImageLoaderDev::FreeLoaded()
{
    for (void *pData : loadedImages)
    {
        stbi_image_free(pData);
    }

    loadedImages.clear();

    fallback->FreeLoaded();
}
