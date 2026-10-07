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

#include <functional>
#include <string>
#include <tuple>
#include <unordered_map>

#include <nvrhi/vulkan.h>

#include "../Common.h"
#include "../RasterizedDataCollector.h"

namespace qray
{

class Framebuffers;

namespace rhi
{
class RhiFrameContext;
class RhiTextureTable;
}

// The A5.2 RHI module of the legacy rasterized overlay: the ported `Rasterizer::DrawToFinalImage`
// (Rasterizer.cpp:235-297), i.e. the engine's `DEFAULT` draw list - sprites, particles, translucent
// world brush surfaces, the emissive/effect rasters - drawn into the traced frame's FINAL image
// (FB_IMAGE_INDEX_FINAL, 28) and SCREEN_EMISSION (62), with the legacy's `DepthCopying` step
// (RasterPass.cpp:90-100) filling a pass-owned D32 depth from DEPTH_NDC (12) first. It is the RHI
// counterpart of the world sub-pass `RhiSkyPass::RenderWorld` proves for graphics, with four
// differences that are the point of this module:
//  1. the target is FINAL + SCREEN_EMISSION + the pass-owned depth, not the raster mode's ALBEDO;
//  2. the depth comes from a full-screen copy of the traced DEPTH_NDC image, so the overlay is
//     depth-tested against the traced scene (`RasterPass::PrepareForFinal`, and the world render
//     pass loads the copied depth, RasterPass.cpp:263-275);
//  3. the window is *inside* `RhiRtComposePass::Render`, between the checkerboard (which writes
//     FINAL 28 and SCREEN_EMISSION 62 through their storage views, RhiRtComposePass.h:114-115) and
//     `CmPrepareFinal` (which reads 62 and 25 and rewrites 28 through its sets,
//     RhiRtComposePass.h:116-117);
//  4. the shader's emissive blend-mode writes into `framebufPrimaryToReflRefr` (25) are the writes
//     `CmPrepareFinal` resolves (RsWorld.frag.hlsl:149-157), so the binding-25 storage image is a
//     live part of the target set, not a scratch.
//
// What it reproduces, exactly as the legacy pass has it:
//  - Shaders: "VertDefault" = RsRasterizer.vert.spv and "FragWorld" = RsWorld.frag.spv, the pair
//    RasterPass hands to its world RasterizerPipelines (RasterPass.cpp:74-75; ShaderManager.cpp:59,
//    :62), plus the `DepthCopying` pair "VertFullscreenQuad" = RsFullscreenQuad.vert.spv and
//    "FragDepthCopying" = RsDepthCopying.frag.spv (DepthCopying.cpp:24-25; ShaderManager.cpp:64-65).
//  - Sets, in the shader's own order (RsWorld.frag.hlsl:68-75): the shared bindless texture table at
//    0, the engine's global uniform at 1 (raw binding 0, the layout's constant-buffer offset is 0),
//    the per-slot tonemapping block at 2 (StructuredBuffer<ShTonemapping>, raw binding 0), the set-3
//    hole at 3 (RsWorld.frag declares no set 3; `volumeEnableType == 0` makes the engine's
//    volumetric set dead, so the layout is a zero-descriptor stand-in that carries the push
//    constants), and the partial framebuffers layout at 4 (one Texture_UAV for the shader's raw
//    binding 25). The five sets are the shape `RhiSkyPass::CreateWorld` proves.
//  - Push constants: one 88-byte block, the legacy RasterizedPushConst (Rasterizer.cpp:33-65) - the
//    model-view-projection, the color and the two texture indices - with the offsets the shader
//    block spells (RsWorld.frag.hlsl:79-84). The legacy host never writes the block's remaining
//    bytes, so the NVRHI block is the same 88 bytes.
//  - Spec constants: the vertex stage's `applyVertexColorGamma` and the fragment stage's
//    `alphaTest`, both SpecId 0 and 4 bytes, baked per state like RasterizerPipelines does
//    (RasterizerPipelines.cpp:315-343; the shader's alpha test discards below 0.5,
//    RsWorld.frag.hlsl:138-144).
//  - Per-draw state: the alpha-test / blend / depth / line-list key of
//    RasterizerPipelines::ConvertToStateFlags (RasterizerPipelines.cpp:57-113) and one pipeline per
//    key, created lazily and cached, because NVRHI does not deduplicate pipelines.
//  - The viewport/scissor rule of the legacy loop: the scissor stays at the full render area, the
//    viewport is the draw's own `QrViewport` when it has one and the full target otherwise
//    (Rasterizer.cpp:356-357, :389-396, :436-446). Every `DEFAULT` upload today passes a null
//    viewProjection and a null viewport (r_sprite.c:242, r_part.c:972, r_world.c:996, :1241,
//    r_alias.c:258), so the default is the common path.
//  - The default view-projection: the frame's plain view times the *jittered* projection
//    (Rasterizer.cpp:266-271, ApplyJitter at :176-184), exactly what the traced frame used, so the
//    overlay lands on the traced pixels and depth-tests against the copied traced depth. The jitter
//    is an argument for that reason.
//  - The draw loop: `vkCmdDrawIndexed` when the entry has indices and `vkCmdDraw` otherwise
//    (Rasterizer.cpp:412-419). The legacy has no per-entry filter for this stream; the collector
//    accepts depth state here (only SWAPCHAIN rejects it, RasterizedDataCollector.cpp:153-167), so
//    unlike the UI pass this loop keeps the depth-tested pipelines and skips only an entry that has
//    neither vertices nor indices, which the legacy would turn into a zero-vertex draw.
//
// The Y convention, the one inherited decision checked here (a5c_overlay_recon.md 6.2):
//  - The legacy feeds `vkCmdSetViewport` a Vulkan viewport with a positive height whose `y` is the
//    rectangle's top edge (Rasterizer.cpp:436-446). NVRHI's Vulkan backend converts the D3D-style
//    rectangle it is given with `VKViewportWithDXCoords` (vulkan-graphics.cpp:528-531), which flips
//    the height; passing the rectangle unchanged would draw the overlay vertically mirrored, and -
//    worse for this pass - would test each fragment against the depth of the mirrored row. The
//    pass therefore passes the inverted rectangle, which the backend turns back into the legacy's
//    own positive-height VkViewport, the same trick and the same reason as RhiUiPass (see the
//    comment on the helper in the .cpp). FINAL is an engine-convention image: the traced passes put
//    the view's first row in the image's first row (the present's mirror flag, RhiPresent.frag.hlsl:
//    82-88) and the legacy world pass writes into the same image with the legacy viewport.
//
// The target-state contract with the compose (the window a5c_overlay_recon.md 2.3 describes):
//  - The engine leaves every framebuffer image in VK_IMAGE_LAYOUT_GENERAL - NVRHI's
//    UnorderedAccess - and a native wrap keeps no state between command lists
//    (RhiTextureSource.h). Render announces UnorderedAccess for FINAL, SCREEN_EMISSION, DEPTH_NDC
//    and the binding-25 image before its first binding - that is the truth the compose's
//    checkerboard and the raygens left - lets NVRHI move FINAL and SCREEN_EMISSION to the
//    render-target layout for the framebuffer use, and restores all four to UnorderedAccess at the
//    end. The continuation (`CmPrepareFinal`) uses them through the compose's own wraps (it samples
//    SCREEN_EMISSION 62 and the binding-25 image and rewrites FINAL 28 through its storage view),
//    and those wraps' tracked state is still UnorderedAccess from the compose's own announcement,
//    so the transitions the continuation emits name GENERAL as the old layout - which is why the
//    restore is not optional.
//  - What the host must guarantee around the call: record it on the same open command list of
//    `frameIndex`, strictly after the compose's checkerboard and strictly before the
//    `CmPrepareFinal` dispatch; do not record another pass between the two compose halves that
//    touches 25/28/62/DEPTH_NDC without leaving them in GENERAL. The window the compose module
//    offers for that is `RhiRtComposePass::Render`'s trailing `pfnRasterOverlay` callback (it
//    invokes it exactly there, once per recorded call), so the host wires Render through a lambda
//    that captures the frame's inputs.
//
// The depth: a pass-owned D32 per slot at the render size, created here and retired through the
// frame context (ReleaseTargets). It exists because the legacy `DepthCopying` needs a depth
// attachment to write `SV_Depth` into (DepthCopying.cpp:123-167, :278-286) and the world pass then
// depth-tests against it (RasterPass.cpp:263-275). Unlike the legacy's depth copy, no clear is
// recorded: the full-screen quad writes every pixel, so the copy defines the whole image.
//
// The depth copy and the world draws are two dynamic-rendering passes over that one D32 (the
// legacy is two render passes over its depth image, DepthCopying's and the world's) and NVRHI emits
// no barrier between them: both uses require the same DepthWrite state, so the state tracker has
// nothing to transition, and the automatic-barrier pass does not model the depth-write/depth-read
// hazard. The same shape exists today between RhiSkyPass's sky and its world sub-pass over the
// sky's own depth; the legacy leaves the same ordering to the render pass's attachment load/store
// (its world render pass loads the depth the copy stored). The first A5.2 gate should therefore be
// watched for a sync-validation READ-AFTER-WRITE on the pass-owned depth if sync validation is
// enabled; nothing in the NVRHI API lets the pass insert that barrier itself.
//
// The smoke half (master's shader smoke): the same Render also draws the frame's smoke draw list -
// the DEFAULT entries whose pipeline state carries QR_RASTERIZED_GEOMETRY_STATE_SMOKE (r_smoke.c:
// 358-376 uploads all puffs as one batch, and the collector keeps no separate smoke stream, so the
// host filters the DEFAULT list in VulkanDevice.cpp) - with the pair the legacy smoke pipelines
// load: "VertSmoke" = RsSmoke.vert.spv and "FragSmoke" = RsSmoke.frag.spv (RasterPass.cpp:79-88;
// ShaderManager.cpp:61-62), from the same folder. What the smoke pipeline reproduces from the
// legacy pair:
//  - the six-attribute vertex layout of RasterizedDataCollector::GetSmokeVertexLayout
//    (RasterizedDataCollector.cpp:56-89), in the collector's own struct: position at 0, the packed
//    color at 56, the corner at 32, the puff parameters (normal) at 16, the look pair
//    (texCoordLayer1) at 40 and the cluster (R32_UINT) at 60, over the same 80-byte stride the
//    world layout uses;
//  - the legacy RasterizedPushConst in full, 120 bytes: the view-projection, the color and the two
//    texture indices at 0/64/80/84 (Rasterizer.cpp:33-65) plus smokeNoise at 88 and smokeLook at
//    104, the two blocks RsSmoke.frag reads (RsSmoke.frag:24-28). The legacy host writes them once
//    per draw from the DrawInfo the collector filled (RasterizedDataCollector.cpp:243-244);
//  - the state of the DrawInfo, decoded exactly as RasterizerPipelines::CreatePipeline does for the
//    smoke object (blend on with one / one-minus-src-alpha, depth test on, no depth write, no
//    alpha test, triangle list; RasterPass.cpp:79-88 and RasterizerPipelines.cpp:305-475);
//  - no specialization constants: RsSmoke.vert and RsSmoke.frag declare none (measured with
//    spirv-dis over both blobs), while the legacy pipeline still maps SpecId 0 for both stages -
//    values Vulkan ignores - so the smoke pipeline has nothing to bake and its cache key is the
//    state flags alone (unlike the world pipeline, which bakes applyVertexColorGamma);
//  - the seven sets the two stages declare, in the engine's order:
//      0 textures    - the shared table, the same layout and set the world draw binds (RsSmoke.vert
//                      statically uses the `globalTextures` pair through its light helpers);
//      1 uniform     - the world draw's uniform layout and set;
//      2 tonemapping - the world draw's tonemapping layout and set (RsSmoke.frag declares set 2 but
//                      does not read it statically; the engine's set 2 occupies the position, and
//                      the unfilled-item rule of the validation device is satisfied by binding it);
//      3 push hole   - the same empty-layout idea as the world pass, with the 120-byte push range;
//      4 framebuffers- the six items RsSmoke.frag statically uses, the partial-set pattern the world
//                      pass proves: DEPTH_WORLD (9) and the two Q2 a-trous LF ping images
//                      (101 SH / 103 COCG) as Texture_SRV slots 9/101/103 (raw 124 + index) and
//                      their samplers as Sampler slots 9/101/103 (raw 248 + index), the offsets the
//                      engine's set declares (measured over renderer/Build/RsSmoke.frag.spv);
//      5 TLAS        - the slot's rt::IAccelStruct from RhiAccelStructs::GetTopLevel as a raw
//                      binding 0 (RsSmoke.vert's shadow ray queries, SmokeLight.h:6-31);
//      6 lights      - the direct pass's own set-6 layout and per-slot set (RhiRtDirectPass::
//                      GetLightLayout / GetLightSet, the same borrowed pair the indirect pass
//                      documents): RsSmoke.vert reads lightSources and the q2LightList* buffers
//                      through SmokeLight.h, and the light copies stay the direct pass's
//                      single-consumer work. The layout arrives once through SetSmokeLightLayout
//                      (the direct pass is created after this one, VulkanDevice_Init.cpp) and the
//                      set is taken per frame by Render.
// The smoke draws are recorded after the world draws of the same window (the two halves share the
// pass-owned depth, the FINAL/SCREEN_EMISSION targets and the default view-projection); the legacy
// alternates the two pipelines inside one Rasterizer::Draw loop in the collector's own order
// (Rasterizer.cpp:394-420, :469-479), which in Quake is the order the parallel upload tasks
// appended in and is not a fixed world-then-smoke order (gl_rmain.c:1041-1085 submits the world,
// the sky/water, the entities, the alpha entities, the particles - R_DrawSmoke included - and the
// view model as one dependency graph). The window itself is unchanged: both halves run before
// CmPrepareFinal and the 2D UI, so smoke stays under the HUD exactly as the legacy frame has it
// (VulkanDevice.cpp:1068 - the raster overlay - then :1084 - Finalize - then :1200 - the UI).
//
// A5.0 note: the push-constant struct, the state-key mirror, the pipeline cache and the small
// layout helpers below are a deliberate third copy of RhiSkyPass's raster-draw machinery - like
// RhiUiPass, this module stays self-contained instead of refactoring the sky pass. A5.0's shared
// raster-draw helper is expected to fold the copies together.
//
// The pass is a no-op until Create succeeded and while an input is missing (no framebuffers, no
// engine image, no geometry buffers, no camera, no uniform or tonemapping buffer); every early
// return is quiet after the first warning. The smoke half is further gated on its own inputs: the
// two smoke blobs, the direct pass's light layout and set, the slot's TLAS and the three engine
// images its set 4 binds; a missing one skips the smoke list alone and leaves the world half
// exactly as it was. It is not thread-safe: Render uses the per-slot target of the frameIndex it
// is given, which is the engine's single-threaded per-slot frame model (RhiFrameContext).
class RhiRasterOverlayPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    RhiRasterOverlayPass();
    ~RhiRasterOverlayPass();

