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

#include "LightManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Generated/ShaderCommonC.h"
#include "QrException.h"
#include "Utils.h"

namespace qray
{

constexpr double kPi = 3.1415926535897932384626433;

constexpr float kMinColorSum = 0.0001f;
constexpr float kMinSphereRadius = 0.005f;

constexpr uint32_t kLightArrayMaxSize = LightManager::LIGHT_ARRAY_ENTRY_COUNT;

static_assert(sizeof(QrDtalMemberUpload) == sizeof(ShDtalMember), "the DTAL member upload record has to match the shader layout");
static_assert(sizeof(ShQ2LightTail) == 16, "the overflow tail entry is one 16-byte record");
static_assert(LightManager::DTAL_MEMBER_CAPACITY <= QR_DTAL_MAX_UPLOAD_MEMBERS, "renderer member capacity is part of the public budget");

static_assert(LightManager::LIGHT_STATS_CLUSTER_COUNT == Q2_MAX_CLUSTERS, "cluster count of the light statistics buffer");
static_assert(LightManager::LIGHT_STATS_SLOT_COUNT == Q2_LIGHT_LIST_STATS_BUFFERS, "slots of the light statistics buffer");

namespace
{
    VkDeviceSize GetLightStatsSlotSize()
    {
        return sizeof(uint32_t) * Q2_MAX_CLUSTERS * Q2_LIGHT_LIST_MAX_PER_CELL *
               Q2_LIGHT_LIST_STATS_SIDES * 2;
    }

    uint32_t GetLightArrayEnd(uint32_t regCount, uint32_t dirCount)
    {
        return LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET + regCount;
    }

    float GetAngularRadius(float angularDiameterDegrees)
    {
        return static_cast<float>(0.5 * static_cast<double>(angularDiameterDegrees) * kPi / 180.0);
    }

    bool IsColorTooDim(const float color[3])
    {
        float sum = 0.0f;

        for (int i = 0; i < 3; i++)
        {
            sum += std::max(color[i], 0.0f);
        }

        return sum < kMinColorSum;
    }

    ShLightEncoded EncodeAsDirectionalLight(const QrDirectionalLightUploadInfo &info)
    {
        float direction[3] = { info.direction.data[0], info.direction.data[1], info.direction.data[2] };
        Utils::Normalize(direction);

        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_DIRECTIONAL;

        std::copy(info.color.data, info.color.data + 3, light.color);
        std::copy(direction, direction + 3, light.data_0);
        light.data_0[3] = GetAngularRadius(info.angularDiameterDegrees);

        return light;
    }

    ShLightEncoded EncodeAsSphereLight(const QrSphericalLightUploadInfo &info)
    {
        const float radius = std::max(kMinSphereRadius, info.radius);
        const float area = static_cast<float>(kPi) * radius * radius;

        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_SPHERE;

        for (int i = 0; i < 3; i++)
        {
            light.color[i] = info.color.data[i] / area;
            light.data_0[i] = info.position.data[i];
            light.data_1[i] = info.normal.data[i];
        }

        light.data_0[3] = radius;

        return light;
    }

    ShLightEncoded EncodeAsTriangleLight(const QrPolygonalLightUploadInfo &info, const QrFloat3D &unnormalizedNormal)
    {
        const float area = Utils::Length(unnormalizedNormal.data) * 0.5f;
        assert(area > 0.0f);

        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_TRIANGLE;

        for (int i = 0; i < 3; i++)
        {
            light.color[i] = info.color.data[i] / area;
            light.data_0[i] = info.positions[0].data[i];
            light.data_1[i] = info.positions[1].data[i];
            light.data_2[i] = info.positions[2].data[i];
        }

        light.data_0[3] = unnormalizedNormal.data[0];
        light.data_1[3] = unnormalizedNormal.data[1];
        light.data_2[3] = unnormalizedNormal.data[2];

        return light;
    }

    ShLightEncoded EncodeAsTexturedAreaLight(const QrTexturedAreaLightUploadInfo &info, uint32_t textureIndex)
    {
        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_TEXTURED_AREA;

        for (int i = 0; i < 3; i++)
        {
            light.color[i] = info.color.data[i];
            light.data_0[i] = info.A.data[i];
            light.data_1[i] = info.B.data[i];
            light.data_2[i] = info.C.data[i];
            light.data_7[i] = info.normal.data[i];
        }

        memcpy(&light.data_0[3], &textureIndex, sizeof(uint32_t));
        light.data_1[3] = info.meanEmiss;
        light.data_2[3] = static_cast<float>(info.numVerts);

        const int numVerts = std::max(0, std::min(info.numVerts, MAX_TEXTURED_AREA_LIGHT_VERTS));
        float *const uvSlots[4] = { light.data_3, light.data_4, light.data_5, light.data_6 };

        for (int i = 0; i < numVerts; i++)
        {
            float *slot = uvSlots[i >> 1] + (i & 1) * 2;
            slot[0] = info.uvVerts[i].data[0];
            slot[1] = info.uvVerts[i].data[1];
        }

        light.data_7[3] = info.area;

        const bool projector = std::isfinite(info.projector) && info.projector > 0.5f;
        const bool angleValid = std::isfinite(info.angleOuter) && info.angleOuter > 0.0f &&
                                info.angleOuter <= static_cast<float>(kPi / 2.0);
        const float angleOuter = angleValid ? info.angleOuter
                                            : (projector ? static_cast<float>(kPi / 3.0) : 0.0f);
        const float angleInner = (std::isfinite(info.angleInner) && info.angleInner >= 0.0f) ? info.angleInner : 0.0f;

        if (angleOuter > 0.0f)
        {
            light.coneCosInner = std::cos(std::min(angleInner, angleOuter * 0.999f));
            light.coneCosOuter = std::cos(angleOuter);
        }
        else
        {
            light.coneCosInner = 0.0f;
            light.coneCosOuter = 0.0f;
        }

        light.projector = projector ? 1.0f : 0.0f;

        return light;
    }

