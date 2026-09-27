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

#include "RhiRasterSkyPass.h"

#include "RhiPipeline.h"
#include "RhiResources.h"
#include "RhiTextureTable.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <utility>

#include "../Matrix.h"

using namespace vkpt;

namespace
{

// The engine's blobs by the names ShaderManager knows them: "VertDefault" is RsRasterizer.vert.spv
// (ShaderManager.cpp:62), "FragSky" is RsSky.frag.spv (ShaderManager.cpp:60). RasterPass passes
// exactly that pair to its sky RasterizerPipelines (RasterPass.cpp:64-65); the cube pass uses the
// multiview vertex half instead, which this module cannot (see the header).
const char *const VERTEX_SHADER_FILE_NAME = "RsRasterizer.vert.spv";
const char *const PIXEL_SHADER_FILE_NAME  = "RsSky.frag.spv";

// The legacy push-constant range is 88 bytes (Rasterizer.cpp:66, :485-489) while both raster sky
// blobs declare a 92/96-byte block: RsSky.frag's last member, emissionMultiplier at offset 88, is
// declared but never read, and the legacy host never writes it (HLSL/RsSky.frag.hlsl:30-36). The
// module mirrors the legacy value, so both renderers push the same bytes and the shader-visible
// prefix stays what the legacy path produces.
constexpr uint32_t RASTERIZED_PUSH_CONSTANT_SIZE = 88;

// Both stages use SpecId 0 for their single constant: RsRasterizer.vert declares
// applyVertexColorGamma, RsSky.frag declares alphaTest, each 4 bytes and written as a uint32
// (RasterizerPipelines.cpp:315-343).
constexpr uint32_t SPEC_CONSTANT_APPLY_VERTEX_COLOR_GAMMA = 0;
constexpr uint32_t SPEC_CONSTANT_ALPHA_TEST = 0;

// The legacy cube depth's format (RenderCubemap.cpp:36, CUBEMAP_DEPTH_FORMAT) and the NVRHI name of
// the same VkFormat.
constexpr nvrhi::Format DEPTH_FORMAT = nvrhi::Format::D16;

// The state key of RasterizerPipelines::ConvertToStateFlags, bit for bit (RasterizerPipelines.cpp:
// 29-55). The numbers are repeated instead of shared because that function is private to the legacy
// translation unit; keeping them identical is what makes a DrawInfo land in the same state in both
// renderers.
constexpr uint32_t PIPELINE_STATE_MASK_IS_ALPHA_TEST          = 1 << 0;
constexpr uint32_t PIPELINE_STATE_MASK_BLEND_ENABLE           = 1 << 1;
constexpr uint32_t PIPELINE_STATE_MASK_DEPTH_TEST_ENABLE      = 1 << 2;
constexpr uint32_t PIPELINE_STATE_MASK_DEPTH_WRITE_ENABLE     = 1 << 3;
constexpr uint32_t PIPELINE_STATE_MASK_IS_LINES               = 1 << 4;
constexpr uint32_t PS_SRC_OFFSET                              = 5;

constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_ONE                 = 1 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_ZERO                = 2 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_SRC_COLOR           = 3 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_SRC_COLOR = 4 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_DST_COLOR           = 5 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_DST_COLOR = 6 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_SRC_ALPHA           = 7 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_SRC_ALPHA = 8 << PS_SRC_OFFSET;
constexpr uint32_t PIPELINE_STATE_MASK_BLEND_SRC                      = 15 << PS_SRC_OFFSET;
constexpr uint32_t PS_DST_OFFSET                                      = 4 + PS_SRC_OFFSET;

constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_ONE                 = 1 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_ZERO                = 2 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_SRC_COLOR           = 3 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_SRC_COLOR = 4 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_DST_COLOR           = 5 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_DST_COLOR = 6 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_SRC_ALPHA           = 7 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_SRC_ALPHA = 8 << PS_DST_OFFSET;
constexpr uint32_t PIPELINE_STATE_MASK_BLEND_DST                      = 15 << PS_DST_OFFSET;

// The vertex spec constant carries no state of a DrawInfo: RasterizerPipelines gets one value per
// object and bakes it into every pipeline it creates (RasterizerPipelines.cpp:96, :230, :323),
// while this pass is told the value per Render call. The legacy key uses bits 0..12, so bit 13 is
// free and the flag joins the key instead of forcing a cache flush when it changes.
constexpr uint32_t PIPELINE_STATE_VALUE_VERTEX_COLOR_GAMMA = 1 << 13;

// The mirror of RasterizerPipelines::ConvertToStateFlags (RasterizerPipelines.cpp:57-113), byte for
// byte, including the quirk that an unknown blend factor zeroes the whole key.
uint32_t ConvertToStateFlags(RgRasterizedGeometryStateFlags pipelineState, RgBlendFactor blendFuncSrc, RgBlendFactor blendFuncDst)
{
    uint32_t r = 0;

    if (pipelineState & RG_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE)
    {
        r |= PIPELINE_STATE_MASK_BLEND_ENABLE;

        switch (blendFuncSrc)
        {
            case RG_BLEND_FACTOR_ONE:                   r |= PIPELINE_STATE_VALUE_BLEND_SRC_ONE;            break;
            case RG_BLEND_FACTOR_ZERO:                  r |= PIPELINE_STATE_VALUE_BLEND_SRC_ZERO;           break;
            case RG_BLEND_FACTOR_SRC_COLOR:             r |= PIPELINE_STATE_VALUE_BLEND_SRC_SRC_COLOR;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:   r |= PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_SRC_COLOR;  break;
            case RG_BLEND_FACTOR_DST_COLOR:             r |= PIPELINE_STATE_VALUE_BLEND_SRC_DST_COLOR;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_DST_COLOR:   r |= PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_DST_COLOR;  break;
            case RG_BLEND_FACTOR_SRC_ALPHA:             r |= PIPELINE_STATE_VALUE_BLEND_SRC_SRC_ALPHA;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:   r |= PIPELINE_STATE_VALUE_BLEND_SRC_ONE_MINUS_SRC_ALPHA;  break;
            default: assert(0); r = 0;
        }

        switch (blendFuncDst)
        {
            case RG_BLEND_FACTOR_ONE:                   r |= PIPELINE_STATE_VALUE_BLEND_DST_ONE;            break;
            case RG_BLEND_FACTOR_ZERO:                  r |= PIPELINE_STATE_VALUE_BLEND_DST_ZERO;           break;
            case RG_BLEND_FACTOR_SRC_COLOR:             r |= PIPELINE_STATE_VALUE_BLEND_DST_SRC_COLOR;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:   r |= PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_SRC_COLOR;  break;
            case RG_BLEND_FACTOR_DST_COLOR:             r |= PIPELINE_STATE_VALUE_BLEND_DST_DST_COLOR;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_DST_COLOR:   r |= PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_DST_COLOR;  break;
            case RG_BLEND_FACTOR_SRC_ALPHA:             r |= PIPELINE_STATE_VALUE_BLEND_DST_SRC_ALPHA;      break;
            case RG_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:   r |= PIPELINE_STATE_VALUE_BLEND_DST_ONE_MINUS_SRC_ALPHA;  break;
            default: assert(0); r = 0;
        }
    }

    if (pipelineState & RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST)
    {
        r |= PIPELINE_STATE_MASK_DEPTH_TEST_ENABLE;
    }

    if (pipelineState & RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE)
    {
        r |= PIPELINE_STATE_MASK_DEPTH_WRITE_ENABLE;
    }

    if (pipelineState & RG_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST)
    {
        r |= PIPELINE_STATE_MASK_IS_LINES;
    }

    if (pipelineState & RG_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST)
    {
        r |= PIPELINE_STATE_MASK_IS_ALPHA_TEST;
    }

    return r;
}

// The blend factor the state key encodes in its four-bit field, as the NVRHI name of the same
// factor (RasterizerPipelines::ConvertBlendFactorToVk, RasterizerPipelines.cpp:198-212).
nvrhi::BlendFactor DecodeBlendFactor(uint32_t stateFlags, uint32_t offset)
{
    switch ((stateFlags >> offset) & 15)
    {
        case 1:  return nvrhi::BlendFactor::One;
        case 2:  return nvrhi::BlendFactor::Zero;
        case 3:  return nvrhi::BlendFactor::SrcColor;
        case 4:  return nvrhi::BlendFactor::InvSrcColor; // OneMinusSrcColor
        case 5:  return nvrhi::BlendFactor::DstColor;
        case 6:  return nvrhi::BlendFactor::InvDstColor; // OneMinusDstColor
        case 7:  return nvrhi::BlendFactor::SrcAlpha;
        case 8:  return nvrhi::BlendFactor::InvSrcAlpha; // OneMinusSrcAlpha
        // Code 0 is what the legacy key holds when blending is off: ConvertToStateFlags only fills
        // these two fields inside its RG_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE branch
        // (RasterizerPipelines.cpp:57-113), so an opaque draw - the sky is one - reaches this
        // decoder with zeros. The factor is ignored by setBlendEnable(false) either way.
        case 0:  return nvrhi::BlendFactor::One;
        default: assert(0); return nvrhi::BlendFactor::One;
    }
}

// The legacy viewport of RenderCubemap's static state, as an NVRHI viewport that makes the Vulkan
// backend emit the legacy's own VkViewport. The legacy `vkCmdSetViewport` takes (x, y, w, +h)
// (RenderCubemap.cpp:460-468), while `VKViewportWithDXCoords` (vulkan-graphics.cpp:528-531)
// computes `(minX, maxY, maxX - minX, -(maxY - minY))`: with minY = y + h and maxY = y the emitted
// viewport is (x, y, w, +h) again. The inverted rectangle is the point of the helper; the class
// comment of the header explains why this cube needs the legacy convention - the same helper
// RhiDecalPass, RhiUiPass and RhiRasterOverlayPass carry.
nvrhi::Viewport ToLegacyViewport(const VkViewport &v)
{
    return nvrhi::Viewport(v.x, v.x + v.width, v.y + v.height, v.y, v.minDepth, v.maxDepth);
}

// The per-draw block, byte for byte the legacy RasterizedPushConst (Rasterizer.cpp:33-65): the
// model-view-projection, the color, and the two texture indices. The type is repeated because
// RasterizedPushConst is private to the Rasterizer translation unit; the offsets below are the ones
// Rasterizer.cpp asserts, and the fragment half matches the members RsSky.frag reads at 64/80/84.
//
// The difference to RhiSkyPass's block is where the view-projection comes from: the legacy cube
// pass pushes the bare model (RenderCubemap.cpp:43-55, a 84-byte block) and reads the per-face
// matrix from the uniform in its multiview vertex shader (RsRasterizerMultiview.vert:55). This
// module uses the non-multiview RsRasterizer.vert, which takes the full product in the push block,
// so the per-face matrix is folded in here. The product is the same one RasterizedPushConst builds
// from a DrawInfo's viewProj/defaultViewProj (Rasterizer.cpp:40-58): the transposed model times the
// view-projection, which the shader then multiplies the position with.
struct RasterSkyPushConstants
{
    float    vp[16];
    float    c[4];
    uint32_t t;
    uint32_t e;

