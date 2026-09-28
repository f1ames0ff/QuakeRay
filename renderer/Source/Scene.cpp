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

#include "Scene.h"
#include "Generated/ShaderCommonC.h"
#include "QrException.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace qray;

qray::Scene::Scene(
    VkDevice _device,
    std::shared_ptr<PhysicalDevice> _physDevice,
    std::shared_ptr<MemoryAllocator> &_allocator,
    std::shared_ptr<CommandBufferManager> &_cmdManager,
    std::shared_ptr<TextureManager> &_textureManager,
    const std::shared_ptr<const GlobalUniform> &_uniform,
    const std::shared_ptr<const ShaderManager> &_shaderManager)
:
    toResubmitMovable(false),
    isRecordingStatic(false),
    submittedStaticInCurrentFrame(false),
    aabbInitialized(false)
{
    const float maxF = std::numeric_limits<float>::max();
    aabbMin[0] = aabbMin[1] = aabbMin[2] = maxF;
    aabbMax[0] = aabbMax[1] = aabbMax[2] = -maxF;

    VertexCollectorFilterTypeFlags_Init();

    lightManager = std::make_shared<LightManager>(_device, _allocator, _textureManager->GetTalCdfBuffer());
    geomInfoMgr = std::make_shared<GeomInfoManager>(_device, _allocator);

    asManager = std::make_shared<ASManager>(_device, _physDevice, _allocator, _cmdManager, _textureManager, geomInfoMgr);

    vertPreproc = std::make_shared<VertexPreprocessing>(_device, _uniform, asManager, _shaderManager);
}

qray::Scene::~Scene()
{
}

void qray::Scene::PrepareForFrame(VkCommandBuffer cmd, uint32_t frameIndex)
{
    dynamicUniqueIDToSimpleIndex.clear();

    geomInfoMgr->PrepareForFrame(frameIndex);
    lightManager->PrepareForFrame(cmd, frameIndex);

    asManager->BeginDynamicGeometry(cmd, frameIndex);
}

void qray::Scene::PreprocessVertices(VkCommandBuffer cmd, uint32_t frameIndex,
                                     const std::shared_ptr<GlobalUniform> &uniform,
                                     const ShVertPreprocessing &push)
{
    vertPreproc->Preprocess(cmd, frameIndex, GetVertexPreprocessingMode(), uniform, asManager, push);
    submittedStaticInCurrentFrame = false;
}

uint32_t qray::Scene::GetVertexPreprocessingMode() const
{
    if (submittedStaticInCurrentFrame)
    {
        return VERT_PREPROC_MODE_ALL;
    }

    if (toResubmitMovable)
    {
        return VERT_PREPROC_MODE_DYNAMIC_AND_MOVABLE;
    }

    return VERT_PREPROC_MODE_ONLY_DYNAMIC;
}

bool qray::Scene::Upload(uint32_t frameIndex, const QrGeometryUploadInfo &uploadInfo)
{
    assert(!DoesUniqueIDExist(uploadInfo.uniqueID));

    {
        const QrVertex *verts = uploadInfo.pVertices;
        const uint32_t count = uploadInfo.vertexCount;

        for (uint32_t i = 0; i < count; i++)
        {
            const float *src = verts[i].position;
            float p[3];

            for (int k = 0; k < 3; k++)
            {
                p[k] = uploadInfo.transform.matrix[k][0] * src[0] +
                       uploadInfo.transform.matrix[k][1] * src[1] +
                       uploadInfo.transform.matrix[k][2] * src[2] +
                       uploadInfo.transform.matrix[k][3];
            }

            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            {
                continue;
            }

            if (std::abs(p[0]) > 1.0e7f || std::abs(p[1]) > 1.0e7f || std::abs(p[2]) > 1.0e7f)
            {
                continue;
            }

            if (!aabbInitialized)
            {
                aabbInitialized = true;
                std::copy(p, p + 3, aabbMin);
                std::copy(p, p + 3, aabbMax);
            }
            else
            {
                for (int k = 0; k < 3; k++)
                {
                    aabbMin[k] = std::min(aabbMin[k], p[k]);
                    aabbMax[k] = std::max(aabbMax[k], p[k]);
                }
            }
        }
    }

    if (uploadInfo.geomType == QR_GEOMETRY_TYPE_DYNAMIC)
    {
        if (isRecordingStatic)
        {
            throw QrException(QR_WRONG_FUNCTION_CALL, "Dynamic geometry must not be uploaded between qrStartNewScene and qrSubmitStaticGeometries calls");
        }

        const uint32_t simpleIndex = asManager->AddDynamicGeometry(frameIndex, uploadInfo);

        if (simpleIndex != UINT32_MAX)
        {
            dynamicUniqueIDToSimpleIndex[uploadInfo.uniqueID] = simpleIndex;
            return true;
        }
    }
    else
    {
        if (!isRecordingStatic)
        {
            throw QrException(QR_WRONG_FUNCTION_CALL, "Submitting static geometry is only allowed between qrStartNewScene and qrSubmitStaticGeometries calls");
        }

        const uint32_t simpleIndex = asManager->AddStaticGeometry(frameIndex, uploadInfo);

        if (simpleIndex != UINT32_MAX)
        {
            staticUniqueIDToSimpleIndex[uploadInfo.uniqueID] = simpleIndex;

            if (uploadInfo.geomType == QR_GEOMETRY_TYPE_STATIC_MOVABLE)
            {
                movableGeomIndices.push_back(simpleIndex);
            }

            return true;
        }
    }

    return false;
}

