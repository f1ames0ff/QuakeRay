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

#include "QrException.h"

#include <cassert>
#include <iterator>

const char* qray::QrException::GetQrResultName(QrResult r)
{
    static constexpr const char *RESULT_NAMES[] =
    {
        "QR_SUCCESS",
        "QR_GRAPHICS_API_ERROR",
        "QR_CANT_FIND_PHYSICAL_DEVICE",
        "QR_WRONG_ARGUMENT",
        "QR_TOO_MANY_INSTANCES",
        "QR_WRONG_INSTANCE",
        "QR_FRAME_WASNT_STARTED",
        "QR_FRAME_WASNT_ENDED",
        "QR_CANT_UPDATE_TRANSFORM",
        "QR_CANT_UPDATE_TEXCOORDS",
        "QR_CANT_UPDATE_MATERIAL",
        "QR_CANT_UPDATE_ANIMATED_MATERIAL",
        "QR_CANT_UPLOAD_RASTERIZED_GEOMETRY",
        "QR_WRONG_MATERIAL_PARAMETER",
        "QR_WRONG_FUNCTION_CALL",
        "QR_ERROR_CANT_FIND_BLUE_NOISE",
        "QR_ERROR_CANT_FIND_WATER_TEXTURES",
    };

    const size_t index = static_cast<size_t>(r);

    if (index < std::size(RESULT_NAMES))
    {
        return RESULT_NAMES[index];
    }

    assert(0);
    return "Unknown QrResult";
}

qray::QrException::QrException(QrResult result)
    : runtime_error(GetQrResultName(result)), errorCode(result)
{
    assert(errorCode != QR_SUCCESS);
}

qray::QrException::QrException(QrResult result, const std::string &message)
    : runtime_error(message), errorCode(result)
{
    assert(errorCode != QR_SUCCESS);
}

qray::QrException::QrException(QrResult result, const char *message)
    : runtime_error(message), errorCode(result)
{
    assert(errorCode != QR_SUCCESS);
}

QrResult qray::QrException::GetErrorCode() const
{
    return errorCode;
}
