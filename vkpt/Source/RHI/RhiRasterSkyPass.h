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

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include "../Common.h"
#include "../RasterizedDataCollector.h"

namespace vkpt
{

namespace rhi
{
class RhiTextureTable;
}

// The raster sky's cube content: the legacy `Rasterizer::DrawSkyToCubemap` -> `RenderCubemap::Draw`
// pair (Rasterizer.cpp:164-172, RenderCubemap.cpp:148-231) recorded through the RHI layer into the
// cube the A5.4 procedural sky owns. It is the writer of the `renderCubemap` image for
// SKY_TYPE_RASTERIZED_GEOMETRY (a5a_sky_recon.md §5): under that sky type the traced primary takes
// its sky colour from the raster ALBEDO, but `getSkyFiltered` returns `renderCubemap`
// (RaygenCommon.hlsli:393-417), so the specular reflections and the indirect ambient read this
// cube and would otherwise get a 1x1 placeholder (RhiRtPrimaryPass.cpp:435-457,
// RhiRtIndirectPass.h:115-121). `RhiProceduralSkyPass` is the cube pair's owner;
// `RhiProceduralSkyPass::GetCubemapTexture()` is the borrowed `renderCubemap`
// (RhiProceduralSkyPass.h:158-164, its own seam note for this module), and under this sky type the
// env cube is irrelevant - `getSkyFiltered` never samples it for RASTERIZED_GEOMETRY.
//
// What it reproduces, exactly as the legacy cube pass has it:
//  - Shaders: "VertDefault" = RsRasterizer.vert.spv and "FragSky" = RsSky.frag.spv, the pair
//    RasterPass's sky sub-pass uses (ShaderManager.cpp:60-62, RasterPass.cpp:64-65) - *not* the
//    `VertDefaultMultiview` blob of RenderCubemap's multiview pipeline (RenderCubemap.cpp:471-479):
//    NVRHI has no multiview at this pin, so the module draws each face as its own pass and the
//    per-face view-projection of the multiview shader (RsRasterizerMultiview.vert:55) is folded
//    into the push block instead (see Render).
//  - The pipeline's layouts: the shared bindless texture table as the first layout, so it lands at
//    descriptor set 0 (the shader's DESC_SET_TEXTURES) in NVRHI's legacy binding mode, plus one
//    88-byte push-constant layout. No other set is declared by either blob.
//  - The push-constant block, byte for byte the legacy `RasterizedPushConst` (Rasterizer.cpp:33-65),
//    whose offsets are the ones RenderCubemap's own 84-byte multiview block carries for its first
//    three members (RenderCubemap.cpp:43-55): vp[16] at 0, color[4] at 64, textureIndex at 80,
//    emissionTextureIndex at 84. The fragment half of RsSky.frag declares a 92-byte block but reads
//    only colour and textureIndex (HLSL/RsSky.frag.hlsl:30-36); the legacy host never writes the
//    last four bytes, so the module mirrors the 88-byte legacy value.
//  - Spec constants: the vertex stage's applyVertexColorGamma and the fragment stage's alphaTest,
//    both SpecId 0 and 4 bytes (RasterizerPipelines.cpp:315-343); the vertex value joins the
//    pipeline key the way RhiSkyPass bakes it (RhiSkyPass.cpp:133-137).
//  - Vertex input: binding 0, the collector's stride, locations in the declaration order that
//    RasterizedDataCollector::GetVertexLayout produces.
//  - Per-draw state: the alpha-test / blend / depth / line-list key of
//    RasterizerPipelines::ConvertToStateFlags and one pipeline per key created lazily and cached,
//    because NVRHI does not deduplicate pipelines.
//  - The target: the borrowed cube's one (mip, face) subresource plus the module's own depth, with
//    the legacy depth clear before the face's first draw. The legacy render pass clears the depth
//    with loadOp CLEAR and its one D16 image is 1 mip / 6 faces (RenderCubemap.cpp:36, :393-400),
//    while this module's depth is 11 mips, one per colour mip, and each (mip, face) pair has its
//    own framebuffer and is cleared once (66 clears per Render).
//  - The viewport is the legacy cube pass's: a static positive-height VkViewport from (0, 0) to
//    the face size with depth [0, 1] (RenderCubemap.cpp:460-468) and a scissor over the same area,
//    for every draw of a face (RenderCubemap.cpp:194-199, :479 `DisableDynamicState`). The module
//    passes the inverted NVRHI rectangle through the same legacy-viewport helper RhiDecalPass,
//    RhiUiPass and RhiRasterOverlayPass carry, because the Vulkan backend converts with
//    `VKViewportWithDXCoords` (vulkan-graphics.cpp:528-531); a per-draw `DrawInfo::viewport` is
//    deliberately ignored, because the legacy cube loop ignores it too.
//  - Colour is never cleared: the legacy colour attachment's loadOp is DONT_CARE
//    (RenderCubemap.cpp:385) and the game's sky uploads draw a full-face opaque quad per face
//    (Sky_DrawSkyBox, Quake/gl_sky.c:1036-1091, plus the alpha-blended overlay of
//    Sky_ProcessPoly, :828-886). NVRHI's loadOp is always LOAD (vulkan-graphics.cpp:80), which is
//    the closest equivalent - the first use of a not-yet-written subresource is an
//    Undefined-sourced transition (state-tracking.cpp:422-424), so nothing is preserved and
//    nothing has to be.
//
// The per-mip difference: the legacy rasters mip 0 and then blits the chain
// (RenderCubemap.cpp:230, :233-330); the pinned NVRHI has no blit, so this module rasters the same
// draw list once per mip level, with the mip's own viewport/scissor and the same per-face
// view-projection (the projection is in NDC, the viewport scales it to the mip). Mip 0 is the
// legacy's own rasterization; mip m is the draw list rasterized at mip m's resolution rather than
// the legacy's linear box average of level m-1 - the same class of deviation the procedural sky
// module records for its per-mip dispatch (RhiProceduralSkyPass.h:98-112). Unlike that module, the
// draw list is raster geometry, so a mip level is not a resampled mip 0 but the quads' own
// coverage at that size; on a typical sky quad set (a handful of large quads) it is the cheapest
// part of the pass, while the fixed cost is one depth clear per (mip, face) = 66 clears. What the
// mips are for: `getSkyFiltered` samples the cube at the roughness lod under this sky type, so a
// one-mip cube would clamp every reflection to lod 0.
//
// State discipline:
//  - The cube is borrowed and NVRHI-owned, created by RhiProceduralSkyPass with
//    `initialState = NonPixelShaderResource` and `keepInitialState = true`
//    (RhiProceduralSkyPass.cpp:80-96), so every command list starts it in that state and
//    `TrackPendingTextures`/`beginTrackingTextureState` are not needed for it. The render-target
//    use moves the face's subresources to COLOR_ATTACHMENT_OPTIMAL and the depth use to
//    DEPTH_ATTACHMENT_OPTIMAL through NVRHI's automatic barriers
//    (common/misc.cpp:187-208); the module ends by requiring NonPixelShaderResource for the whole
//    cube, which is the state the set-8 SRV consumers and the next frame's `keepInitialState`
//    close both expect.
//  - The module's own depth is created with `initialState = DepthWrite` and
//    `keepInitialState = true`, exactly like RhiSkyPass's depth (RhiSkyPass.cpp:711-724): every
//    list starts and returns it in the depth-attachment layout, and the explicit clear is the
//    legacy loadOp CLEAR. The first clear of a subresource transitions it to CopyDest
//    (vulkan-texture.cpp:647-688) and the framebuffer use right after finds DepthWrite, so the clear
//    is the only transfer use of the image.
//  - Nothing is announced/restored through a wrap: both images are NVRHI-owned, so NVRHI's own
//    tracker is the truth and there is no engine image whose resting layout the module could lie
//    about.
//
// What the host has to do around it:
//  - create the A5.4 `RhiProceduralSkyPass` first and pass its `GetCubemapTexture()` here as the
//    borrowed cube. The A5.4 pass owns the image for the whole run and never re-creates it, so the
//    depth texture and all 66 framebuffers are built once in Create and stay valid until this pass
//    is destroyed; Render records from them without allocating any per-frame resource (only a
//    pipeline for a state key that frame has not produced before, exactly RhiSkyPass's lazy cache).
//    The cube must outlive this pass (the framebuffers hold a reference, but the host contract is
//    the A5.4 pass's lifetime); a null one fails Create, and so does a cube that is not the
//    expected 1024-square, 6-face, 11-mip RGBA16_FLOAT render target (the module's own depth and
//    framebuffers are fixed to that shape, so a mismatch is refused loudly instead of being drawn
//    wrong);
//  - call SetGeometryBuffers() with the same two RgVertex / R32_UINT wraps `RhiSkyPass` receives
//    (VulkanDevice_Init.cpp:727-771);
//  - record one Render per traced frame on the frame context's open command list, when the
//    uniform's `skyType` is SKY_TYPE_RASTERIZED_GEOMETRY, before the passes that sample the cube
//    (the legacy order is DrawSkyToCubemap -> DrawSkyToAlbedo -> pathTracer->Bind,
//    VulkanDevice.cpp:748-753) and before the present - with the frame's sky draw list and the
//    frame's six per-face matrices (see Render). The same call belongs next to the raster ALBEDO
//    half; the ALBEDO half is the coordinator's, this module never touches ALBEDO.
//
// Deliberately out of scope ("must not pull in", a5a_sky_recon.md §5): the engine's
// `RenderCubemap` object and its images (this module writes the RHI's own cube, not the engine's),
// the env cube and SKY_TYPE_CUBEMAP, the ALBEDO half, the multiview blob, the procedural sky's
// compute path, and every new cube/accessor/wrap - the module takes the cube as an argument and
// adds only the depth and the framebuffers.
//
// A5.0 note: the push-constant struct, the state-key mirror, the lazy pipeline cache and the
// legacy-viewport helper below are a deliberate copy of RhiSkyPass's raster-draw machinery - like
// RhiUiPass and RhiRasterOverlayPass, this module stays self-contained instead of refactoring the
// sky pass. A5.0's shared raster-draw helper is expected to fold the copies together.
//
// The pass is a no-op until Create succeeded and while an input is missing (no geometry buffers,
// no draws); every early return is quiet after the first warning. It is not thread-safe: Render
// records on the caller's open command list, which is the engine's single-threaded per-slot frame
// model (RhiFrameContext).
class RhiRasterSkyPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    // The shape of the borrowed cube and of the module's own depth (RenderCubemap.cpp:34-37, :80:
    // 1024x1024, 6 faces, 11 mips, D16).
    static constexpr uint32_t CUBEMAP_SIZE = 1024;
    static constexpr uint32_t CUBEMAP_FACE_COUNT = 6;
    static constexpr uint32_t CUBEMAP_MIP_LEVELS = 11;

