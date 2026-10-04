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

#include "ASManager.h"
#include "LightManager.h"
#include "VertexPreprocessing.h"

namespace qray
{

class Scene
{
public:
    explicit Scene(
        VkDevice device,
        std::shared_ptr<PhysicalDevice> physDevice,
        std::shared_ptr<MemoryAllocator> &allocator,
        std::shared_ptr<CommandBufferManager> &cmdManager,
        std::shared_ptr<TextureManager> &textureManager,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<const ShaderManager> &shaderManager);

    ~Scene();

    Scene(const Scene& other) = delete;
    Scene(Scene&& other) noexcept = delete;
    Scene& operator=(const Scene& other) = delete;
    Scene& operator=(Scene&& other) noexcept = delete;

    void PrepareForFrame(VkCommandBuffer cmd, uint32_t frameIndex);

    void PreprocessVertices(VkCommandBuffer cmd, uint32_t frameIndex,
                            const std::shared_ptr<GlobalUniform> &uniform,
                            const ShVertPreprocessing &push);

    bool Upload(uint32_t frameIndex, const QrGeometryUploadInfo &uploadInfo);
    bool UpdateTransform(const QrUpdateTransformInfo &updateInfo);
    bool UpdateTexCoords(const QrUpdateTexCoordsInfo &texCoordsInfo);

    void UploadLight(uint32_t frameIndex, const QrSphericalLightUploadInfo &lightInfo);
    void UploadLight(uint32_t frameIndex, const QrPolygonalLightUploadInfo &lightInfo);
    void UploadLight(uint32_t frameIndex, const QrTexturedAreaLightUploadInfo &lightInfo, uint32_t textureIndex);
    void UploadLight(uint32_t frameIndex, const QrDirectionalLightUploadInfo &lightInfo);
    void UploadLight(uint32_t frameIndex, const QrSpotLightUploadInfo &lightInfo);
    bool UploadDtalGroups(uint32_t frameIndex, const QrDtalGroupUploadBatch &batch, const uint32_t *pTextureIndices);

    void SubmitStatic();
    void StartNewStatic();

    const std::shared_ptr<ASManager> &GetASManager();
    const std::shared_ptr<LightManager> &GetLightManager();
    const std::shared_ptr<VertexPreprocessing> &GetVertexPreprocessing();

    bool HasAABB() const;
    void GetAABB(float outMin[3], float outMax[3]) const;

    bool DoesUniqueIDExist(uint64_t uniqueID) const;
    bool DoesDynamicUniqueIDExist(uint64_t uniqueID) const;

private:
    bool TryGetStaticSimpleIndex(uint64_t uniqueID, uint32_t *result) const;

    uint32_t GetVertexPreprocessingMode() const;

private:
    std::shared_ptr<ASManager> asManager;
    std::shared_ptr<LightManager> lightManager;
    std::shared_ptr<GeomInfoManager> geomInfoMgr;
    std::shared_ptr<VertexPreprocessing> vertPreproc;

    rgl::unordered_map<uint64_t, uint32_t> dynamicUniqueIDToSimpleIndex;
    rgl::unordered_map<uint64_t, uint32_t> staticUniqueIDToSimpleIndex;

    std::vector<uint32_t> movableGeomIndices;
    bool toResubmitMovable;

    bool isRecordingStatic;
    bool submittedStaticInCurrentFrame;

    float aabbMin[3];
    float aabbMax[3];
    bool  aabbInitialized;
};

}
