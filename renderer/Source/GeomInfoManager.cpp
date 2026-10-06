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

#include "GeomInfoManager.h"

#include <algorithm>

#include "Matrix.h"
#include "VertexCollectorFilterType.h"
#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"

static_assert(sizeof(qray::ShGeometryInstance) % 16 == 0, "Std430 structs must be aligned by 16 bytes");

qray::GeomInfoManager::GeomInfoManager(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator) :
    device(_device),
    staticGeomCount(0),
    dynamicGeomCount(0)
{
    buffer = std::make_shared<AutoBuffer>(device, _allocator);
    matchPrev = std::make_shared<AutoBuffer>(device, _allocator);

    const uint32_t allBottomLevelGeomsCount = VertexCollectorFilterTypeFlags_GetAllBottomLevelGeomsCount();

    buffer->Create(
        allBottomLevelGeomsCount * sizeof(qray::ShGeometryInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "Geometry info buffer");

    matchPrev->Create(
        allBottomLevelGeomsCount * sizeof(int32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        "Match previous Geometry infos buffer");

    matchPrevShadow = std::make_unique<int32_t[]>(allBottomLevelGeomsCount);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        copyRegionLowerBounds[i].resize(MAX_TOP_LEVEL_INSTANCE_COUNT, UINT32_MAX);
        copyRegionUpperBounds[i].resize(MAX_TOP_LEVEL_INSTANCE_COUNT, 0);
    }
}

qray::GeomInfoManager::~GeomInfoManager()
{
}

bool qray::GeomInfoManager::CopyFromStaging(VkCommandBuffer cmd, uint32_t frameIndex, bool insertBarrier)
{
    CmdLabel label(cmd, "Copying geom infos");

    const auto appendCopyRegion =
        [](VkBufferCopy *pCopyInfos, VkBufferMemoryBarrier *pBarriers, uint32_t &infoCount,
           VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
    {
        VkBufferCopy &copy = pCopyInfos[infoCount];
        copy = {};
        copy.srcOffset = offset;
        copy.dstOffset = offset;
        copy.size = size;

        VkBufferMemoryBarrier &barrier = pBarriers[infoCount];
        barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = buffer;
        barrier.offset = offset;
        barrier.size = size;

        infoCount++;
    };

    {
        VkBufferCopy copyInfos[MAX_TOP_LEVEL_INSTANCE_COUNT];
        VkBufferMemoryBarrier barriers[MAX_TOP_LEVEL_INSTANCE_COUNT];

        uint32_t infoCount = 0;

        for (auto cf : VertexCollectorFilterGroup_ChangeFrequency)
        {
            const uint64_t upperBoundSize = cf & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC ?
                matchPrevCopyInfo.maxDynamicGeomCount * sizeof(int32_t) :
                matchPrevCopyInfo.maxStaticGeomCount * sizeof(int32_t);

            if (upperBoundSize == 0)
            {
                continue;
            }

            for (auto pt : VertexCollectorFilterGroup_PassThrough)
            {
                for (auto pm : VertexCollectorFilterGroup_PrimaryVisibility)
                {
                    const uint64_t offset =
                        VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(cf | pt | pm) * sizeof(int32_t);

                    const uint64_t size = std::min(
                        upperBoundSize,
                        VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(cf | pt | pm) * sizeof(int32_t));

                    uint8_t *pDst = (uint8_t *)matchPrev->GetMapped(frameIndex);
                    const uint8_t *pSrc = (const uint8_t *)matchPrevShadow.get();

                    memcpy(pDst + offset, pSrc + offset, size);

                    appendCopyRegion(copyInfos, barriers, infoCount, matchPrev->GetDeviceLocal(), offset, size);
                }
            }
        }

        if (infoCount > 0)
        {
            matchPrev->CopyFromStaging(cmd, frameIndex, copyInfos, infoCount);

            if (insertBarrier)
            {
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0,
                    0, nullptr,
                    infoCount, barriers,
                    0, nullptr);
            }
        }
    }

    {
        VkBufferCopy copyInfos[MAX_TOP_LEVEL_INSTANCE_COUNT];
        VkBufferMemoryBarrier barriers[MAX_TOP_LEVEL_INSTANCE_COUNT];

        uint32_t infoCount = 0;

        for (auto cf : VertexCollectorFilterGroup_ChangeFrequency)
        {
            for (auto pt : VertexCollectorFilterGroup_PassThrough)
            {
                for (auto pm : VertexCollectorFilterGroup_PrimaryVisibility)
                {
                    const uint32_t flagsId = VertexCollectorFilterTypeFlags_GetID(cf | pt | pm);

                    const uint32_t lower = copyRegionLowerBounds[frameIndex][flagsId];
                    const uint32_t upper = copyRegionUpperBounds[frameIndex][flagsId];

                    if (lower >= upper)
                    {
                        continue;
                    }

                    const uint64_t offset = sizeof(ShGeometryInstance) *
                        (uint64_t)(VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(cf | pt | pm) + lower);

                    const uint64_t size = sizeof(ShGeometryInstance) * (upper - lower);

                    appendCopyRegion(copyInfos, barriers, infoCount, buffer->GetDeviceLocal(), offset, size);
                }
            }
        }

        if (infoCount == 0)
        {
            return false;
        }

        buffer->CopyFromStaging(cmd, frameIndex, copyInfos, infoCount);

        if (insertBarrier)
        {
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0,
                0, nullptr,
                infoCount, barriers,
                0, nullptr);
        }
    }

    return true;
}

