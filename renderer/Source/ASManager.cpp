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

#include "ASManager.h"

#include <array>
#include <cstring>

#include "Utils.h"
#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"

using namespace qray;

ASManager::ASManager(
    VkDevice _device,
    std::shared_ptr<PhysicalDevice> physDevice,
    std::shared_ptr<MemoryAllocator> _allocator,
    std::shared_ptr<CommandBufferManager> _cmdManager,
    std::shared_ptr<TextureManager> _textureManager,
    std::shared_ptr<GeomInfoManager> _geomInfoManager)
:
    device(_device),
    allocator(std::move(_allocator)),
    staticCopyFence(VK_NULL_HANDLE),
    cmdManager(std::move(_cmdManager)),
    textureMgr(std::move(_textureManager)),
    geomInfoMgr(std::move(_geomInfoManager)),
    descPool(VK_NULL_HANDLE),
    buffersDescSetLayout(VK_NULL_HANDLE),
    asDescSetLayout(VK_NULL_HANDLE)
{
    typedef VertexCollectorFilterTypeFlags FL;
    typedef VertexCollectorFilterTypeFlagBits FT;

    VertexCollectorFilterTypeFlags_IterateOverFlags([this] (FL filter)
    {
        if (filter & FT::CF_DYNAMIC)
        {
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            {
                allDynamicBlas[i].emplace_back(std::make_unique<BLASComponent>(device, filter));
            }
        }
        else
        {
            allStaticBlas.emplace_back(std::make_unique<BLASComponent>(device, filter));
        }
    });

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        tlas[i] = std::make_unique<TLASComponent>(device, "TLAS main");
    }

    const uint32_t scratchOffsetAlignment = physDevice->GetASProperties().minAccelerationStructureScratchOffsetAlignment;
    scratchBuffer = std::make_shared<ScratchBuffer>(allocator, scratchOffsetAlignment);
    asBuilder = std::make_shared<ASBuilder>(device, scratchBuffer);

    collectorStatic = std::make_shared<VertexCollector>(
        device, allocator, geomInfoMgr,
        MAX_STATIC_VERTEX_COUNT * sizeof(ShVertex),
        FT::CF_STATIC_NON_MOVABLE | FT::CF_STATIC_MOVABLE |
        FT::MASK_PASS_THROUGH_GROUP |
        FT::MASK_PRIMARY_VISIBILITY_GROUP);

    textureMgr->Subscribe(collectorStatic);

    collectorDynamic[0] = std::make_shared<VertexCollector>(
        device, allocator, geomInfoMgr,
        MAX_DYNAMIC_VERTEX_COUNT * sizeof(ShVertex),
        FT::CF_DYNAMIC |
        FT::MASK_PASS_THROUGH_GROUP |
        FT::MASK_PRIMARY_VISIBILITY_GROUP);

    for (uint32_t i = 1; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        collectorDynamic[i] = std::make_shared<VertexCollector>(collectorDynamic[0], allocator);
    }

    previousDynamicPositions.Init(
        allocator, MAX_DYNAMIC_VERTEX_COUNT * sizeof(ShVertex),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "Previous frame's vertex data");

    previousDynamicIndices.Init(
        allocator, MAX_DYNAMIC_VERTEX_COUNT * sizeof(uint32_t),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "Previous frame's index data");

    instanceBuffer = std::make_unique<AutoBuffer>(device, allocator);

    const VkDeviceSize instanceBufferSize = MAX_TOP_LEVEL_INSTANCE_COUNT * sizeof(VkAccelerationStructureInstanceKHR);
    instanceBuffer->Create(
        instanceBufferSize,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        "TLAS instance buffer");

    static_assert(std::size(TLASPrepareResult{}.instances) == MAX_TOP_LEVEL_INSTANCE_COUNT);

    CreateDescriptors();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        UpdateBufferDescriptors(i);
    }

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = 0;

    VkResult r = vkCreateFence(device, &fenceInfo, nullptr, &staticCopyFence);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, staticCopyFence, VK_OBJECT_TYPE_FENCE, "Static BLAS fence");
}