    explicit RasterSkyPushConstants(const RasterizedDataCollector::DrawInfo &info, const float *faceViewProj)
    {
        float model[16];
        Matrix::ToMat4Transposed(model, info.transform);

        Matrix::Multiply(vp, model, faceViewProj);

        memcpy(c, info.color.Get(), 4 * sizeof(float));
        t = info.textureIndex;
        e = info.emissionTextureIndex;
    }
};

static_assert(offsetof(RasterSkyPushConstants, vp) == 0);
static_assert(offsetof(RasterSkyPushConstants, c) == 64);
static_assert(offsetof(RasterSkyPushConstants, t) == 80);
static_assert(offsetof(RasterSkyPushConstants, e) == 84);
static_assert(sizeof(RasterSkyPushConstants) == 88);

void LogMessage(const RhiRasterSkyPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

// The numbers the input layout of Create is built from, taken from the collector's own struct and
// asserted so that a change of RgVertex cannot silently break the RHI pipeline. They are the
// offsets the legacy VkVertexInputAttributeDescriptions use (RasterizedDataCollector.cpp:31-54).
static_assert(offsetof(RgVertex, position) == 0);
static_assert(offsetof(RgVertex, texCoord) == 32);
static_assert(offsetof(RgVertex, packedColor) == 56);
static_assert(sizeof(RgVertex) == 80);

}

RhiRasterSkyPass::RhiRasterSkyPass() = default;

RhiRasterSkyPass::~RhiRasterSkyPass()
{
    if (device != nullptr)
    {
        // The framebuffers reference the borrowed cube and the module's own depth; the host
        // destroys the pass while it can still idle the device (VulkanDevice does that before the
        // skeleton as well), so nothing has to go through a retire queue here.
        device->waitForIdle();
    }

    // Pipelines reference their (specialized) shaders, so they go first.
    ReleaseResources();

    vertexBuffer = nullptr;
    indexBuffer = nullptr;
    visibleCube = nullptr;
    textureTable = nullptr;
}

bool RhiRasterSkyPass::Create(nvrhi::IDevice *pDevice,
                              rhi::RhiTextureTable *pTextureTable,
                              const char *pShaderFolderPath,
                              nvrhi::ITexture *pVisibleCube,
                              PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    device = pDevice;
    print = std::move(pfnPrint);
    shaderFolderPath = pShaderFolderPath != nullptr ? pShaderFolderPath : "";
    textureTable = pTextureTable;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the raster sky pass needs an RHI device");
        return false;
    }

