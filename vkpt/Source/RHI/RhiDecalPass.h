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

#include <functional>
#include <string>
#include <tuple>

#include <nvrhi/vulkan.h>

#include "../Common.h"

namespace vkpt
{

class Framebuffers;

namespace rhi
{
class RhiFrameContext;
class RhiTextureTable;
}

// The A5.6 RHI module of the engine's decals: the ported `DecalManager::Draw`
// (DecalManager.cpp:133-208), recorded as its own small pass into the engine's ALBEDO image
// (FB_IMAGE_INDEX_ALBEDO, 0) on the frame's open command list right after
// `RhiRtPrimaryPass::Render` and before `RhiRtDirectPass::Render` - the legacy order
// (VulkanDevice.cpp:901 -> :904) - so the direct and the indirect pass read the decal-modified
// G-buffer. It replaces the legacy `SubmitForFrame` + `Draw` pair (VulkanDevice.cpp:881, :904),
// neither of which the RHI frame reaches (RenderThroughRhi succeeds and the legacy
// `VulkanDevice::Render` is skipped, VulkanDevice.cpp:1665-1669). It is the decal cut of
// a5c_overlay_recon.md §5 (their "A5.3", this stage's A5.6/S1).
//
// What it reproduces, exactly as the legacy pass has it:
//  - Shaders: the engine's own "VertDecal" = RsDecal.vert.spv and "FragDecal" = RsDecal.frag.spv
//    (ShaderManager.cpp:85-86), the pair DecalManager hands to `CreatePipelines`
//    (DecalManager.cpp:356-357).
//  - The interface the two shipped blobs declare (measured with spirv-dis over
//    vkpt/Build/RsDecal.{vert,frag}.spv, the HLSL-built pair the shader build deploys):
//      set 0 binding 0    globalUniform                     - ShGlobalUniform, the engine uniform;
//      set 1 binding 143  framebufSurfacePosition_Sampled   - the G-buffer image 19, the only
//                         framebuffer image either stage reads (the legacy barriers four more at
//                         DecalManager.cpp:162-172, but the shader reads none of them);
//      set 2 binding 0/1  globalTextures / globalTextures_Sampler - the shared bindless table;
//      set 3 binding 0    decalInstances                    - RWStructuredBuffer<ShDecalInstance>,
//                         its runtime array stride 80.
//    Neither blob declares a push constant or a specialization constant, so this pipeline has
//    neither - unlike every raster pass of RhiSkyPass/RhiUiPass/RhiRasterOverlayPass.
//  - One colour attachment (ALBEDO), no depth: the single attachment of the legacy render pass
//    with loadOp LOAD and storeOp STORE in GENERAL/GENERAL (DecalManager.cpp:252-299) and its
//    one-attachment framebuffer with no depth (DecalManager.cpp:301-321). NVRHI's attachment
//    loadOp is always LOAD (vulkan-graphics.cpp:80), the legacy's own value.
//  - Pipeline state: alpha blend with SRC_ALPHA / ONE_MINUS_SRC_ALPHA for colour and for alpha,
//    an add op and a colour write mask of R|G|B only (DecalManager.cpp:403-410); TRIANGLE_STRIP
//    (DecalManager.cpp:365-368), no culling, counter-clockwise front faces, fill, depth clipping
//    on (DecalManager.cpp:377-388); depth test and write off with LESS_OR_EQUAL, stencil off
//    (DecalManager.cpp:395-401); one sample; no vertex bindings and no attributes at all
//    (DecalManager.cpp:360-363).
//  - The draw: one instanced draw of a 14-vertex TRIANGLE_STRIP cube per decal
//    (DecalManager.cpp:205, :30) with no vertex or index buffer - the cube is generated from
//    `SV_VertexID % 14` in the vertex blob - with the viewport and the scissor both at the full
//    render area (DecalManager.cpp:176-177, :202-203). The viewport goes through the legacy
//    convention: NVRHI's Vulkan backend converts a D3D rectangle with a negative height
//    (vulkan-graphics.cpp:528-531), so the pass passes the inverted rectangle and the backend
//    emits the legacy's own positive-height VkViewport - the helper and the reason RhiUiPass and
//    RhiRasterOverlayPass document. ALBEDO is an engine-convention image: the primary raygen
//    writes it and the present mirrors the sample for traced frames (RhiPresent.frag.hlsl:
//    82-88).
//  - One pipeline, created lazily on the first frame that has decals and rebuilt only when the
//    ALBEDO format changes: the legacy has exactly one pipeline for every decal
//    (DecalManager.cpp:348-449), so there is no per-draw state key and no cache.
//
// The instance-buffer contract (the one thing the coordinator has to add around the call), stated
// against stream S2's DecalManager accessors (DecalManager.h:57-106, the PortalList accessor
// shape):
//  - SetInstanceBuffer() takes the host's wrap of `DecalManager::GetDeviceLocalBuffer()`: a native
//    VkBuffer wrap with byteSize = `DecalManager::GetBufferSize()` = DECAL_MAX_COUNT *
//    sizeof(ShDecalInstance) = 327,680 B (the engine buffer's full capacity, not this frame's live
//    range: a wrap is cached across frames whose counts differ, and NVRHI's copyBuffer asserts the
//    copied range fits the wrap's desc, vulkan-buffer.cpp:232-233), structStride = 80 (the blob's
//    runtime-array stride and Generated/ShaderCommonC.h:426-433) and canHaveUAVs = true, because
//    the shader declares the buffer as a writable SSBO and set 3 binds it as a
//    StructuredBuffer_UAV - NVRHI's validation device refuses either flag missing
//    (validation-device.cpp:1693-1715). The engine creates that buffer once (the constructor,
//    DecalManager.cpp:59-62) and never re-creates it, so the wrap is stable; it must outlive this
//    pass, and the set over it is rebuilt when the pointer the host passes changes.
//  - The coordinator fills the device-local buffer itself: the legacy `SubmitForFrame`
//    (DecalManager.cpp:121-131) copies `DecalManager::GetCopySize()` bytes - `GetDecalCount() *
//    sizeof(ShDecalInstance)`, zero when the frame has no decals - from
//    `DecalManager::GetStagingBuffer(frameIndex)` into `GetDeviceLocalBuffer()`, and that legacy
//    copy never runs under `rhiframe`. The host has to wrap the slot's staging buffer as a copy
//    source (AutoBuffer's staging is per-slot, DecalManager.cpp:59-62) and record the same ranged
//    copyBuffer on the RHI list before this pass. The module does not copy anything itself.
//  - 'decalCount' is `DecalManager::GetDecalCount()`, the legacy's own per-frame count
//    (`decalCount`, DecalManager.h:125), the value `Draw` passes as the instance count and
//    `SubmitForFrame` copies, reset by `PrepareForFrame` at the start of every frame
//    (DecalManager.cpp:90-93, called from VulkanDevice.cpp:106 in every mode). The pass takes the
//    count as an argument and never reads DecalManager itself. Stream S2's `ResetUploads()` is
//    not needed for correctness; if the host does call it, it must not run between the game's
//    uploads and this Render and not before the count and the handles have been captured: it
//    zeroes the count the draw and the host's copy both read (DecalManager.h:85-105).
//
// The ALBEDO state discipline, and why it is not optional:
//  - The engine leaves every framebuffer image in VK_IMAGE_LAYOUT_GENERAL - NVRHI's
//    UnorderedAccess - and a native wrap keeps no state between command lists
//    (RhiTextureSource.h). The primary pass wrote ALBEDO and image 19 through its own wraps, so
//    the physical layouts are GENERAL when this pass starts. Render announces UnorderedAccess
//    for its own wraps of both (beginTrackingTextureState), lets the framebuffer use move ALBEDO
//    to the render-target layout and the set-1 binding move image 19 to the sampled layout, and
//    restores both to UnorderedAccess at the end. The direct and the indirect pass announce the
//    same state on their own wraps next (RhiRtDirectPass.cpp:758-769, :809-820); their
//    transitions name GENERAL as the old layout, which is only true because the restore happens
//    here.
//
// What the host has to do around it:
//  - call Create() with the device, the frame context, the shared texture table and the shader
//    folder; SetInstanceBuffer() once the engine's instance-buffer wrap exists; then Render()
//    per frame on the frame context's open list of that slot, after the primary pass and before
//    the direct pass, with the frame's decal count and the same static uniform wrap the traced
//    passes bind;
//  - call ReleaseTargets() before Framebuffers::PrepareForSize destroys the engine framebuffer
//    images, at the same point as the other passes' ReleaseTargets.
//
// The pass is a no-op until Create succeeded and while an input is missing (no framebuffers, no
// engine image, no instance buffer, no uniform); a frame whose count is zero is the legacy's own
// first statement (DecalManager.cpp:135-138) and returns before any wrap, state announcement or
// draw, so it is quiet and creates nothing. Every other early return is quiet after the first
// warning. It is not thread-safe: Render uses the per-slot target of the frameIndex it is given,
// which is the engine's single-threaded per-slot frame model (RhiFrameContext).
//
// Gate note: no code in this repository calls `rgUploadDecal` (the only occurrences of the name
// are the API declaration and its forwarder, vkpt.h:610, vkpt.cpp:194-197), so on the shipped
// game the count is zero every frame and this pass records nothing - the gate is structural
// (creation clean, no new sets bound, no new VUID) unless a game-side uploader is added, exactly
// like the never-uploaded lens flares (a5c_overlay_recon.md §1.2.4).
//
// A5.0 note: the module stays self-contained, as its siblings do - the legacy-viewport helper
// and the wrap/announce/restore discipline are its own copy, not a refactor of the sky pass, and
// no raster-draw state machinery is duplicated because this pipeline has no per-draw state at
// all.
class RhiDecalPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    RhiDecalPass();
    ~RhiDecalPass();

