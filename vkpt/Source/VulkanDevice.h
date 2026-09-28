// Copyright (c) 2020-2021 Sultim Tsyrendashiev
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

#include <vkpt/vkpt.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common.h"

#include "CommandBufferManager.h"
#include "PhysicalDevice.h"
#include "Scene.h"
#include "Swapchain.h"
#include "Queues.h"
#include "GlobalUniform.h"
#include "RasterizedDataCollector.h"
#include "Framebuffers.h"
#include "MemoryAllocator.h"
#include "TextureManager.h"
#include "BlueNoise.h"
#include "Tonemapping.h"
#include "CubemapManager.h"
#include "UserFunction.h"
#include "DLSS.h"
#include "RenderResolutionHelper.h"
#include "DecalManager.h"
#include "FSR.h"
#include "FrameState.h"
#include "LibraryConfig.h"
#include "PortalList.h"
#include "WorldLights.h"
#include "ClusterLightLists.h"
#include "RayStats.h"

namespace vkpt
{

class NvrhiContext;
class NvrhiFrameSkeleton;
class RhiDecalPass;
class RhiFsrPass;
class RhiPostEffectPass;
class RhiProceduralSkyPass;
class RhiRasterOverlayPass;
class RhiRasterSkyPass;
class RhiRtComposePass;
class RhiRtDirectPass;
class RhiRtGodRaysPass;
class RhiRtIndirectPass;
class RhiRtPrimaryPass;
class RhiRtReflRefrPass;
class RhiShadowMapPass;
class RhiUiPass;
struct NvrhiRequirements;

namespace rhi
{
class RhiAccelStructs;
class RhiFrameContext;
class RhiTextureTable;
}

class VulkanDevice
{
public:
    explicit VulkanDevice(const RgInstanceCreateInfo *pInfo);
    ~VulkanDevice();


    VulkanDevice(const VulkanDevice& other) = delete;
    VulkanDevice(VulkanDevice&& other) noexcept = delete;
    VulkanDevice& operator=(const VulkanDevice& other) = delete;
    VulkanDevice& operator=(VulkanDevice&& other) noexcept = delete;


    void UploadGeometry(const RgGeometryUploadInfo *pUploadInfo);
    void UpdateGeometryTransform(const RgUpdateTransformInfo *pUpdateInfo);
    void UpdateGeometryTexCoords(const RgUpdateTexCoordsInfo *pUpdateInfo);

    void UploadRasterizedGeometry(const RgRasterizedGeometryUploadInfo *pUploadInfo,
                                  const float *pViewProjection, const RgViewport *pViewport);
    void UploadDecal(const RgDecalUploadInfo *pUploadInfo);
    void UploadPortal(const RgPortalUploadInfo *pUploadInfo);

    void SubmitStaticGeometries();
    void StartNewStaticScene();

    void UploadDirectionalLight(const RgDirectionalLightUploadInfo *pLightInfo);
    void UploadSphericalLight(const RgSphericalLightUploadInfo *pLightInfo);
    void UploadSpotlight(const RgSpotLightUploadInfo *pLightInfo);
    void UploadPolygonalLight(const RgPolygonalLightUploadInfo *pLightInfo);
    void UploadTexturedAreaLight(const RgTexturedAreaLightUploadInfo *pLightInfo);

    void UploadTexturedAreaLights(const RgTexturedAreaLightUploadInfo *pLightInfos, uint32_t count);

    void UploadClusterLightSources(const RgClusterLightSourcesUploadInfo *pInfo);

    void GetClusterLightStats(RgClusterLightStats *pStats);
    void GetClusterLightGrants(uint32_t *pGranted, uint32_t *pDenied, uint32_t maxCount, uint32_t *pCount);
    void GetClusterLightList(uint32_t cluster, uint64_t *pLightUniqueIds, uint32_t maxCount, uint32_t *pCount);

    void UploadWorldLights(const RgWorldLightsUploadInfo *pInfo);

    // Q2RTX-style fog volumes (used by the new Q2RTX core path)
    void SetFogVolumes(uint32_t count, const RgFogVolume *pVolumes);

    void CreateMaterial(const RgMaterialCreateInfo *pCreateInfo, RgMaterial *pResult);
    void CreateAnimatedMaterial(const RgAnimatedMaterialCreateInfo *pCreateInfo, RgMaterial *pResult);
    void ChangeAnimatedMaterialFrame(RgMaterial animatedMaterial, uint32_t frameIndex);
    void UpdateMaterial(const RgMaterialUpdateInfo *pUpdateInfo);
    void DestroyMaterial(RgMaterial material);

    void CreateSkyboxCubemap(const RgCubemapCreateInfo *pCreateInfo, RgCubemap *pResult);
    void DestroyCubemap(RgCubemap cubemap);


