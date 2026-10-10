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

#include <cstdint>
#include <mutex>
#include <vector>

#include "qray/qray.h"
#include "Common.h"
#include "Containers.h"
#include "AutoBuffer.h"
#include "LightDefs.h"

namespace qray
{

struct ShLightEncoded;

constexpr uint64_t kLightUidHole = ~0ull;

class LightManager
{
public:
    static constexpr uint32_t LIGHT_ARRAY_ENTRY_COUNT = 4096;
    static constexpr uint32_t DTAL_MEMBER_CAPACITY = QR_DTAL_MAX_UPLOAD_MEMBERS;

    static constexpr uint32_t LIGHT_STATS_CLUSTER_COUNT = 8192;
    static constexpr uint32_t LIGHT_STATS_SLOT_COUNT = 3;

    static constexpr uint32_t CLUSTER_SKY_VIS_WORD_COUNT = LIGHT_STATS_CLUSTER_COUNT / 32;

    LightManager(VkDevice device, std::shared_ptr<MemoryAllocator> &allocator, VkBuffer talCdfBuffer);
    ~LightManager();

    LightManager(const LightManager &other) = delete;
    LightManager(LightManager &&other) noexcept = delete;
    LightManager &operator=(const LightManager &other) = delete;
    LightManager &operator=(LightManager &&other) noexcept = delete;

    void PrepareForFrame(VkCommandBuffer cmd, uint32_t frameIndex);
    void Reset();

    uint32_t GetLightCount() const;
    uint32_t GetLightCountPrev() const;
    uint32_t DoesDirectionalLightExist() const;

    bool GetLastDirectionalLight(float outColor[3], float outDirection[3], float *outAngularRadius) const;

    uint32_t GetLightIndexIgnoreFPVShadows(uint32_t frameIndex, uint64_t *pLightUniqueId) const;

    void AddSphericalLight(uint32_t frameIndex, const QrSphericalLightUploadInfo &info);
    void AddPolygonalLight(uint32_t frameIndex, const QrPolygonalLightUploadInfo &info);
    void AddTexturedAreaLight(uint32_t frameIndex, const QrTexturedAreaLightUploadInfo &info, uint32_t textureIndex);
    void AddDirectionalLight(uint32_t frameIndex, const QrDirectionalLightUploadInfo &info);
    void AddSpotlight(uint32_t frameIndex, const QrSpotLightUploadInfo &info);

    void BeginDeferredUploads(uint32_t slot);
    void EndDeferredUploads();
    void FlushDeferredUploads();
    bool AddDtalGroups(uint32_t frameIndex, const QrDtalGroupUploadBatch &batch, const uint32_t *pTextureIndices);

    struct ClusterLightTailRange
    {
        const uint32_t *pOffsets = nullptr;
        const uint64_t *pUniqueIds = nullptr;
        const float    *pProb = nullptr;
        const float    *pMarginal = nullptr;
        const uint32_t *pAlias = nullptr;
        const float    *pBeta = nullptr;
        uint32_t        tailCount = 0;
    };

    void SetClusterLightLists(uint32_t frameIndex, uint32_t numClusters,
                              const uint32_t *pOffsets, const uint64_t *pLightUniqueIds,
                              uint32_t totalLightCount, uint64_t listGeneration,
                              const ClusterLightTailRange &tails);

    uint32_t GetLastPublicationMask() const { return publicationMask; }

    void SetClusterSkyVisibility(const uint8_t *pBits, uint32_t numClusters);

    void ResetLightStats(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t frameId);
    void BarrierQ2ClusterLists(VkCommandBuffer cmd, uint32_t frameIndex);

    uint32_t GetLightStatsClusterTarget() const;
    VkDeviceSize GetLightStatsClusterSize() const;

    VkDescriptorSetLayout GetDescSetLayout();
    VkDescriptorSet GetDescSet(uint32_t frameIndex);

    struct Buffers
    {
        VkBuffer lights;
        VkBuffer listOffsets;
        VkBuffer listLights;
        VkBuffer lightStats;
        VkBuffer clusterSkyVis;
        VkBuffer dtalMembers;
        VkBuffer tailOffsets;
        VkBuffer tailEntries;
    };

    struct Copy
    {
        VkBuffer staging;
        VkDeviceSize size;
    };

    struct FrameCopies
    {
        Copy lights;
        Copy listOffsets;
        Copy listLights;
        Copy clusterSkyVis;
        Copy dtalMembers;
        Copy tailOffsets;
        Copy tailEntries;
    };

    Buffers GetBuffers() const;
    FrameCopies GetFrameCopies(uint32_t frame) const;
    void ConsumeFrameCopies(uint32_t frame);

private:
    struct RegistryEntry
    {
        uint32_t generation;
        uint32_t arrayIndex;
        uint64_t uniqueID;
    };

    static constexpr uint32_t LIGHT_REGISTRY_SIZE = 2 * LIGHT_ARRAY_ENTRY_COUNT;
    static constexpr uint32_t LIGHT_REGISTRY_MASK = LIGHT_REGISTRY_SIZE - 1;