    void EncodeCone(ShLightEncoded &light, float angleInner, float angleOuter, bool projector)
    {
        const bool angleValid = std::isfinite(angleOuter) && angleOuter > 0.0f &&
                                angleOuter <= static_cast<float>(kPi / 2.0);
        const float outer = angleValid ? angleOuter : (projector ? static_cast<float>(kPi / 3.0) : 0.0f);
        const float inner = (std::isfinite(angleInner) && angleInner >= 0.0f) ? angleInner : 0.0f;

        if (outer > 0.0f)
        {
            light.coneCosInner = std::cos(std::min(inner, outer * 0.999f));
            light.coneCosOuter = std::cos(outer);
        }
        else
        {
            light.coneCosInner = 0.0f;
            light.coneCosOuter = 0.0f;
        }
    }

    ShLightEncoded EncodeAsDtalGroup(const QrDtalGroupUploadInfo &info, uint32_t textureIndex)
    {
        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_DTAL_GROUP;

        for (int i = 0; i < 3; i++)
        {
            light.color[i] = info.color.data[i];
            light.data_2[i] = info.center.data[i];
            light.data_7[i] = info.normal.data[i];
        }

        const uint32_t memberBase = info.memberBase;
        const uint32_t memberCount = info.memberCount;

        memcpy(&light.data_0[0], &memberBase, sizeof(uint32_t));
        memcpy(&light.data_0[1], &memberCount, sizeof(uint32_t));
        memcpy(&light.data_0[3], &textureIndex, sizeof(uint32_t));

        light.data_0[2] = (std::isfinite(info.reach) && info.reach > 0.0f) ? info.reach : 0.0f;
        light.data_1[0] = (std::isfinite(info.meanEmiss) && info.meanEmiss > 0.0f) ? info.meanEmiss : 0.0f;
        light.data_1[1] = (std::isfinite(info.area) && info.area > 0.0f) ? info.area : 0.0f;
        light.data_1[2] = (std::isfinite(info.estimatedPower) && info.estimatedPower > 0.0f) ? info.estimatedPower : 0.0f;
        light.data_1[3] = (std::isfinite(info.boundsRadius) && info.boundsRadius > 0.0f) ? info.boundsRadius : 0.0f;

        const bool projector = std::isfinite(info.projector) && info.projector > 0.5f;
        light.data_2[3] = projector ? 1.0f : 0.0f;
        light.data_7[3] = 0.0f;
        EncodeCone(light, info.angleInner, info.angleOuter, projector);

        return light;
    }

    ShLightEncoded EncodeAsSpotLight(const QrSpotLightUploadInfo &info)
    {
        float direction[3] = { info.direction.data[0], info.direction.data[1], info.direction.data[2] };
        Utils::Normalize(direction);

        const float radius = std::max(kMinSphereRadius, info.radius);
        const float area = static_cast<float>(kPi) * radius * radius;

        /* The cone edge is a smoothstep, and one with equal edges is undefined, so the inner
           angle is pulled strictly inside the outer one before the cosines are taken. The clamp
           stays in angle space and the outer angle keeps a floor, so the cosines of a beam
           narrow enough to round together still differ. */
        const float angleOuter = std::max(info.angleOuter, static_cast<float>(kPi / 180.0));
        const float angleInner = std::min(std::max(std::isfinite(info.angleInner) ? info.angleInner : 0.0f, 0.0f), angleOuter * 0.999f);

        ShLightEncoded light = {};
        light.lightType = LIGHT_TYPE_SPOT;

        for (int i = 0; i < 3; i++)
        {
            light.color[i] = info.color.data[i] / area;
            light.data_0[i] = info.position.data[i];
            light.data_1[i] = direction[i];
        }

        light.data_0[3] = radius;
        light.data_2[0] = std::cos(angleInner);
        light.data_2[1] = std::cos(angleOuter);

        return light;
    }
}

qray::LightManager::LightManager(
    VkDevice _device,
    std::shared_ptr<MemoryAllocator> &_allocator,
    VkBuffer _talCdfBuffer)
    : device(_device)
    , talCdf(_talCdfBuffer)
    , regLightCount(0)
    , regLightCount_Prev(0)
    , dirLightCount(0)
    , dirLightCount_Prev(0)
    , descSetLayout(VK_NULL_HANDLE)
    , descPool(VK_NULL_HANDLE)
    , descSets{}
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        registryGeneration[i] = 1;
    }

    lightsBuffer = std::make_shared<AutoBuffer>(device, _allocator);
    lightsBuffer->Create(sizeof(ShLightEncoded) * kLightArrayMaxSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         "Lights buffer");

    lightsBuffer_Prev.Init(_allocator, sizeof(ShLightEncoded) * kLightArrayMaxSize,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "Lights buffer - prev");

    lightListOffsets = std::make_shared<AutoBuffer>(device, _allocator);
    lightListOffsets->Create(sizeof(uint32_t) * (Q2_MAX_CLUSTERS + 1),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             "Q2 light list offsets");

    lightListLights = std::make_shared<AutoBuffer>(device, _allocator);
    lightListLights->Create(sizeof(uint32_t) * Q2_MAX_CLUSTERS * Q2_LIGHT_LIST_MAX_PER_CELL,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            "Q2 light list lights");

    lightListTailOffsets = std::make_shared<AutoBuffer>(device, _allocator);
    lightListTailOffsets->Create(sizeof(uint32_t) * (Q2_MAX_CLUSTERS + 1 + Q2_MAX_CLUSTERS),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 "Q2 light list tail offsets");

    lightListTailEntries = std::make_shared<AutoBuffer>(device, _allocator);
    lightListTailEntries->Create(sizeof(ShQ2LightTail) * Q2_LIGHT_LIST_TAIL_CAPACITY,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 "Q2 light list tail entries");

    lightStats.Init(_allocator, GetLightStatsSlotSize() * LIGHT_STATS_SLOT_COUNT,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "Q2 light stats");

    clusterSkyVis = std::make_shared<AutoBuffer>(device, _allocator);
    clusterSkyVis->Create(sizeof(uint32_t) * CLUSTER_SKY_VIS_WORD_COUNT,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          "Q2 cluster sky visibility");

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        memset(clusterSkyVis->GetMapped(i), 0xFF, sizeof(uint32_t) * CLUSTER_SKY_VIS_WORD_COUNT);
        clusterSkyVisCopyPending[i] = true;
    }

    dtalMembersBuffer = std::make_shared<AutoBuffer>(device, _allocator);
    dtalMembersBuffer->Create(sizeof(ShDtalMember) * DTAL_MEMBER_CAPACITY,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              "DTAL group members");

    prevToCurIndex = std::make_shared<AutoBuffer>(device, _allocator);
    prevToCurIndex->Create(sizeof(uint32_t) * kLightArrayMaxSize,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "Lights buffer - prev to cur");

    curToPrevIndex = std::make_shared<AutoBuffer>(device, _allocator);
    curToPrevIndex->Create(sizeof(uint32_t) * kLightArrayMaxSize,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "Lights buffer - cur to prev");

    CreateDescriptors();
}