void ASManager::CreateDescriptors()
{
    VkResult r = VK_SUCCESS;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};

    {
        const VkDescriptorSetLayoutBinding bindings[] =
        {
            { BINDING_VERTEX_BUFFER_STATIC,          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_VERTEX_BUFFER_DYNAMIC,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_INDEX_BUFFER_STATIC,           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_INDEX_BUFFER_DYNAMIC,          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_GEOMETRY_INSTANCES,            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_GEOMETRY_INSTANCES_MATCH_PREV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_PREV_POSITIONS_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
            { BINDING_PREV_INDEX_BUFFER_DYNAMIC,     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = (uint32_t)std::size(bindings);
        layoutInfo.pBindings = bindings;

        r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &buffersDescSetLayout);
        VK_CHECKERROR(r);

        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT * (uint32_t)std::size(bindings);
    }

    {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = BINDING_ACCELERATION_STRUCTURE_MAIN;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &asDescSetLayout);
        VK_CHECKERROR(r);

        poolSizes[1].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    }

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT * 2;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "AS manager Desc pool");
    SET_DEBUG_NAME(device, buffersDescSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Vertex data Desc set layout");
    SET_DEBUG_NAME(device, asDescSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "TLAS Desc set layout");

    VkDescriptorSetAllocateInfo descSetInfo = {};
    descSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descSetInfo.descriptorPool = descPool;
    descSetInfo.descriptorSetCount = 1;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        descSetInfo.pSetLayouts = &buffersDescSetLayout;
        r = vkAllocateDescriptorSets(device, &descSetInfo, &buffersDescSets[i]);
        VK_CHECKERROR(r);

        descSetInfo.pSetLayouts = &asDescSetLayout;
        r = vkAllocateDescriptorSets(device, &descSetInfo, &asDescSets[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, buffersDescSets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, "Vertex data Desc set");
        SET_DEBUG_NAME(device, asDescSets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, "TLAS Desc set");
    }
}

void ASManager::UpdateBufferDescriptors(uint32_t frameIndex)
{
    const VkBuffer buffers[] =
    {
        collectorStatic->GetVertexBuffer(),
        collectorDynamic[frameIndex]->GetVertexBuffer(),
        collectorStatic->GetIndexBuffer(),
        collectorDynamic[frameIndex]->GetIndexBuffer(),
        geomInfoMgr->GetBuffer(),
        geomInfoMgr->GetMatchPrevBuffer(),
        previousDynamicPositions.GetBuffer(),
        previousDynamicIndices.GetBuffer(),
    };

    const uint32_t bindings[] =
    {
        BINDING_VERTEX_BUFFER_STATIC,
        BINDING_VERTEX_BUFFER_DYNAMIC,
        BINDING_INDEX_BUFFER_STATIC,
        BINDING_INDEX_BUFFER_DYNAMIC,
        BINDING_GEOMETRY_INSTANCES,
        BINDING_GEOMETRY_INSTANCES_MATCH_PREV,
        BINDING_PREV_POSITIONS_BUFFER_DYNAMIC,
        BINDING_PREV_INDEX_BUFFER_DYNAMIC,
    };

    constexpr uint32_t bindingCount = 8;
    static_assert(std::size(buffers) == bindingCount);
    static_assert(std::size(bindings) == bindingCount);

    std::array<VkDescriptorBufferInfo, bindingCount> bufferInfos{};
    std::array<VkWriteDescriptorSet, bindingCount> writes{};

    for (uint32_t i = 0; i < bindingCount; i++)
    {
        bufferInfos[i].buffer = buffers[i];
        bufferInfos[i].offset = 0;
        bufferInfos[i].range = VK_WHOLE_SIZE;

        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = buffersDescSets[frameIndex];
        writes[i].dstBinding = bindings[i];
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &bufferInfos[i];
    }

    vkUpdateDescriptorSets(device, bindingCount, writes.data(), 0, nullptr);
}

void ASManager::UpdateASDescriptors(uint32_t frameIndex)
{
    const VkAccelerationStructureKHR asHandle = tlas[frameIndex]->GetAS();
    assert(asHandle != VK_NULL_HANDLE);

    if (asDescHandles[frameIndex] == asHandle)
    {
        return;
    }

    asDescHandles[frameIndex] = asHandle;

    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &asHandle;

    VkWriteDescriptorSet wrt = {};
    wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wrt.pNext = &asInfo;
    wrt.dstSet = asDescSets[frameIndex];
    wrt.dstBinding = BINDING_ACCELERATION_STRUCTURE_MAIN;
    wrt.dstArrayElement = 0;
    wrt.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wrt.descriptorCount = 1;

    vkUpdateDescriptorSets(device, 1, &wrt, 0, nullptr);
}

