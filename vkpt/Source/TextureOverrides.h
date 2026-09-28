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

#include <variant>

#include "Common.h"
#include "vkpt/vkpt.h"
#include "ImageLoader.h"
#include "ImageLoaderDev.h"

namespace vkpt
{

constexpr uint32_t TEXTURE_DEBUG_NAME_MAX_LENGTH = 32;

struct TextureOverrides
{
public:
    struct OverrideInfo
    {
        const char *commonFolderPath = "";
        const char *postfixes[TEXTURES_PER_MATERIAL_COUNT] = {};
        bool overridenIsSRGB[TEXTURES_PER_MATERIAL_COUNT] = {};
        bool originalIsSRGB[TEXTURES_PER_MATERIAL_COUNT] = {};
    };

    using Loader = std::variant<ImageLoader *, ImageLoaderDev *>;

public:
    explicit TextureOverrides(
        const char *relativePath,
        const RgTextureSet &defaultTextures,
        const RgExtent2D &defaultSize,
        const OverrideInfo &info,
        Loader loader);
    ~TextureOverrides();

    TextureOverrides(const TextureOverrides &other) = delete;
    TextureOverrides(TextureOverrides &&other) noexcept = delete;
    TextureOverrides &operator=(const TextureOverrides &other) = delete;
    TextureOverrides &operator=(TextureOverrides &&other) noexcept = delete;

    [[nodiscard]] const std::optional<ImageLoader::ResultInfo> &GetResult(uint32_t index) const;
    [[nodiscard]] const char *GetDebugName() const;
    [[nodiscard]] std::optional<std::filesystem::path> &&GetPathAndRemove(uint32_t index);

private:
    Loader loader;

    std::optional<ImageLoader::ResultInfo> results[TEXTURES_PER_MATERIAL_COUNT];
    char debugname[TEXTURE_DEBUG_NAME_MAX_LENGTH];
    std::optional<std::filesystem::path> paths[TEXTURES_PER_MATERIAL_COUNT];
};

}