qray::LightManager::~LightManager()
{
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
}

void qray::LightManager::PrepareForFrame(VkCommandBuffer cmd, uint32_t frameIndex)
{
    regLightCount_Prev = regLightCount;
    dirLightCount_Prev = dirLightCount;

    regLightCount = 0;
    dirLightCount = 0;

    const uint32_t prevArrayEnd = GetLightArrayEnd(regLightCount_Prev, dirLightCount_Prev);

    if (prevArrayEnd > 0)
    {
        VkBufferCopy copyInfo = {};
        copyInfo.srcOffset = 0;
        copyInfo.dstOffset = 0;
        copyInfo.size = prevArrayEnd * sizeof(ShLightEncoded);

        vkCmdCopyBuffer(cmd, lightsBuffer->GetDeviceLocal(), lightsBuffer_Prev.GetBuffer(), 1, &copyInfo);
    }

    memset(prevToCurIndex->GetMapped(frameIndex), 0xFF, sizeof(uint32_t) * prevArrayEnd);

    if (++registryGeneration[frameIndex] == 0)
    {
        memset(registry[frameIndex], 0, sizeof(registry[frameIndex]));
        registryGeneration[frameIndex] = 1;
    }

    registeredLightOrder[frameIndex].clear();
    registeredLightIndex[frameIndex].clear();
}

void qray::LightManager::Reset()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        const uint32_t arrayEnd = std::max(GetLightArrayEnd(regLightCount, dirLightCount),
                                           GetLightArrayEnd(regLightCount_Prev, dirLightCount_Prev));

        memset(prevToCurIndex->GetMapped(i), 0xFF, sizeof(uint32_t) * arrayEnd);
        memset(curToPrevIndex->GetMapped(i), 0xFF, sizeof(uint32_t) * arrayEnd);

        registeredLightOrder[i].clear();
        registeredLightIndex[i].clear();

        memset(registry[i], 0, sizeof(registry[i]));
        registryGeneration[i] = 1;

        publishedListValid[i] = false;
        publishedLightOrder[i].clear();
        publishedLightIndex[i].clear();
        publishedTailWords[i] = 0;
        publishedTailClusters[i] = 0;
        lightListCopyPending[i] = false;

        memset(clusterSkyVis->GetMapped(i), 0xFF, sizeof(uint32_t) * CLUSTER_SKY_VIS_WORD_COUNT);
        clusterSkyVisCopyPending[i] = true;

        dtalMembersCopyPending[i] = false;
    }

    dtalMemberCount = 0;

    deviceListValid = false;

    statsClusterTarget = Q2_MAX_CLUSTERS;
    memset(statsClusterCleared, 0, sizeof(statsClusterCleared));

    regLightCount_Prev = regLightCount = 0;
    dirLightCount_Prev = dirLightCount = 0;
}

qray::LightArrayIndex qray::LightManager::GetIndex(const ShLightEncoded &encodedLight) const
{
    switch (encodedLight.lightType)
    {
    case LIGHT_TYPE_DIRECTIONAL:
        return LightArrayIndex{ LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET + dirLightCount };

    case LIGHT_TYPE_SPHERE:
    case LIGHT_TYPE_TRIANGLE:
    case LIGHT_TYPE_SPOT:
    case LIGHT_TYPE_TEXTURED_AREA:
    case LIGHT_TYPE_DTAL_GROUP:
        return LightArrayIndex{ LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET + regLightCount };

    default:
        assert(0);
        return LightArrayIndex{ 0 };
    }
}

