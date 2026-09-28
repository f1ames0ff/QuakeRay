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

#include <optional>
#include <vector>
#include <filesystem>

#include "Common.h"
#include "Const.h"
#include "UserFunction.h"

struct ktxTexture;

namespace vkpt
{

class ImageLoader final
{
public:
    struct ResultInfo
    {
        uint32_t        levelOffsets[MAX_PREGENERATED_MIPMAP_LEVELS];
        uint32_t        levelSizes[MAX_PREGENERATED_MIPMAP_LEVELS];
        uint32_t        levelCount;
        bool            isPregenerated;
        const uint8_t   *pData;
        uint32_t        dataSize;
        RgExtent2D      baseSize;
        VkFormat        format;
    };

    struct LayeredResultInfo
    {
        std::vector<const uint8_t *> layerData;
        uint32_t        dataSize;
        RgExtent2D      baseSize;
        VkFormat        format;
    };

public:
    explicit ImageLoader(std::shared_ptr<UserFileLoad> userFileLoad);
    ~ImageLoader();

    ImageLoader(const ImageLoader &other) = delete;
    ImageLoader(ImageLoader &&other) noexcept = delete;
    ImageLoader &operator=(const ImageLoader &other) = delete;
    ImageLoader &operator=(ImageLoader &&other) noexcept = delete;

    std::optional<ResultInfo> Load(const std::filesystem::path &path);
    std::optional<LayeredResultInfo> LoadLayered(const std::filesystem::path &path);

    void FreeLoaded();

private:
    bool LoadTextureFile(const std::filesystem::path &path, ktxTexture **ppTexture);

private:
    std::shared_ptr<UserFileLoad> userFileLoad;
    std::vector<ktxTexture *> loadedImages;
};

}