    void StartFrame(const RgStartFrameInfo *pStartInfo);
    void DrawFrame(const RgDrawFrameInfo *pFrameInfo);


    bool IsSuspended() const;
    bool IsRenderUpscaleTechniqueAvailable(RgRenderUpscaleTechnique technique) const;

    void GetFrameStats(uint32_t *pRays, uint32_t *pFpsX10) const;

    void GetFrameStatsEx(RgFrameStats *pStats) const;


    void Print(const char *pMessage) const;

private:
    void CreateInstance(const RgInstanceCreateInfo &info);
    void CreateDevice();
    void CreateNvrhiDevice();
    void CreateSyncPrimitives();
    static VkSurfaceKHR GetSurfaceFromUser(VkInstance instance, const RgInstanceCreateInfo &info);
    void ValidateCreateInfo(const RgInstanceCreateInfo *pInfo);

    void DestroyInstance();
    void DestroyDevice();
    void DestroySyncPrimitives();

    void FillUniform(ShGlobalUniform *gu, const RgDrawFrameInfo &drawInfo) const;

    VkCommandBuffer BeginFrame(const RgStartFrameInfo &startInfo);
    // Draws the current frame through the RHI layer and submits it, together with
    // the command buffer of the frame. The RHI frame skeleton is the only renderer
    // now, so an unavailable (or missing) skeleton is fatal: this throws instead of
    // handing the frame back. It returns false only for the skeleton's own defensive
    // refusals (a zero-sized swapchain, a stale swapchain framebuffer list), which
    // the caller ends without drawing. 'drawInfo' is the same struct Render
    // receives; only the sky params of it are read (the sky viewer position).
    bool RenderThroughRhi(const RgDrawFrameInfo &drawInfo);
    void EndFrame(VkCommandBuffer cmd);

private:
    VkInstance          instance;
    VkDevice            device;
    VkSurfaceKHR        surface;

    FrameState          currentFrameState;

    // incremented every frame
    uint32_t            frameId;