void qray::LightManager::IncrementCount(const ShLightEncoded &encodedLight)
{
    switch (encodedLight.lightType)
    {
    case LIGHT_TYPE_DIRECTIONAL:
        dirLightCount++;
        break;

    case LIGHT_TYPE_SPHERE:
    case LIGHT_TYPE_TRIANGLE:
    case LIGHT_TYPE_SPOT:
    case LIGHT_TYPE_TEXTURED_AREA:
    case LIGHT_TYPE_DTAL_GROUP:
        regLightCount++;
        break;

    default:
        assert(0);
    }
}

uint32_t qray::LightManager::GetRegistrySlot(const RegistryEntry *entries, uint32_t generation,
                                             uint64_t uniqueID, bool &found)
{
    const uint64_t hash = uniqueID * 0x9E3779B97F4A7C15ull;
    uint32_t       slot = static_cast<uint32_t>(hash >> 32) & LIGHT_REGISTRY_MASK;

    found = false;

    for (;;)
    {
        const RegistryEntry &entry = entries[slot];

        if (entry.generation != generation)
        {
            return slot;
        }

        if (entry.uniqueID == uniqueID)
        {
            found = true;
            return slot;
        }

        slot = (slot + 1) & LIGHT_REGISTRY_MASK;
    }
}

bool qray::LightManager::FindRegisteredLight(uint32_t frameIndex, uint64_t uniqueID,
                                             uint32_t &outArrayIndex) const
{
    bool found = false;
    const uint32_t slot = GetRegistrySlot(registry[frameIndex], registryGeneration[frameIndex], uniqueID, found);

    if (found)
    {
        outArrayIndex = registry[frameIndex][slot].arrayIndex;
    }

    return found;
}

bool qray::LightManager::DeviceHoldsPublishedList(uint32_t frameIndex) const
{
    return deviceListValid &&
           deviceListGeneration == publishedListGeneration[frameIndex] &&
           deviceListClusters == publishedListClusters[frameIndex] &&
           deviceListWords == publishedListWords[frameIndex] &&
           deviceTailWords == publishedTailWords[frameIndex] &&
           deviceTailClusters == publishedTailClusters[frameIndex] &&
           deviceLightOrder == publishedLightOrder[frameIndex] &&
           deviceLightIndex == publishedLightIndex[frameIndex];
}

void qray::LightManager::RecordDeviceListPublication(uint32_t frameIndex)
{
    deviceListValid = true;
    deviceListGeneration = publishedListGeneration[frameIndex];
    deviceListClusters = publishedListClusters[frameIndex];
    deviceListWords = publishedListWords[frameIndex];
    deviceTailWords = publishedTailWords[frameIndex];
    deviceTailClusters = publishedTailClusters[frameIndex];
    deviceLightOrder = publishedLightOrder[frameIndex];
    deviceLightIndex = publishedLightIndex[frameIndex];
}

void qray::LightManager::AddLight(uint32_t frameIndex, uint64_t uniqueId,
                                  const ShLightEncoded &encodedLight)
{
    std::lock_guard<std::mutex> registryLock(registryMutex);

    bool found = false;
    const uint32_t registrySlot = GetRegistrySlot(registry[frameIndex], registryGeneration[frameIndex],
                                                  uniqueId, found);

    if (found)
    {
        return;
    }

    if (GetLightArrayEnd(regLightCount, dirLightCount) >= kLightArrayMaxSize)
    {
        fprintf(stderr, "qray: light array overflow (regLightCount=%u dirLightCount=%u LIGHT_ARRAY_MAX_SIZE=%u) - dropping light(s)\n",
                regLightCount, dirLightCount, kLightArrayMaxSize);
        assert(0);
        return;
    }

    const LightArrayIndex index = GetIndex(encodedLight);
    IncrementCount(encodedLight);

    auto *pDst = static_cast<ShLightEncoded *>(lightsBuffer->GetMapped(frameIndex));
    memcpy(&pDst[index.GetArrayIndex()], &encodedLight, sizeof(ShLightEncoded));

    const uint32_t ordinal = uint32_t(registeredLightOrder[frameIndex].size());

    FillMatchPrev(frameIndex, index, uniqueId, ordinal);

    RegistryEntry &entry = registry[frameIndex][registrySlot];
    entry.generation = registryGeneration[frameIndex];
    entry.arrayIndex = index.GetArrayIndex();
    entry.uniqueID = uniqueId;

    registeredLightOrder[frameIndex].push_back(uniqueId);
    registeredLightIndex[frameIndex].push_back(index.GetArrayIndex());
}

void qray::LightManager::AddSphericalLight(uint32_t frameIndex, const QrSphericalLightUploadInfo &info)
{
    if (IsColorTooDim(info.color.data))
    {
        return;
    }

    AddLight(frameIndex, info.uniqueID, EncodeAsSphereLight(info));
}

void qray::LightManager::AddPolygonalLight(uint32_t frameIndex, const QrPolygonalLightUploadInfo &info)
{
    if (IsColorTooDim(info.color.data))
    {
        return;
    }

    const QrFloat3D unnormalizedNormal = Utils::GetUnnormalizedNormal(info.positions);

    if (Utils::Dot(unnormalizedNormal.data, unnormalizedNormal.data) <= 0.0f)
    {
        return;
    }

    AddLight(frameIndex, info.uniqueID, EncodeAsTriangleLight(info, unnormalizedNormal));
}

void qray::LightManager::AddTexturedAreaLight(uint32_t frameIndex, const QrTexturedAreaLightUploadInfo &info,
                                              uint32_t textureIndex)
{
    if (IsColorTooDim(info.color.data))
    {
        return;
    }

    AddLight(frameIndex, info.uniqueID, EncodeAsTexturedAreaLight(info, textureIndex));
}