    if (textureTable == nullptr || !textureTable->IsCreated() ||
        textureTable->GetLayout() == nullptr || textureTable->GetTable() == nullptr)
    {
        LogMessage(print, "Warning: RHI: the raster sky pass needs the shared texture table of the RHI layer (set 0)");
        return false;
    }

    if (pVisibleCube == nullptr)
    {
        LogMessage(print, "Warning: RHI: the raster sky pass needs the procedural sky pass's visible cube");
        return false;
    }

    // The module's own depth and its 66 framebuffers are built over the exact shape of the A5.4
    // cube (1024-square, 6 faces, 11 mips, RGBA16_FLOAT, render target), so a cube of another
    // shape is refused here instead of being drawn into with mismatched subresources. Taking the
    // pipeline's colour format from the cube keeps the framebuffer info and the attachment from
    // drifting; the A5.4 cube makes it nvrhi::Format::RGBA16_FLOAT, the legacy CUBEMAP_FORMAT
    // (VK_FORMAT_R16G16B16A16_SFLOAT, RenderCubemap.cpp:35).
    const nvrhi::TextureDesc &cubeDesc = pVisibleCube->getDesc();
    const bool cubeShapeOk =
        cubeDesc.dimension == nvrhi::TextureDimension::TextureCube &&
        cubeDesc.arraySize == CUBEMAP_FACE_COUNT &&
        cubeDesc.mipLevels == CUBEMAP_MIP_LEVELS &&
        cubeDesc.width == CUBEMAP_SIZE &&
        cubeDesc.height == CUBEMAP_SIZE &&
        cubeDesc.format == nvrhi::Format::RGBA16_FLOAT &&
        cubeDesc.isRenderTarget;