ASManager::~ASManager()
{
    for (auto &as : allStaticBlas)
    {
        as->Destroy();
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        for (auto &as : allDynamicBlas[i])
        {
            as->Destroy();
        }

        tlas[i]->Destroy();
    }

    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, buffersDescSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, asDescSetLayout, nullptr);
    vkDestroyFence(device, staticCopyFence, nullptr);
}

bool ASManager::SetupBLAS(BLASComponent &blas, const std::shared_ptr<VertexCollector> &vertCollector)
{
    const VertexCollectorFilterTypeFlags filter = blas.GetFilter();
    const std::vector<VkAccelerationStructureGeometryKHR> &geoms = vertCollector->GetASGeometries(filter);

    blas.SetGeometryCount((uint32_t)geoms.size());

    if (blas.IsEmpty())
    {
        return false;
    }

    const std::vector<VkAccelerationStructureBuildRangeInfoKHR> &ranges = vertCollector->GetASBuildRangeInfos(filter);
    const std::vector<uint32_t> &primCounts = vertCollector->GetPrimitiveCounts(filter);

    const bool fastTrace = !IsFastBuild(filter);
    const bool update = false;

    const auto buildSizes = asBuilder->GetBottomBuildSizes(geoms.size(), geoms.data(), primCounts.data(), fastTrace);

    blas.RecreateIfNotValid(buildSizes, allocator);

    assert(blas.GetAS() != VK_NULL_HANDLE);

    asBuilder->AddBLAS(blas.GetAS(), (uint32_t)geoms.size(),
                       geoms.data(), ranges.data(),
                       buildSizes,
                       fastTrace, update, blas.GetFilter() & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE);

    return true;
}

uint32_t ASManager::AddStaticGeometry(uint32_t frameIndex, const QrGeometryUploadInfo &info)
{
    if (info.geomType == QR_GEOMETRY_TYPE_STATIC || info.geomType == QR_GEOMETRY_TYPE_STATIC_MOVABLE)
    {
        MaterialTextures materials[] =
        {
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[0]),
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[1]),
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[2]),
        };

        return collectorStatic->AddGeometry(frameIndex, info, materials);
    }

    assert(0);
    return UINT32_MAX;
}

uint32_t ASManager::AddDynamicGeometry(uint32_t frameIndex, const QrGeometryUploadInfo &info)
{
    if (info.geomType == QR_GEOMETRY_TYPE_DYNAMIC)
    {
        MaterialTextures materials[] =
        {
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[0]),
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[1]),
            textureMgr->GetMaterialTextures(info.geomMaterial.layerMaterials[2]),
        };

        return collectorDynamic[frameIndex]->AddGeometry(frameIndex, info, materials);
    }

    assert(0);
    return UINT32_MAX;
}

void ASManager::ResetStaticGeometry()
{
    collectorStatic->Reset();
    geomInfoMgr->ResetWithStatic();
}

void ASManager::BeginStaticGeometry()
{
    collectorStatic->Reset();
    geomInfoMgr->ResetWithStatic();

    collectorStatic->BeginCollecting(true);
}

void ASManager::SubmitStaticGeometry()
{
    collectorStatic->EndCollecting();

    vkDeviceWaitIdle(device);

    typedef VertexCollectorFilterTypeFlagBits FT;

    const auto staticFlags = FT::CF_STATIC_NON_MOVABLE | FT::CF_STATIC_MOVABLE;

    for (auto &staticBlas : allStaticBlas)
    {
        assert(!(staticBlas->GetFilter() & FT::CF_DYNAMIC));

        if (staticBlas->GetFilter() & staticFlags)
        {
            staticBlas->Destroy();
            staticBlas->SetGeometryCount(0);
        }
    }

    assert(asBuilder->IsEmpty());

    if (collectorStatic->AreGeometriesEmpty(staticFlags))
    {
        staticGeneration++;
        return;
    }

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    collectorStatic->CopyFromStaging(cmd);

    for (auto &staticBlas : allStaticBlas)
    {
        if (staticBlas->GetFilter() & staticFlags)
        {
            SetupBLAS(*staticBlas, collectorStatic);
        }
    }

    asBuilder->BuildBottomLevel(cmd);

    geomInfoMgr->CopyFromStaging(cmd, 0, false);

    cmdManager->Submit(cmd, staticCopyFence);
    Utils::WaitAndResetFence(device, staticCopyFence);

    staticGeneration++;
}

