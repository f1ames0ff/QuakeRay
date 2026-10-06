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
#include <memory>
#include <string>
#include <vector>

#include <nvrhi/vulkan.h>

#include <qray/qray.h>

#include "../Common.h"
#include "../ISwapchainDependency.h"
#include "../RasterizedDataCollector.h"
#include "RhiCloudsPass.h"
#include "RhiProceduralSkyPass.h"

namespace qray
{

class Framebuffers;
class GlobalUniform;
class RenderResolutionHelper;
class RhiBloomPass;
class RhiDecalPass;
class RhiFsrPass;
class RhiPostEffectPass;
class RhiRasterOverlayPass;
class RhiRasterSkyPass;
class RhiRtComposePass;
class RhiRtDirectPass;
class RhiRtGodRaysPass;
class RhiRtIndirectPass;
class RhiRtPrimaryPass;
class RhiRtReflRefrPass;
class RhiShadowMapPass;
class RhiSkyPass;
class RhiUiPass;
class Swapchain;
class Tonemapping;
class VertexCollector;

namespace rhi
{
class RhiAccelStructs;
class RhiFrameContext;
class RhiTextureTable;
}

// The RHI frame skeleton: the frame is recorded and submitted through the RHI
// layer instead of the Vulkan command buffers of the renderer. It drives the
// rasterized sky - the first real engine pass on the RHI path (RHI/RhiSkyPass.h)
// - and presents the result: the sky draws into the engine's ALBEDO image and a
// fullscreen triangle samples that ALBEDO into the swapchain image that was
// acquired for this frame.
// The pass uses the RHI resource, binding and pipeline helpers
// (RhiResources.h, RhiPipeline.h) - a volatile constant buffer that carries the
// present's exposure and one sampler - so the plumbing of stage A2 is exercised
// while the drawn image is the engine's own sky.
// The sky pass itself is owned here, because every input its Create() needs is
// one this class already receives; the host hands over the collector's geometry
// buffers once through GetSkyPass().
// On top of the sky, the same pass also draws the rasterized world into the same
// ALBEDO and depth: once the engine's framebuffers exist, this class wraps the
// engine's global uniform and tonemapping buffers, calls RhiSkyPass::CreateWorld
// with the engine's image handles and, every frame, records the world draw list
// with a stand-in avgLuminance and a fresh copy of the uniform (see Render and
// PrepareWorld).
class NvrhiFrameSkeleton final : public ISwapchainDependency
{
public:
    using PrintFunction = std::function<void(const char *)>;

    // The per-frame inputs of the rasterized sky, filled by the host from the same values the
    // legacy Rasterizer::DrawSkyToAlbedo call receives (VulkanDevice.cpp:748-756): 'framebuffers'
    // is the engine framebuffer set ALBEDO lives in, 'width'/'height' are the render resolution
    // ALBEDO is sized to, and 'view', 'projection', 'jitter' and 'skyViewerPos' are passed through
    // to RhiSkyPass::SetSkyCamera unchanged. 'draws'/'drawCount' are the frame's
    // RasterizedDataCollector::GetSkyDrawInfos() (Rasterizer::GetDataCollector), and
    // 'applyVertexColorGamma' is the instance-wide boolean of the same name (RasterPass.cpp:67).
    //
    // The rasterized world sub-pass on top of the sky reads three more frame inputs:
    //  - 'worldDraws'/'worldDrawCount', the frame's RasterizedDataCollector::GetRasterDrawInfos()
    //    (Rasterizer::GetDataCollector) - the list the legacy world draw consumes
    //    (VulkanDevice.cpp:1071-1082);
    //  - 'uniform', the engine's global uniform: the world shader's set 1, and the source of the
    //    bytes the skeleton writes into the uniform wrap every frame (the engine's own
    //    GlobalUniform::Upload never runs on the RHI path);
    //  - 'tonemapping', the engine's tonemapping object: the source of the per-slot set 2 buffers
    //    and the carrier of the avgLuminance stand-in the exposure chain would normally fill.
    // All three are optional: when one is null, or the world could not be created, the skeleton
    // records the sky and the present exactly as it did before the world existed.
    struct SkyFrameInputs
    {
        const Framebuffers *framebuffers = nullptr;
        const RasterizedDataCollector::DrawInfo *draws = nullptr;
        uint32_t drawCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        // The upscaled (window) resolution the TAAU pass writes: the engine's
        // renderResolution.UpscaledWidth()/Height(), which equals the render size in the default
        // configuration (all rt_upscale_* off) and grows with rt_renderscale.
        uint32_t upscaledWidth = 0;
        uint32_t upscaledHeight = 0;
        float view[16] = {};
        float projection[16] = {};
        float jitter[2] = {};
        float skyViewerPos[3] = {};
        bool applyVertexColorGamma = false;

        // -- the raster sky's cube half (RHI/RhiRasterSkyPass.h) --
        // The six per-face view-projections of GlobalUniform::viewProjCubemap (ShaderCommonC.h:318),
        // the same bytes the legacy multiview vertex shader reads by gl_ViewIndex
        // (RsRasterizerMultiview.vert:55) and the host fills in FillUniform
        // (VulkanDevice.cpp:258-263). They go to RhiRasterSkyPass::Render, one column-major mat4 per
        // cube face, face f for array slice f; the ALBEDO half does not read them.
        float skyFaceViewProj[6][16] = {};