    if (!cubeShapeOk)
    {
        LogMessage(print, "Warning: RHI: the raster sky pass needs a 1024x1024, "
                          + std::to_string(CUBEMAP_FACE_COUNT) + "-face, "
                          + std::to_string(CUBEMAP_MIP_LEVELS) + "-mip RGBA16_FLOAT render-target cube "
                          "(RhiProceduralSkyPass::GetCubemapTexture()), the raster sky is skipped");
        return false;
    }

    visibleCube = pVisibleCube;
    pipelineColorFormat = cubeDesc.format;

    if (!LoadShader(VERTEX_SHADER_FILE_NAME, nvrhi::ShaderType::Vertex, vertexShader) ||
        !LoadShader(PIXEL_SHADER_FILE_NAME, nvrhi::ShaderType::Pixel, pixelShader))
    {
        ReleaseResources();
        return false;
    }

    // The RgVertex input layout the collector feeds: one binding at slot 0 with the collector's
    // stride and three attributes, whose order is their location (the Vulkan backend numbers the
    // attributes by their position in the array, vulkan-shader.cpp:176-197). The offsets are taken
    // from the same struct RasterizedDataCollector::GetVertexLayout uses, so the RHI pipeline and
    // the legacy one cannot drift (RgVertex, vkpt.h:349-361).
    const uint32_t vertexStride = RasterizedDataCollector::GetVertexStride();

    const nvrhi::VertexAttributeDesc vertexAttributes[] =
    {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setBufferIndex(0)
            .setOffset(offsetof(RgVertex, position))
            .setElementStride(vertexStride),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setBufferIndex(0)
            .setOffset(offsetof(RgVertex, packedColor))
            .setElementStride(vertexStride),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setBufferIndex(0)
            .setOffset(offsetof(RgVertex, texCoord))
            .setElementStride(vertexStride),
    };