    RhiDecalPass(const RhiDecalPass &other) = delete;
    RhiDecalPass(RhiDecalPass &&other) noexcept = delete;
    RhiDecalPass &operator=(const RhiDecalPass &other) = delete;
    RhiDecalPass &operator=(RhiDecalPass &&other) noexcept = delete;

    // 'pDevice' is the RHI device; 'pFrameContext' is the host's RHI frame model
    // (RHI/RhiFrameContext.h), which owns the retire queues every replaced wrap, framebuffer,
    // set and pipeline goes through; 'pTextureTable' is the host's shared bindless table
    // (RHI/RhiTextureTable.h), whose layout becomes descriptor set 2 and whose table is bound
    // with it. None of the three is owned, all have to outlive this object, and a null or
    // unusable one makes Create fail. 'pShaderFolderPath' is the folder ShaderManager loads the
    // engine blobs from, with the trailing separator; RsDecal.vert.spv and RsDecal.frag.spv are
    // read from it. Returns false and leaves the pass unusable if a shader, a binding layout or
    // the shared table cannot be used; the host logs that through 'pfnPrint'.
    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                rhi::RhiTextureTable *pTextureTable,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    // The shader's set 3 binding 0, the wrap of `DecalManager`'s device-local instance buffer
    // (`DecalManager::GetDeviceLocalBuffer()`, DecalManager.h:70; the engine buffer itself is
    // DecalManager.cpp:59-62). The pass neither owns nor fills the buffer: the caller has to
    // record the frame's staging -> device copy on the RHI list before Render and keep both
    // buffers valid until the submission finishes. Render is a no-op until the pointer is set.
    // The set over the wrap is rebuilt when the pointer changes; a pointer whose desc lacks
    // structStride or canHaveUAVs would be refused when that set is created
    // (validation-device.cpp:1693-1715).
    void SetInstanceBuffer(nvrhi::IBuffer *pInstanceBuffer);

