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

#include <span>
#include <vector>

#include "Buffer.h"
#include "Common.h"
#include "GeomInfoManager.h"
#include "IMaterialDependency.h"
#include "Material.h"
#include "VertexCollectorFilter.h"
#include "qray/qray.h"

namespace qray
{

struct ShGeometryInstance;
struct ShVertex;

class VertexCollector : public IMaterialDependency
{
public:
    explicit VertexCollector(
        VkDevice device,
        const std::shared_ptr<MemoryAllocator> &allocator,
        std::shared_ptr<GeomInfoManager> geomInfoManager,
        VkDeviceSize bufferSize,
        VertexCollectorFilterTypeFlags filters);

    explicit VertexCollector(
        const std::shared_ptr<const VertexCollector> &src,
        const std::shared_ptr<MemoryAllocator> &allocator);

    ~VertexCollector() override;

    VertexCollector(const VertexCollector& other) = delete;
    VertexCollector(VertexCollector&& other) noexcept = delete;
    VertexCollector& operator=(const VertexCollector& other) = delete;
    VertexCollector& operator=(VertexCollector&& other) noexcept = delete;


    void BeginCollecting(bool isStatic);
    uint32_t AddGeometry(uint32_t frameIndex, const QrGeometryUploadInfo &info, std::span<MaterialTextures, 3> materials);
    void EndCollecting();


    virtual void Reset();
    bool CopyFromStaging(VkCommandBuffer cmd);


    void UpdateTransform(uint32_t simpleIndex, const QrUpdateTransformInfo &updateInfo);
    void UpdateTexCoords(uint32_t simpleIndex, const QrUpdateTexCoordsInfo &texCoordsInfo, bool isStatic);


    void OnMaterialChange(uint32_t materialIndex, const MaterialTextures &newInfo) override;


    VkBuffer GetVertexBuffer() const;
    VkBuffer GetIndexBuffer() const;
    uint32_t GetCurrentVertexCount() const;
    uint32_t GetCurrentIndexCount() const;

    VkDeviceAddress GetVertexBufferAddress() const;
    VkDeviceAddress GetIndexBufferAddress() const;
    VkDeviceAddress GetTransformsBufferAddress() const;
    VkDeviceSize GetVertexBufferSize() const;
    VkDeviceSize GetIndexBufferSize() const;
    const VkTransformMatrixKHR *GetTransformsStaging() const;
    VkBuffer GetStagingVertexBuffer() const;
    VkBuffer GetStagingIndexBuffer() const;

    struct GeometryDrawInfo
    {
        VkBuffer vertexBuffer;
        VkBuffer indexBuffer;
        uint32_t baseVertex;
        uint32_t firstIndex;
        uint32_t indexCount;
        float model[16];
    };
    std::vector<GeometryDrawInfo> GetGeometryDrawInfos() const;


    const std::vector<uint32_t> &GetPrimitiveCounts(VertexCollectorFilterTypeFlags filter) const;

    const std::vector<VkAccelerationStructureGeometryKHR> &GetASGeometries(VertexCollectorFilterTypeFlags filter) const;

    const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &GetASBuildRangeInfos(VertexCollectorFilterTypeFlags filter) const;


    bool AreGeometriesEmpty(VertexCollectorFilterTypeFlags flags) const;
    bool AreGeometriesEmpty(VertexCollectorFilterTypeFlagBits type) const;


    void InsertVertexPreprocessBeginBarrier(VkCommandBuffer cmd);
    void InsertVertexPreprocessFinishBarrier(VkCommandBuffer cmd);

private:
    void InitStagingBuffers(const std::shared_ptr<MemoryAllocator> &allocator);

    void CopyDataToStaging(const QrGeometryUploadInfo &info, uint32_t vertIndex);

    bool CopyVertexDataFromStaging(VkCommandBuffer cmd);
    bool CopyIndexDataFromStaging(VkCommandBuffer cmd);
    bool CopyTransformsFromStaging(VkCommandBuffer cmd);

    void AddMaterialDependency(uint32_t simpleIndex, uint32_t layer, uint32_t materialIndex);

    void InitFilters(VertexCollectorFilterTypeFlags flags);

    void AddFilter(VertexCollectorFilterTypeFlags filterGroup);
    uint32_t PushGeometry(VertexCollectorFilterTypeFlags type, const VkAccelerationStructureGeometryKHR &geom);
    void PushPrimitiveCount(VertexCollectorFilterTypeFlags type, uint32_t primCount);
    void PushRangeInfo(VertexCollectorFilterTypeFlags type, const VkAccelerationStructureBuildRangeInfoKHR &rangeInfo);

    uint32_t GetGeometryCount(VertexCollectorFilterTypeFlags type);
    uint32_t GetAllGeometryCount() const;

private:
    struct MaterialRef
    {
        uint32_t simpleIndex;
        uint32_t layer;
    };

private:
    VkDevice device;
    VertexCollectorFilterTypeFlags filtersFlags;

    Buffer stagingVertBuffer;
    std::shared_ptr<Buffer> vertBuffer;

    Buffer stagingIndexBuffer;
    std::shared_ptr<Buffer> indexBuffer;

    Buffer stagingTransformsBuffer;
    std::shared_ptr<Buffer> transformsBuffer;

    std::shared_ptr<GeomInfoManager> geomInfoMgr;

    uint32_t curVertexCount;
    uint32_t curIndexCount;
    uint32_t curPrimitiveCount;
    uint32_t curTransformCount;

    ShVertex *mappedVertexData;
    uint32_t *mappedIndexData;
    VkTransformMatrixKHR *mappedTransformData;

    rgl::unordered_map<uint32_t, std::vector<MaterialRef>> materialDependencies;
    rgl::unordered_map<VertexCollectorFilterTypeFlags, std::shared_ptr<VertexCollectorFilter>> filters;

    rgl::unordered_map<uint32_t, uint32_t> simpleIndexToTransformIndex;
};

}