void ASManager::BeginDynamicGeometry(VkCommandBuffer cmd, uint32_t frameIndex)
{
    scratchBuffer->Reset();

    static_assert(MAX_FRAMES_IN_FLIGHT == 2, "");
    const uint32_t prevFrameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    CopyDynamicDataToPrevBuffers(cmd, prevFrameIndex);

    collectorDynamic[frameIndex]->Reset();
    collectorDynamic[frameIndex]->BeginCollecting(false);
}

void ASManager::UpdateStaticMovableTransform(uint32_t simpleIndex, const QrUpdateTransformInfo &updateInfo)
{
    collectorStatic->UpdateTransform(simpleIndex, updateInfo);
}

void ASManager::UpdateStaticTexCoords(uint32_t simpleIndex, const QrUpdateTexCoordsInfo &texCoordsInfo)
{
    collectorStatic->UpdateTexCoords(simpleIndex, texCoordsInfo, true);
}

bool ASManager::GetTLASInstanceForFilter(VertexCollectorFilterTypeFlags filter, uint32_t rayCullMaskWorld, bool allowGeometryWithSkyFlag, VkAccelerationStructureInstanceKHR &instance)
{
    typedef VertexCollectorFilterTypeFlagBits FT;

    instance.transform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    instance.instanceCustomIndex = 0;

    if (filter & FT::CF_DYNAMIC)
    {
        instance.instanceCustomIndex = INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC;
    }

    if (filter & FT::PV_FIRST_PERSON)
    {
        instance.mask = INSTANCE_MASK_FIRST_PERSON;
        instance.instanceCustomIndex |= INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON;
    }
    else if (filter & FT::PV_FIRST_PERSON_VIEWER)
    {
        instance.mask = INSTANCE_MASK_FIRST_PERSON_VIEWER;
        instance.instanceCustomIndex |= INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER;
    }
    else
    {
        if (filter & FT::PV_WORLD_0)
        {
            instance.mask = INSTANCE_MASK_WORLD_0;

            if (!(rayCullMaskWorld & INSTANCE_MASK_WORLD_0))
            {
                instance = {};
                return false;
            }
        }
        else if (filter & FT::PV_WORLD_1)
        {
            instance.mask = INSTANCE_MASK_WORLD_1;

            if (!(rayCullMaskWorld & INSTANCE_MASK_WORLD_1))
            {
                instance = {};
                return false;
            }
        }
        else if (filter & FT::PV_WORLD_2)
        {
            instance.mask = INSTANCE_MASK_WORLD_2;

            if (!(rayCullMaskWorld & INSTANCE_MASK_WORLD_2))
            {
                instance = {};
                return false;
            }

        #if RAYCULLMASK_SKY_IS_WORLD2
            if (allowGeometryWithSkyFlag)
            {
                instance.instanceCustomIndex |= INSTANCE_CUSTOM_INDEX_FLAG_SKY;
            }
        #else
            #error Handle sky, if there is no WORLD_2
        #endif
        }
        else
        {
            assert(0);
        }
    }

    if (filter & FT::PT_REFRACT)
    {
        const bool isWorld = !(filter & FT::PV_FIRST_PERSON) && !(filter & FT::PV_FIRST_PERSON_VIEWER);

        if (isWorld)
        {
            instance.mask = INSTANCE_MASK_REFRACT;
        }
    }

    if (filter & FT::PT_ALPHA_TESTED)
    {
        instance.instanceShaderBindingTableRecordOffset = SBT_INDEX_HITGROUP_ALPHA_TESTED;
        instance.flags =
            VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR |
            VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
    }
    else
    {
        assert((filter & FT::PT_OPAQUE) || (filter & FT::PT_REFRACT));

        instance.instanceShaderBindingTableRecordOffset = SBT_INDEX_HITGROUP_FULLY_OPAQUE;
        instance.flags =
            VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR |
            VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
    }

    return true;
}

bool ASManager::SetupTLASInstanceFromBLAS(const BLASComponent &blas, uint32_t rayCullMaskWorld, bool allowGeometryWithSkyFlag, VkAccelerationStructureInstanceKHR &instance)
{
    if (blas.GetAS() == VK_NULL_HANDLE || blas.IsEmpty())
    {
        return false;
    }

    if (!GetTLASInstanceForFilter(blas.GetFilter(), rayCullMaskWorld, allowGeometryWithSkyFlag, instance))
    {
        return false;
    }

    instance.accelerationStructureReference = blas.GetASAddress();

    return true;
}

