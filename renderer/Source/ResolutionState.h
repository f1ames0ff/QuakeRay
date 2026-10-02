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


namespace qray
{

struct ResolutionState
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t upscaledWidth;
    uint32_t upscaledHeight;

    friend bool operator==(const ResolutionState &lhs, const ResolutionState &rhs)
    {
        return lhs.renderWidth == rhs.renderWidth
            && lhs.renderHeight == rhs.renderHeight
            && lhs.upscaledWidth == rhs.upscaledWidth
            && lhs.upscaledHeight == rhs.upscaledHeight;
    }

    friend bool operator!=(const ResolutionState &lhs, const ResolutionState &rhs)
    {
        return !(lhs == rhs);
    }
};

}