    RhiRasterOverlayPass(const RhiRasterOverlayPass &other) = delete;
    RhiRasterOverlayPass(RhiRasterOverlayPass &&other) noexcept = delete;
    RhiRasterOverlayPass &operator=(const RhiRasterOverlayPass &other) = delete;
    RhiRasterOverlayPass &operator=(RhiRasterOverlayPass &&other) noexcept = delete;

    // 'pDevice' is the RHI device; 'pTextureTable' is the host's shared RHI texture table
    // (RHI/RhiTextureTable.h), bound as descriptor set 0; 'pFrameContext' is the host's RHI frame
    // context (RHI/RhiFrameContext.h), which owns the retire queues every replaced wrap,
    // framebuffer, set and pipeline of a resize goes through. None of the three is owned, they have
    // to outlive this object, and a null one makes Create fail. 'pShaderFolderPath' is the folder
    // ShaderManager loads the engine blobs from, with the trailing separator; the four blobs above
    // are loaded from it. Returns false and leaves the pass unusable if a shader, the input layout,
    // a layout, the set-3 hole set or the depth-copy pipeline cannot be created; the host logs that
    // through 'pfnPrint'.
    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiTextureTable *pTextureTable,
                rhi::RhiFrameContext *pFrameContext,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    // The world shader's set 2: the engine's per-slot ShTonemapping block, the same wraps the host
    // builds for `RhiSkyPass::CreateWorld` - `Tonemapping::GetBuffer(frameIndex)` through
    // createHandleForNativeBuffer with `structStride = Tonemapping::GetElementSize()`
    // (NvrhiFrameSkeleton::PrepareWorld). The shader reads it for the exposure factor of the
    // rasterized colour (RsWorld.frag.hlsl:121), and Render is a no-op until the slot's buffer is
    // set. The wraps are borrowed and have to outlive the pass. Returns false without changing
    // anything if the pass was not created or a buffer is null or has structStride == 0, which the
    // backend would reject when the set is created (vulkan-resource-bindings.cpp:535-536).
    bool SetTonemappingBuffers(nvrhi::IBuffer *const pTonemappingBuffers[MAX_FRAMES_IN_FLIGHT]);