    RhiRasterSkyPass();
    ~RhiRasterSkyPass();

    RhiRasterSkyPass(const RhiRasterSkyPass &other) = delete;
    RhiRasterSkyPass(RhiRasterSkyPass &&other) noexcept = delete;
    RhiRasterSkyPass &operator=(const RhiRasterSkyPass &other) = delete;
    RhiRasterSkyPass &operator=(RhiRasterSkyPass &&other) noexcept = delete;

    // 'pDevice' is the RHI device; 'pTextureTable' is the host's shared RHI texture table
    // (RHI/RhiTextureTable.h), bound as descriptor set 0; 'pShaderFolderPath' is the folder
    // ShaderManager loads the engine blobs from, with the trailing separator; RsRasterizer.vert.spv
    // and RsSky.frag.spv are loaded from it. None of the three is owned, all have to outlive this
    // object, and a null or unusable one makes Create fail. 'pVisibleCube' is the borrowed
    // `RhiProceduralSkyPass::GetCubemapTexture()` - the A5.4 `renderCubemap` image (1024-square,
    // 6 faces, 11 mips, RGBA16_FLOAT, `isRenderTarget`, same owner as the pass) - and is written by
    // this module. It must outlive this pass and must not be re-created or re-wrapped; this pass
    // never releases it. The host logs failures through 'pfnPrint'. Create also builds the depth
    // texture and the 66 framebuffers over the cube, so it fails (and leaves nothing behind) if a
    // resource cannot be created. Calling it again after success returns true.
    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiTextureTable *pTextureTable,
                const char *pShaderFolderPath,
                nvrhi::ITexture *pVisibleCube,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    // The buffers every draw is bound to, in the collector's own formats: RgVertex records of
    // RasterizedDataCollector::GetVertexStride() bytes and R32_UINT indices
    // (Rasterizer.cpp:385-386). The pass neither owns nor fills them - the caller has to make sure
    // that the frame's geometry is in them when Render records and that they stay valid until the
    // submission finishes. This is the same wrap pair, fed by the same legacy `CopyFromStaging`,
    // that RhiSkyPass::SetGeometryBuffers takes; RhiUiPass.h:168-175 records why the sky is the
    // stream that tolerates the legacy copy's frame lag. Render is a no-op until both are set.
    void SetGeometryBuffers(nvrhi::IBuffer *pVertexBuffer, nvrhi::IBuffer *pIndexBuffer);