void qray::GeomInfoManager::ResetMatchPrevForGroup(uint32_t frameIndex, VertexCollectorFilterTypeFlags groupFlags)
{
    int32_t *prevIndexToCurIndex = matchPrevShadow.get();

    const uint32_t offsetInArray = VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(groupFlags);
    int32_t *toReset = prevIndexToCurIndex + offsetInArray;

    const uint32_t maxGeomCount = groupFlags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC ?
        dynamicGeomCount : staticGeomCount;

    const uint32_t resetCount = std::min(maxGeomCount, VertexCollectorFilterTypeFlags_GetAmountInGlobalArray(groupFlags));

    memset(toReset, 0xFF, resetCount * sizeof(int32_t));
}

void qray::GeomInfoManager::ResetOnlyDynamic(uint32_t frameIndex)
{
    typedef VertexCollectorFilterTypeFlagBits FT;

    if (dynamicGeomCount > 0)
    {
        geomType.resize(staticGeomCount);
        simpleToLocalIndex.resize(staticGeomCount);

        for (auto pt : VertexCollectorFilterGroup_PassThrough)
        {
            for (auto pm : VertexCollectorFilterGroup_PrimaryVisibility)
            {
                ResetMatchPrevForGroup(frameIndex, FT::CF_DYNAMIC | pt | pm);
            }
        }

        dynamicGeomCount = 0;
    }

    std::fill(copyRegionLowerBounds[frameIndex].begin(), copyRegionLowerBounds[frameIndex].end(), UINT32_MAX);
    std::fill(copyRegionUpperBounds[frameIndex].begin(), copyRegionUpperBounds[frameIndex].end(), 0);
}

void qray::GeomInfoManager::ResetWithStatic()
{
    movableIDToGeomFrameInfo.clear();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        for (auto cf : VertexCollectorFilterGroup_ChangeFrequency)
        {
            for (auto pt : VertexCollectorFilterGroup_PassThrough)
            {
                for (auto pm : VertexCollectorFilterGroup_PrimaryVisibility)
                {
                    ResetMatchPrevForGroup(i, cf | pt | pm);
                }
            }
        }
    }

    staticGeomCount = 0;
    dynamicGeomCount = 0;

    geomType.clear();
    simpleToLocalIndex.clear();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        ResetOnlyDynamic(i);
    }
}

