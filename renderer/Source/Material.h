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

#include "Common.h"
#include "Const.h"
#include "SamplerManager.h"

namespace qray
{


struct Texture
{
    VkImage                 image = VK_NULL_HANDLE;
    VkImageView             view = VK_NULL_HANDLE;
    SamplerManager::Handle  samplerHandle = SamplerManager::Handle();

    // Info the RHI texture table (RHI/RhiTextureTable.h) needs to wrap this slot. Filled by
    // TextureManager::InsertTexture; the defaults keep value-initialised records valid.
    VkFormat                format = VK_FORMAT_UNDEFINED;
    VkExtent2D              baseSize = {};
    uint32_t                mipLevels = 1;
};


struct MaterialTextures
{
    // Indices to use in shaders, each index represents QrTextureData from QrTextureSet
    uint32_t                indices[TEXTURES_PER_MATERIAL_COUNT];
};


struct Material
{
    MaterialTextures        textures;
    uint32_t                isUpdateable;
};


struct AnimatedMaterial
{
    // Indices of static materials.
    std::vector<uint32_t>   materialIndices;
    uint32_t                currentFrame = 0;
};


}