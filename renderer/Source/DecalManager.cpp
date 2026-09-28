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

#include "DecalManager.h"

#include "Matrix.h"
#include "Generated/ShaderCommonC.h"

namespace
{
    constexpr uint32_t DECAL_MAX_COUNT = 4096;
}

qray::DecalManager::DecalManager(
    VkDevice _device,
    std::shared_ptr<MemoryAllocator> &_allocator,
    const std::shared_ptr<ShaderManager> &_shaderManager,
    const std::shared_ptr<GlobalUniform> &_uniform,
    std::shared_ptr<Framebuffers> _storageFramebuffers,
    const std::shared_ptr<TextureManager> &_textureManager)
    : decalCount(0)
{
    instanceBuffer = std::make_unique<AutoBuffer>(_allocator);
    instanceBuffer->Create(
        DECAL_MAX_COUNT * sizeof(ShDecalInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "Decal instance buffer");
}

qray::DecalManager::~DecalManager() = default;

void qray::DecalManager::PrepareForFrame(uint32_t frameIndex)
{
    decalCount = 0;
}

void qray::DecalManager::Upload(uint32_t frameIndex, const QrDecalUploadInfo &uploadInfo,
                                const std::shared_ptr<TextureManager> &textureManager)
{
    if (decalCount >= DECAL_MAX_COUNT)
    {
        assert(0);
        return;
    }

    const MaterialTextures materialTextures = textureManager->GetMaterialTextures(uploadInfo.material);

    ShDecalInstance *pFrameInstances = static_cast<ShDecalInstance *>(instanceBuffer->GetMapped(frameIndex));

    ShDecalInstance &decalInstance = pFrameInstances[decalCount];
    decalInstance = {};
    decalInstance.textureAlbedoAlpha      = materialTextures.indices[MATERIAL_ALBEDO_ALPHA_INDEX];
    decalInstance.textureRougnessMetallic = materialTextures.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];
    decalInstance.textureNormals          = materialTextures.indices[MATERIAL_NORMAL_INDEX];
    Matrix::ToMat4Transposed(decalInstance.transform, uploadInfo.transform);

    decalCount++;
}

VkBuffer qray::DecalManager::GetStagingBuffer(uint32_t frameIndex)
{
    return instanceBuffer->GetStaging(frameIndex);
}

VkBuffer qray::DecalManager::GetDeviceLocalBuffer() const
{
    return instanceBuffer->GetDeviceLocal();
}

VkDeviceSize qray::DecalManager::GetBufferSize() const
{
    return instanceBuffer->GetSize();
}

VkDeviceSize qray::DecalManager::GetCopySize() const
{
    return static_cast<VkDeviceSize>(decalCount) * sizeof(ShDecalInstance);
}

uint32_t qray::DecalManager::GetDecalCount() const
{
    return decalCount;
}

void qray::DecalManager::ResetUploads()
{
    decalCount = 0;
}

void qray::DecalManager::OnShaderReload(const ShaderManager *shaderManager)
{
}

void qray::DecalManager::OnFramebuffersSizeChange(const ResolutionState &resolutionState)
{
}
