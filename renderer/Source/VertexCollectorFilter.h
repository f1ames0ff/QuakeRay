// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#include <vector>

#include "Common.h"
#include "VertexCollectorFilterType.h"

namespace qray
{

class VertexCollectorFilter
{
public:
    explicit VertexCollectorFilter(VertexCollectorFilterTypeFlags filter);
    ~VertexCollectorFilter();

    VertexCollectorFilter(const VertexCollectorFilter& other) = delete;
    VertexCollectorFilter(VertexCollectorFilter&& other) noexcept = delete;
    VertexCollectorFilter& operator=(const VertexCollectorFilter& other) = delete;
    VertexCollectorFilter& operator=(VertexCollectorFilter&& other) noexcept = delete;

    const std::vector<uint32_t>
        &GetPrimitiveCounts() const;
    const std::vector<VkAccelerationStructureGeometryKHR>
        &GetASGeometries() const;
    const std::vector<VkAccelerationStructureBuildRangeInfoKHR>
        &GetASBuildRangeInfos() const;

    void Reset();

    uint32_t PushGeometry(VertexCollectorFilterTypeFlags type, const VkAccelerationStructureGeometryKHR& geom);
    void PushPrimitiveCount(VertexCollectorFilterTypeFlags type, uint32_t primCount);
    void PushRangeInfo(VertexCollectorFilterTypeFlags type, const VkAccelerationStructureBuildRangeInfoKHR &rangeInfo);

    VertexCollectorFilterTypeFlags GetFilter() const;
    uint32_t GetGeometryCount() const;

private:
    VertexCollectorFilterTypeFlags filter;

    std::vector<uint32_t> primitiveCounts;
    std::vector<VkAccelerationStructureGeometryKHR> asGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> asBuildRangeInfos;
};

}