uint32_t qray::GeomInfoManager::GetGlobalGeomIndex(uint32_t localGeomIndex, VertexCollectorFilterTypeFlags flags)
{
    return VertexCollectorFilterTypeFlags_GetOffsetInGlobalArray(flags) + localGeomIndex;
}

qray::ShGeometryInstance *qray::GeomInfoManager::GetGeomInfoAddressByGlobalIndex(
    uint32_t frameIndex, uint32_t globalGeomIndex)
{
    auto *mapped = (ShGeometryInstance *)buffer->GetMapped(frameIndex);

    return &mapped[globalGeomIndex];
}

uint32_t qray::GeomInfoManager::ConvertSimpleIndexToGlobal(uint32_t simpleIndex) const
{
    assert(simpleIndex < geomType.size());
    assert(geomType.size() == simpleToLocalIndex.size());

    const VertexCollectorFilterTypeFlags flags = geomType[simpleIndex];
    const uint32_t localGeomIndex = simpleToLocalIndex[simpleIndex];

    return GetGlobalGeomIndex(localGeomIndex, flags);
}

void qray::GeomInfoManager::PrepareForFrame(uint32_t frameIndex)
{
    matchPrevCopyInfo.maxDynamicGeomCount = dynamicGeomCount;
    matchPrevCopyInfo.maxStaticGeomCount = staticGeomCount;

    dynamicIDToGeomFrameInfo[frameIndex].clear();
    ResetOnlyDynamic(frameIndex);
}

uint32_t qray::GeomInfoManager::WriteGeomInfo(
    uint32_t frameIndex,
    uint64_t geomUniqueID,
    uint32_t localGeomIndex,
    VertexCollectorFilterTypeFlags flags,
    ShGeometryInstance &src)
{
    assert(src.baseVertexIndex % 3 == 0);
    assert(src.baseIndexIndex % 3 == 0);

    const uint32_t simpleIndex = GetCount();

    assert(simpleIndex == geomType.size());
    assert(geomType.size() == simpleToLocalIndex.size());

    geomType.push_back(flags);
    simpleToLocalIndex.push_back(localGeomIndex);

    const bool isStatic = !(flags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC);

    uint32_t frameBegin = frameIndex;
    uint32_t frameEnd = frameIndex + 1;

    if (isStatic)
    {
        assert(dynamicGeomCount == 0);
        assert(staticGeomCount == simpleIndex);
        staticGeomCount++;

        frameBegin = 0;
        frameEnd = MAX_FRAMES_IN_FLIGHT;
    }
    else
    {
        assert(staticGeomCount <= simpleIndex);
        assert(dynamicGeomCount == simpleIndex - staticGeomCount);
        dynamicGeomCount++;
    }

    const uint32_t globalGeomIndex = GetGlobalGeomIndex(localGeomIndex, flags);
    const uint32_t flagsId = VertexCollectorFilterTypeFlags_GetID(flags);

    for (uint32_t i = frameBegin; i < frameEnd; i++)
    {
        FillWithPrevFrameData(flags, geomUniqueID, globalGeomIndex, src, i);

        ShGeometryInstance *dst = GetGeomInfoAddressByGlobalIndex(i, globalGeomIndex);
        memcpy(dst, &src, sizeof(ShGeometryInstance));

        MarkGeomInfoIndexToCopy(i, localGeomIndex, flagsId);
    }

    WriteInfoForNextUsage(flags, geomUniqueID, globalGeomIndex, src, frameIndex);

    return simpleIndex;
}

void qray::GeomInfoManager::MarkGeomInfoIndexToCopy(uint32_t frameIndex, uint32_t localGeomIndex, uint32_t flagsId)
{
    assert(flagsId < MAX_TOP_LEVEL_INSTANCE_COUNT);

    copyRegionLowerBounds[frameIndex][flagsId] = std::min(localGeomIndex, copyRegionLowerBounds[frameIndex][flagsId]);
    copyRegionUpperBounds[frameIndex][flagsId] = std::max(localGeomIndex + 1, copyRegionUpperBounds[frameIndex][flagsId]);
}