    // The buffers every draw is bound to, in the collector's own formats: QrVertex records of
    // RasterizedDataCollector::GetVertexStride() bytes and R32_UINT indices. The pass neither owns
    // nor fills them - the caller has to make sure that the frame's geometry is in them when Render
    // records and that they stay valid until the submission finishes. Render is a no-op until both
    // are set.
    //
    // Wiring hazard, the same one RhiUiPass documents: the legacy `CopyFromStaging` that fills the
    // collector's device-local buffers is recorded on the legacy command buffer
    // (VulkanDevice.cpp:1471), which the engine submits *after* the RHI list, so an RHI list of
    // frame N reading the device-local buffers would draw frame N-1's ranges. The overlay is
    // re-uploaded every frame (sprites, particles, translucent surfaces), so the caller has to bind
    // the per-slot *staging* buffers through native wraps instead
    // (RasterizedDataCollector::GetVertexStagingBuffer/GetIndexStagingBuffer), exactly as the
    // skeleton wires the UI pass.
    void SetGeometryBuffers(nvrhi::IBuffer *pVertexBuffer, nvrhi::IBuffer *pIndexBuffer);

    // Installs the direct pass's set-6 light layout (RhiRtDirectPass::GetLightLayout) for the smoke
    // and particle pipelines. The direct pass is created after this one in VulkanDevice_Init, so the
    // layout cannot be a Create argument; the overlay keeps the handle, which also keeps the layout
    // alive through the engine's teardown order (VulkanDevice_Init.cpp resets the direct pass before
    // this pass, while this pass still holds pipelines built over that layout). A re-install with a
    // different layout drops the cached lit pipelines; a null one disables both lit halves, as does
    // never calling this. Returns false without changing anything when the pass is not created.
    bool SetSmokeLightLayout(nvrhi::BindingLayoutHandle pLightLayout);