        // -- the rasterized world sub-pass --
        const RasterizedDataCollector::DrawInfo *worldDraws = nullptr;
        uint32_t worldDrawCount = 0;

        // -- the smoke half of the raster overlay (A5.5) --
        // The frame's smoke draws: the entries of the same DEFAULT collector list that carry
        // QR_RASTERIZED_GEOMETRY_STATE_SMOKE, filtered by the host (the collector keeps no separate
        // smoke stream - the legacy uploads all puffs as one batch, r_smoke.c:358-376). They go to
        // RhiRasterOverlayPass::Render beside the world list and are drawn with the ported RsSmoke
        // pair inside the compose window, after the world draws and before the 2D UI. The half's
        // other inputs are the skeleton's own: the slot's TLAS (accelStructs->GetTopLevel) and the
        // direct pass's set-6 light layout and set.
        const RasterizedDataCollector::DrawInfo *smokeDraws = nullptr;
        const QrDrawFrameVoxelSmokeParams *voxelSmokeParams = nullptr;
        uint32_t smokeDrawCount = 0;

        const RasterizedDataCollector::DrawInfo *particleDraws = nullptr;
        uint32_t particleDrawCount = 0;

        // -- the 2D UI pass (A5.1) --
        // The frame's SWAPCHAIN draw list and the collector's per-slot staging vertex and index
        // buffers: the UI is rewritten every frame, so the pass reads the staging - the device copy
        // of the collector is recorded on the legacy command buffer, which is submitted after the
        // RHI list. 'disableRasterization' is the host's per-frame gate (VulkanDevice.cpp:1199); a
        // zero staging handle or an empty list simply skips the UI.
        const RasterizedDataCollector::DrawInfo *swapchainDraws = nullptr;
        uint32_t swapchainDrawCount = 0;
        uint64_t swapchainVertexStaging = 0;
        uint64_t swapchainIndexStaging = 0;
        uint64_t swapchainVertexStagingSize = 0;
        uint64_t swapchainIndexStagingSize = 0;
        bool disableRasterization = false;

        // -- the god rays and their shadow map (A5.2) --
        // The host block of the legacy frame (VulkanDevice.cpp:908-1007) precomputed: the final
        // switch, the intensity and the eccentricity, the sun (the direction toward the sun for the
        // params and the light direction for the shadow map), the world box and the geometry
        // sources the shadow map draws. The shadow map's view-projection is not here: the skeleton
        // takes it from the shadow pass's render of the same frame. Everything runs only under
        // 'hasAabb' (the legacy guard); with 'enabled' false the skeleton still records the clear
        // path so image 64 is never stale.
        struct GodRays
        {
            bool enabled = false;                   // godRaysOn: the final switch, not the cvar
            bool hasAabb = false;                   // scene->HasAABB()
            float intensity = 0.0f;
            float eccentricity = 0.75f;
            float aabbMin[3] = {};
            float aabbMax[3] = {};
            float shadowLightDirection[3] = {};     // the shadow map's from-sun light direction
            float sunDirection[4] = {};             // toward the sun, for the params (xyz)
            float sunColor[4] = {};                 // the fixed-up color (xyz)
            float worldCenter[3] = {};
            float worldHalfSizeInv[3] = {};         // 1 / max(halfSize, 1) per axis
            const VertexCollector *staticCollector = nullptr;
            const VertexCollector *dynamicCollector = nullptr;
        } godRays;

        // -- the portals (A5.3) --
        // The engine PortalList buffers for this slot: the staging the game's teleport uploads go to
        // and the device-local 4032-byte array the reflrefr's set 9 binds. The skeleton wraps them
        // (the staging per slot as a copy source, the device-local once as a static constant
        // buffer), records the staging -> device copy before the reflect/refract dispatch and hands
        // the device wrap to the pass; the host resets the engine's uploaded-index bookkeeping after
        // the frame's uploads. Zero handles skip the copy.
        uint64_t portalStaging = 0;
        uint64_t portalDevice = 0;
        uint64_t portalSize = 0;

        // -- the procedural sky (A5.4) --
        // The `RenderCubemap::DrawProcedural` params of the legacy host block
        // (VulkanDevice.cpp:755-863), precomputed by the host every frame. The skeleton records the
        // compute before the trace only when the uniform selects SKY_TYPE_PROCEDURAL; the module
        // early-outs by these bytes, so an unchanged frame (clouds off) costs one memcmp.
        RhiProceduralSkyPass::Params proceduralSkyParams = {};

        bool cloudsLayer = false;
        uint32_t cloudsQuality = 2;
        RhiCloudsPass::LayerParams cloudsParams = {};
        RhiCloudsPass::ShadowParams cloudsShadowParams = {};

        // -- the decals (A5.6) --
        // The engine DecalManager buffers for this slot: the staging the game's uploads go to and
        // the device-local instance array the pass's set 3 binds (stride sizeof(ShDecalInstance)).
        // The skeleton wraps them, copies the frame's live range (skipped at zero) and hands the
        // device wrap to the pass. Zero handles or a zero copy size skip the copy and the draw.
        uint64_t decalStaging = 0;
        uint64_t decalDevice = 0;
        uint64_t decalBufferSize = 0;
        uint64_t decalCopySize = 0;
        uint32_t decalCount = 0;

