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

#include "ASBuilder.h"

#include <algorithm>
#include <utility>

#include "Utils.h"

using namespace qray;

namespace
{
    VkBuildAccelerationStructureFlagsKHR GetBuildFlags(bool fastTrace)
    {
        return fastTrace ?
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR :
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }

    void SetupBuildGeometryInfo(
        VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
        VkAccelerationStructureTypeKHR type,
        VkAccelerationStructureKHR as,
        VkDeviceAddress scratchAddress,
        VkBuildAccelerationStructureFlagsKHR flags,
        bool update)
    {
        buildInfo = {};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = type;
        buildInfo.flags = flags;
        buildInfo.mode = update ?
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR :
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.srcAccelerationStructure = update ? as : VK_NULL_HANDLE;
        buildInfo.dstAccelerationStructure = as;
        buildInfo.scratchData.deviceAddress = scratchAddress;
        buildInfo.ppGeometries = nullptr;
    }
}

ASBuilder::ASBuilder(VkDevice device, std::shared_ptr<ScratchBuffer> commonScratchBuffer) :
    device(device),
    scratchBuffer(std::move(commonScratchBuffer))
{
}

VkAccelerationStructureBuildSizesInfoKHR ASBuilder::GetBuildSizes(
    VkAccelerationStructureTypeKHR type,
    uint32_t geometryCount,
    const VkAccelerationStructureGeometryKHR *pGeometries,
    const uint32_t *pMaxPrimitiveCount,
    bool fastTrace) const
{
    assert(geometryCount > 0);

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = type;
    buildInfo.flags = GetBuildFlags(fastTrace);
    buildInfo.geometryCount = geometryCount;
    buildInfo.pGeometries = pGeometries;
    buildInfo.ppGeometries = nullptr;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    svkGetAccelerationStructureBuildSizesKHR(
        device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, pMaxPrimitiveCount, &sizeInfo);

    return sizeInfo;
}

VkAccelerationStructureBuildSizesInfoKHR ASBuilder::GetBottomBuildSizes(
    uint32_t geometryCount,
    const VkAccelerationStructureGeometryKHR *pGeometries,
    const uint32_t *pMaxPrimitiveCount, bool fastTrace) const
{
    return GetBuildSizes(
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometryCount,
        pGeometries, pMaxPrimitiveCount, fastTrace);
}

VkAccelerationStructureBuildSizesInfoKHR ASBuilder::GetTopBuildSizes(
    const VkAccelerationStructureGeometryKHR *pGeometry,
    uint32_t maxPrimitiveCount, bool fastTrace) const
{
    return GetBuildSizes(
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1,
        pGeometry, &maxPrimitiveCount, fastTrace);
}

void ASBuilder::AddBLAS(
    VkAccelerationStructureKHR as, uint32_t geometryCount,
    const VkAccelerationStructureGeometryKHR *pGeometries,
    const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfos,
    const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
    bool fastTrace, bool update, bool isBLASUpdateable)
{
    assert(topLBuildInfo.geomInfos.empty() && topLBuildInfo.rangeInfos.empty());
    assert(geometryCount > 0);

    const VkDeviceSize scratchSize = std::max(buildSizes.updateScratchSize, buildSizes.buildScratchSize);

    VkBuildAccelerationStructureFlagsKHR flags = GetBuildFlags(fastTrace);

    if (isBLASUpdateable || update)
    {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    SetupBuildGeometryInfo(
        buildInfo, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, as,
        scratchBuffer->GetScratchAddress(scratchSize), flags, update);

    buildInfo.geometryCount = geometryCount;
    buildInfo.pGeometries = pGeometries;

    bottomLBuildInfo.geomInfos.push_back(buildInfo);
    bottomLBuildInfo.rangeInfos.push_back(pRangeInfos);
}

void ASBuilder::BuildBottomLevel(VkCommandBuffer cmd)
{
    assert(bottomLBuildInfo.geomInfos.size() == bottomLBuildInfo.rangeInfos.size());
    assert(!bottomLBuildInfo.geomInfos.empty());

    svkCmdBuildAccelerationStructuresKHR(
        cmd, bottomLBuildInfo.geomInfos.size(),
        bottomLBuildInfo.geomInfos.data(), bottomLBuildInfo.rangeInfos.data());

    bottomLBuildInfo.geomInfos.clear();
    bottomLBuildInfo.rangeInfos.clear();
}

void ASBuilder::AddTLAS(
    VkAccelerationStructureKHR as,
    const VkAccelerationStructureGeometryKHR *pGeometry,
    const VkAccelerationStructureBuildRangeInfoKHR *pRangeInfo,
    const VkAccelerationStructureBuildSizesInfoKHR &buildSizes,
    bool fastTrace, bool update)
{
    assert(bottomLBuildInfo.geomInfos.empty() && bottomLBuildInfo.rangeInfos.empty());

    const VkDeviceSize scratchSize = update ? buildSizes.updateScratchSize : buildSizes.buildScratchSize;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    SetupBuildGeometryInfo(
        buildInfo, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, as,
        scratchBuffer->GetScratchAddress(scratchSize), GetBuildFlags(fastTrace), update);

    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = pGeometry;

    topLBuildInfo.geomInfos.push_back(buildInfo);
    topLBuildInfo.rangeInfos.push_back(pRangeInfo);
}

void ASBuilder::BuildTopLevel(VkCommandBuffer cmd)
{
    assert(topLBuildInfo.geomInfos.size() == topLBuildInfo.rangeInfos.size());
    assert(!topLBuildInfo.geomInfos.empty());

    svkCmdBuildAccelerationStructuresKHR(
        cmd, topLBuildInfo.geomInfos.size(),
        topLBuildInfo.geomInfos.data(), topLBuildInfo.rangeInfos.data());

    topLBuildInfo.geomInfos.clear();
    topLBuildInfo.rangeInfos.clear();
}

bool ASBuilder::IsEmpty() const
{
    return bottomLBuildInfo.geomInfos.empty() && bottomLBuildInfo.rangeInfos.empty() &&
        topLBuildInfo.geomInfos.empty() && topLBuildInfo.rangeInfos.empty();
}