    bool SetRayStatsLayout(nvrhi::BindingLayoutHandle pRayStatsLayout);

    void SetParticleTimer(nvrhi::ITimerQuery *pParticleTimerQuery);

    // One call per frame, on the frame context's open command list of 'frameIndex', inside the
    // compose's window (see the class comment). It (re)resolves the engine images, (re)wraps them
    // and (re)builds the per-slot depth, framebuffers and sets when an image or the size changed,
    // announces the states, records the depth copy, the world draws, the particle draws and the
    // smoke draws, and restores the states.
    //
    // Argument sources, all of them the host's:
    //  - 'pCommandList': the frame context's open list of 'frameIndex'
    //    (`RhiFrameContext::GetCommandList(frameIndex)`), the same list the compose's two halves are
    //    recorded on;
    //  - 'pFramebuffers': the engine's framebuffer registry (`VulkanDevice`'s Framebuffers object,
    //    the pointer `SkyFrameInputs::framebuffers` carries). The pass resolves FB_IMAGE_INDEX_FINAL
    //    (28), FB_IMAGE_INDEX_SCREEN_EMISSION (62, through `GetScreenEmissionHandles`),
    //    FB_IMAGE_INDEX_DEPTH_NDC (12) and FB_IMAGE_INDEX_PRIMARY_TO_REFL_REFR (25, through
    //    `GetPrimaryToReflRefrHandles`) for the slot, so an engine framebuffer re-create is picked
    //    up without a second Create;
    //  - 'width'/'height': the render resolution (`RenderResolutionHelper::Width/Height`, the values
    //    `RhiRtComposePass::Render` takes for the same frame);
    //  - 'jitter': the frame's jitter in pixels, `uniform->GetData()->jitterX/jitterY`
    //    (`SkyFrameInputs::jitter`, the legacy `Rasterizer::DrawToFinalImage` jitter,
    //    VulkanDevice.cpp:1080). A null pointer skips the call; the value is required for parity -
    //    all DEFAULT uploads rely on the default view-projection;
    //  - 'pUniformBuffer': the static wrap of `GlobalUniform::GetBuffer()`, the same wrap the frame
    //    skeleton writes and the traced passes bind (set 1). A null, volatile or non-constant
    //    buffer skips the call;
    //  - 'pDraws'/'drawCount': the frame's `RasterizedDataCollector::GetRasterDrawInfos()` (the
    //    `DEFAULT` list, `SkyFrameInputs::worldDraws`). A null array counts as zero draws. An entry
    //    is skipped only when it has neither indices nor vertices; the legacy has no other filter
    //    for this stream (Rasterizer.cpp:412-419). A zero list still records the depth copy, which
    //    the legacy also does before its own empty-list early-out (Rasterizer.cpp:262-263,
    //    :341-347); nothing is drawn into FINAL/SCREEN_EMISSION then;
    //  - 'pView'/'pProj': the frame's column-major view and projection, `uniform->GetData()->view/
    //    projection` (`SkyFrameInputs::view/projection`). A null one skips the call. They build the
    //    default view-projection together with 'jitter' (Rasterizer.cpp:266-271);
    //  - 'applyVertexColorGamma': the per-frame value of the instance's rasterizedVertexColorGamma
    //    (RasterPass.cpp:67); it is baked into the pipeline's vertex spec constant, so it joins the
    //    pipeline key.
    //
    // Argument sources of the smoke half, also the host's:
    //  - 'pSmokeDraws'/'smokeDrawCount': the frame's smoke list, `SkyFrameInputs::smokeDraws` - the
    //    DEFAULT entries the host filtered by QR_RASTERIZED_GEOMETRY_STATE_SMOKE (VulkanDevice.cpp;
    //    the collector keeps no separate smoke stream). They are drawn with the smoke pipelines and
    //    their 120-byte push blocks, after the world list, and only when every smoke input below is
    //    present. A null/zero list, no smoke blobs, no light layout/set or no TLAS skips the smoke
    //    half alone; the world loop then keeps the entries and draws them as it did before this
    //    half existed (a zero-area draw - the six vertices of a puff all carry the puff origin);
    //  - 'pParticleDraws'/'particleDrawCount': the frame's lit-particle list,
    //    `SkyFrameInputs::particleDraws` - the DEFAULT entries the host filtered by
    //    QR_RASTERIZED_GEOMETRY_STATE_PARTICLE. They are drawn with the particle pipelines between
    //    the world list and the smoke list, and only when their own inputs - the two blobs, the
    //    particle layouts, the shared light layout/set and the slot's TLAS - are all present. A
    //    null/zero list or a missing piece skips the particle half alone; the world loop then keeps
    //    the entries and draws them flat, as it did before this half existed;
    //  - 'pSmokeTopLevel': the slot's rt::IAccelStruct (`RhiAccelStructs::GetTopLevel`, the same
    //    object the traced passes bind), wrapped into the smoke pipeline's set 5 and shared with the
    //    particle pipeline. The set is rebuilt when the object changes; a null one skips both lit
    //    halves;
    //  - 'pSmokeLightSet': this frame's set 6 of the direct pass (`RhiRtDirectPass::GetLightSet`,
    //    the layout `SetSmokeLightLayout` installed), shared by the smoke and particle pipelines.
    //    Null - the direct pass did not render this frame, or the overlay never got a layout - skips
    //    both halves. The set is borrowed and has to stay valid until the submission completes.
    //
    // The host records the call only while the frame wants rasterization (`!drawInfo.
    // disableRasterization`, VulkanDevice.cpp:1068); false in every shipped configuration.
    void Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                const Framebuffers *pFramebuffers,
                uint32_t width,
                uint32_t height,
                const float jitter[2],
                nvrhi::IBuffer *pUniformBuffer,
                const RasterizedDataCollector::DrawInfo *pDraws,
                uint32_t drawCount,
                const float *pView,
                const float *pProj,
                bool applyVertexColorGamma,
                const RasterizedDataCollector::DrawInfo *pSmokeDraws,
                uint32_t smokeDrawCount,
                const RasterizedDataCollector::DrawInfo *pParticleDraws,
                uint32_t particleDrawCount,
                nvrhi::rt::IAccelStruct *pSmokeTopLevel,
                nvrhi::IBindingSet *pSmokeLightSet,
                nvrhi::IBindingSet *pRayStatsSet);

    // Drops the per-slot wraps, the pass-owned depth images, the framebuffers and the sets. The
    // caller has to call it before the engine destroys its framebuffer images (the
    // Framebuffers::PrepareForSize path), next to the other pass's ReleaseTargets(). The next Render
    // re-reads the engine's accessors and re-wraps, so the pass survives a resize without a second
    // Create; the pipelines and the set-3 hole set are format-independent and stay. Releases go
    // through the frame context's retire queue when one exists; the destructor drops directly, after
    // a device idle.
    void ReleaseTargets();