        // -- the upscaler (A5.7) --
        // The engine resolution helper the FSR module takes (its own technique selection decides
        // whether the module runs) and the camera values the FSR Apply takes; the jitter and the
        // time delta come from the uniform copy already in the frame inputs.
        const RenderResolutionHelper *renderResolution = nullptr;
        float cameraNear = 0.0f;
        float cameraFar = 0.0f;
        float fovYRadians = 0.0f;

        // -- the post-upscale effect chain (RHI/RhiPostEffectPass.h) --
        // The frame's post-effect params, exactly the `drawInfo.postEffectParams` block the legacy
        // Render consumes (VulkanDevice.cpp:1166-1223): the pointers the game fills in
        // gl_vidsdl.c:2147-2154. The pass reads them synchronously during its two calls - the
        // pointed-to objects are the game's per-frame stack temporaries, exactly as in the legacy
        // frame - and keeps its own transition state across frames, the way the engine's effect
        // objects do. `postEffectFrameId` is the engine's frame counter (VulkanDevice::frameId),
        // the value the legacy wipe carries as its `startFrameId` (VulkanDevice.cpp:1212); only the
        // wipe reads it.
        QrDrawFramePostEffectsParams postEffectParams = {};
        uint32_t postEffectFrameId = 0;

        // The engine's global uniform: the world shader's set 1, the source of the bytes the
        // skeleton writes into the uniform wrap every frame, and the CPU copy the host-only exposure
        // parameters read. The host owns it, so the field keeps the shared_ptr (the traced mode's
        // Tonemapping::PrepareExposureParams takes the same shape).
        std::shared_ptr<GlobalUniform> uniform;
        Tonemapping *tonemapping = nullptr;

        // The exposure controls of the frame, copied from the draw info exactly as the legacy Render
        // copies them (VulkanDevice.cpp:1051-1059): the bias is authoritative (the game clamps it),
        // the contrast is clamped in the engine. They feed the traced mode's host-only
        // exposure-parameter write; the raster mode's neutral stand-in does not use them.
        float exposureBias = 0.0f;
        float tonemapPower = 0.6f;
        uint32_t tonemapType = 1;
        QrDrawFrameTonemappingParams exposureParams = {};

        // -- the acceleration-structure stream --

        // -- the acceleration-structure stream --

        // The frame inputs RhiAccelStructs::BuildTopLevel needs to synthesise the instance list and
        // build the slot's TLAS from the module's own BLAS: the uniform's world-ray cull mask, the
        // instance-wide sky flag and the draw info's "disable ray-traced geometry" flag - the same
        // three values the host passes to the engine's own TLAS preparation (VulkanDevice.cpp:1285).
        uint32_t rayCullMaskWorld = 0;
        bool allowGeometryWithSkyFlag = false;
        bool disableRayTracedGeometry = false;
    };

    // The frame mode of the whole run. Only Traced is reachable now: with the bring-up flags
    // retired, the host selects it unconditionally.
    //  - Rasterized: the raster sky and world sub-passes draw into ALBEDO and the present samples
    //                it;
    //  - Traced:     the real ray-tracing chain: the primary-visibility pass fills the engine's
    //                checkerboard G-buffer and the direct-lighting pass adds the light term, which
    //                the present composes as its diagnostic.
    enum class FrameMode
    {
        Rasterized,
        Traced,
    };