bool qray::LightManager::AddDtalGroups(uint32_t frameIndex, const QrDtalGroupUploadBatch &batch,
                                       const uint32_t *pTextureIndices)
{
    if (batch.pGroups == nullptr || pTextureIndices == nullptr || batch.groupCount == 0)
    {
        return false;
    }

    if (batch.memberCount > DTAL_MEMBER_CAPACITY)
    {
        fprintf(stderr, "qray: DTAL member budget exceeded (%u > %u) - groups not published\n",
                batch.memberCount, DTAL_MEMBER_CAPACITY);
        return false;
    }

    uint32_t published = 0;

    for (uint32_t i = 0; i < batch.groupCount; i++)
    {
        const QrDtalGroupUploadInfo &info = batch.pGroups[i];

        if (info.memberCount == 0 || info.memberBase > batch.memberCount ||
            info.memberCount > batch.memberCount - info.memberBase)
        {
            fprintf(stderr, "qray: DTAL group %u has an invalid member range - skipped\n", i);
            continue;
        }

        const ShLightEncoded encoded = EncodeAsDtalGroup(info, pTextureIndices[i]);
        AddLight(frameIndex, info.uniqueID, encoded);
        published++;
    }

    if (published == 0)
    {
        return false;
    }

    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
    {
        auto *pDst = static_cast<ShDtalMember *>(dtalMembersBuffer->GetMapped(f));
        memcpy(pDst, batch.pMembers, sizeof(ShDtalMember) * batch.memberCount);
        dtalMembersCopyPending[f] = true;
    }

    dtalMemberCount = batch.memberCount;

    return true;
}

void qray::LightManager::AddSpotlight(uint32_t frameIndex, const QrSpotLightUploadInfo &info)
{
    /* `!(x > 0)` rather than `x <= 0`: the latter takes a nan angle for a valid one. */
    if (IsColorTooDim(info.color.data) || info.radius < 0.0f || !(info.angleOuter > 0.0f))
    {
        return;
    }

    AddLight(frameIndex, info.uniqueID, EncodeAsSpotLight(info));
}

void qray::LightManager::AddDirectionalLight(uint32_t frameIndex, const QrDirectionalLightUploadInfo &info)
{
    if (dirLightCount > 0)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Only one directional light is allowed");
    }

    if (IsColorTooDim(info.color.data) || info.angularDiameterDegrees < 0.0f)
    {
        return;
    }

    lastDirLightColor[0] = info.color.data[0];
    lastDirLightColor[1] = info.color.data[1];
    lastDirLightColor[2] = info.color.data[2];

    float direction[3] = { info.direction.data[0], info.direction.data[1], info.direction.data[2] };
    Utils::Normalize(direction);

    lastDirLightDirection[0] = direction[0];
    lastDirLightDirection[1] = direction[1];
    lastDirLightDirection[2] = direction[2];

    lastDirLightAngularRadius = GetAngularRadius(info.angularDiameterDegrees);

    AddLight(frameIndex, info.uniqueID, EncodeAsDirectionalLight(info));
}

bool qray::LightManager::GetLastDirectionalLight(float outColor[3], float outDirection[3],
                                                 float *outAngularRadius) const
{
    if (dirLightCount == 0)
    {
        return false;
    }

    for (int i = 0; i < 3; i++)
    {
        outColor[i] = lastDirLightColor[i];
        outDirection[i] = lastDirLightDirection[i];
    }

    *outAngularRadius = lastDirLightAngularRadius;

    return true;
}

qray::LightManager::Buffers qray::LightManager::GetBuffers() const
{
    return Buffers
    {
        lightsBuffer->GetDeviceLocal(),
        lightListOffsets->GetDeviceLocal(),
        lightListLights->GetDeviceLocal(),
        lightStats.GetBuffer(),
        clusterSkyVis->GetDeviceLocal(),
        dtalMembersBuffer->GetDeviceLocal(),
        lightListTailOffsets->GetDeviceLocal(),
        lightListTailEntries->GetDeviceLocal(),
    };
}

qray::LightManager::FrameCopies qray::LightManager::GetFrameCopies(uint32_t frame) const
{
    assert(frame < MAX_FRAMES_IN_FLIGHT);

    FrameCopies copies = {};

    copies.lights =
    {
        lightsBuffer->GetStaging(frame),
        sizeof(ShLightEncoded) * GetLightArrayEnd(regLightCount, dirLightCount),
    };

    if (lightListCopyPending[frame])
    {
        copies.listOffsets =
        {
            lightListOffsets->GetStaging(frame),
            sizeof(uint32_t) * (Q2_MAX_CLUSTERS + 1),
        };
        copies.listLights =
        {
            lightListLights->GetStaging(frame),
            sizeof(uint32_t) * publishedListWords[frame],
        };
        copies.tailOffsets =
        {
            lightListTailOffsets->GetStaging(frame),
            sizeof(uint32_t) * (2 * Q2_MAX_CLUSTERS + 1),
        };
        copies.tailEntries =
        {
            lightListTailEntries->GetStaging(frame),
            sizeof(ShQ2LightTail) * publishedTailWords[frame],
        };
    }

    if (clusterSkyVisCopyPending[frame])
    {
        copies.clusterSkyVis =
        {
            clusterSkyVis->GetStaging(frame),
            sizeof(uint32_t) * CLUSTER_SKY_VIS_WORD_COUNT,
        };
    }

    if (dtalMembersCopyPending[frame] && dtalMemberCount > 0)
    {
        copies.dtalMembers =
        {
            dtalMembersBuffer->GetStaging(frame),
            sizeof(ShDtalMember) * dtalMemberCount,
        };
    }

    return copies;
}