void qray::GeomInfoManager::FillWithPrevFrameData(
    VertexCollectorFilterTypeFlags flags, uint64_t geomUniqueID,
    uint32_t currentGlobalGeomIndex, ShGeometryInstance &dst, int32_t frameIndex)
{
    int32_t *prevIndexToCurIndex = matchPrevShadow.get();

    const rgl::unordered_map<uint64_t, GeomFrameInfo> *prevIdToInfo = nullptr;

    const bool isMovable = flags & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE;
    const bool isDynamic = flags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC;

    if (isDynamic)
    {
        static_assert(MAX_FRAMES_IN_FLIGHT == 2, "Assuming MAX_FRAMES_IN_FLIGHT==2");
        const uint32_t prevFrame = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

        prevIdToInfo = &dynamicIDToGeomFrameInfo[prevFrame];
    }
    else
    {
        prevIndexToCurIndex[currentGlobalGeomIndex] = (int32_t)currentGlobalGeomIndex;

        if (isMovable)
        {
            prevIdToInfo = &movableIDToGeomFrameInfo;
        }
        else
        {
            MarkNoPrevInfo(dst);
            return;
        }
    }

    const auto prev = prevIdToInfo->find(geomUniqueID);

    if (prev == prevIdToInfo->end())
    {
        MarkNoPrevInfo(dst);
        return;
    }

    if (prev->second.vertexCount != dst.vertexCount ||
        prev->second.indexCount != dst.indexCount)
    {
        MarkNoPrevInfo(dst);
        return;
    }

    dst.prevBaseVertexIndex = prev->second.baseVertexIndex;
    dst.prevBaseIndexIndex = prev->second.baseIndexIndex;
    memcpy(dst.prevModel, prev->second.model, sizeof(float) * 16);

    if (isDynamic)
    {
        prevIndexToCurIndex[prev->second.prevGlobalGeomIndex] = currentGlobalGeomIndex;
    }
}

void qray::GeomInfoManager::MarkNoPrevInfo(ShGeometryInstance &dst)
{
    dst.prevBaseVertexIndex = UINT32_MAX;
}

void qray::GeomInfoManager::MarkMovableHasPrevInfo(ShGeometryInstance &dst)
{
    dst.prevBaseVertexIndex = dst.baseVertexIndex;
}

void qray::GeomInfoManager::WriteInfoForNextUsage(
    VertexCollectorFilterTypeFlags flags, uint64_t geomUniqueID,
    uint32_t currentGlobalGeomIndex, const ShGeometryInstance &src, int32_t frameIndex)
{
    const bool isMovable = flags & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE;
    const bool isDynamic = flags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC;

    rgl::unordered_map<uint64_t, GeomFrameInfo> *idToInfo = nullptr;

    if (isDynamic)
    {
        idToInfo = &dynamicIDToGeomFrameInfo[frameIndex];
    }
    else if (isMovable)
    {
        idToInfo = &movableIDToGeomFrameInfo;
    }
    else
    {
        return;
    }

    assert(idToInfo->find(geomUniqueID) == idToInfo->end());

    GeomFrameInfo frameInfo = {};
    memcpy(frameInfo.model, src.model, sizeof(float) * 16);
    frameInfo.baseVertexIndex = src.baseVertexIndex;
    frameInfo.baseIndexIndex = src.baseIndexIndex;
    frameInfo.vertexCount = src.vertexCount;
    frameInfo.indexCount = src.indexCount;
    frameInfo.prevGlobalGeomIndex = currentGlobalGeomIndex;

    (*idToInfo)[geomUniqueID] = frameInfo;
}

