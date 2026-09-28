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

#include "RgException.h"

#include <cassert>
#include <iterator>

const char* vkpt::RgException::GetRgResultName(RgResult r)
{
    static constexpr const char *RESULT_NAMES[] =
    {
        "RG_SUCCESS",
        "RG_GRAPHICS_API_ERROR",
        "RG_CANT_FIND_PHYSICAL_DEVICE",
        "RG_WRONG_ARGUMENT",
        "RG_TOO_MANY_INSTANCES",
        "RG_WRONG_INSTANCE",
        "RG_FRAME_WASNT_STARTED",
        "RG_FRAME_WASNT_ENDED",
        "RG_CANT_UPDATE_TRANSFORM",
        "RG_CANT_UPDATE_TEXCOORDS",
        "RG_CANT_UPDATE_MATERIAL",
        "RG_CANT_UPDATE_ANIMATED_MATERIAL",
        "RG_CANT_UPLOAD_RASTERIZED_GEOMETRY",
        "RG_WRONG_MATERIAL_PARAMETER",
        "RG_WRONG_FUNCTION_CALL",
        "RG_ERROR_CANT_FIND_BLUE_NOISE",
        "RG_ERROR_CANT_FIND_WATER_TEXTURES",
    };

    const size_t index = static_cast<size_t>(r);

    if (index < std::size(RESULT_NAMES))
    {
        return RESULT_NAMES[index];
    }

    assert(0);
    return "Unknown RgResult";
}

vkpt::RgException::RgException(RgResult result)
    : runtime_error(GetRgResultName(result)), errorCode(result)
{
    assert(errorCode != RG_SUCCESS);
}

vkpt::RgException::RgException(RgResult result, const std::string &message)
    : runtime_error(message), errorCode(result)
{
    assert(errorCode != RG_SUCCESS);
}

vkpt::RgException::RgException(RgResult result, const char *message)
    : runtime_error(message), errorCode(result)
{
    assert(errorCode != RG_SUCCESS);
}

RgResult vkpt::RgException::GetErrorCode() const
{
    return errorCode;
}