    // 'pTextureTable' is the shared RHI texture table of the host (RHI/RhiTextureTable.h), bound by
    // the sky pass as descriptor set 0; the table owns every wrapped engine texture and its
    // samplers. Not owned, it has to outlive this object, and a null pointer makes the pass
    // unavailable.
    // 'pFrameContext' is the host's RHI frame context (RHI/RhiFrameContext.h): it owns the per-slot
    // command lists and the retire queues, and Render records through it. Not owned, it has to
    // outlive this object, and a null pointer makes the pass unavailable.
    // 'pAccelStructs' is the host's acceleration-structure stream (rhi::RhiAccelStructs,
    // RHI/RhiAccelStructs.h): Render records its static and per-frame builds on the same open list,
    // in every mode, before the sky/trace and the present. Not owned; a null or not-created one
    // makes the skeleton unavailable.
    // 'pRtPrimaryPass' is the host's primary-visibility ray-tracing pass (RhiRtPrimaryPass,
    // RHI/RhiRtPrimaryPass.h): when 'mode' is Traced, Render drives it - into the engine's
    // checkerboard G-buffer images, ALBEDO included. Not owned; a null or not-created one with
    // that mode makes the skeleton unavailable.
    // 'pRtDirectPass' is the host's direct-lighting ray-tracing pass (RhiRtDirectPass,
    // RHI/RhiRtDirectPass.h): when 'mode' is Traced, Render drives it right after the primary - it
    // borrows the primary's shared layout handles, so the primary has to outlive it and be
    // destroyed after it. Not owned; a null or not-created one with that mode makes the skeleton
    // unavailable.
    // 'pRtIndirectPass' is the host's indirect / GI pass (RhiRtIndirectPass, RHI/RhiRtIndirectPass.h):
    // when 'mode' is Traced, Render drives it after the direct pass - it borrows the primary's
    // layout handles and the direct pass's light set, so both have to outlive it and be destroyed
    // after it. Not owned; a null or not-created one with that mode makes the skeleton unavailable.
    // 'pRtComposePass' is the host's compose pass (RhiRtComposePass, RHI/RhiRtComposePass.h): when
    // it is non-null, the traced chain runs it after the indirect pass and the present samples its
    // display-referred FINAL image directly, instead of ALBEDO plus the direct term. Optional: a
    // null one keeps the A4.2a present; the host creates it unconditionally with the other passes.
    // 'pReflRefrPass' is the host's Q2 reflect/refract pass (RhiRtReflRefrPass,
    // RHI/RhiRtReflRefrPass.h): in the traced chain, after the god-rays input trace and before the
    // reproject, Render records it when the uniform's reflect-refract depth is positive, from the
    // engine's portal buffers the frame inputs carry. Optional: a null one draws the frame without
    // reflections.
    // 'pProceduralSkyPass' is the host's procedural sky pass (RhiProceduralSkyPass,
    // RHI/RhiProceduralSkyPass.h): when it is non-null, the traced chain records its compute before
    // the primary whenever the uniform selects SKY_TYPE_PROCEDURAL, from the params the frame inputs
    // carry, so the RT passes' set 8 samples a written cube. Optional: a null one leaves the passes'
    // placeholders in place.
    // 'pRasterSkyPass' is the host's raster sky pass (RhiRasterSkyPass, RHI/RhiRasterSkyPass.h): the
    // cube half of SKY_TYPE_RASTERIZED_GEOMETRY. When it is non-null, the traced chain records its
    // draws - into the cube the procedural sky pass owns, so that pass has to outlive and be
    // destroyed after this one - before the primary, next to the sky pass's raster ALBEDO half,
    // whenever the uniform selects that sky type. Optional: a null one leaves the cube unwritten.
    // 'pRasterOverlayPass' is the host's raster overlay pass (RhiRasterOverlayPass,
    // RHI/RhiRasterOverlayPass.h): when it is non-null, the compose call's window invokes it over
    // the frame's DEFAULT draw list into FINAL and SCREEN_EMISSION, exactly where the legacy frame
    // records `Rasterizer::DrawToFinalImage`; the skeleton installs its geometry and tonemapping
    // wraps, and passes the frame's smoke list (SkyFrameInputs::smokeDraws), the slot's TLAS and
    // the direct pass's light set alongside, so the pass's smoke half draws the ported RsSmoke pair
    // in the same window. Optional: a null one draws the frame without the raster overlay.
    // 'pDecalPass' is the host's decal pass (RhiDecalPass, RHI/RhiDecalPass.h): right after the
    // primary, Render records it over the engine's decal instance buffer, so the direct and
    // indirect passes see the decal-modified ALBEDO. Optional: a null one draws the frame without
    // decals; the engine uploads none in this game, so the pass is normally a no-op.
    // 'pFsrPass' is the host's FSR upscaler module (RhiFsrPass, RHI/RhiFsrPass.h): after the compose
    // ran, Render drives the engine's own FidelityFX context over the frame's FINAL and, on success,
    // copies its output into the TAAU target the UI and the present sample; otherwise it records the
    // TAAU. Optional: a null one keeps the TAAU always.
    // 'pPostEffectPass' is the host's post-upscale effect chain (RhiPostEffectPass,
    // RHI/RhiPostEffectPass.h): after the upscaler and before the UI, Render records the legacy
    // `postEffectParams` consumers 1-7 (the color tint and its variants, the inverse-BW and
    // hue-shift effects, the chromatic aberration, the distorted sides, the waves, the radial
    // blur), and after the UI block Render records the wipe and the CRT half - the legacy order
    // (VulkanDevice.cpp:1166-1223). Optional: a null one draws the frame without the post effects
    // (the default-on chromatic aberration among them).
    // 'pShadowMapPass' and 'pGodRaysPass' are the host's A5.2 passes (RhiShadowMapPass,
    // RhiRtGodRaysPass): in the traced chain, once the primary ran, Render records the shadow map,
    // and when it drew something the god-rays trace and filter whose output CmPrepareFinal adds.
    // Optional: a null one draws the frame without shafts; the frame inputs carry the host-computed
    // sun/world/intensity values (VulkanDevice.cpp:908-1007).
    // 'pUiPass' is the host's 2D-UI pass (RhiUiPass, RHI/RhiUiPass.h): when it is non-null and the
    // compose ran, Render draws the frame's SWAPCHAIN overlay (the HUD, the console, the menus) into
    // the compose's upscaled image after the TAAU, from the per-slot staging geometry the frame
    // inputs carry. Optional: a null one draws the frame without the UI.
    explicit NvrhiFrameSkeleton(nvrhi::IDevice *pDevice,
                                const Swapchain *pSwapchain,
                                const char *pShaderFolderPath,
                                rhi::RhiTextureTable *pTextureTable,
                                rhi::RhiFrameContext *pFrameContext,
                                rhi::RhiAccelStructs *pAccelStructs,
                                RhiRtPrimaryPass *pRtPrimaryPass,
                                RhiRtDirectPass *pRtDirectPass,
                                RhiRtIndirectPass *pRtIndirectPass,
                                RhiRtComposePass *pRtComposePass,
                                RhiRtReflRefrPass *pReflRefrPass,
                                RhiProceduralSkyPass *pProceduralSkyPass,
                                RhiCloudsPass *pCloudsPass,
                                RhiRasterSkyPass *pRasterSkyPass,
                                RhiRasterOverlayPass *pRasterOverlayPass,
                                RhiDecalPass *pDecalPass,
                                RhiFsrPass *pFsrPass,
                                RhiPostEffectPass *pPostEffectPass,
                                RhiBloomPass *pBloomPass,
                                RhiShadowMapPass *pShadowMapPass,
                                RhiRtGodRaysPass *pGodRaysPass,
                                RhiUiPass *pUiPass,
                                FrameMode mode,
                                PrintFunction pfnPrint);
    ~NvrhiFrameSkeleton() override;

