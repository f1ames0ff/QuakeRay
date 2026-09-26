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

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/vulkan.h>

#include "../Common.h"
#include "../ResolutionState.h"

// RgFloat2D, the type of the jitter argument. UserFunction.h is the engine header that reaches the
// public `vkpt/vkpt.h`; the engine FSR header includes it for the same reason (FSR.h:24).
#include "../UserFunction.h"

namespace vkpt
{

class Framebuffers;
class RenderResolutionHelper;

namespace FidelityFX
{
class FSR;
}

namespace rhi
{
class RhiFrameContext;
}

// The A5.7 interop module of the FSR 3.1 upscaler: it records the engine's already-created
// `FidelityFX::FSR` upscale dispatch on the RHI command list, replacing the A4.5 TAAU
// (`RhiRtComposePass::RenderTaaU`) as the present's source while the frame's technique is FSR2 or
// FSR3 - the RHI counterpart of the legacy branch `VulkanDevice.cpp:1109-1131`.
//
// What it is, and why the engine object. `FidelityFX::FSR` is the open FidelityFX API
// (`ffx_api/ffx_api.h`, `ffx_upscale.h`, `vk/ffx_api_vk.h`, FSR.cpp:23-25) over a prebuilt signed
// Vulkan backend; FSR 2 and FSR 3.1 live in that one DLL and the version is selected by the
// provider query and `ffxOverrideVersion` (FSR.cpp:204-249, :312-314). Under `rhiframe` the engine
// object is created (VulkanDevice_Init.cpp:296-299) and subscribed to the framebuffers' size change
// (:367), so its FFX context, its render/upscale sizes and its jitter phase query already exist in
// the RHI frame. This module deliberately owns no context: `FSR::Apply` (FSR.h:48-58) is the exact
// legacy call, and reusing it keeps the provider selection, the sizes
// (`OnFramebuffersSizeChange`, FSR.cpp:185-202), the jitter phase (`GetJitter`, :444-480) and the
// FFX descriptor shapes in one place. A module-owned context would duplicate all of them and could
// diverge from the jitter the frame's uniform carries.
//
// The engine call and its four images (`FSR::Apply`, FSR.cpp:370-442; FSR.cpp:392-410):
//   color   FINAL (28)          B10G11R11_UFLOAT_PACK32  render size
//   depth   DEPTH_NDC (12)      R32_SFLOAT               render size
//   motion  MOTION_DLSS (31)    R16G16_SFLOAT            render size
//   output  UPSCALED_PONG (30)  B10G11R11_UFLOAT_PACK32  upscaled size
// The formats and flags are the generated `ShFramebuffers_Formats` / `ShFramebuffers_Flags` tables;
// none of the four is a swapped history image (`ShFramebuffers_Bindings` == `BindingsSwapped` for
// 12/28/30/31), so `Framebuffers::GetImageHandles` answers the same image for both slots. FINAL is
// written by the compose's checkerboard and prepare-final, DEPTH_NDC and MOTION_DLSS by the
// frame's primary raygen (`RaygenPrimary.hlsli:259`, :498); the module only wraps them.
//
// Interop discipline - the A5c §4.5 point 3 / `[UNKNOWN 2]` hazard, the heart of this module:
//  - The engine's `Apply` records its own barriers on the raw VkCommandBuffer it is given: forward,
//    GENERAL -> SHADER_READ_ONLY_OPTIMAL for the three inputs and GENERAL -> GENERAL for the
//    output, all four with `oldLayout = GENERAL` (FSR.cpp:88-133, called at :399); after the
//    `ffxDispatch` the backwards call (:439) returns the inputs to GENERAL and leaves the output
//    there. Both layouts it names are the engine's resting state, VK_IMAGE_LAYOUT_GENERAL, whose
//    NVRHI name is UnorderedAccess (vulkan-constants.cpp:242-245).
//  - NVRHI's automatic barriers are queued into a pending list and recorded on `commitBarriers()`
//    or on the next state set (nvrhi.h:3722-3731; vulkan-commandlist.cpp:296-304). A native call
//    recorded before that flush would execute its own barriers first, and the engine's
//    `oldLayout = GENERAL` would then describe a layout the image is not in - the pending
//    transitions from the earlier RHI passes (the compose's restore of FINAL/MOTION_DLSS, for
//    one) would be recorded *after* the FFX dispatch. `Render` therefore announces the engine's
//    resting state for the four wraps, queues a same-state UnorderedAccess requirement for each
//    (NVRHI's same-state UAV barrier, state-tracking.cpp:184-207; it maps to ALL_COMMANDS with
//    SHADER_READ|SHADER_WRITE access and the GENERAL layout, vulkan-constants.cpp:242-245) and
//    calls `commitBarriers()` before it takes the native command buffer. That is exactly the A4.2b
//    light-statistics mechanism (RhiRtDirectPass.cpp:1320-1332: "Into the fill's state and out of
//    the UnorderedAccess claim the wrap starts every list with, committed before the native fill:
//    a pending barrier would otherwise be flushed after it and the fill would run unordered
//    against the previous frame's raygen atomics it overwrites"), whose ranged `vkCmdFillBuffer`
//    is the same native-call shape on the same kind of wrap (RhiRtDirectPass.cpp:1298-1334).
//  - After a successful `Apply` the four images are physically GENERAL again and this module's
//    wraps claim UnorderedAccess, so no further correction is needed: the engine's backwards
//    barrier *is* the correction. The failure path - `Apply` answering FINAL - is repaired
//    explicitly, because its two sub-cases leave different states: with a context the forward
//    barrier was already recorded and `ffxDispatch` failed (the inputs are in
//    SHADER_READ_ONLY_OPTIMAL, FSR.cpp:430-437, and the backwards barrier that would have returned
//    them is skipped), without a context nothing was recorded at all (FSR.cpp:379-388). `Render`
//    tells them apart by the DLL-level provider query `IsUpscaleVersionAvailable` - no provider
//    means no context can have existed (RecreateContext returns before creating one) - declares
//    the inputs to be in the SHADER_READ_ONLY_OPTIMAL the forward barrier left them in when a
//    provider exists, and then requires UnorderedAccess on all four. That transition is recorded
//    by the next state set or at the list's close (vulkan-commandlist.cpp:74-80). A `HasContext`
//    accessor on `FidelityFX::FSR` would make the choice exact instead of inferred; this module
//    does not add one (the engine header is outside its ownership).
//  - The module keeps one wrap of each image per slot and announces the state on every list it
//    records, because the engine leaves the images in GENERAL but a native wrap keeps no state
//    between command lists (RhiTextureSource.h). The host has to leave the same invariant in place
//    between frames: whatever writes 30 or 29 last (the copy contract below included) has to
//    return them to UnorderedAccess on its own list.
//
// Inputs (all from the legacy call site, VulkanDevice.cpp:1122-1130):
//  - `renderResolution`: the engine helper `VulkanDevice::renderResolution`, the object that
//    produced the render and upscaled sizes the framebuffers and the uniform were built for. It is
//    the argument `Apply` takes (FSR.h:52); the module cannot synthesise it (its state is private
//    and `Setup` consumes the draw info), so the host passes it through;
//  - `jitterOffset`: the legacy apply site reads `{uniform->GetData()->jitterX, jitterY}`
//    (VulkanDevice.cpp:725). Under FSR those bytes were written from
//    `FidelityFX::FSR::GetJitter(renderResolution.GetResolutionState(), frameId)`
//    (VulkanDevice.cpp:186-188): the phase count comes from the FFX query
//    `ffxQueryDescUpscaleGetJitterPhaseCount` over (renderWidth, displayWidth), the phase index is
//    `frameId % phaseCount` (FSR.cpp:444-480), and `frameId` is the engine's frame counter
//    (`ShGlobalUniform::frameId`, VulkanDevice.cpp:172) - not `frameIndex` and not the
//    light-statistics rotation. The host passes `sky.jitter`, the same two floats;
//  - `timeDelta`: `uniform->GetData()->timeDelta` (VulkanDevice.cpp:1126; written at :173);
//  - `nearPlane` / `farPlane` / `fovVerticalRad`: `drawInfo.cameraNear`, `drawInfo.cameraFar`,
//    `drawInfo.fovYRadians` (VulkanDevice.cpp:1127-1129);
//  - `resetAccumulation`: the legacy camera-cut heuristic `|cameraPosition - cameraPositionPrev| >
//    100` (VulkanDevice.cpp:1114-1120). A framebuffer size change resets the accumulation inside
//    the engine object by itself (a new context, FSR.cpp:185-202), so the flag only has to carry
//    the cut; the host computes it from the same uniform bytes (`ShGlobalUniform::cameraPosition`
//    / `cameraPositionPrev`, the CPU-side pair at VulkanDevice.cpp:160-163).
// `preExposure` is 1.0f inside `Apply` (FSR.cpp:422), sharpening is off (:419-420) and HDR is on
// (:326), all engine-side and not arguments here.
//
// Output contract with the host (the wiring lives in the skeleton, not in this module):
//  - `Render` returns true only after a recorded `ffxDispatch` that answered UPSCALED_PONG: image
//    30 of the slot then holds the upscaled frame, and `GetOutputTexture(frameIndex)` is its wrap.
//    It returns false when the pass is not created, an input is missing or not sized as the
//    render/upscaled resolution, the engine FSR object has no context (the technique is not FSR,
//    or the DLL has no provider - `Apply` answers FINAL) or the dispatch failed. On false no
//    upscaled output was produced, and the host records the A4.5 `RhiRtComposePass::RenderTaaU`
//    path for that frame instead (the legacy `else` branch, VulkanDevice.cpp:1132-1136). The
//    technique selection itself is the host's, exactly the legacy shape
//    (`renderResolution.IsAmdFsr2Enabled() || IsAmdFsr3Enabled()`); this module reads no cvars;
//  - the legacy `BlitForEffects` copy that followed the upscale (VulkanDevice.cpp:1144) has no RHI
//    counterpart in this cut (no bloom/sharpen chain), and the engine's image shapes allow either
//    of two wirings:
//      (a) copy 30 -> 29 on the same list, so the 2D UI (`RhiUiPass`, which draws into
//          `RhiRtComposePass::GetUpscaledTexture`) and the present keep their target. NVRHI's
//          `copyTexture` queues the destination's CopyDest and the source's CopySource transition
//          and commits them itself (vulkan-texture.cpp:415-479); the host then has to move both
//          back to UnorderedAccess on that list before anything else uses them -
//          `setTextureState(29Wrap, AllSubresources, UnorderedAccess)` after the copy (before the
//          UI pass and the present, whose own announcements would otherwise overwrite the
//          CopyDest claim without a barrier) and `setTextureState(30Wrap, AllSubresources,
//          UnorderedAccess)` before the list closes, so the next frame's `Render` finds the engine
//          resting layout it announces. Both images carry the engine's TRANSFER usage
//          (ShFramebuffers_Flags[29/30]); the plan's A5.7 wiring is this shape
//          (nvrhi-renderer-refactor.md:3345-3346);
//      (b) sample 30 directly - the present's source and, if a UI is recorded, its target become
//          `GetOutputTexture(frameIndex)` and no copy is recorded; the UI's own
//          render-target/UnorderedAccess discipline (RhiUiPass.h) covers the state. The images
//          allow this too (30 is a colour attachment and a storage/sampled image), and it is the
//          recon's §4.5 point 4 shape.
//    Either way `RhiRtComposePass::RenderTaaU` is *not* called when `Render` returned true: both
//    techniques write the same upscaled output position, and the legacy code is an if/else.
//
// Host contract:
//  - `Create` takes the RHI device, the host's frame model and the engine FSR object; all three
//    are borrowed and have to outlive this module. It does not require an FSR provider to be
//    present in the DLL - the technique may be switched later and `SetUpscaleVersion` recreates
//    the context on the fly - so a missing provider surfaces as `Render` returning false;
//  - one `Render` per frame slot, on the frame context's open command list of the same slot, after
//    `RhiRtComposePass::Render` and instead of `RenderTaaU`, before the present - the legacy
//    position (VulkanDevice.cpp:1100-1136);
//  - call `ReleaseTargets()` before `Framebuffers::PrepareForSize` destroys the engine images
//    (next to `RhiRtComposePass::ReleaseTargets`); the next `Render` re-reads the handles and
//    re-wraps, so the pass survives a resize without a second `Create`.
//
// The pass is a no-op until Create succeeded and while an input is missing (no framebuffers, an
// image handle or extent the RHI cannot wrap, a zero render/upscaled size); every early return is
// quiet after the first warning. It is not thread-safe: each call uses the per-slot target of the
// frameIndex it is given, which is the engine's single-threaded per-slot frame model
// (RhiFrameContext).
//
// Not pulled in, deliberately: DLSS/NGX (`DLSS::Apply`, DLSS.cpp:351-426, has the same shape but
// needs `nvngx_dlss.dll` deployment, the app GUID and NVIDIA-only support - a written deferral to
// A6/A7-ML, a5c_overlay_recon.md §4.2/§4.5), FSR4, frame generation, ray regeneration and any
// D3D12 plumbing. The module changes nothing about the compose chain's internals; it consumes the
// FINAL/DEPTH_NDC/MOTION_DLSS images the chain and the primary left in the engine's GENERAL state.
class RhiFsrPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    RhiFsrPass();
    ~RhiFsrPass();

