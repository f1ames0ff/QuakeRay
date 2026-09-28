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

#include <stdexcept>

#include "vkpt/vkpt.h"

namespace vkpt
{

class RgException : public std::runtime_error
{
public:
    explicit RgException(RgResult errorCode);
    explicit RgException(RgResult errorCode, const std::string &_Message);
    explicit RgException(RgResult errorCode, const char *_Message);

    RgResult GetErrorCode() const;
    static const char* GetRgResultName(RgResult r);

private:
    RgResult errorCode;
};

}