    NvrhiFrameSkeleton(const NvrhiFrameSkeleton &other) = delete;
    NvrhiFrameSkeleton(NvrhiFrameSkeleton &&other) noexcept = delete;
    NvrhiFrameSkeleton &operator=(const NvrhiFrameSkeleton &other) = delete;
    NvrhiFrameSkeleton &operator=(NvrhiFrameSkeleton &&other) noexcept = delete;

    // Called by the swapchain when its images are (re)created or destroyed.
    void OnSwapchainCreate(const Swapchain *pSwapchain) override;
    void OnSwapchainDestroy() override;

    // Records the frame for the image the swapchain acquired into the frame context's slot
    // 'frameIndex' and submits it on the RHI graphics queue; the context owns that slot's command
    // list and its submission. The acceleration-structure stream (RhiAccelStructs) is recorded
    // first - its builds, the patch of the uniform's per-instance geometry offsets the traced
    // modes need, and the RHI-side run of the engine's vertex preprocessing over the module's
    // dynamic copies (the dynamic geometry's generated shading normals, see
    // RhiAccelStructs::RecordVertexPreprocessing) - then the sky pass's Prepare (which prepares the
    // ALBEDO wrap the present samples and, in the traced modes, the announcement the trace's own
    // ALBEDO wrap relies on) and then the frame splits by frameMode:
    //  - Rasterized: the sky pass's SetSkyCamera/Render run, the rasterized world sub-pass (once it
    //    could be created) draws into the same target with the engine's uniform and the
    //    avgLuminance stand-in, and the present samples the ALBEDO wrap of the same slot into the
    //    swapchain image of the acquired index;
    //  - Traced: the engine's primary-visibility raygen writes the slot's checkerboard G-buffer
    //    (ALBEDO included) and, from A4.2 on, the direct-lighting pass adds the light term; the
    //    present composes the two and follows.
    // The raster world sub-pass is not recorded in the two traced modes; the raster sky's ALBEDO
    // half is, for the SKY_TYPE_RASTERIZED_GEOMETRY frame only, together with the raster sky pass's
    // cube half right before the primary (see Render's implementation).
    // 'semaphoreToWait' is the semaphore the swapchain signals on acquire, 'semaphoreToSignal' is
    // the one the presentation engine waits on. Returns false if the pass is unavailable.
    bool Render(const Swapchain *pSwapchain, uint32_t frameIndex, const SkyFrameInputs &sky,
                VkSemaphore semaphoreToWait, VkSemaphore semaphoreToSignal);

    // True if the pass cannot be used at all, so that the caller can avoid
    // taking the frame apart on every frame.
    bool IsUnavailable() const;

    // The rasterized sky pass this skeleton drives: the host creates the geometry buffers over the
    // collector's VkBuffers once and hands them over with SetGeometryBuffers(). Null when the pass
    // could not be created (the skeleton is unavailable then).
    RhiSkyPass *GetSkyPass() const { return skyPass.get(); }

    void RequestScreenshot(const std::string &path);

    // The GPU timings of the most recent frame the timer queries produced. 'pPassMs' receives
    // QR_GPU_PASS_COUNT entries in the order of qrGetGpuPassName. Returns false until the first
    // frame's timestamps could be read back (and forever when the timer queries are unavailable).
    bool GetGpuTimings(float *pFrameMs, float *pPassMs) const;

private:
    static nvrhi::Format ConvertSurfaceFormat(VkFormat format);

    bool LoadShader(const char *pFileName, nvrhi::ShaderType type, nvrhi::ShaderHandle &result);

    // Creates the present's binding layout, constant buffer and sampler. The binding set is per
    // frame slot and is built by PreparePresentBindingSet once the slot's ALBEDO wrap is known.
    bool CreatePassResources();

    // Creates or reuses the present binding set of 'frameIndex' for the slot's ALBEDO wrap and the
    // slot's direct-term storage image: a regular NVRHI binding set holds one item per texture, and
    // both wraps are replaced when the engine re-creates its framebuffers, so the set follows them.
    bool PreparePresentBindingSet(uint32_t frameIndex, nvrhi::ITexture *albedo, nvrhi::ITexture *directTexture);

    // The direct term's storage image of the slot: the wrap of the engine's unfiltered-direct
    // framebuffer image, created on the first frame the framebuffers exist and replaced when the
    // engine re-created them (the same point at which the sky pass re-wraps ALBEDO). Every mode
    // resolves it, because the layout's unordered-access item is always filled, but the shader
    // reads it only while the traced chain runs. Returns null when the image is unavailable; the
    // caller skips the present then, as it does without ALBEDO. The wrap is the skeleton's, so it
    // is released with the other swapchain resources.
    nvrhi::ITexture *ResolvePresentDirectTexture(uint32_t frameIndex, const SkyFrameInputs &sky);