private:
    struct Target
    {
        // The engine images the slot currently wraps. A change means the engine re-created them (or
        // the slot's accessor answer changed) and every wrap, the depth and the framebuffers have to
        // follow.
        VkImage finalImage = VK_NULL_HANDLE;
        VkImage screenEmissionImage = VK_NULL_HANDLE;
        VkImage depthNdcImage = VK_NULL_HANDLE;
        VkImage storageImage = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;

        nvrhi::TextureHandle finalTexture;
        nvrhi::TextureHandle screenEmissionTexture;
        nvrhi::TextureHandle depthNdcTexture;
        nvrhi::TextureHandle storageTexture;

        // The pass-owned depth the DepthCopying quad writes and the world pass tests against, in the
        // legacy's format (D32).
        nvrhi::TextureHandle depthTexture;

        // The depth-only framebuffer of the depth copy and the world framebuffer over FINAL,
        // SCREEN_EMISSION and the depth - the attachments of the legacy world framebuffer
        // (RasterPass.cpp:115-119).
        nvrhi::FramebufferHandle depthCopyFramebuffer;
        nvrhi::FramebufferHandle framebuffer;

        // Set 0 of the depth copy (the DEPTH_NDC sampled view at raw 136) and set 4 of the world
        // pass (the binding-25 storage image).
        nvrhi::BindingSetHandle depthCopySet;
        nvrhi::BindingSetHandle framebuffersSet;

        // Set 1 (the engine uniform) and set 2 (the slot's tonemapping block), rebuilt when the
        // pointer the host passed changed.
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::BindingSetHandle uniformSet;
        nvrhi::IBuffer *tonemappingBuffer = nullptr;
        nvrhi::BindingSetHandle tonemappingSet;

        // -- the smoke half (see the class comment) --
        //
        // The engine images the smoke shader's set 4 binds, wrapped per slot like the four core
        // ones: DEPTH_WORLD (9, the depth fade) and the two Q2 a-trous LF ping images (101 SH /
        // 103 COCG, the ambient term). A missing one leaves smokeFramebuffersSet null and the smoke
        // half skipped for the frame; a change of one rebuilds the slot like a core image change.
        VkImage smokeDepthWorldImage = VK_NULL_HANDLE;
        VkImage smokePingLfShImage = VK_NULL_HANDLE;
        VkImage smokePingLfCocgImage = VK_NULL_HANDLE;
        nvrhi::TextureHandle smokeDepthWorldTexture;
        nvrhi::TextureHandle smokePingLfShTexture;
        nvrhi::TextureHandle smokePingLfCocgTexture;

        // The glass mask and the traced pane depth the particle vertex stage reads to
        // shift lit particles like the traced background behind the same pane.
        nvrhi::TextureHandle glassMaskTexture;

        // Set 4 of the smoke pipeline (the six sampled items over those three images) and set 5
        // (the slot's TLAS over the shared rt::IAccelStruct object; rebuilt when it changes).
        nvrhi::BindingSetHandle smokeFramebuffersSet;
        nvrhi::rt::IAccelStruct *smokeTopLevel = nullptr;
        nvrhi::BindingSetHandle smokeTlasSet;

        nvrhi::BindingSetHandle particleFramebuffersSet;

        // False until every wrap, the depth, both framebuffers and the slot's sets exist.
        bool valid = false;
    };

    bool LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result);

    // Resolves the engine images, rebuilds the slot's wraps, depth, framebuffers and sets when one
    // of them or the size changed, announces the states and refreshes the slot's uniform and
    // tonemapping sets. The four core images are required; the three the smoke set 4 binds are
    // optional and only decide whether the smoke half can run. 'pSmokeTopLevel' is the slot's TLAS
    // object, whose set 5 is (re)built here. Returns false when the call must be skipped.
    bool PrepareTarget(nvrhi::ICommandList *pCommandList, uint32_t frameIndex, Target &target,
                       const Framebuffers &framebuffers, uint32_t width, uint32_t height,
                       nvrhi::IBuffer *pUniformBuffer, nvrhi::rt::IAccelStruct *pSmokeTopLevel);
    bool CreateTargetObjects(Target &target,
                             const std::tuple<VkImage, VkImageView, VkFormat> &finalImage,
                             const std::tuple<VkImage, VkImageView, VkFormat> &screenEmission,
                             const std::tuple<VkImage, VkImageView, VkFormat> &depthNdc,
                             const std::tuple<VkImage, VkImageView, VkFormat> &storageImage,
                             const std::tuple<VkImage, VkImageView, VkFormat> &smokeDepthWorld,
                             const std::tuple<VkImage, VkImageView, VkFormat> &smokePingLfSh,
                             const std::tuple<VkImage, VkImageView, VkFormat> &smokePingLfCocg,
                             const std::tuple<VkImage, VkImageView, VkFormat> &glassMask,
                             uint32_t frameIndex, uint32_t width, uint32_t height);
    bool UpdateBufferSets(Target &target, uint32_t frameIndex, nvrhi::IBuffer *pUniformBuffer);
    bool PrepareSmokeTlasSet(Target &target, nvrhi::rt::IAccelStruct *pSmokeTopLevel);

    void RecordDepthCopy(nvrhi::ICommandList *pCommandList, const Target &target,
                         uint32_t width, uint32_t height);
    void RecordWorldDraws(nvrhi::ICommandList *pCommandList, const Target &target,
                          uint32_t width, uint32_t height, const float *defaultViewProj,
                          const RasterizedDataCollector::DrawInfo *pDraws, uint32_t drawCount,
                          bool applyVertexColorGamma, bool skipSmokeEntries, bool skipParticleEntries);
    void RecordSmokeDraws(nvrhi::ICommandList *pCommandList, const Target &target,
                          uint32_t width, uint32_t height, const float *defaultViewProj,
                          const RasterizedDataCollector::DrawInfo *pDraws, uint32_t drawCount,
                          nvrhi::IBindingSet *pSmokeLightSet, nvrhi::IBindingSet *pRayStatsSet);
    void RecordParticleDraws(nvrhi::ICommandList *pCommandList, const Target &target,
                             uint32_t width, uint32_t height, const float *defaultViewProj,
                             const RasterizedDataCollector::DrawInfo *pDraws, uint32_t drawCount,
                             nvrhi::IBindingSet *pLightSet, nvrhi::IBindingSet *pRayStatsSet);

    void ReleaseTarget(Target &target);
    void ReleasePipelineCache();
    void ReleaseSmokePipelineCache();
    void ReleaseParticlePipelineCache();

    nvrhi::IGraphicsPipeline *GetWorldPipeline(uint32_t stateFlags, bool applyVertexColorGamma);
    nvrhi::GraphicsPipelineHandle CreateWorldPipeline(uint32_t stateFlags, bool applyVertexColorGamma);

    nvrhi::IGraphicsPipeline *GetSmokePipeline(uint32_t stateFlags);
    nvrhi::GraphicsPipelineHandle CreateSmokePipeline(uint32_t stateFlags);
    bool PrewarmSmokePipeline();

    nvrhi::IGraphicsPipeline *GetParticlePipeline(uint32_t stateFlags);
    nvrhi::GraphicsPipelineHandle CreateParticlePipeline(uint32_t stateFlags);
    bool PrewarmParticlePipeline();

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;

    // The four blobs: the world pair (the vertex half is shared) and the depth-copy pair.
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle worldPixelShader;
    nvrhi::ShaderHandle depthCopyVertexShader;
    nvrhi::ShaderHandle depthCopyPixelShader;

    nvrhi::InputLayoutHandle inputLayout;

    // The world pipeline's layouts 1..4, in the shader's set order: the engine's global uniform, the
    // tonemapping block, the set-3 hole (a zero-item layout that carries the push constants) and the
    // partial framebuffers layout (one Texture_UAV for the shader's binding 25). Set 0 is the shared
    // texture table.
    nvrhi::BindingLayoutHandle worldUniformLayout;
    nvrhi::BindingLayoutHandle worldTonemappingLayout;
    nvrhi::BindingLayoutHandle worldPushConstantLayout;
    nvrhi::BindingLayoutHandle worldFramebuffersLayout;

    // The depth copy's single layout: the DEPTH_NDC sampled view at raw 136 plus the legacy's 8-byte
    // fragment push block (depthCopyingPush's two uints, which the compiled blob does not read but
    // the NVRHI validation device requires the draw to set).
    nvrhi::BindingLayoutHandle depthCopyLayout;

    // The real, empty set that fills the set-3 hole. A null entry in GraphicsState::bindings would
    // be dereferenced by the backend's automatic-barrier pass (vulkan-state-tracking.cpp:105)
    // before the bind pass could treat it as a hole.
    nvrhi::BindingSetHandle worldHoleSet;

    // The depth-copy pipeline, static: no per-draw state, one full-screen quad per frame.
    nvrhi::GraphicsPipelineHandle depthCopyPipeline;

    // -- the smoke half (see the class comment) --

    // The two blobs the legacy smoke pipelines load (RasterPass.cpp:79-88): "VertSmoke" =
    // RsSmoke.vert.spv and "FragSmoke" = RsSmoke.frag.spv, and the six-attribute input layout of
    // RasterizedDataCollector::GetSmokeVertexLayout that goes with them. The pair is optional: a
    // missing blob disables the smoke half with one warning instead of failing Create, so a tree
    // whose shader build does not carry the pair yet cannot take the world overlay down with it.
    nvrhi::ShaderHandle smokeVertexShader;
    nvrhi::ShaderHandle smokePixelShader;
    nvrhi::InputLayoutHandle smokeInputLayout;

    // The smoke pipeline's layouts 3..5, in the shader's set order: the set-3 hole with the
    // 120-byte push range, the partial framebuffers layout of the six sampled items the smoke
    // fragment statically uses (SRV slots 9/101/103 at raw 124 + index, Sampler slots 9/101/103 at
    // raw 248 + index) and the TLAS at raw binding 0. Sets 0..2 are the world pipeline's layouts.
    nvrhi::BindingLayoutHandle smokePushConstantLayout;
    nvrhi::BindingLayoutHandle smokeFramebuffersLayout;
    nvrhi::BindingLayoutHandle smokeTlasLayout;

    // The real, empty set of the smoke set-3 hole (the worldHoleSet argument) and the sampler the
    // smoke set 4 binds for the three images: the engine's own framebuffer samplers are VkSampler
    // objects the RHI cannot wrap (RhiTextureSource.h), while the shader's texelFetch reads ignore
    // the filter mode, so the bridge's linear/clamp engine-texture sampler serves them. The
    // particle framebuffers set binds the same sampler for the two LF ping images.
    nvrhi::BindingSetHandle smokeHoleSet;
    nvrhi::SamplerHandle smokeSampler;

    nvrhi::ShaderHandle particleVertexShader;
    nvrhi::ShaderHandle particlePixelShader;
    nvrhi::InputLayoutHandle particleInputLayout;

    nvrhi::BindingLayoutHandle particlePushConstantLayout;
    nvrhi::BindingLayoutHandle particleFramebuffersLayout;

    nvrhi::BindingSetHandle particleHoleSet;

    // The direct pass's set-6 light layout, installed by SetSmokeLightLayout. The handle - not a raw
    // pointer - also keeps the borrowed layout alive through the engine's teardown order (the
    // direct pass is destroyed before this pass, VulkanDevice_Init.cpp). A change drops the cached
    // smoke pipelines, which reference it.
    nvrhi::BindingLayoutHandle smokeLightLayout;

    nvrhi::BindingLayoutHandle rayStatsLayout;
    nvrhi::ITimerQuery *particleTimerQuery = nullptr;

    // One smoke pipeline per state key, like worldPipelines but with no vertex-gamma bit: the smoke
    // shaders declare no specialization constant. Built lazily against smokeLightLayout.
    std::unordered_map<uint32_t, nvrhi::GraphicsPipelineHandle> smokePipelines;

    std::unordered_map<uint32_t, nvrhi::GraphicsPipelineHandle> particlePipelines;

    // The host's table and frame model; not owned, both outlive this object. The table provides the
    // bindless set and the first-use tracking of the engine textures it wrapped; the frame context
    // owns the retire queues.
    rhi::RhiTextureTable *textureTable = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;

    // The geometry the draws bind; not owned, the caller keeps them alive.
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;

    // The set-2 wraps of SetTonemappingBuffers; not owned.
    nvrhi::IBuffer *tonemappingBuffers[MAX_FRAMES_IN_FLIGHT] = {};

    // One entry per engine frame slot: the pass-owned depth and the framebuffers over it are per
    // slot, exactly like RasterPass's per-slot depth and world framebuffer arrays (RasterPass.h:
    // 91-98), and the wraps follow the slot's own accessor answer. None of the four images is one
    // of the engine's swapped history images (`ShFramebuffers_Bindings[i] ==
    // ShFramebuffers_BindingsSwapped[i]` for 12/25/28/62), so the two slots' wraps reference the
    // same images - the per-slot ownership only keeps the retire and resize contract uniform with
    // RasterPass's.
    Target targets[MAX_FRAMES_IN_FLIGHT];

    // Valid only for the formats the cached world pipelines were built with. A change means the
    // engine re-created FINAL or SCREEN_EMISSION with another format and every cached pipeline
    // belongs to the wrong framebuffer info.
    nvrhi::Format pipelineColor0Format = nvrhi::Format::UNKNOWN;
    nvrhi::Format pipelineColor1Format = nvrhi::Format::UNKNOWN;
    std::unordered_map<uint32_t, nvrhi::GraphicsPipelineHandle> worldPipelines;

    bool warnedMissingTargets = false;
    bool warnedMissingGeometry = false;
    bool warnedMissingCamera = false;
    bool warnedMissingUniform = false;
    bool warnedMissingTonemapping = false;
    bool warnedFailedPipeline = false;
    bool warnedMissingSmokeTargets = false;
    bool warnedMissingSmokeInputs = false;
    bool warnedFailedSmokePipeline = false;
    bool warnedMissingParticleInputs = false;
    bool warnedFailedParticlePipeline = false;

    bool created = false;
};

}
