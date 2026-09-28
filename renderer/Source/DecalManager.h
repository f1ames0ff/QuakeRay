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

#pragma once

#include "AutoBuffer.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"
#include "ShaderManager.h"
#include "TextureManager.h"

namespace qray
{

class DecalManager : public IShaderDependency, public IFramebuffersDependency
{
public:
    DecalManager(VkDevice device,
                 std::shared_ptr<MemoryAllocator> &allocator,
                 const std::shared_ptr<ShaderManager> &shaderManager,
                 const std::shared_ptr<GlobalUniform> &uniform,
                 std::shared_ptr<Framebuffers> _storageFramebuffers,
                 const std::shared_ptr<TextureManager> &textureManager);
    ~DecalManager() override;

    DecalManager(const DecalManager &other) = delete;
    DecalManager(DecalManager &&other) noexcept = delete;
    DecalManager &operator=(const DecalManager &other) = delete;
    DecalManager &operator=(DecalManager &&other) noexcept = delete;

    void PrepareForFrame(uint32_t frameIndex);
    void Upload(uint32_t frameIndex, const QrDecalUploadInfo &uploadInfo,
                const std::shared_ptr<TextureManager> &textureManager);

    VkBuffer GetStagingBuffer(uint32_t frameIndex);
    VkBuffer GetDeviceLocalBuffer() const;
    VkDeviceSize GetBufferSize() const;
    VkDeviceSize GetCopySize() const;
    uint32_t GetDecalCount() const;

    void ResetUploads();

    void OnShaderReload(const ShaderManager *shaderManager) override;
    void OnFramebuffersSizeChange(const ResolutionState &resolutionState) override;

private:
    std::unique_ptr<AutoBuffer> instanceBuffer;
    uint32_t decalCount;
};

}