    // Records the raster sky into the borrowed cube: for every mip level (0 first, the size
    // `max(1024 >> mip, 1)`) and every face (0..5, the array slice of the same number), it binds
    // the table as set 0 and the geometry buffers, clears that (mip, face) depth once - the
    // legacy's loadOp CLEAR - and then draws the whole list in the collector's order,
    // solid before alpha, as the data comes. 'pDraws'/'drawCount' are the frame's
    // `RasterizedDataCollector::GetSkyDrawInfos()`, the same list the raster ALBEDO half draws
    // (Rasterizer.cpp:219). An empty or null list records nothing at all, before any state
    // announcement or allocation, and so does a call without geometry buffers. 'applyVertexColorGamma'
    // is the per-frame value of the instance's rasterizedVertexColorGamma (RasterPass.cpp:67);
    // it is baked into the pipeline's vertex spec constant, so it joins the pipeline key.
    //
    // 'faceViewProj' is the six per-face view-projections, one column-major mat4 per cube face,
    // face f for array slice f: the values `GlobalUniform::viewProjCubemap[16 * f]` carries
    // (ShaderCommonC.h:318), i.e. `Matrix::GetCubemapViewProjMat` per face (Matrix.cpp:317-375,
    // filled at VulkanDevice.cpp:258-263). They are the matrices the legacy multiview vertex shader
    // reads by `gl_ViewIndex` (RsRasterizerMultiview.vert:55); this module's non-multiview vertex
    // shader takes the full product in its push block instead, so the per-draw value is
    // `vp = ToMat4Transposed(model, info.transform) x faceViewProj[face]` - the same product
    // RasterizedPushConst builds from a view-projection (Rasterizer.cpp:40-58) and byte-identical
    // to what the legacy shader computes for that face. A per-draw `DrawInfo::viewProj` is
    // deliberately not consulted: the legacy cube pass has no per-draw transform either. Must not
    // be null.
    //
    // No-op when the pass is not created, the command list or the matrices are null, or the draw
    // list is empty. On return the cube is left in NonPixelShaderResource, the state its set-8 SRV
    // consumers and its next frame's `keepInitialState` both expect.
    void Render(nvrhi::ICommandList *pCommandList,
                const RasterizedDataCollector::DrawInfo *pDraws,
                uint32_t drawCount,
                const float faceViewProj[6][16],
                bool applyVertexColorGamma);

private:
    bool LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result);

    void ReleaseResources();

    nvrhi::IGraphicsPipeline *GetPipeline(uint32_t stateFlags, bool applyVertexColorGamma);
    nvrhi::GraphicsPipelineHandle CreatePipeline(uint32_t stateFlags, bool applyVertexColorGamma);

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;

    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::InputLayoutHandle inputLayout;

    // The pipeline's second layout: one 88-byte push-constant item, no descriptors. The shader's
    // set 0 is the texture table.
    nvrhi::BindingLayoutHandle pushConstantLayout;

    // The host's table; not owned, outlives this object. It provides the bindless set and the
    // first-use tracking of the engine textures it wrapped.
    rhi::RhiTextureTable *textureTable = nullptr;

    // The borrowed cube (RhiProceduralSkyPass::GetCubemapTexture()); not owned, referenced by every
    // framebuffer, and never released or re-wrapped by this pass.
    nvrhi::ITexture *visibleCube = nullptr;

    // The geometry the draws bind; not owned, the caller keeps them alive.
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;

    // The module's own depth: the legacy cube depth's format and face count (D16_UNORM,
    // RenderCubemap.cpp:36, :493-496) plus one mip per colour mip, because the (mip, face)
    // framebuffer's depth attachment has to match the colour attachment's mip. It is a 2D array,
    // not a cube: nothing samples it, the framebuffers address it per face, and the NVRHI
    // validation device refuses arraySize != 1 on a plain Texture2D
    // (validation-device.cpp:174-187). Created once in Create; both keepInitialState and DepthWrite
    // make NVRHI return it to the depth-attachment layout after every list, exactly the layout the
    // engine's own depth descriptor declares.
    nvrhi::TextureHandle depthTexture;

    // One framebuffer per (mip, face), indexed `mip * CUBEMAP_FACE_COUNT + face`: the cube's
    // `TextureSubresourceSet(mip, 1, face, 1)` as the colour attachment and the same subresource of
    // the depth as the depth attachment. NVRHI builds a 2D colour view over that one cube slice
    // (vulkan-graphics.cpp:28-54, :56-84; vulkan-texture.cpp:287-349) - a cube image may be viewed
    // as a per-slice 2D view, which is what makes the six-per-face draws of the A5.4 seam note
    // (RhiProceduralSkyPass.h:158-164) possible without a second image or view of the cube. All
    // 66 are created once in Create and the cube never changes, so nothing is re-created or retired
    // per frame.
    nvrhi::FramebufferHandle framebuffers[CUBEMAP_MIP_LEVELS * CUBEMAP_FACE_COUNT];

    // The format of the borrowed cube the pipelines and the framebuffer info are built with: the
    // A5.4 cube reports RGBA16_FLOAT, the legacy CUBEMAP_FORMAT
    // (VK_FORMAT_R16G16B16A16_SFLOAT, RenderCubemap.cpp:35). Set by Create.
    nvrhi::Format pipelineColorFormat = nvrhi::Format::UNKNOWN;

    // One pipeline per state key, created lazily during Render; the key is
    // RasterizerPipelines::ConvertToStateFlags plus the vertex-gamma flag, exactly RhiSkyPass's
    // cache (RhiSkyPass.cpp:1405-1432).
    std::unordered_map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines;

    bool warnedMissingGeometry = false;
    bool warnedFailedPipeline = false;

    bool created = false;
};

}
