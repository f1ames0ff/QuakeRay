// Copyright (c) 2022 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "DecalManager.h"

#include "Matrix.h"
#include "Generated/ShaderCommonC.h"


constexpr uint32_t DECAL_MAX_COUNT = 4096;


vkpt::DecalManager::DecalManager(
    VkDevice _device,
    std::shared_ptr<MemoryAllocator> &_allocator,
    const std::shared_ptr<ShaderManager> &_shaderManager,
    const std::shared_ptr<GlobalUniform> &_uniform,
    std::shared_ptr<Framebuffers> _storageFramebuffers,
    const std::shared_ptr<TextureManager> &_textureManager)
:
    decalCount(0)
{
    instanceBuffer = std::make_unique<AutoBuffer>(_allocator);
    // The RHI layer wraps this buffer (and its staging slots) through NVRHI, and NVRHI's
    // native-wrap path queries the device address unconditionally when the device has
    // bufferDeviceAddress enabled (vulkan-buffer.cpp:215-220); without the usage bit that query
    // trips VUID-VkBufferDeviceAddressInfo-buffer-02601, the same class the A4.1 fix removed for
    // the collector and staging buffers and the A5.3 fix for the portal buffer. AutoBuffer
    // propagates the bit to the staging buffer.
    instanceBuffer->Create(
        DECAL_MAX_COUNT * sizeof(ShDecalInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "Decal instance buffer");
}

vkpt::DecalManager::~DecalManager() = default;

void vkpt::DecalManager::PrepareForFrame(uint32_t frameIndex)
{
    decalCount = 0;
}

void vkpt::DecalManager::Upload(uint32_t frameIndex, const RgDecalUploadInfo &uploadInfo,
                                 const std::shared_ptr<TextureManager> &textureManager)
{
    if (decalCount >= DECAL_MAX_COUNT)
    {
        assert(0);
        return;
    }

    const uint32_t decalIndex = decalCount;
    decalCount++;

    const MaterialTextures mat = textureManager->GetMaterialTextures(uploadInfo.material);

    ShDecalInstance instance = {};
    instance.textureAlbedoAlpha      = mat.indices[MATERIAL_ALBEDO_ALPHA_INDEX];
    instance.textureRougnessMetallic = mat.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];
    instance.textureNormals          = mat.indices[MATERIAL_NORMAL_INDEX];
    Matrix::ToMat4Transposed(instance.transform, uploadInfo.transform);

    {
        ShDecalInstance *dst = (ShDecalInstance *)instanceBuffer->GetMapped(frameIndex);
        memcpy(&dst[decalIndex], &instance, sizeof(ShDecalInstance));
    }
}

VkBuffer vkpt::DecalManager::GetStagingBuffer(uint32_t frameIndex)
{
    return instanceBuffer->GetStaging(frameIndex);
}

VkBuffer vkpt::DecalManager::GetDeviceLocalBuffer() const
{
    return instanceBuffer->GetDeviceLocal();
}

VkDeviceSize vkpt::DecalManager::GetBufferSize() const
{
    return instanceBuffer->GetSize();
}

VkDeviceSize vkpt::DecalManager::GetCopySize() const
{
    return static_cast<VkDeviceSize>(decalCount) * sizeof(ShDecalInstance);
}

uint32_t vkpt::DecalManager::GetDecalCount() const
{
    return decalCount;
}

void vkpt::DecalManager::ResetUploads()
{
    decalCount = 0;
}

void vkpt::DecalManager::OnShaderReload(const ShaderManager *shaderManager)
{
}

void vkpt::DecalManager::OnFramebuffersSizeChange(const ResolutionState &resolutionState)
{
}