    // One call per frame, on the frame context's open command list of 'frameIndex', after
    // `RhiRtPrimaryPass::Render` of the same slot and before `RhiRtDirectPass::Render` (the
    // legacy order VulkanDevice.cpp:901 -> :904). It resolves the engine images, (re)wraps them
    // and (re)builds the per-slot framebuffer and sets when an image or the size changed,
    // announces the states, records the one draw and restores the states.
    //
    // Argument sources, all of them the host's:
    //  - 'pCommandList': the frame context's open list of 'frameIndex'
    //    (`RhiFrameContext::GetCommandList(frameIndex)`);
    //  - 'pFramebuffers': the engine's framebuffer registry (`VulkanDevice`'s Framebuffers
    //    object, the pointer `SkyFrameInputs::framebuffers` carries). The pass resolves
    //    FB_IMAGE_INDEX_ALBEDO (0) as the colour attachment and FB_IMAGE_INDEX_SURFACE_POSITION
    //    (19) as the shader's set-1 sampled view for the slot, so an engine framebuffer
    //    re-create is picked up without a second Create;
    //  - 'width'/'height': the render resolution (`RenderResolutionHelper::Width/Height`,
    //    `SkyFrameInputs::width/height`) - the extent the two images are created at and the
    //    values the uniform carries as renderWidth/renderHeight, which are what the legacy
    //    viewport and scissor are built from (DecalManager.cpp:176-177);
    //  - 'pUniformBuffer': the static wrap of `GlobalUniform::GetBuffer()`, the same wrap the
    //    frame skeleton writes and the traced passes bind. A null, volatile or non-constant
    //    buffer skips the call;
    //  - 'decalCount': `DecalManager::GetDecalCount()` (DecalManager.h:83), the legacy's
    //    per-frame count (see the class comment). Zero is the legacy's own first statement
    //    (DecalManager.cpp:135-138): the call records nothing at all and creates nothing.
    //
    // No-op when the pass is not created, the frame index is out of range, the framebuffers or
    // the size are missing, the instance buffer or the uniform has not been set, or an engine
    // image cannot be wrapped.
    void Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                const Framebuffers *pFramebuffers,
                uint32_t width,
                uint32_t height,
                nvrhi::IBuffer *pUniformBuffer,
                uint32_t decalCount);

    // Drops the per-slot wraps, the framebuffer and the sets over them. The caller has to call it
    // before the engine destroys its framebuffer images (the Framebuffers::PrepareForSize path),
    // next to the other passes' ReleaseTargets(). The next Render re-reads the engine's accessors
    // and re-wraps, so the pass survives a resize without a second Create; the pipeline and the
    // three binding layouts are format-independent and stay. Releases go through the frame
    // context's retire queue when one exists; the destructor drops directly, after a device idle.
    void ReleaseTargets();