    VkFence             frameFences[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore         imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore         renderFinishedSemaphores[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore         inFrameSemaphores[MAX_FRAMES_IN_FLIGHT] = {};

    bool                waitForOutOfFrameFence;
    VkFence             outOfFrameFences[MAX_FRAMES_IN_FLIGHT] = {};

    std::shared_ptr<PhysicalDevice>         physDevice;
    std::shared_ptr<Queues>                 queues;
    std::shared_ptr<Swapchain>              swapchain;

    std::shared_ptr<MemoryAllocator>        memAllocator;

    std::shared_ptr<CommandBufferManager>   cmdManager;

    std::shared_ptr<Framebuffers>           framebuffers;

    std::shared_ptr<GlobalUniform>          uniform;
    std::shared_ptr<Scene>                  scene;

    std::shared_ptr<ShaderManager>          shaderManager;
    std::shared_ptr<RasterizedDataCollector> rasterizedDataCollector;
    std::shared_ptr<DecalManager>           decalManager;
    std::shared_ptr<PortalList>             portalList;
    std::shared_ptr<Tonemapping>            tonemapping;
    std::shared_ptr<RayStats>               rayStats;
    std::shared_ptr<FidelityFX::FSR>        amdFsr;
    std::shared_ptr<DLSS>                   nvDlss;

    std::shared_ptr<SamplerManager>         worldSamplerManager;
    std::shared_ptr<SamplerManager>         genericSamplerManager;
    std::shared_ptr<BlueNoise>              blueNoise;
    std::shared_ptr<TextureManager>         textureManager;
    std::shared_ptr<CubemapManager>         cubemapManager;

    // The RHI frame model (RHI/RhiFrameContext.h): one command list per engine frame slot plus the
    // retire queue that keeps a resource alive until the queue finished the submission that used it.
    // Created next to the NVRHI device, before the texture table (which retires through it) and the
    // frame skeleton; null if it could not be created.
    std::shared_ptr<rhi::RhiFrameContext>   rhiFrameContext;

    // The RHI copy of the engine's texture table (RHI/RhiTextureTable.h): created next to the NVRHI
    // device, shared with the sampler managers and the frame skeleton. Null if it could not be
    // created; the skeleton is then unavailable, which the frame dispatch treats as fatal.
    std::shared_ptr<rhi::RhiTextureTable>   rhiTextureTable;

    // World tables of the current map (clusters, PVS, emissive faces), built by the host.
    std::shared_ptr<WorldLights>            worldLights;
    // Per-cluster light lists, composed out of the registered sources and the tables above.
    std::shared_ptr<ClusterLightLists>      clusterLightLists;

    LibraryConfig::Config                   libconfig;
    VkDebugUtilsMessengerEXT                debugMessenger;
    std::unique_ptr<UserPrint>              userPrint;
    std::shared_ptr<UserFileLoad>           userFileLoad;

    // Names of the extensions the instance and the device were created with.
    // The RHI layer reads them to know which Vulkan features are available.
    std::vector<std::string>                enabledInstanceExtensions;
    std::vector<std::string>                enabledDeviceExtensions;

    // RHI device (NVIDIA NVRHI) created over the Vulkan device above.
    std::unique_ptr<NvrhiContext>           nvrhi;
    // The RHI acceleration structures (RHI/RhiAccelStructs.h): the NVRHI copy of the engine's
    // static BLAS plus one TLAS per frame slot, built from the engine's ASManager. Created next to
    // the skeleton and referenced by it; a null one makes the skeleton unavailable, which the frame
    // dispatch treats as fatal.
    std::shared_ptr<rhi::RhiAccelStructs>   rhiAccelStructs;
    // The RHI primary-visibility ray-tracing pass (RHI/RhiRtPrimaryPass.h): the real traced G-buffer
    // of A4.1, created with the other RHI passes and referenced by the skeleton. A null one makes
    // the skeleton unavailable, which the frame dispatch treats as fatal.
    std::shared_ptr<RhiRtPrimaryPass>       rhiRtPrimaryPass;
    // The RHI direct-lighting ray-tracing pass (RHI/RhiRtDirectPass.h): the light term of the traced
    // chain (A4.2), created next to the primary pass - it borrows the primary's shared layout
    // handles, so it has to be destroyed before it - and referenced by the skeleton. A null one
    // makes the skeleton unavailable, which the frame dispatch treats as fatal.
    std::shared_ptr<RhiRtDirectPass>        rhiRtDirectPass;
    // The RHI indirect / GI pass (RHI/RhiRtIndirectPass.h): the bounce-light term of the traced
    // chain (A4.3), created next to the direct pass - it borrows the primary's layout handles and
    // the direct pass's light set, so both have to outlive it and be destroyed after it - and
    // referenced by the skeleton. A null one makes the skeleton unavailable, which the frame
    // dispatch treats as fatal.
    std::shared_ptr<RhiRtIndirectPass>      rhiRtIndirectPass;
    // The RHI compose pass (RHI/RhiRtComposePass.h): the real adapter -> interleave -> exposure
    // histogram/average -> checkerboard -> prepare-final chain writing the display-referred FINAL
    // for the traced frame, referenced by the skeleton, which then presents its FINAL image. Null
    // when the creation failed; the traced chain then keeps the A4.2a diagnostic present.
    std::shared_ptr<RhiRtComposePass>       rhiRtComposePass;
    // The RHI shadow-map and god-rays passes of A5.2 (RHI/RhiShadowMapPass.h,
    // RHI/RhiRtGodRaysPass.h): the depth-only raster pass and the two compute dispatches that
    // produce the shafts CmPrepareFinal adds. Created with the other RT passes; the skeleton drives
    // them on the traced frame's list, and the god-rays pass takes the shadow map's texture and
    // sampler plus the blue-noise wrap. Null when the creation failed; the frame is then drawn
    // without shafts.
    std::shared_ptr<RhiShadowMapPass>       rhiShadowMapPass;
    std::shared_ptr<RhiRtGodRaysPass>       rhiRtGodRaysPass;

    // The RHI reflect/refract pass of A5.3 (RHI/RhiRtReflRefrPass.h): the Q2 raygen that overwrites
    // the G-buffer for reflective/refractive pixels and feeds the reflected god rays; it borrows the
    // primary's layout handles, so it is destroyed before it. Created with the other RT passes; the
    // skeleton drives it on the traced frame's list and the engine's portal buffers feed its set 9.
    // Null when the creation failed; the frame is then drawn without reflections.
    std::shared_ptr<RhiRtReflRefrPass>      rhiRtReflRefrPass;

    // The RHI procedural sky pass of A5.4 (RHI/RhiProceduralSkyPass.h): the default sky's cube
    // content, the `RenderCubemap::DrawProcedural` path of the legacy frame. It owns its two cube
    // images and the sampler; the primary, indirect and reflect/refract passes bind them in set 8.
    // Null when the creation failed; the passes then keep their 1x1 placeholders.
    std::shared_ptr<RhiProceduralSkyPass>   rhiProceduralSkyPass;

    // The RHI raster sky pass (RHI/RhiRasterSkyPass.h): the cube half of
    // SKY_TYPE_RASTERIZED_GEOMETRY, the ported `Rasterizer::DrawSkyToCubemap` ->
    // `RenderCubemap::Draw` pair. It writes the procedural sky pass's `renderCubemap` - the cube the
    // primary, indirect and reflect/refract passes sample in set 8 - from this frame's sky draw
    // list, so it borrows that pass's image and is destroyed before it. It binds the same geometry
    // wraps the sky pass receives. Null when the creation failed; the traced frame is then drawn
    // without the raster cube (its reflections keep the unwritten cube).
    std::shared_ptr<RhiRasterSkyPass>       rhiRasterSkyPass;

    // The RHI raster overlay pass of A5.5 (RHI/RhiRasterOverlayPass.h): the ported RsWorld pass over
    // the collector's DEFAULT list into FINAL/SCREEN_EMISSION, recorded inside the compose chain's
    // window, plus the ported RsSmoke half of master's smoke over the same window's smoke list.
    // Null when the creation failed; the frame is then drawn without the overlay.
    std::shared_ptr<RhiRasterOverlayPass>   rhiRasterOverlayPass;

    // The raster overlay's smoke list (A5.5): the frame's DEFAULT entries that carry
    // RG_RASTERIZED_GEOMETRY_STATE_SMOKE, filtered out of
    // RasterizedDataCollector::GetRasterDrawInfos() in RenderThroughRhi - the collector keeps no
    // separate smoke stream, the legacy uploads all puffs as one batch (r_smoke.c:358-376) - and
    // carried to the skeleton through SkyFrameInputs::smokeDraws. A member, not a local, so the
    // per-frame filter does not allocate.
    std::vector<RasterizedDataCollector::DrawInfo> smokeDraws;

    // The RHI decal pass of A5.6 (RHI/RhiDecalPass.h): the ported DecalManager::Draw into ALBEDO
    // right after the traced primary. The engine uploads no decals in this game, so the pass is a
    // runtime no-op kept for parity. Null when the creation failed; the frame is drawn without it.
    std::shared_ptr<RhiDecalPass>           rhiDecalPass;

    // The RHI FSR upscaler module of A5.7 (RHI/RhiFsrPass.h): drives the engine's own FidelityFX
    // FSR 3.1 context on the RHI list (the default upscaler), replacing the TAAU. Null when the
    // creation failed; the frame keeps the TAAU path.
    std::shared_ptr<RhiFsrPass>             rhiFsrPass;

    // The RHI post-upscale effect chain (RHI/RhiPostEffectPass.h): the legacy consumers of
    // `drawInfo.postEffectParams` (the colour tint variants, the chromatic aberration, the waves,
    // the radial blur, the wipe and the CRT) over the upscaled image pair, recorded before the UI
    // and - for the wipe/CRT half - after it. Null when the creation failed; the frame is then
    // drawn without the post-upscale effects.
    std::shared_ptr<RhiPostEffectPass>      rhiPostEffectPass;

    // The RHI 2D-UI pass (RHI/RhiUiPass.h): the game's SWAPCHAIN overlay (the HUD, the console, the
    // menus, the screen effects) drawn into the compose's upscaled image after the TAAU, created
    // with the other RHI passes and referenced by the skeleton. Null when the creation failed; the
    // frame is then drawn without the UI.
    std::shared_ptr<RhiUiPass>              rhiUiPass;
    // The RHI frame skeleton: the only renderer. Created unconditionally at startup; a null or
    // unavailable one makes the frame dispatch throw, because nothing else can draw the frame.
    std::shared_ptr<NvrhiFrameSkeleton>     nvrhiFrameSkeleton;

    // One-shot for the log when the skeleton refuses a frame for a defensive reason (a zero-sized
    // swapchain or a stale swapchain framebuffer list); the frame is then ended without drawing.
    bool                                    warnedSkeletonRefusedFrame = false;

    // Q2RTX-style fog volumes (host data, uploaded into the uniform each frame)
    std::array<RgFogVolume, RG_MAX_FOG_VOLUMES> fogVolumes{};
    uint32_t                                    fogVolumeCount = 0;

    bool                                    rayCullBackFacingTriangles;
    bool                                    allowGeometryWithSkyFlag;

    // The instance-wide applyVertexColorGamma of the rasterized geometry (RgInstanceCreateInfo), the
    // vertex spec constant the RHI sky pipelines bake into their key (RhiSkyPass::Render).
    bool                                    rasterizedVertexColorGamma;

    RenderResolutionHelper                  renderResolution;

    // Last upscale technique requested by the application; used to detect
    // actual FSR version switches (FSR 2 <-> FSR 3.1) without polling each frame.
    std::optional<RgRenderUpscaleTechnique> lastUpscaleTechnique;

    double                                  previousFrameTime;
    double                                  currentFrameTime;

    uint32_t                                statsRays = 0;
    uint32_t                                statsRaysPerCategory[RAY_STATS_CATEGORY_COUNT] = {};
    uint32_t                                statsFpsX10 = 0;
    float                                   statsSmoothedFps = 0.0f;
};

}
