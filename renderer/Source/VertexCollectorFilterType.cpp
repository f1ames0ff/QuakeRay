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

#include "VertexCollectorFilterType.h"

#include <cassert>
#include <cstring>

#include "Const.h"
#include "Generated/ShaderCommonC.h"

namespace
{
    typedef qray::VertexCollectorFilterTypeFlagBits FT;
    typedef qray::VertexCollectorFilterTypeFlags FL;

    constexpr uint32_t MAX_FLAG_VALUE_CF = 4;
    constexpr uint32_t MAX_FLAG_VALUE_PT = 8;
    constexpr uint32_t MAX_FLAG_VALUE_PV = 16;

    typedef uint8_t FlagToIndexType;

    constexpr uint32_t FLAG_TO_INDEX_MAX_VALUE = 1u << (8 * sizeof(FlagToIndexType));

    static_assert(
        sizeof(qray::VertexCollectorFilterGroup_ChangeFrequency)   / sizeof(qray::VertexCollectorFilterGroup_ChangeFrequency[0]) *
        sizeof(qray::VertexCollectorFilterGroup_PassThrough)       / sizeof(qray::VertexCollectorFilterGroup_PassThrough[0]) *
        sizeof(qray::VertexCollectorFilterGroup_PrimaryVisibility) / sizeof(qray::VertexCollectorFilterGroup_PrimaryVisibility[0])
        == MAX_TOP_LEVEL_INSTANCE_COUNT, "It's recommended for MAX_TOP_LEVEL_INSTANCE_COUNT to be such value");

    static_assert(MAX_TOP_LEVEL_INSTANCE_COUNT < FLAG_TO_INDEX_MAX_VALUE, "");

    FlagToIndexType FlagToIndex[MAX_FLAG_VALUE_CF][MAX_FLAG_VALUE_PT][MAX_FLAG_VALUE_PV];

    uint32_t AllBottomLevelGeomsCount = 0;
    uint32_t OffsetInGlobalArray[MAX_FLAG_VALUE_CF][MAX_FLAG_VALUE_PT][MAX_FLAG_VALUE_PV];
    uint32_t AmountInGlobalArray[MAX_FLAG_VALUE_CF][MAX_FLAG_VALUE_PT][MAX_FLAG_VALUE_PV];

    void GetGroupIndices(FL flags, uint32_t &cf, uint32_t &pt, uint32_t &pv)
    {
        cf = (flags & FT::MASK_CHANGE_FREQUENCY_GROUP)    >> qray::VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF;
        pt = (flags & FT::MASK_PASS_THROUGH_GROUP)        >> qray::VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT;
        pv = (flags & FT::MASK_PRIMARY_VISIBILITY_GROUP)  >> qray::VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV;

        assert(cf > 0 && cf <= MAX_FLAG_VALUE_CF);
        assert(pt > 0 && pt <= MAX_FLAG_VALUE_PT);
        assert(pv > 0 && pv <= MAX_FLAG_VALUE_PV);

        cf--;
        pt--;
        pv--;
    }

    struct FLName
    {
        FL          flags;
        const char *name;
    };

    const FLName FL_NAMES[] =
    {
        /* the glass class first: it is an alpha-tested and a refract class at
           once, and the lookup below returns the first subset match */
        { FT::CF_STATIC_NON_MOVABLE | FT::PT_GLASS,         "BLAS static glass"         },
        { FT::CF_STATIC_NON_MOVABLE | FT::PT_OPAQUE,        "BLAS static opaque"        },
        { FT::CF_STATIC_NON_MOVABLE | FT::PT_ALPHA_TESTED,  "BLAS static alpha tested"  },
        { FT::CF_STATIC_NON_MOVABLE | FT::PT_REFRACT,       "BLAS static refract"       },

        { FT::CF_STATIC_MOVABLE     | FT::PT_GLASS,         "BLAS movable glass"        },
        { FT::CF_STATIC_MOVABLE     | FT::PT_OPAQUE,        "BLAS movable opaque"       },
        { FT::CF_STATIC_MOVABLE     | FT::PT_ALPHA_TESTED,  "BLAS movable alpha tested" },
        { FT::CF_STATIC_MOVABLE     | FT::PT_REFRACT,       "BLAS movable refract"      },

        { FT::CF_DYNAMIC            | FT::PT_GLASS,         "BLAS dynamic glass"        },
        { FT::CF_DYNAMIC            | FT::PT_OPAQUE,        "BLAS dynamic opaque"       },
        { FT::CF_DYNAMIC            | FT::PT_ALPHA_TESTED,  "BLAS dynamic alpha tested" },
        { FT::CF_DYNAMIC            | FT::PT_REFRACT,       "BLAS dynamic refract"      },
    };
}

void qray::VertexCollectorFilterTypeFlags_IterateOverFlags(std::function<void(FL)> f)
{
    for (auto cf : VertexCollectorFilterGroup_ChangeFrequency)
    {
        for (auto pt : VertexCollectorFilterGroup_PassThrough)
        {
            for (auto pv : VertexCollectorFilterGroup_PrimaryVisibility)
            {
                f(cf | pt | pv);
            }
        }
    }
}