void qray::LightManager::ConsumeFrameCopies(uint32_t frame)
{
    assert(frame < MAX_FRAMES_IN_FLIGHT);

    if (lightListCopyPending[frame])
    {
        RecordDeviceListPublication(frame);
        lightListCopyPending[frame] = false;
    }

    clusterSkyVisCopyPending[frame] = false;
    dtalMembersCopyPending[frame] = false;
}

uint32_t qray::LightManager::GetLightStatsClusterTarget() const
{
    return std::min(statsClusterTarget, LIGHT_STATS_CLUSTER_COUNT);
}

VkDeviceSize qray::LightManager::GetLightStatsClusterSize() const
{
    return GetLightStatsSlotSize() / LIGHT_STATS_CLUSTER_COUNT;
}

void qray::LightManager::SetClusterLightLists(uint32_t frameIndex, uint32_t numClusters,
                                              const uint32_t *pOffsets, const uint64_t *pLightUniqueIds,
                                              uint32_t totalLightCount, uint64_t listGeneration,
                                              const ClusterLightTailRange &tails)
{
    numClusters = std::min(numClusters, uint32_t(Q2_MAX_CLUSTERS));
    statsClusterTarget = numClusters;

    const uint32_t lightWordCapacity = Q2_MAX_CLUSTERS * Q2_LIGHT_LIST_MAX_PER_CELL;
    const uint32_t listWordCount = std::min(totalLightCount, lightWordCapacity);

    const bool tailsProvided = tails.tailCount > 0 && tails.pOffsets != nullptr && tails.pUniqueIds != nullptr &&
                               tails.pProb != nullptr && tails.pMarginal != nullptr && tails.pAlias != nullptr &&
                               tails.pBeta != nullptr;
    uint32_t tailWordCount =
        tailsProvided ? std::min(tails.tailCount, uint32_t(Q2_LIGHT_LIST_TAIL_CAPACITY)) : 0;

    const uint32_t registeredCount = uint32_t(registeredLightOrder[frameIndex].size());
    const bool samePlaces =
        publishedLightIndex[frameIndex].size() == registeredCount &&
        (registeredCount == 0 ||
         memcmp(publishedLightIndex[frameIndex].data(), registeredLightIndex[frameIndex].data(),
                sizeof(uint32_t) * registeredCount) == 0);

    const bool sameAsPublished =
        publishedListValid[frameIndex] && publishedListGeneration[frameIndex] == listGeneration &&
        publishedListClusters[frameIndex] == numClusters && publishedListWords[frameIndex] == listWordCount &&
        publishedTailWords[frameIndex] == tailWordCount &&
        samePlaces && publishedLightOrder[frameIndex] == registeredLightOrder[frameIndex];

    if (sameAsPublished)
    {
        if (!DeviceHoldsPublishedList(frameIndex))
        {
            lightListCopyPending[frameIndex] = true;
        }
        return;
    }

    uint32_t *pDstOffsets = static_cast<uint32_t *>(lightListOffsets->GetMapped(frameIndex));

    for (uint32_t i = 0; i <= Q2_MAX_CLUSTERS; i++)
    {
        pDstOffsets[i] = 0;
    }

    for (uint32_t i = 0; i <= numClusters; i++)
    {
        pDstOffsets[i] = pOffsets[i];
    }

    uint32_t *pDstLights = static_cast<uint32_t *>(lightListLights->GetMapped(frameIndex));

    struct CachedIndex
    {
        uint64_t uid;
        uint32_t index;
    };

    constexpr uint32_t kCacheSize = 2048;
    constexpr uint32_t kNotCached = ~0u;

    CachedIndex cache[kCacheSize];
    memset(cache, 0xFF, sizeof(cache));

    for (uint32_t i = 0; i < listWordCount; i++)
    {
        const uint64_t uid = pLightUniqueIds[i];

        if (uid == kLightUidHole)
        {
            pDstLights[i] = uint32_t(LIGHT_INDEX_NONE);
            continue;
        }

        const uint64_t hash = uid * 0x9E3779B97F4A7C15ull;
        uint32_t       slot = static_cast<uint32_t>(hash >> 32) & (kCacheSize - 1);

        uint32_t index = uint32_t(LIGHT_INDEX_NONE);
        bool     found = false;

        for (uint32_t probe = 0; probe < kCacheSize; probe++)
        {
            const CachedIndex &entry = cache[slot];

            if (entry.index == kNotCached)
            {
                break;
            }

            if (entry.uid == uid)
            {
                index = entry.index;
                found = true;
                break;
            }

            slot = (slot + 1) & (kCacheSize - 1);
        }

        if (!found)
        {
            uint32_t resolved = 0;
            index = FindRegisteredLight(frameIndex, uid, resolved) ? resolved : uint32_t(LIGHT_INDEX_NONE);

            if (cache[slot].index == kNotCached)
            {
                cache[slot].uid = uid;
                cache[slot].index = index;
            }
        }

        pDstLights[i] = index;
    }

    uint32_t *pTailOffsets = static_cast<uint32_t *>(lightListTailOffsets->GetMapped(frameIndex));
    ShQ2LightTail *pTailEntries = static_cast<ShQ2LightTail *>(lightListTailEntries->GetMapped(frameIndex));

    for (uint32_t i = 0; i < 2 * Q2_MAX_CLUSTERS + 1; i++)
    {
        pTailOffsets[i] = 0;
    }

    if (tailWordCount > 0)
    {
        for (uint32_t i = 0; i <= numClusters; i++)
        {
            pTailOffsets[i] = tails.pOffsets[i];
        }

        float *pTailBeta = reinterpret_cast<float *>(pTailOffsets + Q2_MAX_CLUSTERS + 1);

        for (uint32_t c = 0; c < numClusters; c++)
        {
            const float beta = tails.pBeta[c];

            pTailBeta[c] = (std::isfinite(beta) && beta > 0.0f) ? (beta > 1.0f ? 1.0f : beta) : 0.0f;
        }

        uint32_t unresolved = 0;

        for (uint32_t i = 0; i < tailWordCount; i++)
        {
            const uint64_t uid = tails.pUniqueIds[i];
            uint32_t       index = uint32_t(LIGHT_INDEX_NONE);

            if (uid != kLightUidHole)
            {
                const uint64_t hash = uid * 0x9E3779B97F4A7C15ull;
                uint32_t       slot = static_cast<uint32_t>(hash >> 32) & (kCacheSize - 1);

                bool found = false;

                for (uint32_t probe = 0; probe < kCacheSize; probe++)
                {
                    const CachedIndex &entry = cache[slot];

                    if (entry.index == kNotCached)
                    {
                        break;
                    }

                    if (entry.uid == uid)
                    {
                        index = entry.index;
                        found = true;
                        break;
                    }

                    slot = (slot + 1) & (kCacheSize - 1);
                }

                if (!found)
                {
                    uint32_t resolved = 0;
                    index = FindRegisteredLight(frameIndex, uid, resolved) ? resolved : uint32_t(LIGHT_INDEX_NONE);

                    if (cache[slot].index == kNotCached)
                    {
                        cache[slot].uid = uid;
                        cache[slot].index = index;
                    }
                }
            }

            pTailEntries[i].lightIndex = index;
            pTailEntries[i].aliasIndex = (index != uint32_t(LIGHT_INDEX_NONE)) ? tails.pAlias[i] : i;
            pTailEntries[i].prob = tails.pProb[i];
            pTailEntries[i].marginalProb = (index != uint32_t(LIGHT_INDEX_NONE)) ? tails.pMarginal[i] : 0.0f;

            if (index == uint32_t(LIGHT_INDEX_NONE))
            {
                unresolved++;
            }
        }

        if (unresolved > 0)
        {
            /* An accepted overflow source without a renderer record is a coherence failure:
               publish a consistent fast-only frame instead of a distribution with holes. */
            fprintf(stderr, "qray: %u overflow tail sources have no renderer record - the tail is left empty\n",
                    unresolved);

            tailWordCount = 0;

            for (uint32_t i = 0; i < 2 * Q2_MAX_CLUSTERS + 1; i++)
            {
                pTailOffsets[i] = 0;
            }
        }
    }

    publishedListValid[frameIndex] = true;
    publishedListGeneration[frameIndex] = listGeneration;
    publishedListClusters[frameIndex] = numClusters;
    publishedListWords[frameIndex] = listWordCount;
    publishedTailWords[frameIndex] = tailWordCount;
    publishedTailClusters[frameIndex] = numClusters;
    publishedLightOrder[frameIndex] = registeredLightOrder[frameIndex];
    publishedLightIndex[frameIndex].assign(registeredLightIndex[frameIndex].begin(),
                                           registeredLightIndex[frameIndex].end());

    lightListCopyPending[frameIndex] = true;
}

