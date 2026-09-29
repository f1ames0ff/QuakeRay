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

#include <cstdint>
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

    void SetClusterLightLists(uint32_t frameIndex, uint32_t numClusters,
                              const uint32_t *pOffsets, const uint64_t *pLightUniqueIds,
                              uint32_t totalLightCount, uint64_t listGeneration);

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
    };

    Buffers GetBuffers() const;
    FrameCopies GetFrameCopies(uint32_t frame) const;
    void ConsumeFrameCopies(uint32_t frame);

    float GetRegisteredLightPower(uint32_t frameIndex, uint64_t uniqueID) const;

private:
    struct RegistryEntry
    {
        uint32_t generation;
        uint32_t arrayIndex;
        uint64_t uniqueID;
        float    power;
    };

    static constexpr uint32_t LIGHT_REGISTRY_SIZE = 2 * LIGHT_ARRAY_ENTRY_COUNT;
    static constexpr uint32_t LIGHT_REGISTRY_MASK = LIGHT_REGISTRY_SIZE - 1;

    static uint32_t GetRegistrySlot(const RegistryEntry *entries, uint32_t generation, uint64_t uniqueID, bool &found);

    bool FindRegisteredLight(uint32_t frameIndex, uint64_t uniqueID, uint32_t &outArrayIndex) const;

    LightArrayIndex GetIndex(const ShLightEncoded &encodedLight) const;
    void IncrementCount(const ShLightEncoded &encodedLight);
    void AddLight(uint32_t frameIndex, uint64_t uniqueId, const ShLightEncoded &encodedLight, float power);

    void FillMatchPrev(uint32_t curFrameIndex, LightArrayIndex lightIndexInCurFrame, UniqueLightID uniqueID, uint32_t ordinal);

    bool DeviceHoldsPublishedList(uint32_t frameIndex) const;
    void RecordDeviceListPublication(uint32_t frameIndex);

    void CreateDescriptors();
    void UpdateDescriptors(uint32_t frameIndex);

private:
    VkDevice device;
    VkBuffer talCdf;

    std::shared_ptr<AutoBuffer> lightsBuffer;
    Buffer lightsBuffer_Prev;

    std::shared_ptr<AutoBuffer> lightListOffsets;
    std::shared_ptr<AutoBuffer> lightListLights;

    std::shared_ptr<AutoBuffer> clusterSkyVis;
    bool                        clusterSkyVisCopyPending[MAX_FRAMES_IN_FLIGHT] = {};

    bool     lightListCopyPending[MAX_FRAMES_IN_FLIGHT] = {};
    bool     publishedListValid[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t publishedListGeneration[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedListClusters[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t publishedListWords[MAX_FRAMES_IN_FLIGHT] = {};
    std::vector<uint64_t> publishedLightOrder[MAX_FRAMES_IN_FLIGHT];
    std::vector<uint32_t> publishedLightIndex[MAX_FRAMES_IN_FLIGHT];

    bool     deviceListValid = false;
    uint64_t deviceListGeneration = 0;
    uint32_t deviceListClusters = 0;
    uint32_t deviceListWords = 0;
    std::vector<uint64_t> deviceLightOrder;
    std::vector<uint32_t> deviceLightIndex;

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