bool qray::Scene::HasAABB() const
{
    return aabbInitialized;
}

void qray::Scene::GetAABB(float outMin[3], float outMax[3]) const
{
    outMin[0] = aabbMin[0];
    outMin[1] = aabbMin[1];
    outMin[2] = aabbMin[2];
    outMax[0] = aabbMax[0];
    outMax[1] = aabbMax[1];
    outMax[2] = aabbMax[2];
}

bool qray::Scene::UpdateTransform(const QrUpdateTransformInfo &updateInfo)
{
    uint32_t simpleIndex;

    if (!TryGetStaticSimpleIndex(updateInfo.movableStaticUniqueID, &simpleIndex))
    {
        throw QrException(QR_CANT_UPDATE_TRANSFORM, "Can't find static geometry with unique ID=" + std::to_string(updateInfo.movableStaticUniqueID));
    }

    if (std::find(movableGeomIndices.begin(), movableGeomIndices.end(), simpleIndex) == movableGeomIndices.end())
    {
        throw QrException(QR_CANT_UPDATE_TRANSFORM, "Static geometry with unique ID=" + std::to_string(updateInfo.movableStaticUniqueID) + " isn't movable");
    }

    asManager->UpdateStaticMovableTransform(simpleIndex, updateInfo);

    if (!isRecordingStatic)
    {
        toResubmitMovable = true;
    }

    return true;
}

bool qray::Scene::UpdateTexCoords(const QrUpdateTexCoordsInfo &texCoordsInfo)
{
    uint32_t simpleIndex;

    if (!TryGetStaticSimpleIndex(texCoordsInfo.staticUniqueID, &simpleIndex))
    {
        throw QrException(QR_CANT_UPDATE_TEXCOORDS, "Can't find static geometry with unique ID=" + std::to_string(texCoordsInfo.staticUniqueID));
    }

    asManager->UpdateStaticTexCoords(simpleIndex, texCoordsInfo);
    return true;
}

void qray::Scene::SubmitStatic()
{
    if (!isRecordingStatic)
    {
        asManager->BeginStaticGeometry();
    }

    asManager->SubmitStaticGeometry();
    isRecordingStatic = false;

    submittedStaticInCurrentFrame = true;
}

void qray::Scene::StartNewStatic()
{
    if (isRecordingStatic)
    {
        throw QrException(QR_WRONG_FUNCTION_CALL, "qrStartNewScene must be called only once before qrSubmitStaticGeometries");
    }

    isRecordingStatic = true;
    asManager->BeginStaticGeometry();
    lightManager->Reset();

    staticUniqueIDToSimpleIndex.clear();
    movableGeomIndices.clear();
}

const std::shared_ptr<ASManager> &qray::Scene::GetASManager()
{
    return asManager;
}

const std::shared_ptr<LightManager> &qray::Scene::GetLightManager()
{
    return lightManager;
}

const std::shared_ptr<VertexPreprocessing> &qray::Scene::GetVertexPreprocessing()
{
    return vertPreproc;
}

bool qray::Scene::DoesUniqueIDExist(uint64_t uniqueID) const
{
    return
        staticUniqueIDToSimpleIndex.find(uniqueID) != staticUniqueIDToSimpleIndex.end() ||
        dynamicUniqueIDToSimpleIndex.find(uniqueID) != dynamicUniqueIDToSimpleIndex.end();
}

bool qray::Scene::DoesDynamicUniqueIDExist(uint64_t uniqueID) const
{
    return dynamicUniqueIDToSimpleIndex.find(uniqueID) != dynamicUniqueIDToSimpleIndex.end();
}

bool qray::Scene::TryGetStaticSimpleIndex(uint64_t uniqueID, uint32_t *result) const
{
    const auto f = staticUniqueIDToSimpleIndex.find(uniqueID);

    if (f != staticUniqueIDToSimpleIndex.end())
    {
        *result = f->second;
        return true;
    }

    return false;
}

void qray::Scene::UploadLight(uint32_t frameIndex, const QrDirectionalLightUploadInfo &lightInfo)
{
    lightManager->AddDirectionalLight(frameIndex, lightInfo);
}

void qray::Scene::UploadLight(uint32_t frameIndex, const QrSphericalLightUploadInfo &lightInfo)
{
    lightManager->AddSphericalLight(frameIndex, lightInfo);
}

void qray::Scene::UploadLight(uint32_t frameIndex, const QrPolygonalLightUploadInfo &lightInfo)
{
    lightManager->AddPolygonalLight(frameIndex, lightInfo);
}

void qray::Scene::UploadLight(uint32_t frameIndex, const QrTexturedAreaLightUploadInfo &lightInfo, uint32_t textureIndex)
{
    lightManager->AddTexturedAreaLight(frameIndex, lightInfo, textureIndex);
}

void qray::Scene::UploadLight(uint32_t frameIndex, const QrSpotLightUploadInfo &lightInfo)
{
    lightManager->AddSpotlight(frameIndex, lightInfo);
}