    RhiFsrPass(const RhiFsrPass &other) = delete;
    RhiFsrPass(RhiFsrPass &&other) noexcept = delete;
    RhiFsrPass &operator=(const RhiFsrPass &other) = delete;
    RhiFsrPass &operator=(RhiFsrPass &&other) noexcept = delete;

    // 'pDevice' is the RHI device; 'pFrameContext' is the host's frame model (RHI/RhiFrameContext.h)
    // that owns the per-slot command lists and the retire queue every replaced wrap goes through;
    // 'pFsr' is the engine's FidelityFX object (FSR.h), created and kept in sync with the
    // framebuffers by VulkanDevice_Init.cpp:296-299/:367 - it is taken non-const because
    // `FSR::Apply` is non-const. None of the three is owned; all have to outlive this object, and a
    // null or unusable one makes Create fail. The pass logs through 'pfnPrint'.
    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                FidelityFX::FSR *pFsr,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    // One call per frame, on the frame context's open command list of 'frameIndex', after the
    // compose's `Render` and instead of `RhiRtComposePass::RenderTaaU` of the same slot, before the
    // present. It (re)wraps the slot's four engine images when a handle or a size changed, performs
    // the state announcement and the barrier flush that the native FFX dispatch requires (the class
    // comment), takes the list's native VkCommandBuffer
    // (`getNativeObject(ObjectTypes::VK_CommandBuffer)`, vulkan-commandlist.cpp:50-58) and calls
    // `FidelityFX::FSR::Apply` with the caller's scene values.
    //
    // 'pFramebuffers' is the engine's framebuffer registry - the raw pointer every other RHI module
    // takes; internally it is borrowed as a non-owning shared_ptr alias for the engine call, which
    // documents the signature `Apply` has and never stores it (FSR.cpp:370-442 dereferences only).
    // 'renderResolution' and the scene values are the legacy apply-site arguments, with the sources
    // the class comment names; all of them have to be the frame's own (the values the uniform the
    // frame's raygen read was built from), or the upscale's history and reprojection disagree with
    // the traced frame.
    //
    // Returns true only when image 30 (UPSCALED_PONG) of this slot holds the result and
    // `GetOutputTexture(frameIndex)` may be used as the present's source; false when nothing was
    // recorded and the host has to fall back to the TAAU path (the class comment).
    bool Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                const Framebuffers *pFramebuffers,
                const RenderResolutionHelper &renderResolution,
                RgFloat2D jitterOffset,
                float timeDelta,
                float nearPlane,
                float farPlane,
                float fovVerticalRad,
                bool resetAccumulation);

    // The module's wrap of the engine's UPSCALED_PONG (30) of 'frameIndex', or null while that slot
    // has no prepared target (an engine image is missing or wrongly sized, a wrap failed, or
    // `ReleaseTargets` just ran). It is the image `FidelityFX::FSR::Apply` writes and the wrap
    // `GetOutputTexture`'s caller records the copy from (or samples directly). The handle is owned
    // by this pass; the caller must not release it, and it has to rebuild whatever set references
    // the pointer when it changes, because a re-created engine framebuffer or a size change
    // replaces the wrap. The state contract for the caller: after a successful `Render` the image
    // is in the engine's GENERAL layout, which NVRHI names UnorderedAccess and which this pass's
    // own wrap claims; the caller's copy or sample has to follow the state steps of the class
    // comment and leave the image in UnorderedAccess for the next frame.
    nvrhi::ITexture *GetOutputTexture(uint32_t frameIndex) const;

    // Drops every slot's four image wraps and retires them through the frame context's queue. The
    // caller has to call it before the engine destroys its framebuffer images (the
    // Framebuffers::PrepareForSize path) - otherwise the wraps reference destroyed VkImages. The
    // next Render re-reads the handles and re-wraps, so the pass survives a resize without a second
    // Create.
    void ReleaseTargets();

    // The four engine images of the module, in the order the slot stores their handles and sizes:
    // the three inputs of `FSR::Apply` and its output. The .cpp's table maps each to its
    // FramebufferImageIndex (FINAL 28, DEPTH_NDC 12, MOTION_DLSS 31, UPSCALED_PONG 30).
    enum ImageSlot : uint32_t
    {
        IMAGE_FINAL = 0,
        IMAGE_DEPTH_NDC = 1,
        IMAGE_MOTION = 2,
        IMAGE_OUTPUT = 3,
        IMAGE_COUNT = 4,
    };