namespace
{
    void WriteInstanceGeomInfo(int32_t *instanceGeomInfoOffset, int32_t *instanceGeomCount, uint32_t index, const BLASComponent &blas)
    {
        assert(index < MAX_TOP_LEVEL_INSTANCE_COUNT);

        const int32_t arrayOffset = VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(blas.GetFilter());
        const int32_t geomCount = blas.GetGeomCount();

        assert(geomCount > 0 && geomCount < MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT);

        instanceGeomInfoOffset[index] = arrayOffset;
        instanceGeomCount[index] = geomCount;
    }
}

std::pair<ASManager::TLASPrepareResult, ShVertPreprocessing> ASManager::PrepareForBuildingTLAS(
    uint32_t frameIndex,
    ShGlobalUniform &uniformData,
    uint32_t uniformData_rayCullMaskWorld,
    bool allowGeometryWithSkyFlag,
    bool disableRTGeometry) const
{
    typedef VertexCollectorFilterTypeFlagBits FT;

    static_assert(std::size(TLASPrepareResult{}.instances) == MAX_TOP_LEVEL_INSTANCE_COUNT, "Change TLASPrepareResult sizes");

    TLASPrepareResult result = {};
    ShVertPreprocessing push = {};

    if (disableRTGeometry)
    {
        return std::make_pair(result, push);
    }

    int32_t *instanceGeomInfoOffset = uniformData.instanceGeomInfoOffset;
    int32_t *instanceGeomCount = uniformData.instanceGeomCount;

    const std::vector<std::unique_ptr<BLASComponent>> *blasArrays[] =
    {
        &allStaticBlas,
        &allDynamicBlas[frameIndex],
    };

    for (const auto *blasArray : blasArrays)
    {
        for (const auto &blas : *blasArray)
        {
            const bool isDynamic = blas->GetFilter() & FT::CF_DYNAMIC;

            const bool isAdded = SetupTLASInstanceFromBLAS(
                *blas, uniformData_rayCullMaskWorld, allowGeometryWithSkyFlag, result.instances[result.instanceCount]);

            if (!isAdded)
            {
                continue;
            }

            if (isDynamic)
            {
                push.tlasInstanceIsDynamicBits[result.instanceCount / MAX_TOP_LEVEL_INSTANCE_COUNT] |= 1 << (result.instanceCount % MAX_TOP_LEVEL_INSTANCE_COUNT);
            }

            WriteInstanceGeomInfo(instanceGeomInfoOffset, instanceGeomCount, result.instanceCount, *blas);
            result.instanceCount++;
        }
    }

    push.tlasInstanceCount = result.instanceCount;

    return std::make_pair(result, push);
}

void ASManager::BuildTLAS(VkCommandBuffer cmd, uint32_t frameIndex, const TLASPrepareResult &r)
{
    CmdLabel label(cmd, "Building TLAS");

    if (r.instanceCount > 0)
    {
        auto *mapped = (VkAccelerationStructureInstanceKHR *)instanceBuffer->GetMapped(frameIndex);

        memcpy(mapped, r.instances, r.instanceCount * sizeof(VkAccelerationStructureInstanceKHR));

        instanceBuffer->CopyFromStaging(cmd, frameIndex);
    }

    TLASComponent *pCurrentTLAS = tlas[frameIndex].get();

    VkAccelerationStructureGeometryKHR instGeom = {};
    instGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    instGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    instGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;

    auto &instData = instGeom.geometry.instances;
    instData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instData.arrayOfPointers = VK_FALSE;
    instData.data.deviceAddress = r.instanceCount > 0 ? instanceBuffer->GetDeviceAddress() : 0;

    if (!tlasBuildSizesValid[frameIndex] || tlasBuildSizesInstanceCount[frameIndex] != r.instanceCount)
    {
        tlasBuildSizes[frameIndex] = asBuilder->GetTopBuildSizes(&instGeom, r.instanceCount, false);
        tlasBuildSizesInstanceCount[frameIndex] = r.instanceCount;
        tlasBuildSizesValid[frameIndex] = true;
    }

    const VkAccelerationStructureBuildSizesInfoKHR &buildSizes = tlasBuildSizes[frameIndex];

    pCurrentTLAS->RecreateIfNotValid(buildSizes, allocator);

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = r.instanceCount;

    assert(asBuilder->IsEmpty());
    assert(pCurrentTLAS->GetAS() != VK_NULL_HANDLE);

    asBuilder->AddTLAS(pCurrentTLAS->GetAS(), &instGeom, &range, buildSizes, true, false);

    asBuilder->BuildTopLevel(cmd);

    Utils::ASBuildMemoryBarrier(cmd);

    UpdateASDescriptors(frameIndex);
}