    // The one-time world setup, called by Render on the first frame that got past the sky's
    // Prepare: wraps the engine's uniform and tonemapping buffers (once - the engine never
    // re-creates either) and hands them, together with this frame's engine image handles, to
    // RhiSkyPass::CreateWorld. It has to run this late because the Framebuffers accessors read the
    // image vector only Framebuffers::PrepareForSize fills, which the host calls per frame right
    // before Render - calling CreateWorld from the constructor would resolve empty handles.
    // Returns true once the world is created; a frame that simply has no world inputs yet is not a
    // failure and leaves the world to be attempted again, while a wrap or CreateWorld failure sets
    // worldCreationFailed (every failure mode there is permanent).
    bool PrepareWorld(uint32_t frameIndex, const SkyFrameInputs &sky);

    bool CreatePipeline(nvrhi::Format colorFormat);
    bool CreateSwapchainResources(const Swapchain *pSwapchain);
    void DestroySwapchainResources();

    nvrhi::vulkan::IDevice *device;
    PrintFunction print;
    std::string shaderFolderPath;
    std::string screenshotPath;
    nvrhi::StagingTextureHandle screenshotStaging;
    bool screenshotPending = false;

    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;

    // The rasterized sky pass of the RHI path; created in the constructor from the same device,
    // table, frame context and shader folder, and driven by Render below.
    std::unique_ptr<RhiSkyPass> skyPass;

    // The host's acceleration-structure stream (rhi::RhiAccelStructs, RHI/RhiAccelStructs.h):
    // Render records its static and per-frame builds on the slot's list, in both modes, before the
    // sky/trace and the present. Not owned; the host creates it next to this skeleton and keeps it
    // alive until after the skeleton (VulkanDevice_Init), so it outlives every Render.
    rhi::RhiAccelStructs *accelStructs = nullptr;

    // The host's primary-visibility ray-tracing pass (RhiRtPrimaryPass, RHI/RhiRtPrimaryPass.h),
    // driven instead of the raster sky/world chain when frameMode is Traced. Not owned; a null or
    // not-created one with that mode makes the skeleton unavailable, which the frame dispatch
    // treats as fatal.
    RhiRtPrimaryPass *rtPrimaryPass = nullptr;

    // The host's direct-lighting ray-tracing pass (RhiRtDirectPass, RHI/RhiRtDirectPass.h), driven
    // right after the primary when frameMode is Traced. It borrows the primary's layout handles, so
    // the host destroys it before the primary. Not owned; a null or not-created one makes the
    // skeleton unavailable, which the frame dispatch treats as fatal.
    RhiRtDirectPass *rtDirectPass = nullptr;

    // The host's indirect / GI pass (RhiRtIndirectPass, RHI/RhiRtIndirectPass.h), driven right after
    // the direct pass when frameMode is Traced. It borrows the primary's layout handles and the
    // direct pass's light set, so the host destroys it before both. Not owned; a null or not-created
    // one makes the skeleton unavailable, which the frame dispatch treats as fatal.
    RhiRtIndirectPass *rtIndirectPass = nullptr;

    // The host's compose pass (RhiRtComposePass, RHI/RhiRtComposePass.h), driven after the indirect
    // pass when it is non-null; the present then samples its display-referred FINAL image. Not
    // owned; null when the creation failed, in which case the traced chain keeps the A4.2a
    // diagnostic present.
    RhiRtComposePass *rtComposePass = nullptr;

    // The host's reflect/refract pass (RhiRtReflRefrPass, RHI/RhiRtReflRefrPass.h), driven in the
    // traced chain after the god-rays input trace and before the reproject when the uniform's
    // reflect-refract depth is positive. Not owned; null when the host's creation failed, in which
    // case the frame is drawn without reflections.
    RhiRtReflRefrPass *reflRefrPass = nullptr;

    // The host's procedural sky pass (RhiProceduralSkyPass, RHI/RhiProceduralSkyPass.h), driven in
    // the traced chain before the primary whenever the uniform selects SKY_TYPE_PROCEDURAL: it
    // writes the cube the RT passes' set 8 samples. Not owned; null when the host's creation failed,
    // in which case the passes sample their placeholders.
    RhiProceduralSkyPass *proceduralSkyPass = nullptr;

    // The host's cloud layer pass (RhiCloudsPass, RHI/RhiCloudsPass.h), driven in the traced chain
    // right before the procedural sky when the frame asks for the layer: it writes the layer the
    // sky's composite samples and the shadow volume of that layer. Not owned; null when the host's
    // creation failed or the frame's `cloudsLayer` is off, in which case the sky keeps its flat
    // clouds.
    RhiCloudsPass *cloudsPass = nullptr;

    // The host's raster sky pass (RhiRasterSkyPass, RHI/RhiRasterSkyPass.h), driven in the traced
    // chain right after the procedural-sky block and before the primary whenever the uniform
    // selects SKY_TYPE_RASTERIZED_GEOMETRY: it writes the raster sky's cube (the procedural sky
    // pass's image) with the frame's sky draw list from `skyFaceViewProj`. Not owned; null when the
    // host's creation failed, in which case the cube stays unwritten.
    RhiRasterSkyPass *rasterSkyPass = nullptr;

    // The host's raster overlay pass (RhiRasterOverlayPass, RHI/RhiRasterOverlayPass.h), invoked by
    // the compose call's window over the frame's DEFAULT draw list: it writes FINAL and
    // SCREEN_EMISSION exactly where the legacy frame records `Rasterizer::DrawToFinalImage`. Not
    // owned; null when the host's creation failed, in which case the frame is drawn without it.
    RhiRasterOverlayPass *rasterOverlayPass = nullptr;