    // The vertex shader argument is ignored by the Vulkan backend (vulkan-shader.cpp:136-138) and
    // passed for the D3D backends NVRHI supports.
    inputLayout = device->createInputLayout(vertexAttributes, uint32_t(std::size(vertexAttributes)), vertexShader);
    if (inputLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the raster sky pass input layout");
        ReleaseResources();
        return false;
    }

    // The pipeline's second layout, and its only job is the push-constant block: no descriptors, so
    // no binding set has to be created or bound for it. The texture table stays the first layout and
    // therefore descriptor set 0, which is the shader's DESC_SET_TEXTURES.
    {
        const nvrhi::BindingLayoutItem layoutItems[] =
        {
            nvrhi::BindingLayoutItem::PushConstants(0, RASTERIZED_PUSH_CONSTANT_SIZE),
        };

        pushConstantLayout = rhi::createBindingLayout(device, layoutItems, "RhiRasterSky push constants");
        if (pushConstantLayout == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the raster sky pass push-constant layout");
            ReleaseResources();
            return false;
        }
    }

    // The pass's own depth image: the legacy cube depth's format (RenderCubemap.cpp:36) with one
    // mip per colour mip, because a (mip, face) framebuffer's depth attachment has to match the
    // colour attachment's mip. It is a 2D array, not a cube: the framebuffers address it per face
    // and the NVRHI validation device refuses arraySize != 1 on a plain Texture2D
    // (validation-device.cpp:174-187). initialState/keepInitialState make NVRHI start and end every
    // list in the depth-attachment layout, the layout the engine's own depth descriptor declares;
    // the first use of a subresource is the explicit clear, which the backend does through CopyDest.
    {
        nvrhi::TextureDesc depthDesc;
        depthDesc.width = CUBEMAP_SIZE;
        depthDesc.height = CUBEMAP_SIZE;
        depthDesc.format = DEPTH_FORMAT;
        depthDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        depthDesc.mipLevels = CUBEMAP_MIP_LEVELS;
        depthDesc.arraySize = CUBEMAP_FACE_COUNT;
        depthDesc.sampleCount = 1;
        depthDesc.isShaderResource = false;
        depthDesc.isRenderTarget = true;
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;

        depthTexture = rhi::createTexture(device, depthDesc, "RhiRasterSky depth");
        if (depthTexture == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the raster sky depth image");
            ReleaseResources();
            return false;
        }
    }

    // One framebuffer per (mip, face), the same subresource pair as its colour and depth attachment.
    // The colour attachment is one cube slice; the Vulkan backend turns that into a 2D view over
    // (mip, layer) (vulkan-graphics.cpp:28-54, :56-84; vulkan-texture.cpp:287-349), so the cube
    // needs no second view and no re-wrap. Nothing here depends on the frame index or the frame
    // resolution: the cube is fixed and never re-created, so Render allocates nothing.
    for (uint32_t mip = 0; mip < CUBEMAP_MIP_LEVELS; mip++)
    {
        for (uint32_t face = 0; face < CUBEMAP_FACE_COUNT; face++)
        {
            const nvrhi::TextureSubresourceSet subresources(mip, 1, face, 1);

            nvrhi::FramebufferDesc framebufferDesc;
            framebufferDesc.addColorAttachment(visibleCube, subresources);
            framebufferDesc.setDepthAttachment(depthTexture, subresources);

            nvrhi::FramebufferHandle &framebuffer = framebuffers[mip * CUBEMAP_FACE_COUNT + face];
            framebuffer = device->createFramebuffer(framebufferDesc);

            if (framebuffer == nullptr)
            {
                LogMessage(print, "Warning: RHI: failed to create the raster sky framebuffers");
                ReleaseResources();
                return false;
            }
        }
    }

    created = true;
    return true;
}

void RhiRasterSkyPass::SetGeometryBuffers(nvrhi::IBuffer *pVertexBuffer, nvrhi::IBuffer *pIndexBuffer)
{
    vertexBuffer = pVertexBuffer;
    indexBuffer = pIndexBuffer;
}

