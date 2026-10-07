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

#include <atomic>

#include "ASBuilder.h"
#include "CommandBufferManager.h"
#include "GlobalUniform.h"
#include "ScratchBuffer.h"
#include "TextureManager.h"
#include "VertexCollector.h"
#include "ASComponent.h"

namespace qray
{

struct ShVertPreprocessing;

class ASManager
{
public:
    struct TLASPrepareResult
    {
        /* must match MAX_TOP_LEVEL_INSTANCE_COUNT (the asserts in ASManager.cpp pin it) */
        VkAccelerationStructureInstanceKHR instances[61];
        uint32_t instanceCount;
    };

public:
    ASManager(VkDevice device,
              std::shared_ptr<PhysicalDevice> physDevice,
              std::shared_ptr<MemoryAllocator> allocator,
              std::shared_ptr<CommandBufferManager> cmdManager,
              std::shared_ptr<TextureManager> textureManager,
              std::shared_ptr<GeomInfoManager> geomInfoManager);
    ~ASManager();

    ASManager(const ASManager& other) = delete;
    ASManager(ASManager&& other) noexcept = delete;
    ASManager& operator=(const ASManager& other) = delete;
    ASManager& operator=(ASManager&& other) noexcept = delete;


    void BeginStaticGeometry();
    uint32_t AddStaticGeometry(uint32_t frameIndex, const QrGeometryUploadInfo &info);
    void SubmitStaticGeometry();
    void ResetStaticGeometry();

    void BeginDynamicGeometry(VkCommandBuffer cmd, uint32_t frameIndex);
    uint32_t AddDynamicGeometry(uint32_t frameIndex, const QrGeometryUploadInfo &info);


    void UpdateStaticMovableTransform(uint32_t simpleIndex, const QrUpdateTransformInfo &updateInfo);

    void UpdateStaticTexCoords(uint32_t simpleIndex, const QrUpdateTexCoordsInfo &texCoordsInfo);


    std::pair<TLASPrepareResult, ShVertPreprocessing> PrepareForBuildingTLAS(
        uint32_t frameIndex,
        ShGlobalUniform &uniformData,
        uint32_t uniformData_rayCullMaskWorld,
        bool allowGeometryWithSkyFlag,
        bool disableRTGeometry) const;
    void BuildTLAS(
        VkCommandBuffer cmd, uint32_t frameIndex,
        const TLASPrepareResult &info);


    void CopyDynamicDataToPrevBuffers(VkCommandBuffer cmd, uint32_t frameIndex);


    void OnVertexPreprocessingBegin(VkCommandBuffer cmd, uint32_t frameIndex, bool onlyDynamic);
    void OnVertexPreprocessingFinish(VkCommandBuffer cmd, uint32_t frameIndex, bool onlyDynamic);


    VkDescriptorSet GetBuffersDescSet(uint32_t frameIndex) const;
    VkDescriptorSet GetTLASDescSet(uint32_t frameIndex) const;

    VkDescriptorSetLayout GetBuffersDescSetLayout() const;
    VkDescriptorSetLayout GetTLASDescSetLayout() const;

    const std::shared_ptr<VertexCollector> &GetStaticCollector() const;
    const std::shared_ptr<VertexCollector> &GetDynamicCollector(uint32_t frameIndex) const;

    const std::shared_ptr<GeomInfoManager> &GetGeomInfoManager() const;

    const std::vector<std::unique_ptr<BLASComponent>> &GetStaticBlasComponents() const;
    VkBuffer GetInstanceBuffer() const;
    VkDeviceSize GetInstanceBufferSize() const;

    static bool GetTLASInstanceForFilter(VertexCollectorFilterTypeFlags filter,
                                         uint32_t rayCullMaskWorld,
                                         bool allowGeometryWithSkyFlag,
                                         VkAccelerationStructureInstanceKHR &instance);

    uint32_t GetStaticGeneration() const;
    uint32_t GetStaticMovableRevision() const;

private:
    void CreateDescriptors();
    void UpdateBufferDescriptors(uint32_t frameIndex);
    void UpdateASDescriptors(uint32_t frameIndex);

    bool SetupBLAS(
        BLASComponent &as,
        const std::shared_ptr<VertexCollector> &vertCollector);

    static bool SetupTLASInstanceFromBLAS(
        const BLASComponent &as,
        uint32_t rayCullMaskWorld,
        bool allowGeometryWithSkyFlag,
        VkAccelerationStructureInstanceKHR &instance);

    static bool IsFastBuild(VertexCollectorFilterTypeFlags filter);

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;

    VkFence staticCopyFence;

    std::shared_ptr<VertexCollector> collectorStatic;
    std::shared_ptr<VertexCollector> collectorDynamic[MAX_FRAMES_IN_FLIGHT];
    Buffer previousDynamicPositions;
    Buffer previousDynamicIndices;

    std::shared_ptr<ScratchBuffer> scratchBuffer;
    std::shared_ptr<ASBuilder> asBuilder;

    std::shared_ptr<CommandBufferManager> cmdManager;
    std::shared_ptr<TextureManager> textureMgr;
    std::shared_ptr<GeomInfoManager> geomInfoMgr;

    std::vector<std::unique_ptr<BLASComponent>> allStaticBlas;
    std::vector<std::unique_ptr<BLASComponent>> allDynamicBlas[MAX_FRAMES_IN_FLIGHT];

    uint32_t staticGeneration = 0;
    std::atomic<uint32_t> staticMovableRevision{0};

    VkAccelerationStructureBuildSizesInfoKHR tlasBuildSizes[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t tlasBuildSizesInstanceCount[MAX_FRAMES_IN_FLIGHT] = {};
    bool tlasBuildSizesValid[MAX_FRAMES_IN_FLIGHT] = {};

    std::unique_ptr<AutoBuffer> instanceBuffer;
    std::unique_ptr<TLASComponent> tlas[MAX_FRAMES_IN_FLIGHT];

    VkDescriptorPool descPool;

    VkDescriptorSetLayout buffersDescSetLayout;
    VkDescriptorSet buffersDescSets[MAX_FRAMES_IN_FLIGHT];

    VkDescriptorSetLayout asDescSetLayout;
    VkDescriptorSet asDescSets[MAX_FRAMES_IN_FLIGHT];
    VkAccelerationStructureKHR asDescHandles[MAX_FRAMES_IN_FLIGHT] = {};
};

}