void qray::GeomInfoManager::WriteStaticGeomInfoMaterials(
    uint32_t simpleIndex, uint32_t layer, const MaterialTextures &src)
{
    assert(simpleIndex < geomType.size());
    assert(!(geomType[simpleIndex] & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC));
    assert(geomType.size() == simpleToLocalIndex.size());

    const uint32_t flagsId = VertexCollectorFilterTypeFlags_GetID(geomType[simpleIndex]);
    const uint32_t globalIndex = ConvertSimpleIndexToGlobal(simpleIndex);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        ShGeometryInstance *dst = GetGeomInfoAddressByGlobalIndex(i, globalIndex);

        uint32_t *pMatArr = &dst->materials0A;

        memcpy(&pMatArr[layer * TEXTURES_PER_MATERIAL_COUNT], src.indices, TEXTURES_PER_MATERIAL_COUNT * sizeof(uint32_t));

        MarkGeomInfoIndexToCopy(i, simpleToLocalIndex[simpleIndex], flagsId);
    }
}

void qray::GeomInfoManager::WriteStaticGeomInfoTransform(
    uint32_t simpleIndex, uint64_t geomUniqueID, const QrTransform &src)
{
    if (simpleIndex >= geomType.size())
    {
        assert(0);
        return;
    }

    assert(geomType.size() == simpleToLocalIndex.size());

    const auto flags = geomType[simpleIndex];
    const uint32_t flagsId = VertexCollectorFilterTypeFlags_GetID(flags);

    if (!(flags & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE))
    {
        assert(0);
        return;
    }

    float modelMatrix[16];
    Matrix::ToMat4Transposed(modelMatrix, src);

    auto prev = movableIDToGeomFrameInfo.find(geomUniqueID);

    if (prev == movableIDToGeomFrameInfo.end())
    {
        assert(0);
        return;
    }

    float *prevModelMatrix = prev->second.model;

    const uint32_t localGeomIndex = simpleToLocalIndex[simpleIndex];
    const uint32_t globalIndex = GetGlobalGeomIndex(localGeomIndex, flags);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        ShGeometryInstance *dst = GetGeomInfoAddressByGlobalIndex(i, globalIndex);

        memcpy(dst->model, modelMatrix, 16 * sizeof(float));
        memcpy(dst->prevModel, prevModelMatrix, 16 * sizeof(float));

        MarkMovableHasPrevInfo(*dst);

        MarkGeomInfoIndexToCopy(i, localGeomIndex, flagsId);
    }

    memcpy(prevModelMatrix, modelMatrix, 16 * sizeof(float));
}

uint32_t qray::GeomInfoManager::GetCount() const
{
    return staticGeomCount + dynamicGeomCount;
}

uint32_t qray::GeomInfoManager::GetStaticCount() const
{
    return staticGeomCount;
}

uint32_t qray::GeomInfoManager::GetDynamicCount() const
{
    return dynamicGeomCount;
}

VkBuffer qray::GeomInfoManager::GetBuffer() const
{
    return buffer->GetDeviceLocal();
}

VkBuffer qray::GeomInfoManager::GetMatchPrevBuffer() const
{
    return matchPrev->GetDeviceLocal();
}

VkBuffer qray::GeomInfoManager::GetStagingBuffer(uint32_t frameIndex)
{
    return buffer->GetStaging(frameIndex);
}

VkDeviceSize qray::GeomInfoManager::GetBufferSize() const
{
    return buffer->GetSize();
}

VkDeviceSize qray::GeomInfoManager::GetMatchPrevSize() const
{
    return matchPrev->GetSize();
}

const int32_t *qray::GeomInfoManager::GetMatchPrevData() const
{
    return matchPrevShadow.get();
}

uint32_t qray::GeomInfoManager::GetStaticGeomBaseVertexIndex(uint32_t simpleIndex)
{
    return GetGeomInfoAddressByGlobalIndex(0, ConvertSimpleIndexToGlobal(simpleIndex))->baseVertexIndex;
}
