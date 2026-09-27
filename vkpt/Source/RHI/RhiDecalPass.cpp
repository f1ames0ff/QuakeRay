/*
* Copyright (c) 2026 Sultim Tsyrendashiev
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include "RhiDecalPass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiTextureSource.h"
#include "RhiTextureTable.h"

#include <cstddef>
#include <string>
#include <tuple>
#include <utility>

#include "../Framebuffers.h"
#include "../Generated/ShaderCommonC.h"

using namespace vkpt;

namespace
{

// The engine's blob names, the ones ShaderManager knows "VertDecal"/"FragDecal" as
// (ShaderManager.cpp:85-86), which is the pair DecalManager hands to CreatePipelines
// (DecalManager.cpp:356-357).
const char *const VERTEX_SHADER_FILE_NAME = "RsDecal.vert.spv";
const char *const PIXEL_SHADER_FILE_NAME  = "RsDecal.frag.spv";

// The legacy cube: 14 vertices in a TRIANGLE_STRIP (DecalManager.cpp:30-31), generated in the
// vertex blob from `SV_VertexID % 14`, so there is no vertex or index buffer to bind
// (DecalManager.cpp:360-363).
constexpr uint32_t CUBE_VERTEX_COUNT = 14;

// The shader's set-1 sampled view of the surface position, at the raw binding the engine's
// generated arrays carry. Deriving it from `ShFramebuffers_Sampled_Bindings` instead of spelling
// 143 keeps the layout item and the set item from drifting from the shader build, exactly as
// RhiRtDirectPass derives its framebuffer bindings; measured, the shipped frag blob decorates
// `framebufSurfacePosition_Sampled` with DescriptorSet 1, Binding 143.
uint32_t SurfacePositionBinding()
{
    return ShFramebuffers_Sampled_Bindings[FB_IMAGE_INDEX_SURFACE_POSITION];
}

// The instance stride is the shader's runtime-array stride (the shipped blobs' ArrayStride 80)
// and the engine's own element (DecalManager.cpp:117, :130, :151); the generated struct is the
// authority, asserted here so a change of ShDecalInstance cannot silently break the layout and
// the copy the host records. The offsets are the ones the blob's OpMemberDecorate carries.
static_assert(sizeof(ShDecalInstance) == 80, "the decal instance stride has to stay the shader's 80 bytes");
static_assert(offsetof(ShDecalInstance, transform) == 0);
static_assert(offsetof(ShDecalInstance, textureAlbedoAlpha) == 64);
static_assert(offsetof(ShDecalInstance, textureRougnessMetallic) == 68);
static_assert(offsetof(ShDecalInstance, textureNormals) == 72);
static_assert(BINDING_DECAL_INSTANCES == 0, "the decal instance buffer has to stay the shader's set-3 binding 0");

// The legacy viewport of DecalManager::Draw, as an NVRHI viewport that makes the Vulkan backend
// emit the legacy's own VkViewport. The legacy `vkCmdSetViewport` takes (x, y, w, +h)
// (DecalManager.cpp:176, :203), while `VKViewportWithDXCoords` (vulkan-graphics.cpp:528-531)
// computes `(minX, maxY, maxX - minX, -(maxY - minY))`: with minY = y + h and maxY = y the
// emitted viewport is (x, y, w, +h) again. The inverted rectangle is the point of the helper; the
// class comment of the header explains why this engine-convention image needs the legacy
// convention - the same helper RhiUiPass and RhiRasterOverlayPass carry.
nvrhi::Viewport ToLegacyViewport(const VkViewport &v)
{
    return nvrhi::Viewport(v.x, v.x + v.width, v.y + v.height, v.y, v.minDepth, v.maxDepth);
}

void LogMessage(const RhiDecalPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

}

RhiDecalPass::RhiDecalPass() = default;

RhiDecalPass::~RhiDecalPass()
{
    if (device != nullptr)
    {
        // The wraps reference engine images, and the framebuffer, the sets, the pipeline and the
        // layouts reference the device; the host destroys the pass while it can still idle the
        // device (VulkanDevice does that before the skeleton as well), so nothing has to go
        // through a retire queue here.
        device->waitForIdle();
    }

    // The pipeline references its shaders, so it goes first.
    pipeline = nullptr;

    for (Target &target : targets)
    {
        target.framebuffersSet = nullptr;
        target.uniformSet = nullptr;
        target.instanceSet = nullptr;
        target.framebuffer = nullptr;
        target.albedoTexture = nullptr;
        target.surfacePositionTexture = nullptr;
        target.albedoImage = VK_NULL_HANDLE;
        target.surfacePositionImage = VK_NULL_HANDLE;
        target.width = 0;
        target.height = 0;
        target.uniformBuffer = nullptr;
        target.instanceBuffer = nullptr;
        target.valid = false;
    }

    instancesLayout = nullptr;
    framebuffersLayout = nullptr;
    uniformLayout = nullptr;
    pixelShader = nullptr;
    vertexShader = nullptr;
}

bool RhiDecalPass::Create(nvrhi::IDevice *pDevice,
                          rhi::RhiFrameContext *pFrameContext,
                          rhi::RhiTextureTable *pTextureTable,
                          const char *pShaderFolderPath,
                          PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    device = pDevice;
    print = std::move(pfnPrint);
    shaderFolderPath = pShaderFolderPath != nullptr ? pShaderFolderPath : "";
    frameContext = pFrameContext;
    textureTable = pTextureTable;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the decal pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the decal pass needs the frame context of the RHI layer");
        return false;
    }

    if (textureTable == nullptr || !textureTable->IsCreated() ||
        textureTable->GetLayout() == nullptr || textureTable->GetTable() == nullptr)
    {
        LogMessage(print, "Warning: RHI: the decal pass needs the shared RHI texture table (set 2)");
        return false;
    }

    if (!LoadShader(VERTEX_SHADER_FILE_NAME, nvrhi::ShaderType::Vertex, vertexShader) ||
        !LoadShader(PIXEL_SHADER_FILE_NAME, nvrhi::ShaderType::Pixel, pixelShader))
    {
        return false;
    }

    // Set 0: the engine's global uniform at raw binding 0. The constant-buffer offset has to be
    // 0 for the item to land there: NVRHI's default is 256 (nvrhi.h:2063-2066), and the item's
    // slot 0 plus the offset is the raw binding the shader spells (RhiPipeline.h).
    {
        const nvrhi::BindingLayoutItem layoutItems[] =
        {
            nvrhi::BindingLayoutItem::ConstantBuffer(0),
        };
        const nvrhi::VulkanBindingOffsets offsets =
            nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0);

        uniformLayout = rhi::createBindingLayout(device, layoutItems, "RhiDecal uniform", &offsets);
    }

    // Set 1: the partial framebuffers layout, in the same sense RhiSkyPass's world set 4 and
    // RhiRasterOverlayPass's set 4 are partial - one item for the one image either decal stage
    // reads. The shader-resource offset is 0 (its default), so the item's slot is the raw
    // binding itself.
    {
        const nvrhi::BindingLayoutItem layoutItems[] =
        {
            nvrhi::BindingLayoutItem::Texture_SRV(SurfacePositionBinding()),
        };
        const nvrhi::VulkanBindingOffsets offsets =
            nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0);

        framebuffersLayout = rhi::createBindingLayout(device, layoutItems,
                                                      "RhiDecal framebuffers (framebufSurfacePosition_Sampled)",
                                                      &offsets);
    }

    // Set 3: the decal instances at raw binding 0. The shader declares the buffer as a writable
    // SSBO (RWStructuredBuffer in its HLSL), so the item is a StructuredBuffer_UAV; the
    // unordered-access offset has to be 0 for the item to land at raw 0, because NVRHI's default
    // is 384.
    {
        const nvrhi::BindingLayoutItem layoutItems[] =
        {
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(BINDING_DECAL_INSTANCES),
        };
        const nvrhi::VulkanBindingOffsets offsets =
            nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0);

        instancesLayout = rhi::createBindingLayout(device, layoutItems, "RhiDecal instances", &offsets);
    }

    if (uniformLayout == nullptr || framebuffersLayout == nullptr || instancesLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a decal pass binding layout");
        return false;
    }

    created = true;
    return true;
}

void RhiDecalPass::SetInstanceBuffer(nvrhi::IBuffer *pInstanceBuffer)
{
    instanceBuffer = pInstanceBuffer;
}

void RhiDecalPass::Render(nvrhi::ICommandList *pCommandList,
                          uint32_t frameIndex,
                          const Framebuffers *pFramebuffers,
                          uint32_t width,
                          uint32_t height,
                          nvrhi::IBuffer *pUniformBuffer,
                          uint32_t decalCount)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    // The legacy Draw's own first statement (DecalManager.cpp:135-138): an empty count returns
    // before the buffer barrier, before the render pass and before the draw. Mirroring it is
    // what keeps a frame without decals quiet and free of any created object - and it must come
    // before every warning below, because this game never uploads a decal.
    if (decalCount == 0)
    {
        return;
    }

    if (pFramebuffers == nullptr || width == 0 || height == 0)
    {
        return;
    }

    if (instanceBuffer == nullptr)
    {
        if (!warnedMissingInstanceBuffer)
        {
            warnedMissingInstanceBuffer = true;
            LogMessage(print, "Warning: RHI: the decal pass has no instance buffer (set 3), the decals are skipped");
        }
        return;
    }

    if (pUniformBuffer == nullptr || pUniformBuffer->getDesc().isVolatile ||
        !pUniformBuffer->getDesc().isConstantBuffer)
    {
        // A volatile buffer would become a dynamic-offset binding (nvrhi.h:2311-2318), which the
        // static ConstantBuffer layout item rejects, and the validation device refuses a
        // ConstantBuffer binding on a desc without isConstantBuffer
        // (validation-device.cpp:1717-1723). The skeleton's wrap of the engine uniform is
        // neither.
        if (!warnedMissingUniform)
        {
            warnedMissingUniform = true;
            LogMessage(print, "Warning: RHI: the decal pass needs the static wrap of the global uniform (set 0)");
        }
        return;
    }

    Target &target = targets[frameIndex];

    if (!PrepareTarget(pCommandList, frameIndex, target, *pFramebuffers, width, height))
    {
        return;
    }

    if (!UpdateBufferSets(target, pUniformBuffer))
    {
        return;
    }

    // The shader samples the bindless table (set 2), so the engine textures the table wrapped
    // since the last frame need their first-use state declared in the first list that binds the
    // table (RhiTextureSource.h); this may be that list.
    textureTable->TrackPendingTextures(pCommandList);

    // The legacy viewport is {0, 0, renderWidth, renderHeight, 0, 1} and the scissor is the same
    // full render area (DecalManager.cpp:176-177, :202-203), on the uniform's renderWidth/Height
    // - which are the 'width'/'height' this call received.
    const VkViewport legacyViewport = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };

    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = target.framebuffer;
    state.viewport.addViewport(ToLegacyViewport(legacyViewport));
    state.viewport.addScissorRect(nvrhi::Rect(0, int(width), 0, int(height)));
    // Sets 0..3 in the layout order the pipeline was built with: the uniform, the surface
    // position, the shared texture table and the instance buffer. No vertex buffer is bound: the
    // pipeline has no vertex input at all (DecalManager.cpp:360-363) and the cube comes from
    // SV_VertexID.
    state.addBindingSet(target.uniformSet);
    state.addBindingSet(target.framebuffersSet);
    state.addBindingSet(textureTable->GetTable());
    state.addBindingSet(target.instanceSet);

    pCommandList->setGraphicsState(state);

    // The legacy draw: 14 vertices of a triangle strip, one instance per decal (DecalManager.cpp:
    // 198). The push constants the legacy pipeline does not have stay unset - the blobs declare
    // no push-constant range.
    nvrhi::DrawArguments args;
    args.vertexCount = CUBE_VERTEX_COUNT;
    args.instanceCount = decalCount;
    pCommandList->draw(args);

    // The framebuffer use left ALBEDO in the render-target layout and the set-1 binding left the
    // surface position sampled, while the engine's own descriptors declare VK_IMAGE_LAYOUT_GENERAL
    // for every framebuffer image - NVRHI's UnorderedAccess. The direct and the indirect pass
    // announce exactly that for their own wraps right after this call and their samples then name
    // GENERAL as the old layout, which is only true because both images are moved back here, at
    // the end of the list that used them.
    pCommandList->setTextureState(target.albedoTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->setTextureState(target.surfacePositionTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

bool RhiDecalPass::PrepareTarget(nvrhi::ICommandList *pCommandList, uint32_t frameIndex,
                                 Target &target, const Framebuffers &framebuffers,
                                 uint32_t width, uint32_t height)
{
    // The two engine images of this slot, exactly the ones the legacy pass touches: ALBEDO as
    // its only colour attachment (DecalManager.cpp:307, :255) and the surface position as the
    // one framebuffer image the fragment blob samples. `GetImageHandles` resolves the slot's own
    // image inside (Framebuffers.cpp:33-53), so the pass follows whatever the accessors answer -
    // an engine framebuffer re-create is picked up without a second Create.
    const std::tuple<VkImage, VkImageView, VkFormat> albedo =
        framebuffers.GetImageHandles(FB_IMAGE_INDEX_ALBEDO, frameIndex);
    const std::tuple<VkImage, VkImageView, VkFormat> surfacePosition =
        framebuffers.GetImageHandles(FB_IMAGE_INDEX_SURFACE_POSITION, frameIndex);

    if (std::get<0>(albedo) == VK_NULL_HANDLE || std::get<1>(albedo) == VK_NULL_HANDLE ||
        std::get<2>(albedo) == VK_FORMAT_UNDEFINED ||
        std::get<0>(surfacePosition) == VK_NULL_HANDLE || std::get<1>(surfacePosition) == VK_NULL_HANDLE ||
        std::get<2>(surfacePosition) == VK_FORMAT_UNDEFINED)
    {
        if (!warnedMissingFramebuffers)
        {
            warnedMissingFramebuffers = true;
            LogMessage(print, "Warning: RHI: the decal pass got no engine framebuffer image for frame " + std::to_string(frameIndex));
        }
        target.valid = false;
        return false;
    }

    // Nothing to do while the slot still wraps the same images at the same size: replacing the
    // wraps, the framebuffer and the sets every frame would create them per frame for nothing.
    // The state announcement below still has to happen, because a render-target wrap keeps no
    // state between command lists (RhiTextureSource.h:109-124).
    const bool targetChanged =
        !target.valid ||
        target.albedoImage != std::get<0>(albedo) ||
        target.surfacePositionImage != std::get<0>(surfacePosition) ||
        target.width != width || target.height != height;

    if (targetChanged)
    {
        ReleaseTarget(target);

        if (!CreateTargetObjects(target, albedo, surfacePosition, frameIndex, width, height))
        {
            ReleaseTarget(target);
            return false;
        }
    }

    // The engine leaves every framebuffer image in VK_IMAGE_LAYOUT_GENERAL - Framebuffers
    // creates each image and immediately barriers it there (Framebuffers.cpp:764-768) and
    // declares that layout on its own descriptors (:796, :804) - which NVRHI names
    // UnorderedAccess. The primary pass wrote both images through its own wraps and left them in
    // exactly that state, so announcing it lets the first framebuffer use and the set-1 binding
    // emit the correct transitions from a truthful old layout.
    pCommandList->beginTrackingTextureState(target.albedoTexture, nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->beginTrackingTextureState(target.surfacePositionTexture, nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);

    return true;
}

bool RhiDecalPass::CreateTargetObjects(Target &target,
                                       const std::tuple<VkImage, VkImageView, VkFormat> &albedo,
                                       const std::tuple<VkImage, VkImageView, VkFormat> &surfacePosition,
                                       uint32_t frameIndex, uint32_t width, uint32_t height)
{
    const std::string frameTag = std::to_string(frameIndex);

    const auto imageHandle = [](const std::tuple<VkImage, VkImageView, VkFormat> &handles)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(std::get<0>(handles)));
    };
    const auto viewHandle = [](const std::tuple<VkImage, VkImageView, VkFormat> &handles)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(std::get<1>(handles)));
    };

    // ALBEDO is a colour attachment here, so it takes wrapEngineRenderTarget; the surface
    // position is bound as a sampled view of an image that rests in GENERAL, which is
    // wrapEngineStorageImage's case (the helper RhiRtDirectPass wraps the same G-buffer image
    // with). Both helpers document their state contracts in RhiTextureSource.h.
    target.albedoTexture = rhi::wrapEngineRenderTarget(
        device, imageHandle(albedo), viewHandle(albedo), std::get<2>(albedo), width, height,
        "RhiDecal ALBEDO frame " + frameTag);
    target.surfacePositionTexture = rhi::wrapEngineStorageImage(
        device, imageHandle(surfacePosition), viewHandle(surfacePosition), std::get<2>(surfacePosition),
        width, height, "RhiDecal surface position frame " + frameTag);

    if (target.albedoTexture == nullptr || target.surfacePositionTexture == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to wrap an engine image of the decal pass");
        return false;
    }

    // The one-attachment framebuffer of the legacy render pass (DecalManager.cpp:252-299,
    // :264-284): the colour target only, no depth. NVRHI's loadOp is always LOAD
    // (vulkan-graphics.cpp:80), which is the legacy loadOp too, and its storeOp is STORE.
    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.addColorAttachment(target.albedoTexture);

    target.framebuffer = device->createFramebuffer(framebufferDesc);
    if (target.framebuffer == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the decal framebuffer");
        return false;
    }

    // Set 1 over the surface-position wrap; its only item is the one binding the fragment blob
    // declares.
    {
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(SurfacePositionBinding(), target.surfacePositionTexture));

        target.framebuffersSet = device->createBindingSet(desc, framebuffersLayout);

        if (target.framebuffersSet == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the decal framebuffers binding set");
            return false;
        }
    }

    // The pipeline is built against the framebuffer's colour format, so a change means the
    // cached one belongs to the wrong framebuffer and has to be replaced. ALBEDO is
    // VK_FORMAT_B10G11R11_UFLOAT_PACK32 (ShaderCommonCFramebuf.cpp:9), which the wrapped texture
    // reports as nvrhi::Format::R11G11B10_FLOAT; the wrap's own format is authoritative because
    // it is the one NVRHI mapped the engine format to.
    const nvrhi::Format colorFormat = target.albedoTexture->getDesc().format;
    if (pipelineColorFormat != colorFormat)
    {
        ReleasePipeline();
        pipelineColorFormat = colorFormat;
    }

    if (pipeline == nullptr)
    {
        pipeline = CreatePipeline();
        if (pipeline == nullptr)
        {
            if (!warnedFailedPipeline)
            {
                warnedFailedPipeline = true;
                LogMessage(print, "Warning: RHI: failed to create the decal pipeline, the decals are skipped");
            }
            return false;
        }
    }

    target.albedoImage = std::get<0>(albedo);
    target.surfacePositionImage = std::get<0>(surfacePosition);
    target.width = width;
    target.height = height;
    target.valid = true;

    return true;
}

bool RhiDecalPass::UpdateBufferSets(Target &target, nvrhi::IBuffer *pUniformBuffer)
{
    // Set 0: the engine uniform. The key is the pointer the host passed, because the wrap can be
    // re-created; the replaced set goes through the retire queue, never a plain drop.
    if (target.uniformSet == nullptr || target.uniformBuffer != pUniformBuffer)
    {
        if (target.uniformSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(target.uniformSet);
        }

        target.uniformSet = nullptr;
        target.uniformBuffer = nullptr;

        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, pUniformBuffer));

        target.uniformSet = device->createBindingSet(desc, uniformLayout);
        if (target.uniformSet == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the decal uniform binding set");
            return false;
        }

        target.uniformBuffer = pUniformBuffer;
    }

    // Set 3: the instance buffer SetInstanceBuffer received. Same handle-change rule.
    if (target.instanceSet == nullptr || target.instanceBuffer != instanceBuffer)
    {
        if (target.instanceSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(target.instanceSet);
        }

        target.instanceSet = nullptr;
        target.instanceBuffer = nullptr;

        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(BINDING_DECAL_INSTANCES, instanceBuffer));

        target.instanceSet = device->createBindingSet(desc, instancesLayout);
        if (target.instanceSet == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the decal instance binding set");
            return false;
        }

        target.instanceBuffer = instanceBuffer;
    }

    return true;
}

void RhiDecalPass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseTarget(target);
    }
}

void RhiDecalPass::ReleaseTarget(Target &target)
{
    // Anything a recorded list may still reference has to go through the frame context's retire
    // queue: the framebuffer references a wrap of an engine image the GPU may still be using, and
    // the sets reference that wrap and the host's buffers. The queue takes its reference now, so
    // the handles below can be cleared immediately.
    if (frameContext != nullptr)
    {
        if (target.framebuffersSet != nullptr)
        {
            frameContext->Retire(target.framebuffersSet);
        }
        if (target.uniformSet != nullptr)
        {
            frameContext->Retire(target.uniformSet);
        }
        if (target.instanceSet != nullptr)
        {
            frameContext->Retire(target.instanceSet);
        }
        if (target.framebuffer != nullptr)
        {
            frameContext->Retire(target.framebuffer);
        }
    }

    target.framebuffersSet = nullptr;
    target.uniformSet = nullptr;
    target.instanceSet = nullptr;
    target.framebuffer = nullptr;
    target.albedoTexture = nullptr;
    target.surfacePositionTexture = nullptr;
    target.albedoImage = VK_NULL_HANDLE;
    target.surfacePositionImage = VK_NULL_HANDLE;
    target.width = 0;
    target.height = 0;
    target.uniformBuffer = nullptr;
    target.instanceBuffer = nullptr;
    target.valid = false;
}

void RhiDecalPass::ReleasePipeline()
{
    if (frameContext != nullptr && pipeline != nullptr)
    {
        frameContext->Retire(pipeline);
    }

    pipeline = nullptr;
}

nvrhi::GraphicsPipelineHandle RhiDecalPass::CreatePipeline()
{
    assert(pipelineColorFormat != nvrhi::Format::UNKNOWN);

    // No input layout is set: the legacy pipeline has zero vertex bindings and zero attributes
    // (DecalManager.cpp:360-363), and the backend tolerates a null one (vulkan-graphics.cpp:
    // 300-306), while the cube comes from `SV_VertexID` in the vertex blob.
    nvrhi::GraphicsPipelineDesc desc;
    desc.setVertexShader(vertexShader);
    desc.setPixelShader(pixelShader);
    desc.primType = nvrhi::PrimitiveType::TriangleStrip;
    // Fill, no culling, counter-clockwise front faces and depth clipping on, the legacy
    // rasterization state (DecalManager.cpp:377-388; its depthClampEnable = FALSE means clipping
    // stays enabled).
    desc.renderState.rasterState.setFillSolid();
    desc.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
    desc.renderState.rasterState.setFrontCounterClockwise(true);
    desc.renderState.rasterState.setDepthClipEnable(true);
    // LESS_OR_EQUAL, the test and the write both off, stencil off (DecalManager.cpp:395-401).
    desc.renderState.depthStencilState.setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
    desc.renderState.depthStencilState.setDepthTestEnable(false);
    desc.renderState.depthStencilState.setDepthWriteEnable(false);
    desc.renderState.depthStencilState.setStencilEnable(false);

    // The legacy blend attachment, with the alpha factors mirroring the colour ones and an add
    // op; the colour write mask keeps R, G and B and drops A (DecalManager.cpp:403-410). The
    // legacy pass has exactly one colour attachment, so only target 0 is declared.
    nvrhi::BlendState::RenderTarget blendTarget;
    blendTarget.setBlendEnable(true)
               .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
               .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
               .setBlendOp(nvrhi::BlendOp::Add)
               .setSrcBlendAlpha(nvrhi::BlendFactor::SrcAlpha)
               .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
               .setBlendOpAlpha(nvrhi::BlendOp::Add)
               .setColorWriteMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue);

    desc.renderState.blendState.setRenderTarget(0, blendTarget);

    // The four layouts in the shader's own set order: the uniform (0), the partial framebuffers
    // layout (1), the shared texture table (2) and the instances (3). NVRHI's legacy binding
    // mode keeps the order the layouts are added in as the descriptor set numbers
    // (vulkan-resource-bindings.cpp:1090-1099).
    desc.addBindingLayout(uniformLayout);
    desc.addBindingLayout(framebuffersLayout);
    desc.addBindingLayout(textureTable->GetLayout());
    desc.addBindingLayout(instancesLayout);

    nvrhi::FramebufferInfo framebufferInfo;
    framebufferInfo.addColorFormat(pipelineColorFormat);
    framebufferInfo.setSampleCount(1);

    nvrhi::GraphicsPipelineHandle result =
        rhi::createGraphicsPipeline(device, desc, framebufferInfo, "RhiDecal pipeline");

    if (result == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the decal pipeline");
    }

    return result;
}

bool RhiDecalPass::LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result)
{
    const std::string path = shaderFolderPath + pFileName;

    // The helper stays silent about a missing or unreadable blob, so that this class keeps its
    // own warning and its 'created == false' path (RhiPipeline.h).
    result = rhi::loadShader(device, path, type, pFileName);
    if (result == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the decal pass shader \"" + path + "\"");
        return false;
    }

    return true;
}
