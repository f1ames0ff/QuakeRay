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

#include <functional>
#include <cassert>

namespace qray
{


// Passed to the library by a user.
typedef uint64_t UniqueLightID;


// Index in the global light array.
// Used to match lights by UniqueLightID between current and previous frames,
// as indices for the same light in them can be different, and only UniqueLightID is constant.
struct LightArrayIndex
{
    typedef uint32_t index_t;
    index_t _indexInGlobalArray;

    index_t GetArrayIndex() const
    {
        return _indexInGlobalArray;
    }
    bool operator==(const LightArrayIndex &other) const
    {
        return _indexInGlobalArray == other._indexInGlobalArray;
    }
};
static_assert(std::is_trivial_v<LightArrayIndex>);


}


// default hash specialializations


template<>
struct std::hash<qray::LightArrayIndex>
{
    std::size_t operator()(qray::LightArrayIndex const &s) const noexcept
    {
        return std::hash<qray::LightArrayIndex::index_t>{}(s._indexInGlobalArray);
    }
};