void RhiRasterSkyPass::Render(nvrhi::ICommandList *pCommandList,
                              const RasterizedDataCollector::DrawInfo *pDraws,
                              uint32_t drawCount,
                              const float faceViewProj[6][16],
                              bool applyVertexColorGamma)
{
    // An empty list is the legacy Draw's own first statement (RenderCubemap.cpp:153-158): it returns
    // before the render pass and before any draw, so a frame without sky geometry stays quiet and
    // records nothing - no state announcement, no clear, no allocation.
    if (!created || pCommandList == nullptr || pDraws == nullptr || drawCount == 0)
    {
        return;
    }

    if (vertexBuffer == nullptr || indexBuffer == nullptr)
    {
        if (!warnedMissingGeometry)
        {
            warnedMissingGeometry = true;
            LogMessage(print, "Warning: RHI: the raster sky pass has no geometry buffers, the raster sky is skipped");
        }
        return;
    }

    if (faceViewProj == nullptr)
    {
        assert(0);
        return;
    }

    // The engine textures the table wrapped since the last frame need their first-use state
    // declared in the first list that binds the table (RhiTextureSource.h); this may be that list.
    textureTable->TrackPendingTextures(pCommandList);

    for (uint32_t mip = 0; mip < CUBEMAP_MIP_LEVELS; mip++)
    {
        // The mip's face size. The legacy rasters mip 0 at 1024 and blits the chain
        // (RenderCubemap.cpp:230, :233-330); this module rasters every mip at its own size instead,
        // because NVRHI has no blit. The per-face view-projections do not change with the mip: they
        // map to NDC and the viewport scales them to this level.
        const uint32_t size = std::max(CUBEMAP_SIZE >> mip, 1u);

        // The legacy cube pass's static viewport and scissor (RenderCubemap.cpp:460-468, :479): the
        // face's whole area, depth [0, 1], and the same area for the scissor of every draw. The
        // rectangle goes through the legacy-viewport helper, so the emitted VkViewport is the
        // legacy's own positive-height one.
        VkViewport legacyViewport = {};
        legacyViewport.x = 0.0f;
        legacyViewport.y = 0.0f;
        legacyViewport.width = float(size);
        legacyViewport.height = float(size);
        legacyViewport.minDepth = 0.0f;
        legacyViewport.maxDepth = 1.0f;

        const nvrhi::Viewport viewport = ToLegacyViewport(legacyViewport);
        const nvrhi::Rect scissor(0, int(size), 0, int(size));

        for (uint32_t face = 0; face < CUBEMAP_FACE_COUNT; face++)
        {
            nvrhi::IFramebuffer *framebuffer = framebuffers[mip * CUBEMAP_FACE_COUNT + face];

            // NVRHI's attachment loadOp is always LOAD (vulkan-graphics.cpp:80), so the clear the
            // legacy cube render pass does on its depth attachment (loadOp CLEAR,
            // RenderCubemap.cpp:395) is an explicit command. It runs once per (mip, face)
            // framebuffer, before the face's first draw and outside any render pass; the colour is
            // never cleared, exactly like the legacy's DONT_CARE colour load (RenderCubemap.cpp:385):
            // the game's skybox path draws a full-face opaque quad per face (Sky_DrawSkyBox,
            // Quake/gl_sky.c:1036-1091).
            const nvrhi::TextureSubresourceSet depthSubresource(mip, 1, face, 1);
            pCommandList->clearDepthStencilTexture(depthTexture, depthSubresource, true, 1.0f, false, 0);

            for (uint32_t i = 0; i < drawCount; i++)
            {
                const RasterizedDataCollector::DrawInfo &info = pDraws[i];

                const uint32_t stateFlags =
                    ConvertToStateFlags(info.pipelineState, info.blendFuncSrc, info.blendFuncDst);

                nvrhi::IGraphicsPipeline *pipeline = GetPipeline(stateFlags, applyVertexColorGamma);
                if (pipeline == nullptr)
                {
                    if (!warnedFailedPipeline)
                    {
                        warnedFailedPipeline = true;
                        LogMessage(print, "Warning: RHI: failed to create a raster sky pipeline, the raster sky is incomplete");
                    }
                    return;
                }

                nvrhi::GraphicsState state;
                state.pipeline = pipeline;
                state.framebuffer = framebuffer;
                state.viewport.addViewport(viewport);
                state.viewport.addScissorRect(scissor);
                // Set 0 is the bindless texture table, the pipeline's first layout; the second
                // layout only carries the push constants and has no descriptors to bind.
                state.addBindingSet(textureTable->GetTable());
                state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(vertexBuffer).setSlot(0).setOffset(0));
                state.setIndexBuffer(nvrhi::IndexBufferBinding()
                                         .setBuffer(indexBuffer)
                                         .setFormat(nvrhi::Format::R32_UINT)
                                         .setOffset(0));

                pCommandList->setGraphicsState(state);

                // The framebuffer use above emits the RenderTarget / DepthWrite barriers for this
                // (mip, face); the next framebuffer's use emits its own, because NVRHI re-applies
                // the attachment states when the framebuffer changes.

                // After the state: changing the state invalidates push constants (nvrhi.h:3430-3432),
                // and the block is rebuilt per draw exactly as Rasterizer::Draw does
                // (Rasterizer.cpp:399-409).
                const RasterSkyPushConstants push(info, faceViewProj[face]);
                pCommandList->setPushConstants(&push, sizeof(push));

                nvrhi::DrawArguments args;
                if (info.indexCount > 0)
                {
                    // NVRHI carries the index count in 'vertexCount' for an indexed draw:
                    // drawIndexed maps it to vkCmdDrawIndexed's indexCount
                    // (vulkan-graphics.cpp:689-700).
                    args.vertexCount = info.indexCount;
                    args.startIndexLocation = info.firstIndex;
                    args.startVertexLocation = info.firstVertex;
                    pCommandList->drawIndexed(args);
                }
                else
                {
                    args.vertexCount = info.vertexCount;
                    args.startVertexLocation = info.firstVertex;
                    pCommandList->draw(args);
                }
            }
        }
    }

    // The whole cube is left in the state the set-8 SRV consumers require and the state the A5.4
    // pass created it with (`initialState`/`keepInitialState`, RhiProceduralSkyPass.cpp:93-94), so
    // the next list's first SRV or UAV use finds the state it expects and the close-time
    // `keepInitialState` requirement is a no-op. The render-target use moved only the subresources
    // this Render touched; the requirement below covers all of them, which is correct because this
    // Render wrote all of them (every mip of every face).
    pCommandList->setTextureState(visibleCube, nvrhi::AllSubresources, nvrhi::ResourceStates::NonPixelShaderResource);
}