void qray::LightManager::SetClusterSkyVisibility(const uint8_t *pBits, uint32_t numClusters)
{
    uint32_t words[CLUSTER_SKY_VIS_WORD_COUNT];
    memset(words, 0xFF, sizeof(words));

    if (pBits != nullptr)
    {
        const uint32_t count = std::min(numClusters, LIGHT_STATS_CLUSTER_COUNT);

        for (uint32_t cluster = 0; cluster < count; cluster++)
        {
            if ((pBits[cluster >> 3] & (1u << (cluster & 7u))) == 0)
            {
                words[cluster >> 5] &= ~(1u << (cluster & 31u));
            }
        }
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        memcpy(clusterSkyVis->GetMapped(i), words, sizeof(words));
        clusterSkyVisCopyPending[i] = true;
    }
}

void qray::LightManager::ResetLightStats(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t frameId)
{
    const uint32_t slot = frameId % LIGHT_STATS_SLOT_COUNT;

    const VkDeviceSize slotSize = GetLightStatsSlotSize();
    const uint32_t clusterCount = std::min(statsClusterTarget, LIGHT_STATS_CLUSTER_COUNT);
    const VkDeviceSize clusterSize = slotSize / LIGHT_STATS_CLUSTER_COUNT;
    const VkDeviceSize fillSize = clusterSize * clusterCount;

    for (uint32_t i = 0; i < LIGHT_STATS_SLOT_COUNT; i++)
    {
        if (i != slot && statsClusterCleared[i] >= clusterCount)
        {
            continue;
        }

        vkCmdFillBuffer(cmd, lightStats.GetBuffer(), slotSize * i, fillSize, 0);
        statsClusterCleared[i] = clusterCount;
    }
}

void qray::LightManager::BarrierQ2ClusterLists(VkCommandBuffer cmd, uint32_t frameIndex)
{
    const VkBuffer buffers[] =
    {
        lightListOffsets->GetDeviceLocal(),
        lightListLights->GetDeviceLocal(),
        lightStats.GetBuffer(),
    };

    VkBufferMemoryBarrier2 barriers[std::size(buffers)] = {};

    for (uint32_t i = 0; i < std::size(buffers); i++)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        barriers[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        barriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barriers[i].buffer = buffers[i];
        barriers[i].offset = 0;
        barriers[i].size = VK_WHOLE_SIZE;
    }

    VkDependencyInfo dependency =
    {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = static_cast<uint32_t>(std::size(barriers)),
        .pBufferMemoryBarriers = barriers
    };

    svkCmdPipelineBarrier2KHR(cmd, &dependency);
}