void ASManager::CopyDynamicDataToPrevBuffers(VkCommandBuffer cmd, uint32_t frameIndex)
{
    const uint32_t vertCount = collectorDynamic[frameIndex]->GetCurrentVertexCount();
    const uint32_t indexCount = collectorDynamic[frameIndex]->GetCurrentIndexCount();

    if (vertCount > 0)
    {
        VkBufferCopy vertRegion = {};
        vertRegion.srcOffset = 0;
        vertRegion.dstOffset = 0;
        vertRegion.size = vertCount * sizeof(ShVertex);

        vkCmdCopyBuffer(
            cmd,
            collectorDynamic[frameIndex]->GetVertexBuffer(),
            previousDynamicPositions.GetBuffer(),
            1, &vertRegion);
    }

    if (indexCount > 0)
    {
        VkBufferCopy indexRegion = {};
        indexRegion.srcOffset = 0;
        indexRegion.dstOffset = 0;
        indexRegion.size = indexCount * sizeof(uint32_t);

        vkCmdCopyBuffer(
            cmd,
            collectorDynamic[frameIndex]->GetIndexBuffer(),
            previousDynamicIndices.GetBuffer(),
            1, &indexRegion);
    }
}

void ASManager::OnVertexPreprocessingBegin(VkCommandBuffer cmd, uint32_t frameIndex, bool onlyDynamic)
{
    if (!onlyDynamic)
    {
        collectorStatic->InsertVertexPreprocessBeginBarrier(cmd);
    }

    collectorDynamic[frameIndex]->InsertVertexPreprocessBeginBarrier(cmd);
}

void ASManager::OnVertexPreprocessingFinish(VkCommandBuffer cmd, uint32_t frameIndex, bool onlyDynamic)
{
    if (!onlyDynamic)
    {
        collectorStatic->InsertVertexPreprocessFinishBarrier(cmd);
    }

    collectorDynamic[frameIndex]->InsertVertexPreprocessFinishBarrier(cmd);
}

bool ASManager::IsFastBuild(VertexCollectorFilterTypeFlags filter)
{
    typedef VertexCollectorFilterTypeFlagBits FT;

    return (filter & FT::CF_DYNAMIC) != 0;
}

VkDescriptorSet ASManager::GetBuffersDescSet(uint32_t frameIndex) const
{
    return buffersDescSets[frameIndex];
}

VkDescriptorSet ASManager::GetTLASDescSet(uint32_t frameIndex) const
{
    if (tlas[frameIndex]->GetAS() == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    return asDescSets[frameIndex];
}

VkDescriptorSetLayout ASManager::GetBuffersDescSetLayout() const
{
    return buffersDescSetLayout;
}

VkDescriptorSetLayout ASManager::GetTLASDescSetLayout() const
{
    return asDescSetLayout;
}

const std::shared_ptr<VertexCollector> &ASManager::GetStaticCollector() const
{
    return collectorStatic;
}

const std::shared_ptr<VertexCollector> &ASManager::GetDynamicCollector(uint32_t frameIndex) const
{
    return collectorDynamic[frameIndex];
}

const std::shared_ptr<GeomInfoManager> &ASManager::GetGeomInfoManager() const
{
    return geomInfoMgr;
}

const std::vector<std::unique_ptr<BLASComponent>> &ASManager::GetStaticBlasComponents() const
{
    return allStaticBlas;
}

VkBuffer ASManager::GetInstanceBuffer() const
{
    return instanceBuffer->GetDeviceLocal();
}

VkDeviceSize ASManager::GetInstanceBufferSize() const
{
    return instanceBuffer->GetSize();
}

uint32_t ASManager::GetStaticGeneration() const
{
    return staticGeneration;
}