void RhiRasterSkyPass::ReleaseResources()
{
    // The pipelines reference their (specialized) shaders, so they go first.
    pipelines.clear();

    for (nvrhi::FramebufferHandle &framebuffer : framebuffers)
    {
        framebuffer = nullptr;
    }

    depthTexture = nullptr;
    pushConstantLayout = nullptr;
    inputLayout = nullptr;
    pixelShader = nullptr;
    vertexShader = nullptr;
}

nvrhi::IGraphicsPipeline *RhiRasterSkyPass::GetPipeline(uint32_t stateFlags, bool applyVertexColorGamma)
{
    assert(pipelineColorFormat != nvrhi::Format::UNKNOWN);

    const uint32_t key = stateFlags | (applyVertexColorGamma ? PIPELINE_STATE_VALUE_VERTEX_COLOR_GAMMA : 0);

    const auto found = pipelines.find(key);
    if (found != pipelines.end())
    {
        return found->second;
    }

    // NVRHI creates a new pipeline object on every call and deduplicates nothing, so this map is
    // the cache (recon 4.7) and it is keyed the way RasterizerPipelines::pipelines is
    // (RasterizerPipelines.cpp:278-296).
    nvrhi::GraphicsPipelineHandle pipeline = CreatePipeline(stateFlags, applyVertexColorGamma);
    if (pipeline == nullptr)
    {
        return nullptr;
    }

    const auto inserted = pipelines.emplace(key, std::move(pipeline));
    return inserted.first->second;
}