void qray::VertexCollectorFilterTypeFlags_Init()
{
    memset(FlagToIndex, 0xFF, sizeof(FlagToIndex));

    AllBottomLevelGeomsCount = 0;
    memset(OffsetInGlobalArray, 0, sizeof(OffsetInGlobalArray));

    uint32_t index = 0;

    for (auto flcf : VertexCollectorFilterGroup_ChangeFrequency)
    {
        for (auto flpt : VertexCollectorFilterGroup_PassThrough)
        {
            for (auto flpv : VertexCollectorFilterGroup_PrimaryVisibility)
            {
                const uint32_t cf = (uint32_t)flcf >> VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_CF;
                const uint32_t pt = (uint32_t)flpt >> VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PT;
                const uint32_t pv = (uint32_t)flpv >> VERTEX_COLLECTOR_FILTER_TYPE_BIT_OFFSET_PV;

                assert(cf > 0 && cf <= MAX_FLAG_VALUE_CF);
                assert(pt > 0 && pt <= MAX_FLAG_VALUE_PT);
                assert(pv > 0 && pv <= MAX_FLAG_VALUE_PV);

                assert(index < MAX_TOP_LEVEL_INSTANCE_COUNT);
                assert(index < FLAG_TO_INDEX_MAX_VALUE);

                FlagToIndex[cf - 1][pt - 1][pv - 1] = (FlagToIndexType)index;

                index++;

                const bool hasLowerAmount =
                    (flpv & FT::PV_FIRST_PERSON) ||
                    (flpv & FT::PV_FIRST_PERSON_VIEWER);

                assert(LOWER_BOTTOM_LEVEL_GEOMETRIES_COUNT < MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT);

                const uint32_t count = hasLowerAmount ?
                    LOWER_BOTTOM_LEVEL_GEOMETRIES_COUNT :
                    MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT;

                AmountInGlobalArray[cf - 1][pt - 1][pv - 1] = count;
                OffsetInGlobalArray[cf - 1][pt - 1][pv - 1] = AllBottomLevelGeomsCount;
                AllBottomLevelGeomsCount += count;
            }
        }
    }
}

uint32_t qray::VertexCollectorFilterTypeFlags_GetID(VertexCollectorFilterTypeFlags flags)
{
    uint32_t cf, pt, pv;
    GetGroupIndices(flags, cf, pt, pv);

    return FlagToIndex[cf][pt][pv];
}

uint32_t qray::VertexCollectorFilterTypeFlags_GetAllBottomLevelGeomsCount()
{
    return AllBottomLevelGeomsCount;
}

uint32_t qray::VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(VertexCollectorFilterTypeFlags flags)
{
    uint32_t cf, pt, pv;
    GetGroupIndices(flags, cf, pt, pv);

    return OffsetInGlobalArray[cf][pt][pv];
}

uint32_t qray::VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(VertexCollectorFilterTypeFlags flags)
{
    uint32_t cf, pt, pv;
    GetGroupIndices(flags, cf, pt, pv);

    return AmountInGlobalArray[cf][pt][pv];
}

const char *qray::VertexCollectorFilterTypeFlags_GetNameForBLAS(FL flags)
{
    for (const FLName &p : FL_NAMES)
    {
        if ((p.flags & flags) == p.flags)
        {
            return p.name;
        }
    }

    assert(0);
    return nullptr;
}

FL qray::VertexCollectorFilterTypeFlags_GetForGeometry(const QrGeometryUploadInfo &info)
{
    FL flags = 0;

    switch (info.geomType)
    {
        case QR_GEOMETRY_TYPE_STATIC:
            flags |= (FL)FT::CF_STATIC_NON_MOVABLE;
            break;

        case QR_GEOMETRY_TYPE_STATIC_MOVABLE:
            flags |= (FL)FT::CF_STATIC_MOVABLE;
            break;

        case QR_GEOMETRY_TYPE_DYNAMIC:
            flags |= (FL)FT::CF_DYNAMIC;
            break;

        default:
            assert(0);
    }

    switch (info.passThroughType)
    {
        case QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE:
        case QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT:
        case QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR:
        case QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL:
            flags |= (FL)FT::PT_OPAQUE;
            break;

        case QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED:
            flags |= (FL)FT::PT_ALPHA_TESTED;
            break;

        case QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT:
            flags |= (FL)FT::PT_GLASS;
            break;

        case QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT:
        case QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT:
            flags |= (FL)FT::PT_REFRACT;
            break;

        default:
            assert(0);
    }

    switch (info.visibilityType)
    {
        case QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0:
            flags |= (FL)FT::PV_WORLD_0;
            break;

        case QR_GEOMETRY_VISIBILITY_TYPE_WORLD_1:
            flags |= (FL)FT::PV_WORLD_1;
            break;

        case QR_GEOMETRY_VISIBILITY_TYPE_WORLD_2:
            flags |= (FL)FT::PV_WORLD_2;
            break;

        case QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON:
            flags |= (FL)FT::PV_FIRST_PERSON;
            break;

        case QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER:
            flags |= (FL)FT::PV_FIRST_PERSON_VIEWER;
            break;

    #if RAYCULLMASK_SKY_IS_WORLD2
        case QR_GEOMETRY_VISIBILITY_TYPE_SKY:
            flags |= (FL)FT::PV_WORLD_2;
            break;
    #else
        #error Handle QR_DRAW_FRAME_RAY_CULL_SKY_BIT, if there is no WORLD_2
    #endif

        default:
            assert(0);
    }

    return flags;
}