private:
    struct Target
    {
        // The engine images the slot currently wraps. A change means the engine re-created its
        // framebuffers (or the swap permutation flipped) and the wraps, the framebuffer and the
        // sets over them have to follow.
        VkImage albedoImage = VK_NULL_HANDLE;
        VkImage surfacePositionImage = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;

        // ALBEDO as the one colour attachment and the surface position as the shader's sampled
        // view. Both are swapped engine images (Bindings[0]/Swapped[0] = 0/1,
        // Bindings[19]/Swapped[19] = 19/20, Generated/ShaderCommonCFramebuf.cpp), so the two
        // slots' wraps are not shared.
        nvrhi::TextureHandle albedoTexture;
        nvrhi::TextureHandle surfacePositionTexture;

        // The one-attachment framebuffer of the legacy render pass: no depth, no stencil.
        nvrhi::FramebufferHandle framebuffer;

        // Set 1 over the surface-position wrap; rebuilt with the wraps.
        nvrhi::BindingSetHandle framebuffersSet;

        // Set 0 (the engine uniform) and set 3 (the instance buffer). Both follow the pointers
        // the host passes, because either wrap can be replaced between frames; the set holds the
        // reference that keeps the wrap alive, hence the replaced one is retired, never dropped.
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::BindingSetHandle uniformSet;
        nvrhi::IBuffer *instanceBuffer = nullptr;
        nvrhi::BindingSetHandle instanceSet;

        // False until the two wraps, the framebuffer and the framebuffers set exist; the uniform
        // and the instance sets are rebuilt separately by UpdateBufferSets and are not part of
        // this flag.
        bool valid = false;
    };

    bool LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result);

    // Resolves the slot's two engine images, rebuilds the wraps, the framebuffer, the
    // framebuffers set and - when the colour format changed - the pipeline, then announces the
    // two images' states. Returns false when the call must be skipped.
    bool PrepareTarget(nvrhi::ICommandList *pCommandList, uint32_t frameIndex, Target &target,
                       const Framebuffers &framebuffers, uint32_t width, uint32_t height);
    bool CreateTargetObjects(Target &target,
                             const std::tuple<VkImage, VkImageView, VkFormat> &albedo,
                             const std::tuple<VkImage, VkImageView, VkFormat> &surfacePosition,
                             uint32_t frameIndex, uint32_t width, uint32_t height);
    bool UpdateBufferSets(Target &target, nvrhi::IBuffer *pUniformBuffer);

    void ReleaseTarget(Target &target);
    void ReleasePipeline();

    nvrhi::GraphicsPipelineHandle CreatePipeline();

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;

    // The two blobs: the engine's decal pair, with no specialization.
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;

    // The pipeline's layouts 0, 1 and 3, in the shader's set order: the engine's global uniform
    // (raw binding 0), the partial framebuffers layout (one Texture_SRV for the shader's raw
    // binding 143) and the instance buffer (one StructuredBuffer_UAV at raw binding 0). Set 2 is
    // the shared texture table and comes from 'textureTable'.
    nvrhi::BindingLayoutHandle uniformLayout;
    nvrhi::BindingLayoutHandle framebuffersLayout;
    nvrhi::BindingLayoutHandle instancesLayout;

    // The host's table and frame model; not owned, both outlive this object. The table provides
    // the bindless set and the first-use tracking of the engine textures it wrapped; the frame
    // context owns the retire queues.
    rhi::RhiTextureTable *textureTable = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;

    // The instance buffer SetInstanceBuffer received; not owned, the caller keeps it alive.
    nvrhi::IBuffer *instanceBuffer = nullptr;

    // One pipeline for every decal: the legacy has a single fixed-state pipeline
    // (DecalManager.cpp:348-449), so there is no state key and no cache. Built lazily on the
    // first target creation and replaced when the colour format changes, because a pipeline is
    // tied to its framebuffer's format; the format is only known from the ALBEDO wrap.
    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhi::Format pipelineColorFormat = nvrhi::Format::UNKNOWN;

    // One entry per engine frame slot: ALBEDO and the surface position are both swapped engine
    // images, so each slot has its own wraps, framebuffer and sets.
    Target targets[MAX_FRAMES_IN_FLIGHT];

    bool warnedMissingFramebuffers = false;
    bool warnedMissingInstanceBuffer = false;
    bool warnedMissingUniform = false;
    bool warnedFailedPipeline = false;

    bool created = false;
};

}
