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

#pragma once

#include <qray/qray.h>

#include "Containers.h"
#include "TextureOverrides.h"

namespace qray
{
    class TextureManager;


    class TextureObserver
    {
    public:
        TextureObserver() = default;
        ~TextureObserver() = default;

        TextureObserver(const TextureObserver& other) = delete;
        TextureObserver(TextureObserver&& other) noexcept = delete;
        TextureObserver& operator=(const TextureObserver& other) = delete;
        TextureObserver& operator=(TextureObserver&& other) noexcept = delete;

        void CheckPathsAndReupload(VkCommandBuffer cmd, uint32_t frameIndex, TextureManager &manager, ImageLoaderDev *loader);

        void RegisterPath(QrMaterial index, std::optional<std::filesystem::path> path, const std::optional<ImageLoader::ResultInfo> &imageInfo, uint32_t textureType);
        void Remove(QrMaterial index);

    private:
        struct DependentFile
        {
            std::filesystem::path path;
            std::filesystem::file_time_type lastWriteTime;
            size_t dataSize;
            VkFormat format;
            uint32_t textureType;
        };

        static bool HaveChanged(std::vector<DependentFile> &files);

    private:
        rgl::unordered_map<QrMaterial, std::vector<DependentFile>> materials;
        std::chrono::time_point<std::chrono::system_clock> lastCheck;
    };
}
