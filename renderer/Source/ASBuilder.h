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

#include <vector>

#include "Common.h"
#include "ScratchBuffer.h"

namespace qray
{

class ASBuilder
{
public:
    explicit ASBuilder(VkDevice device, std::shared_ptr<ScratchBuffer> commonScratchBuffer);

    ASBuilder(const ASBuilder& other) = delete;
    ASBuilder(ASBuilder&& other) noexcept = delete;
    ASBuilder& operator=(const ASBuilder& other) = delete;
    ASBuilder& operator=(ASBuilder&& other) noexcept = delete;

    void AddBLAS(
        VkAccelerationStructureKHR as, uint32_t geometryCount,
        const VkAccelerationStructureGeometryKHR *pGeometries,
        const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfos,
        const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
        bool fastTrace, bool update, bool isBLASUpdateable);

    void BuildBottomLevel(VkCommandBuffer cmd);

    void AddTLAS(
        VkAccelerationStructureKHR as,
        const VkAccelerationStructureGeometryKHR *pGeometry,
        const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfo,
        const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
        bool fastTrace, bool update);

    void BuildTopLevel(VkCommandBuffer cmd);

    VkAccelerationStructureBuildSizesInfoKHR GetBuildSizes(
        VkAccelerationStructureTypeKHR type, uint32_t geometryCount,
        const VkAccelerationStructureGeometryKHR *pGeometries,
        const uint32_t *pMaxPrimitiveCount, bool fastTrace) const;

    VkAccelerationStructureBuildSizesInfoKHR GetBottomBuildSizes(
        uint32_t geometryCount,
        const VkAccelerationStructureGeometryKHR *pGeometries,
        const uint32_t *pMaxPrimitiveCount, bool fastTrace) const;

    VkAccelerationStructureBuildSizesInfoKHR GetTopBuildSizes(
        const VkAccelerationStructureGeometryKHR *pGeometry,
        uint32_t maxPrimitiveCount, bool fastTrace) const;

    bool IsEmpty() const;

private:
    VkDevice device;
    std::shared_ptr<ScratchBuffer> scratchBuffer;

    struct BuildInfo
    {
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> geomInfos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangeInfos;
    };

    BuildInfo bottomLBuildInfo;
    BuildInfo topLBuildInfo;
};

}
