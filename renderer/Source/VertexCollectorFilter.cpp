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

#include "VertexCollectorFilter.h"

#include "QrException.h"

using namespace qray;

VertexCollectorFilter::VertexCollectorFilter(VertexCollectorFilterTypeFlags _filter) :
    filter(_filter)
{
}

VertexCollectorFilter::~VertexCollectorFilter()
{
}

const std::vector<uint32_t> &VertexCollectorFilter::GetPrimitiveCounts() const
{
    return primitiveCounts;
}

const std::vector<VkAccelerationStructureGeometryKHR> &VertexCollectorFilter::GetASGeometries() const
{
    return asGeometries;
}

const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &VertexCollectorFilter::GetASBuildRangeInfos() const
{
    return asBuildRangeInfos;
}

void VertexCollectorFilter::Reset()
{
    asGeometries.clear();
    primitiveCounts.clear();
    asBuildRangeInfos.clear();
}

uint32_t VertexCollectorFilter::PushGeometry(
    VertexCollectorFilterTypeFlags type, const VkAccelerationStructureGeometryKHR &geom)
{
    assert((type & filter) == filter);

    const uint32_t localIndex = (uint32_t)asGeometries.size();
    asGeometries.push_back(geom);

    assert(localIndex < VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(type));

    return localIndex;
}

void VertexCollectorFilter::PushPrimitiveCount(VertexCollectorFilterTypeFlags type, uint32_t primCount)
{
    assert((type & filter) == filter);

    primitiveCounts.push_back(primCount);
}

void VertexCollectorFilter::PushRangeInfo(
    VertexCollectorFilterTypeFlags type, const VkAccelerationStructureBuildRangeInfoKHR &rangeInfo)
{
    assert((type & filter) == filter);

    asBuildRangeInfos.push_back(rangeInfo);
}

VertexCollectorFilterTypeFlags VertexCollectorFilter::GetFilter() const
{
    return filter;
}

uint32_t VertexCollectorFilter::GetGeometryCount() const
{
    return (uint32_t)asGeometries.size();
}
