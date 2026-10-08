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

#include "qray/qray.h"

#include <functional>

namespace qray
{

constexpr uint32_t VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF = 0;
constexpr uint32_t VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT = 3;
constexpr uint32_t VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV = 6;

enum class VertexCollectorFilterTypeFlagBits : uint32_t
{
    NONE                        = 0,

    CF_STATIC_NON_MOVABLE       = 0b00000001 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF,
    CF_STATIC_MOVABLE           = 0b00000010 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF,
    CF_DYNAMIC                  = 0b00000100 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF,
    MASK_CHANGE_FREQUENCY_GROUP = CF_STATIC_NON_MOVABLE | CF_STATIC_MOVABLE | CF_DYNAMIC,

    PT_OPAQUE                   = 0b00000001 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT,
    PT_ALPHA_TESTED             = 0b00000010 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT,
    PT_REFRACT                  = 0b00000100 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT,
    /* Glass is the alpha-tested and the refract class at once: it keeps the
       refract instance mask while the alpha-tested hit group gives it an
       any-hit shader, so a pane can keep an alpha cutout. */
    PT_GLASS                    = 0b00000110 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT,
    MASK_PASS_THROUGH_GROUP     = PT_OPAQUE | PT_ALPHA_TESTED | PT_REFRACT | PT_GLASS,

    PV_WORLD_0                  = 0b00000001 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV,
    PV_WORLD_1                  = 0b00000010 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV,
    PV_WORLD_2                  = 0b00000100 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV,
    PV_FIRST_PERSON             = 0b00001000 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV,
    PV_FIRST_PERSON_VIEWER      = 0b00010000 << VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV,
    MASK_PRIMARY_VISIBILITY_GROUP = PV_WORLD_0 | PV_WORLD_1 | PV_WORLD_2 | PV_FIRST_PERSON | PV_FIRST_PERSON_VIEWER,
};
typedef uint32_t VertexCollectorFilterTypeFlags;


constexpr VertexCollectorFilterTypeFlagBits VertexCollectorFilterGroup_ChangeFrequency[] =
{
    VertexCollectorFilterTypeFlagBits::CF_STATIC_NON_MOVABLE,
    VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE,
    VertexCollectorFilterTypeFlagBits::CF_DYNAMIC,
};

constexpr VertexCollectorFilterTypeFlagBits VertexCollectorFilterGroup_PassThrough[] =
{
    VertexCollectorFilterTypeFlagBits::PT_OPAQUE,
    VertexCollectorFilterTypeFlagBits::PT_ALPHA_TESTED,
    VertexCollectorFilterTypeFlagBits::PT_REFRACT,
    VertexCollectorFilterTypeFlagBits::PT_GLASS,
};

constexpr VertexCollectorFilterTypeFlagBits VertexCollectorFilterGroup_PrimaryVisibility[] =
{
    VertexCollectorFilterTypeFlagBits::PV_WORLD_0,
    VertexCollectorFilterTypeFlagBits::PV_WORLD_1,
    VertexCollectorFilterTypeFlagBits::PV_WORLD_2,
    VertexCollectorFilterTypeFlagBits::PV_FIRST_PERSON,
    VertexCollectorFilterTypeFlagBits::PV_FIRST_PERSON_VIEWER,
};


inline VertexCollectorFilterTypeFlags operator|(
    VertexCollectorFilterTypeFlagBits a, VertexCollectorFilterTypeFlagBits b)
{
    return static_cast<VertexCollectorFilterTypeFlags>(a) |
           static_cast<VertexCollectorFilterTypeFlags>(b);
}

inline VertexCollectorFilterTypeFlags operator|(
    VertexCollectorFilterTypeFlags a, VertexCollectorFilterTypeFlagBits b)
{
    return a | static_cast<VertexCollectorFilterTypeFlags>(b);
}

inline VertexCollectorFilterTypeFlags operator|(
    VertexCollectorFilterTypeFlagBits a, VertexCollectorFilterTypeFlags b)
{
    return static_cast<VertexCollectorFilterTypeFlags>(a) | b;
}

inline VertexCollectorFilterTypeFlags operator&(
    VertexCollectorFilterTypeFlagBits a, VertexCollectorFilterTypeFlagBits b)
{
    return static_cast<VertexCollectorFilterTypeFlags>(a) &
           static_cast<VertexCollectorFilterTypeFlags>(b);
}

inline VertexCollectorFilterTypeFlags operator&(
    VertexCollectorFilterTypeFlags a, VertexCollectorFilterTypeFlagBits b)
{
    return a & static_cast<VertexCollectorFilterTypeFlags>(b);
}

inline VertexCollectorFilterTypeFlags operator&(
    VertexCollectorFilterTypeFlagBits a, VertexCollectorFilterTypeFlags b)
{
    return static_cast<VertexCollectorFilterTypeFlags>(a) & b;
}

void                            VertexCollectorFilterTypeFlags_Init();
uint32_t                        VertexCollectorFilterTypeFlags_GetAllBottomLevelGeomsCount();
uint32_t                        VertexCollectorFilterTypeFlags_GetID(VertexCollectorFilterTypeFlags flags);
uint32_t                        VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(VertexCollectorFilterTypeFlags flags);
uint32_t                        VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(VertexCollectorFilterTypeFlags flags);
const char*                     VertexCollectorFilterTypeFlags_GetNameForBLAS(VertexCollectorFilterTypeFlags flags);
VertexCollectorFilterTypeFlags  VertexCollectorFilterTypeFlags_GetForGeometry(const QrGeometryUploadInfo &info);
void                            VertexCollectorFilterTypeFlags_IterateOverFlags(std::function<void(VertexCollectorFilterTypeFlags)> f);

}
