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

#pragma once

#include "AutoBuffer.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"
#include "ShaderManager.h"
#include "TextureManager.h"

namespace vkpt
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
    void Upload(uint32_t frameIndex, const RgDecalUploadInfo &uploadInfo,
                const std::shared_ptr<TextureManager> &textureManager);

    // Read-only views for the RHI layer's decal-instance copy: the engine's own
    // DecalManager::SubmitForFrame runs only from the legacy VulkanDevice::Render
    // (VulkanDevice.cpp:881) and is bypassed under `rhiframe`, so the RHI pass that draws the
    // decals (RHI/RhiDecalPass.h) records the copy itself: GetCopySize() bytes from the frame
    // slot's GetStagingBuffer(frameIndex) into GetDeviceLocalBuffer(), then draws GetDecalCount()
    // instances - exactly the ranges the legacy SubmitForFrame/Draw pair uses
    // (DecalManager.cpp:121-131, :205). The game stages decals through
    // VulkanDevice::UploadDecal -> DecalManager::Upload (VulkanDevice.cpp:1918-1926); the count is
    // built per frame by PrepareForFrame (DecalManager.cpp:90-93), called from
    // VulkanDevice::BeginFrame (VulkanDevice.cpp:106). Both buffers stay valid while this object
    // lives.
    // (Non-const for the staging buffer only: AutoBuffer's accessor is not const.)
    VkBuffer GetStagingBuffer(uint32_t frameIndex);
    VkBuffer GetDeviceLocalBuffer() const;
    // Capacity of the two buffers above (= DECAL_MAX_COUNT * sizeof(ShDecalInstance)); the byte
    // size the NVRHI wraps must be created with, because a wrap is cached across frames whose live
    // counts differ. This is not the size of the copy - see GetCopySize().
    VkDeviceSize GetBufferSize() const;
    // This frame's live byte range (GetDecalCount() * sizeof(ShDecalInstance), 0 when the frame
    // has no decals): the size the RHI copy must use, exactly the range the legacy SubmitForFrame
    // copies (DecalManager.cpp:130). The RHI pass must skip the copy when it is 0, as the legacy
    // path does (DecalManager.cpp:123-126).
    VkDeviceSize GetCopySize() const;
    // The number of this frame's live instances: the instance count for the RHI draw, mirroring
    // the legacy vkCmdDraw (DecalManager.cpp:205); the RHI pass must skip the draw when it is 0,
    // as the legacy Draw does (DecalManager.cpp:135-138).
    uint32_t GetDecalCount() const;

    // Clears the staged instances (decalCount = 0). Unlike PortalList, DecalManager has no
    // "already uploaded" bookkeeping: Upload only appends (DecalManager.cpp:104-118), a duplicate
    // cannot be rejected, and the legacy SubmitForFrame resets nothing after its copy. The count
    // is already cleared once per frame on both paths: PrepareForFrame runs from
    // VulkanDevice::BeginFrame (VulkanDevice.cpp:106), after the slot's fence wait
    // (VulkanDevice.cpp:41-49) and before the frame's Upload calls. So the RHI frame does not need
    // this call for correctness; it is provided for symmetry with PortalList::ResetUploads and for
    // a frame flow that stops resetting at BeginFrame. If it is called on the RHI path:
    //  - only after the frame's Upload calls, never between them: clearing the count between
    //    Uploads defeats the DECAL_MAX_COUNT capacity check and lets a later Upload write past the
    //    fixed-size buffer (DecalManager.cpp:98-102);
    //  - only after the RHI list has captured GetDecalCount()/GetCopySize() (or after the RHI pass
    //    has read them): both are 0 afterwards, so a copy recorded later is a no-op and the draw
    //    is empty - and the legacy fallback of a failed skeleton render
    //    (VulkanDevice.cpp:1531-1535) would then run the legacy Draw with the frame's decals
    //    already dropped;
    //  - it touches no buffer memory, only the count, so once the count and the handles are
    //    captured it is safe at any point before the recorded copy is submitted: the staging bytes
    //    the copy reads are untouched, and the next Upload that could overwrite them for this slot
    //    runs after the slot's submission has completed (the property AutoBuffer's per-slot
    //    staging exists for).
    void ResetUploads();

    void OnShaderReload(const ShaderManager *shaderManager) override;
    void OnFramebuffersSizeChange(const ResolutionState &resolutionState) override;

private:
    std::unique_ptr<AutoBuffer> instanceBuffer;
    uint32_t decalCount;
};

}