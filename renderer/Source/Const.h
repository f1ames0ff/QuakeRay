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

namespace qray
{

constexpr uint32_t      ALLOCATOR_BLOCK_SIZE_STAGING_TEXTURES   = 64 * 512 * 512 * 4;
constexpr uint32_t      ALLOCATOR_BLOCK_SIZE_TEXTURES           = 64 * 512 * 512 * 4;

constexpr uint32_t      TEXTURE_FILE_PATH_MAX_LENGTH            = 512;
constexpr uint32_t      TEXTURE_FILE_NAME_MAX_LENGTH            = 256;
constexpr uint32_t      TEXTURE_FILE_EXTENSION_MAX_LENGTH       = 16;

constexpr uint32_t      TEXTURE_COUNT_MIN                       = 1024;
constexpr uint32_t      TEXTURE_COUNT_MAX                       = 4096;
constexpr uint32_t      EMPTY_TEXTURE_INDEX                     = 0;
constexpr uint32_t      MATERIALS_MAX_LAYER_COUNT               = 3;
constexpr uint32_t      TEXTURES_PER_MATERIAL_COUNT             = 3;

constexpr const char    *DEFAULT_TEXTURES_PATH                              = "";
constexpr const char    *DEFAULT_TEXTURE_POSTFIX_ALBEDO_ALPHA               = "";
constexpr const char    *DEFAULT_TEXTURE_POSTFIX_ROUGNESS_METALLIC_EMISSION = "_rme";
constexpr const char    *DEFAULT_TEXTURE_POSTFIX_NORMAL                     = "_n";

constexpr uint32_t      MAX_PREGENERATED_MIPMAP_LEVELS          = 20;

// Use WORLD2 mask bit as SKY
#define RAYCULLMASK_SKY_IS_WORLD2 1

}