    static uint32_t GetRegistrySlot(const RegistryEntry *entries, uint32_t generation, uint64_t uniqueID, bool &found);

    bool FindRegisteredLight(uint32_t frameIndex, uint64_t uniqueID, uint32_t &outArrayIndex) const;

    LightArrayIndex GetIndex(const ShLightEncoded &encodedLight) const;
    void IncrementCount(const ShLightEncoded &encodedLight);
    void AddLight(uint32_t frameIndex, uint64_t uniqueId, const ShLightEncoded &encodedLight);

    void FillMatchPrev(uint32_t curFrameIndex, LightArrayIndex lightIndexInCurFrame, UniqueLightID uniqueID, uint32_t ordinal);

    bool DeviceHoldsPublishedList(uint32_t frameIndex) const;
    void RecordDeviceListPublication(uint32_t frameIndex);

    void CreateDescriptors();
    void UpdateDescriptors(uint32_t frameIndex);

private:
    VkDevice device;
    VkBuffer talCdf;

    std::mutex registryMutex;

    std::shared_ptr<AutoBuffer> lightsBuffer;
    Buffer lightsBuffer_Prev;

    std::shared_ptr<AutoBuffer> lightListOffsets;
    std::shared_ptr<AutoBuffer> lightListLights;
    std::shared_ptr<AutoBuffer> lightListTailOffsets;
    std::shared_ptr<AutoBuffer> lightListTailEntries;

    std::shared_ptr<AutoBuffer> clusterSkyVis;
    bool                        clusterSkyVisCopyPending[MAX_FRAMES_IN_FLIGHT] = {};

    std::shared_ptr<AutoBuffer> dtalMembersBuffer;
    uint32_t                    dtalMemberCount = 0;
    bool                        dtalMembersCopyPending[MAX_FRAMES_IN_FLIGHT] = {};

    bool     lightListCopyPending[MAX_FRAMES_IN_FLIGHT] = {};
    bool     publishedListValid[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t publishedListGeneration[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedListClusters[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedListWords[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedTailWords[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedTailClusters[MAX_FRAMES_IN_FLIGHT] = {};
    std::vector<uint64_t> publishedLightOrder[MAX_FRAMES_IN_FLIGHT];
    std::vector<uint32_t> publishedLightIndex[MAX_FRAMES_IN_FLIGHT];

    bool     deviceListValid = false;
    uint64_t deviceListGeneration = 0;
    uint32_t deviceListClusters = 0;
    uint32_t deviceListWords = 0;
    uint32_t deviceTailWords = 0;
    uint32_t deviceTailClusters = 0;
    std::vector<uint64_t> deviceLightOrder;
    std::vector<uint32_t> deviceLightIndex;

    uint32_t publicationMask = 0;

    static constexpr uint32_t kDeferredUploadSlots = 8;
    static constexpr uint32_t kDeferredUploadCapacity = 8192;

    enum DeferredUploadKind : uint32_t
    {
        kDeferredDirectional = 1,
        kDeferredSpherical,
        kDeferredPolygonal,
        kDeferredTexturedArea,
        kDeferredSpot
    };

    struct DeferredUpload
    {
        uint32_t           frameIndex;
        DeferredUploadKind kind;
        uint32_t           textureIndex;
        union
        {
            QrDirectionalLightUploadInfo  dir;
            QrSphericalLightUploadInfo    sph;
            QrPolygonalLightUploadInfo    poly;
            QrTexturedAreaLightUploadInfo area;
            QrSpotLightUploadInfo         spot;
        } payload;
    };

    std::vector<DeferredUpload> deferredUploads[kDeferredUploadSlots];
    uint32_t                    deferredUploadsDropped = 0;

    bool TryDeferUpload(uint32_t frameIndex, DeferredUploadKind kind, const void *pPayload, size_t payloadSize,
                        uint32_t textureIndex);

    Buffer lightStats;

    uint32_t statsClusterTarget = LIGHT_STATS_CLUSTER_COUNT;
    uint32_t statsClusterCleared[LIGHT_STATS_SLOT_COUNT] = {};

    std::shared_ptr<AutoBuffer> prevToCurIndex;
    std::shared_ptr<AutoBuffer> curToPrevIndex;

    RegistryEntry registry[MAX_FRAMES_IN_FLIGHT][LIGHT_REGISTRY_SIZE] = {};
    uint32_t registryGeneration[MAX_FRAMES_IN_FLIGHT] = {};

    std::vector<uint64_t> registeredLightOrder[MAX_FRAMES_IN_FLIGHT];
    std::vector<uint32_t> registeredLightIndex[MAX_FRAMES_IN_FLIGHT];

    uint32_t regLightCount;
    uint32_t regLightCount_Prev;
    uint32_t dirLightCount;
    uint32_t dirLightCount_Prev;

    float lastDirLightColor[3];
    float lastDirLightDirection[3];
    float lastDirLightAngularRadius;

    VkDescriptorSetLayout descSetLayout;
    VkDescriptorPool descPool;
    VkDescriptorSet descSets[MAX_FRAMES_IN_FLIGHT];
};

}
