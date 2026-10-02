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

#include "AutoBuffer.h"
#include "Common.h"
#include "Containers.h"
#include "Material.h"
#include "MemoryAllocator.h"
#include "VertexCollectorFilterType.h"

namespace qray
{

struct ShGeometryInstance;

class GeomInfoManager
{
public:
    explicit GeomInfoManager(
        VkDevice device,
        std::shared_ptr<MemoryAllocator> &allocator);
    ~GeomInfoManager();

    GeomInfoManager(const GeomInfoManager &other) = delete;
    GeomInfoManager(GeomInfoManager &&other) noexcept = delete;
    GeomInfoManager & operator=(const GeomInfoManager &other) = delete;
    GeomInfoManager & operator=(GeomInfoManager &&other) noexcept = delete;


    void PrepareForFrame(uint32_t frameIndex);
    void ResetWithStatic();


    uint32_t WriteGeomInfo(
        uint32_t frameIndex,
        uint64_t geomUniqueID,
        uint32_t localGeomIndex,
        VertexCollectorFilterTypeFlags flags,
        ShGeometryInstance &src);


    void WriteStaticGeomInfoMaterials(uint32_t simpleIndex, uint32_t layer, const MaterialTextures &src);
    void WriteStaticGeomInfoTransform(uint32_t simpleIndex, uint64_t geomUniqueID, const QrTransform &src);


    bool CopyFromStaging(VkCommandBuffer cmd, uint32_t frameIndex, bool insertBarrier = true);


    uint32_t GetCount() const;
    uint32_t GetStaticCount() const;
    uint32_t GetDynamicCount() const;
    VkBuffer GetBuffer() const;
    VkBuffer GetMatchPrevBuffer() const;
    uint32_t GetStaticGeomBaseVertexIndex(uint32_t simpleIndex);

    VkBuffer GetStagingBuffer(uint32_t frameIndex);
    VkDeviceSize GetBufferSize() const;
    VkDeviceSize GetMatchPrevSize() const;
    const int32_t *GetMatchPrevData() const;

private:
    struct GeomFrameInfo
    {
        float model[16];
        uint32_t baseVertexIndex;
        uint32_t baseIndexIndex;
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t prevGlobalGeomIndex;
    };

    struct MatchPrevCopyInfo
    {
        uint32_t maxStaticGeomCount = 0;
        uint32_t maxDynamicGeomCount = 0;
    };

private:
    void ResetMatchPrevForGroup(uint32_t frameIndex, VertexCollectorFilterTypeFlags groupFlags);

    void ResetOnlyDynamic(uint32_t frameIndex);

    static uint32_t GetGlobalGeomIndex(uint32_t localGeomIndex, VertexCollectorFilterTypeFlags flags);
    ShGeometryInstance *GetGeomInfoAddressByGlobalIndex(uint32_t frameIndex, uint32_t globalGeomIndex);

    uint32_t ConvertSimpleIndexToGlobal(uint32_t simpleIndex) const;

    void MarkGeomInfoIndexToCopy(uint32_t frameIndex, uint32_t localGeomIndex, uint32_t flagsOffset);

    void FillWithPrevFrameData(
        VertexCollectorFilterTypeFlags flags, uint64_t geomUniqueID,
        uint32_t currentGlobalGeomIndex, ShGeometryInstance &dst, int32_t frameIndex = 0);

    void MarkNoPrevInfo(ShGeometryInstance &dst);
    void MarkMovableHasPrevInfo(ShGeometryInstance &dst);
    void WriteInfoForNextUsage(
        VertexCollectorFilterTypeFlags flags, uint64_t geomUniqueID,
        uint32_t currentGlobalGeomIndex, const ShGeometryInstance &src, int32_t frameIndex = 0);

private:
    VkDevice device;

    uint32_t staticGeomCount;
    uint32_t dynamicGeomCount;

    std::shared_ptr<AutoBuffer> buffer;
    std::shared_ptr<AutoBuffer> matchPrev;
    std::unique_ptr<int32_t[]> matchPrevShadow;
    MatchPrevCopyInfo matchPrevCopyInfo;

    std::vector<uint32_t> copyRegionLowerBounds[MAX_FRAMES_IN_FLIGHT];
    std::vector<uint32_t> copyRegionUpperBounds[MAX_FRAMES_IN_FLIGHT];

    std::vector<VertexCollectorFilterTypeFlags> geomType;

    std::vector<uint32_t> simpleToLocalIndex;

    rgl::unordered_map<uint64_t, GeomFrameInfo> dynamicIDToGeomFrameInfo[MAX_FRAMES_IN_FLIGHT];
    rgl::unordered_map<uint64_t, GeomFrameInfo> movableIDToGeomFrameInfo;
};

}
