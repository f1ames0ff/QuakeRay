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

#include "ImageLoader.h"

namespace vkpt
{

class ImageLoaderDev final
{
public:
    explicit ImageLoaderDev(std::shared_ptr<ImageLoader> fallback);
    ~ImageLoaderDev();

    ImageLoaderDev(const ImageLoaderDev &other) = delete;
    ImageLoaderDev(ImageLoaderDev &&other) noexcept = delete;
    ImageLoaderDev &operator=(const ImageLoaderDev &other) = delete;
    ImageLoaderDev &operator=(ImageLoaderDev &&other) noexcept = delete;

    std::optional<ImageLoader::ResultInfo> Load(const std::filesystem::path &path);
    void FreeLoaded();

private:
    std::shared_ptr<ImageLoader> fallback;
    std::vector<void *> loadedImages;
};

}