nvrhi::GraphicsPipelineHandle RhiRasterSkyPass::CreatePipeline(uint32_t stateFlags, bool applyVertexColorGamma)
{
    const bool alphaTest   = (stateFlags & PIPELINE_STATE_MASK_IS_ALPHA_TEST) != 0;
    const bool blendEnable = (stateFlags & PIPELINE_STATE_MASK_BLEND_ENABLE) != 0;
    const bool depthTest   = (stateFlags & PIPELINE_STATE_MASK_DEPTH_TEST_ENABLE) != 0;
    const bool depthWrite  = (stateFlags & PIPELINE_STATE_MASK_DEPTH_WRITE_ENABLE) != 0;
    const bool isLines     = (stateFlags & PIPELINE_STATE_MASK_IS_LINES) != 0;

    // One spec constant per stage, both SpecId 0 and 4 bytes (RasterizerPipelines.cpp:315-343):
    // the vertex stage's applyVertexColorGamma and the fragment stage's alphaTest, from the state
    // key. This is what makes the per-state pipelines differ in the state the shader sees, not only
    // in the fixed-function state.
    const nvrhi::ShaderSpecialization vertexSpecialization =
        nvrhi::ShaderSpecialization::UInt32(SPEC_CONSTANT_APPLY_VERTEX_COLOR_GAMMA, applyVertexColorGamma ? 1u : 0u);
    const nvrhi::ShaderSpecialization pixelSpecialization =
        nvrhi::ShaderSpecialization::UInt32(SPEC_CONSTANT_ALPHA_TEST, alphaTest ? 1u : 0u);

    nvrhi::ShaderHandle specializedVertexShader =
        device->createShaderSpecialization(vertexShader, &vertexSpecialization, 1);
    nvrhi::ShaderHandle specializedPixelShader =
        device->createShaderSpecialization(pixelShader, &pixelSpecialization, 1);

    if (specializedVertexShader == nullptr || specializedPixelShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to specialize the raster sky shaders");
        return nullptr;
    }

    // The blend attachment of the legacy sky pipeline state (RasterizerPipelines.cpp:406-420): the
    // alpha factors mirror the colour ones and the op is an add. The cube pass has one colour
    // attachment, so one entry.
    nvrhi::BlendState::RenderTarget blendTarget;
    blendTarget.setBlendEnable(blendEnable)
               .setSrcBlend(DecodeBlendFactor(stateFlags, PS_SRC_OFFSET))
               .setDestBlend(DecodeBlendFactor(stateFlags, PS_DST_OFFSET))
               .setBlendOp(nvrhi::BlendOp::Add)
               .setSrcBlendAlpha(DecodeBlendFactor(stateFlags, PS_SRC_OFFSET))
               .setDestBlendAlpha(DecodeBlendFactor(stateFlags, PS_DST_OFFSET))
               .setBlendOpAlpha(nvrhi::BlendOp::Add)
               .setColorWriteMask(nvrhi::ColorMask::All);

    nvrhi::GraphicsPipelineDesc desc;
    desc.setVertexShader(specializedVertexShader);
    desc.setPixelShader(specializedPixelShader);
    desc.inputLayout = inputLayout;
    desc.primType = isLines ? nvrhi::PrimitiveType::LineList : nvrhi::PrimitiveType::TriangleList;
    // Front face counter-clockwise, fill, no culling and depth clipping on, which is the legacy
    // rasterization state (RasterizerPipelines.cpp:382-390; depthClampEnable = FALSE there means
    // clipping stays enabled).
    desc.renderState.rasterState.setFillSolid();
    desc.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
    desc.renderState.rasterState.setFrontCounterClockwise(true);
    desc.renderState.rasterState.setDepthClipEnable(true);
    // LESS_OR_EQUAL with the test forced on whenever the depth is written, stencil off
    // (RasterizerPipelines.cpp:397-404).
    desc.renderState.depthStencilState.setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
    desc.renderState.depthStencilState.setDepthTestEnable(depthTest || depthWrite);
    desc.renderState.depthStencilState.setDepthWriteEnable(depthWrite);
    desc.renderState.depthStencilState.setStencilEnable(false);
    desc.renderState.blendState.setRenderTarget(0, blendTarget);

    // The table first, so it lands at descriptor set 0, then the push-constant layout; NVRHI's
    // legacy binding mode keeps the order the pass adds the layouts in.
    desc.addBindingLayout(textureTable->GetLayout());
    desc.addBindingLayout(pushConstantLayout);

    nvrhi::FramebufferInfo framebufferInfo;
    framebufferInfo.addColorFormat(pipelineColorFormat);
    framebufferInfo.setDepthFormat(DEPTH_FORMAT);
    framebufferInfo.setSampleCount(1);

    nvrhi::GraphicsPipelineHandle pipeline =
        rhi::createGraphicsPipeline(device, desc, framebufferInfo, "RhiRasterSky pipeline");

    if (pipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a raster sky pipeline");
    }

    return pipeline;
}

bool RhiRasterSkyPass::LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result)
{
    const std::string path = shaderFolderPath + pFileName;

    // The helper stays silent about a missing or unreadable blob, so that this class keeps its own
    // warning and its 'created == false' path (RhiPipeline.h).
    result = rhi::loadShader(device, path, type, pFileName);
    if (result == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the raster sky pass shader \"" + path + "\"");
        return false;
    }

    return true;
}