VkDescriptorSetLayout qray::LightManager::GetDescSetLayout()
{
    return descSetLayout;
}

VkDescriptorSet qray::LightManager::GetDescSet(uint32_t frameIndex)
{
    return descSets[frameIndex];
}

void qray::LightManager::FillMatchPrev(uint32_t curFrameIndex, LightArrayIndex lightIndexInCurFrame,
                                       UniqueLightID uniqueID, uint32_t ordinal)
{
    const uint32_t prevFrameIndex = (curFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    uint32_t lightIndexInPrevFrame = 0;

    if (ordinal < registeredLightOrder[prevFrameIndex].size() &&
        registeredLightOrder[prevFrameIndex][ordinal] == uniqueID)
    {
        lightIndexInPrevFrame = registeredLightIndex[prevFrameIndex][ordinal];
    }
    else if (!FindRegisteredLight(prevFrameIndex, uniqueID, lightIndexInPrevFrame))
    {
        return;
    }

    auto *pPrevToCur = static_cast<uint32_t *>(prevToCurIndex->GetMapped(curFrameIndex));
    pPrevToCur[lightIndexInPrevFrame] = lightIndexInCurFrame.GetArrayIndex();

    auto *pCurToPrev = static_cast<uint32_t *>(curToPrevIndex->GetMapped(curFrameIndex));
    pCurToPrev[lightIndexInCurFrame.GetArrayIndex()] = lightIndexInPrevFrame;
}

constexpr uint32_t BINDINGS[] =
{
    BINDING_LIGHT_SOURCES,
    BINDING_LIGHT_SOURCES_PREV,
    BINDING_LIGHT_SOURCES_INDEX_PREV_TO_CUR,
    BINDING_LIGHT_SOURCES_INDEX_CUR_TO_PREV,
    BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_OFFSETS,
    BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_LIGHTS,
    BINDING_LIGHT_SOURCES_Q2_LIGHT_STATS,
    BINDING_LIGHT_SOURCES_TAL_CDF,
    BINDING_LIGHT_SOURCES_Q2_CLUSTER_SKY_VIS,
    BINDING_LIGHT_SOURCES_DTAL_MEMBERS,
    BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL_OFFSETS,
    BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_TAIL,
};

void qray::LightManager::CreateDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, std::size(BINDINGS)> bindings = {};

    for (uint32_t i = 0; i < std::size(BINDINGS); i++)
    {
        const uint32_t binding = BINDINGS[i];
        assert(i == binding);

        VkDescriptorSetLayoutBinding &b = bindings[binding];
        b.binding = binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(result);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Light buffers Desc set layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(bindings.size()) * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(result);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Light buffers Desc set pool");

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descSetLayout;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        result = vkAllocateDescriptorSets(device, &allocInfo, &descSets[i]);
        VK_CHECKERROR(result);

        SET_DEBUG_NAME(device, descSets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, "Light buffers Desc set");
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        UpdateDescriptors(i);
    }
}

void qray::LightManager::UpdateDescriptors(uint32_t frameIndex)
{
    const VkBuffer buffers[] =
    {
        lightsBuffer->GetDeviceLocal(),
        lightsBuffer_Prev.GetBuffer(),
        prevToCurIndex->GetDeviceLocal(),
        curToPrevIndex->GetDeviceLocal(),
        lightListOffsets->GetDeviceLocal(),
        lightListLights->GetDeviceLocal(),
        lightStats.GetBuffer(),
        talCdf,
        clusterSkyVis->GetDeviceLocal(),
        dtalMembersBuffer->GetDeviceLocal(),
        lightListTailOffsets->GetDeviceLocal(),
        lightListTailEntries->GetDeviceLocal(),
    };
    static_assert(std::size(BINDINGS) == std::size(buffers));

    std::array<VkDescriptorBufferInfo, std::size(BINDINGS)> bufferInfos = {};
    std::array<VkWriteDescriptorSet, std::size(BINDINGS)> writes = {};

    for (uint32_t i = 0; i < std::size(BINDINGS); i++)
    {
        const uint32_t binding = BINDINGS[i];
        assert(i == binding);

        VkDescriptorBufferInfo &bufferInfo = bufferInfos[binding];
        bufferInfo.buffer = buffers[binding];
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet &write = writes[binding];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSets[frameIndex];
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

uint32_t qray::LightManager::GetLightCount() const
{
    return regLightCount;
}

uint32_t qray::LightManager::GetLightCountPrev() const
{
    return regLightCount_Prev;
}

uint32_t qray::LightManager::DoesDirectionalLightExist() const
{
    return dirLightCount > 0 ? 1 : 0;
}

uint32_t qray::LightManager::GetLightIndexIgnoreFPVShadows(uint32_t frameIndex, uint64_t *pLightUniqueId) const
{
    if (pLightUniqueId == nullptr)
    {
        return LIGHT_INDEX_NONE;
    }

    uint32_t index = 0;
    if (!FindRegisteredLight(frameIndex, *pLightUniqueId, index))
    {
        return LIGHT_INDEX_NONE;
    }

    return index;
}

}

static_assert(qray::MAX_FRAMES_IN_FLIGHT == 2);
