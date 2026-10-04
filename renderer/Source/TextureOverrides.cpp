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

#include "TextureOverrides.h"

#include <cstring>
#include <filesystem>
#include <span>

#include "Const.h"
#include "ImageLoader.h"

using namespace qray;

namespace
{
    VkFormat ToUnorm(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
            case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
            case VK_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_UNORM;
            case VK_FORMAT_B8G8R8_SRGB: return VK_FORMAT_B8G8R8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
            case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
            case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
            default: return format;
        }
    }

    VkFormat ToSRGB(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;
            case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;
            case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8_SRGB;
            case VK_FORMAT_B8G8R8_UNORM: return VK_FORMAT_B8G8R8_SRGB;
            case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
            case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
            case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
            case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
            case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
            default: return format;
        }
    }

    template<uint32_t N>
    void SafeCopy(char (&dst)[N], const char *src)
    {
        memset(dst, 0, N);

        if (src == nullptr)
        {
            return;
        }

        for (uint32_t i = 0; i < N - 1; i++)
        {
            if (src[i] == '\0')
            {
                break;
            }

            dst[i] = src[i];
        }
    }

    namespace loader
    {
        auto Load(TextureOverrides::Loader loader, const std::filesystem::path &filepath)
        {
            if (std::holds_alternative<ImageLoaderDev *>(loader))
            {
                return std::get<ImageLoaderDev *>(loader)->Load(filepath);
            }

            return std::get<ImageLoader *>(loader)->Load(filepath);
        }

        void FreeLoaded(TextureOverrides::Loader loader)
        {
            if (std::holds_alternative<ImageLoaderDev *>(loader))
            {
                std::get<ImageLoaderDev *>(loader)->FreeLoaded();
                return;
            }

            std::get<ImageLoader *>(loader)->FreeLoaded();
        }

        std::span<const char *> GetExtensions(TextureOverrides::Loader loader)
        {
            if (std::holds_alternative<ImageLoaderDev *>(loader))
            {
                static const char *devExtensions[] = { ".png", ".tga" };
                return devExtensions;
            }

            static const char *extensions[] = { ".png", ".ktx2" };
            return extensions;
        }
    }

    std::optional<std::filesystem::path> GetTexturePath(
        const char *commonFolderPath, const char *relativePath, const char *postfix, const char *extension)
    {
        if (relativePath == nullptr || relativePath[0] == '\0')
        {
            return std::nullopt;
        }

        return std::filesystem::path(commonFolderPath)
            .append(relativePath)
            .replace_extension("")
            .concat(postfix)
            .replace_extension(extension);
    }
}

TextureOverrides::TextureOverrides(
    const char *_relativePath,
    const QrTextureSet &_defaultTextures,
    const QrExtent2D &_defaultSize,
    const OverrideInfo &_info,
    Loader _loader)
    : loader(_loader)
    , results{}
    , debugname{}
    , paths{}
{
    SafeCopy(debugname, _relativePath);

    const void *defaultData[TEXTURES_PER_MATERIAL_COUNT] =
    {
        _defaultTextures.pDataAlbedoAlpha,
        _defaultTextures.pDataRoughnessMetallicEmission,
        _defaultTextures.pDataNormal,
    };

    constexpr VkFormat defaultSRGBFormat = VK_FORMAT_R8G8B8A8_SRGB;
    constexpr VkFormat defaultLinearFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t defaultBytesPerPixel = 4;

    for (uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++)
    {
        for (const char *extension : loader::GetExtensions(loader))
        {
            auto filepath = GetTexturePath(_info.commonFolderPath, _relativePath, _info.postfixes[i], extension);

            if (!filepath)
            {
                continue;
            }

            auto image = loader::Load(loader, filepath.value());

            if (!image)
            {
                continue;
            }

            image->format = _info.overridenIsSRGB[i] ? ToSRGB(image->format) : ToUnorm(image->format);

            paths[i] = std::move(filepath);
            results[i] = image;

            break;
        }
    }

    const uint32_t defaultDataSize = defaultBytesPerPixel * _defaultSize.width * _defaultSize.height;

    for (uint32_t i = 0; i < TEXTURES_PER_MATERIAL_COUNT; i++)
    {
        if (results[i] || defaultData[i] == nullptr)
        {
            continue;
        }

        ImageLoader::ResultInfo defaultResult{};
        defaultResult.levelOffsets[0] = 0;
        defaultResult.levelSizes[0] = defaultDataSize;
        defaultResult.levelCount = 1;
        defaultResult.isPregenerated = false;
        defaultResult.pData = static_cast<const uint8_t *>(defaultData[i]);
        defaultResult.dataSize = defaultDataSize;
        defaultResult.baseSize = _defaultSize;
        defaultResult.format = _info.originalIsSRGB[i] ? defaultSRGBFormat : defaultLinearFormat;

        results[i] = defaultResult;
    }
}

TextureOverrides::~TextureOverrides()
{
    loader::FreeLoaded(loader);
}

const std::optional<ImageLoader::ResultInfo> &TextureOverrides::GetResult(uint32_t index) const
{
    assert(index < TEXTURES_PER_MATERIAL_COUNT);
    return results[index];
}

const char *TextureOverrides::GetDebugName() const
{
    return debugname;
}

std::optional<std::filesystem::path> &&TextureOverrides::GetPathAndRemove(uint32_t index)
{
    assert(index < TEXTURES_PER_MATERIAL_COUNT);
    return std::move(paths[index]);
}