private:
    // One entry per engine frame slot: the four engine images are wrapped per slot like every other
    // RHI module's framebuffer images, and the handles are kept in the form Render received them,
    // not as VkImages, because they are what the change detection compares. None of the four is a
    // swapped image, but the per-slot shape keeps the module uniform with its siblings and lets a
    // future engine-side swap land without a redesign.
    struct Target
    {
        uint64_t imageHandles[IMAGE_COUNT] = {};
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t upscaledWidth = 0;
        uint32_t upscaledHeight = 0;
        nvrhi::TextureHandle finalTexture;
        nvrhi::TextureHandle depthNdcTexture;
        nvrhi::TextureHandle motionTexture;
        nvrhi::TextureHandle outputTexture;
        bool valid = false;
    };

    // Resolves the slot's four engine image handles and their extents, validates them against
    // 'resolutionState', retires and re-wraps them when a handle or a size changed, and leaves
    // nothing on the command list (the state announcement is Render's). Returns false when an
    // image is missing, a format is not mappable, or an extent is not the one the image's size
    // class predicts.
    bool PrepareTarget(uint32_t frameIndex,
                       Target &target,
                       const Framebuffers &framebuffers,
                       const ResolutionState &resolutionState);

    // Retires the slot's four wraps through the frame context's queue and clears the slot. Used by
    // the size/handle-change path and by ReleaseTargets.
    void ReleaseTarget(Target &target);

    // The pre-dispatch state announcement and flush (the class comment's interop discipline): the
    // four wraps get the engine's resting UnorderedAccess declared, a same-state UnorderedAccess
    // requirement each (the same-state UAV barrier) and one `commitBarriers()` that records every
    // barrier NVRHI still holds pending on this list before the native FFX dispatch's own barriers.
    void AnnounceAndFlushStates(nvrhi::ICommandList *pCommandList, const Target &target) const;

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;

    // Not owned: the host's frame model and the engine's FidelityFX object, both outlive this
    // module. The FSR object is non-const because Apply is.
    rhi::RhiFrameContext *frameContext = nullptr;
    FidelityFX::FSR *fsr = nullptr;

    // One entry per engine frame slot (MAX_FRAMES_IN_FLIGHT, Common.h:31).
    Target targets[MAX_FRAMES_IN_FLIGHT];

    // One-shot warnings for the inputs that can legitimately be missing for a few frames or are a
    // permanent host-side mistake.
    bool warnedMissingFramebuffers = false;
    bool warnedMissingImages = false;
    bool warnedUnexpectedSize = false;
    bool warnedNoNativeCommandBuffer = false;
    bool warnedFailedDispatch = false;

    bool created = false;
};

}