    // The host's decal pass (RhiDecalPass, RHI/RhiDecalPass.h), driven right after the primary: it
    // blends the engine's decal instances into ALBEDO before the direct and indirect passes read it.
    // Not owned; null when the host's creation failed, in which case the frame is drawn without
    // decals (the engine uploads none in this game, so the pass is normally a no-op).
    RhiDecalPass *decalPass = nullptr;

    // The host's FSR upscaler module (RhiFsrPass, RHI/RhiFsrPass.h), driven after the compose's
    // Render when the engine's resolution helper selects FSR 3.1: it upscales FINAL into image 30,
    // which the skeleton copies into the TAAU target (29) so the UI and the present keep their
    // image; otherwise the TAAU records as before. Not owned; null when the host's creation failed.
    RhiFsrPass *fsrPass = nullptr;

    // The host's post-upscale effect chain (RhiPostEffectPass, RHI/RhiPostEffectPass.h), driven in
    // the traced chain right after the upscale and, for its wipe/CRT half, after the UI block. Not
    // owned; null when the host's creation failed, in which case the frame is drawn without the
    // post-upscale effects.
    RhiPostEffectPass *postEffectPass = nullptr;

    RhiBloomPass *bloomPass = nullptr;

    // The wraps of the engine DecalManager buffers (A5.6): the per-slot staging as a copy source and
    // the device-local instance array once as the pass's set 3 buffer (stride
    // sizeof(ShDecalInstance)). Created on the first frame with a live count, re-created if a handle
    // ever changes; the replaced ones go through the frame context's retire queue.
    nvrhi::BufferHandle decalStagingWraps[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle decalDeviceWrap;
    uint64_t decalStagingHandles[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t decalDeviceHandle = 0;

    // The wraps of the engine PortalList buffers (A5.3): the per-slot staging as a copy source and
    // the device-local array once as a static constant buffer (the set 9 handle the pass keeps).
    // Created on the first frame with non-zero frame inputs, re-created if a handle ever changes;
    // the replaced ones go through the frame context's retire queue.
    nvrhi::BufferHandle portalStagingWraps[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle portalDeviceWrap;
    uint64_t portalStagingHandles[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t portalDeviceHandle = 0;

    // The host's shadow-map and god-rays passes (RHI/RhiShadowMapPass.h, RHI/RhiRtGodRaysPass.h),
    // driven in the traced chain on the same list: the shadow map renders after the primary and,
    // when it drew something, the god-rays pass dispatches the trace and the filter whose output
    // CmPrepareFinal reads. Not owned; null when the host's creation failed, in which case the frame
    // is drawn without shafts.
    RhiShadowMapPass *shadowMapPass = nullptr;
    RhiRtGodRaysPass *godRaysPass = nullptr;

    // The 2D-UI pass (RhiUiPass, RHI/RhiUiPass.h), driven in the traced chain once the compose ran:
    // it draws the frame's SWAPCHAIN overlay into the compose's upscaled image. Not owned; null
    // when the host's creation failed, in which case the frame is drawn without the UI.
    RhiUiPass *uiPass = nullptr;

    // The per-slot wraps of the collector's UI staging geometry: the pass binds the frame's own
    // staging buffers (the UI is rewritten every frame, and the engine's device copy is recorded on
    // the legacy command buffer, submitted after this list), wrapped once per slot and re-wrapped
    // through the frame context if a handle ever changes. The handle arrays are the wrap keys.
    nvrhi::BufferHandle uiVertexStagingWraps[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle uiIndexStagingWraps[MAX_FRAMES_IN_FLIGHT];
    uint64_t uiVertexStagingHandles[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t uiIndexStagingHandles[MAX_FRAMES_IN_FLIGHT] = {};

    // The frame mode of the whole run: which chain Render records into ALBEDO. The host hard-wires
    // it to Traced (VulkanDevice_Init.cpp) and it does not change while the skeleton lives.
    FrameMode frameMode = FrameMode::Rasterized;

    // Descriptor set 0 of the present: the constant buffer (binding 256), the ALBEDO texture
    // (binding 0) and its sampler (binding 128), and the direct term's storage image (binding 384),
    // the numbers the shader declares with [[vk::binding(...)]]; RhiPresent.frag declares no other
    // set. The textures move into the per-slot binding sets below, so only the layout, the buffer
    // and the sampler live here.
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BufferHandle presentParamsBuffer;
    nvrhi::SamplerHandle presentSampler;

    // One present binding set per frame slot: it references that slot's ALBEDO wrap (ALBEDO is a
    // ping-pong engine image) and that slot's direct-term image, so it cannot be shared across
    // slots, and it is rebuilt when either wrap is replaced (a framebuffer re-create).
    // presentAlbedoTextures and presentDirectSetTextures are what the rebuild compares against, and
    // they keep the textures the set references alive.
    nvrhi::BindingSetHandle presentBindingSets[MAX_FRAMES_IN_FLIGHT] = {};
    nvrhi::TextureHandle presentAlbedoTextures[MAX_FRAMES_IN_FLIGHT];
    nvrhi::TextureHandle presentDirectSetTextures[MAX_FRAMES_IN_FLIGHT];

    // The direct term's storage image wraps, one per frame slot: the skeleton owns them (the sky
    // pass owns only ALBEDO), the image handle records what each was wrapped from so a framebuffer
    // re-create replaces it, and every replaced wrap goes through the frame context's retire queue.
    nvrhi::TextureHandle presentDirectTextures[MAX_FRAMES_IN_FLIGHT];
    uint64_t presentDirectImageHandles[MAX_FRAMES_IN_FLIGHT] = {};

    // The shared RHI texture table (RhiTextureTable.h, owned by the host): the sky pass binds it as
    // descriptor set 0 and its slot 0 holds the engine's empty texture. Render also asks it to
    // declare the first-use state of the engine textures wrapped since the last frame. Not owned.
    rhi::RhiTextureTable *textureTable = nullptr;

    // The host's RHI frame context (RhiFrameContext.h, owned by the host): the per-slot command
    // lists and retire queues of the RHI frame model. Render records through it, so it owns the
    // only lists this pass uses and the pass itself has no long-lived one. Not owned.
    rhi::RhiFrameContext *frameContext = nullptr;

    // -- the rasterized world sub-pass --

    // The world sub-pass's buffer wraps, created once by PrepareWorld on the first frame whose
    // inputs allow it. The engine's global uniform and tonemapping buffers are created once and
    // never re-created - only their contents change - so one wrap per buffer serves the whole run
    // and the RhiSkyPass::ReleaseTargets re-wrap contract does not apply to them. RhiSkyPass
    // borrows them through CreateWorld; the handles stay here, where they are released with the
    // skeleton, before the engine destroys the buffers.
    nvrhi::BufferHandle worldUniformBuffer;
    nvrhi::BufferHandle worldTonemappingBuffers[MAX_FRAMES_IN_FLIGHT];

    // True once the wraps above exist, whether or not CreateWorld accepted them.
    bool worldBuffersWrapped = false;

    // Set after a failed world creation. Every failure CreateWorld and the wraps can report is
    // permanent (a missing shader, a failed layout or binding set, a buffer in the wrong shape),
    // so Render does not retry it - and the pass's per-call warnings stay one-shot.
    bool worldCreationFailed = false;

    // Frames left until the one-time fallback-slot log; 0 after it has been printed.
    uint32_t framesUntilFallbackLog = 300;

    // -- the GPU pass timings --

    // The frame's sections the timer queries measure, in the order qrGetGpuPassName reports them.
    enum GpuPassIndex
    {
        GPU_PASS_SETUP = 0,
        GPU_PASS_CLOUDS,
        GPU_PASS_SKY,
        GPU_PASS_PRIMARY,
        GPU_PASS_DECALS,
        GPU_PASS_GODRAYS,
        GPU_PASS_REFLREFR,
        GPU_PASS_REFLGODR,
        GPU_PASS_GRADIENT,
        GPU_PASS_DIRECT,
        GPU_PASS_INDIRECT,
        GPU_PASS_COMPOSE,
        GPU_PASS_UPSCALE,
        GPU_PASS_POST,
        GPU_PASS_UI,
        GPU_PASS_POSTUI,
        GPU_PASS_PRESENT,
        GPU_PASS_COUNT
    };

    void CreateGpuTimers();
    void ReadGpuTimings(uint32_t frameIndex);
    void BeginGpuPass(nvrhi::ICommandList *pCommandList, uint32_t frameIndex, uint32_t pass);
    void EndGpuPass(nvrhi::ICommandList *pCommandList, uint32_t frameIndex, uint32_t pass);

    // One timer query pair per frame slot and measured section, plus one for the whole frame. The
    // queries are per slot because a query cannot be reset and re-used while the submission that
    // wrote it is still in flight; BeginSlot waits for the slot's previous submission, which is the
    // point ReadGpuTimings reads the slot's timestamps at.
    bool gpuTimersCreated = false;
    bool gpuTimersReady = false;
    nvrhi::TimerQueryHandle gpuFrameQueries[MAX_FRAMES_IN_FLIGHT];
    nvrhi::TimerQueryHandle gpuPassQueries[MAX_FRAMES_IN_FLIGHT][GPU_PASS_COUNT];

    // The most recent timings read back, in milliseconds, with 0.0f for a section that has not run
    // yet since the renderer started.
    float gpuFrameMs = 0.0f;
    float gpuPassMs[GPU_PASS_COUNT] = {};
    bool gpuTimingValid = false;

    // Set after the one-time warning that there is no ALBEDO wrap to present.
    bool warnedMissingAlbedo = false;
    // One-shot for a missing or unwrappable direct-lighting image of the traced chain.
    bool warnedMissingDirect = false;
    // One-shot for the DLSS deferral: the RHI path has no NGX integration, so a frame that asks
    // for QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS is upscaled by the TAAU instead
    // (RhiFsrPass.h:199-201, deferred to A6/A7-ML).
    bool warnedDlssFallback = false;

    // Valid only for the format the pipeline was created with: the swapchain can
    // switch between its formats when it is recreated.
    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhi::Format pipelineColorFormat = nvrhi::Format::UNKNOWN;

    // One entry per swapchain image, indexed by the acquired image index.
    std::vector<nvrhi::TextureHandle> swapchainTextures;
    std::vector<nvrhi::FramebufferHandle> swapchainFramebuffers;

    // Set when the pass cannot be used at all: the caller then has to keep the
    // old renderer instead of presenting an empty image.
    bool unavailable;
};

}
