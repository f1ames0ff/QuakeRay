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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/vulkan.h>

#include "../Common.h"
// QrDrawFramePostEffectsParams and the nine per-effect parameter structs of the engine's public
// header. UserFunction.h is the engine header that reaches `qray/qray.h`, the same include
// RhiFsrPass.h takes for QrFloat2D; the skeleton hands the frame's own params block over, so the
// module consumes the exact type the legacy frame consumes (VulkanDevice.cpp:1151-1223).
#include "../UserFunction.h"

namespace qray
{

class Framebuffers;

namespace rhi
{
class RhiFrameContext;
}

// The post-upscale effect chain of the traced frame: the RHI counterpart of the
// `QrDrawFrameInfo::postEffectParams` consumers of the legacy frame - the colour tint and its
// variants, the inverse-black-and-white / hue-shift / distorted-sides effects, the chromatic
// aberration, the underwater waves, the radial blur, the effect wipe and the CRT pair
// (VulkanDevice.cpp:1151-1223). Every effect is the engine's own compute blob, dispatched over the
// two upscaled images the upscaler left behind (29 UPSCALED_PING and 30 UPSCALED_PONG), with the
// same push-constant bytes, the same `isSourcePing` specialization and the same transition
// bookkeeping the engine's `EffectSimple`/`EffectWipe` host classes keep.
//
// What the legacy frame records, in order, and what this module ported:
//   1.  `effectColorTint`           EfColorTint.comp.spv             :1166-1169
//   2.  `effectInverseBW`           EfInverseBW.comp.spv             :1170-1173
//   3.  `effectHueShift`            EfHueShift.comp.spv              :1174-1177
//   4.  `effectChromaticAberration` EfChromaticAberration.comp.spv  :1178-1181
//   5.  `effectDistortedSides`      EfDistortedSides.comp.spv        :1182-1185
//   6.  `effectWaves`               EfWaves.comp.spv                 :1186-1189
//   7.  `effectRadialBlur`          EfRadialBlur.comp.spv            :1190-1193
//       [the legacy frame then draws the swapchain overlay, `Rasterizer::DrawToSwapchain` :1199-1209]
//   8.  `effectWipe`                EfWipe.comp.spv                  :1212-1215
//   9.  `effectCrtDemodulateEncode` EfCrtDemodulateEncode.comp.spv  :1218-1219
//       `effectCrtDecode`           EfCrtDecode.comp.spv             :1221-1222
//
// The two entry points split the chain the way the legacy does: `Render` records effects 1-7 before
// the 2D UI, `RenderPostUi` records the wipe and the CRT after it. The split is the legacy order,
// not an accident: the wipe and the CRT are applied after `DrawToSwapchain` because they "work on
// swapchain geometry too" (VulkanDevice.cpp:1211) - the HUD is part of their input - while the
// first seven run over the upscaled image alone. The skeleton calls `Render` between the
// TAAU/FSR step and the UI block and `RenderPostUi` after the UI block, both on the frame's open
// list of the same slot.
//
// The blobs and their descriptor sets (measured with `spirv-dis` over the shipped
// `renderer\Build\Ef*.comp.spv`; every effect is `[numthreads(16, 16, 1)]`):
//   - set 0, the framebuffer storage images: the two upscaled images at raw binding 29
//     (`framebufUpscaledPing`) and 30 (`framebufUpscaledPong`) as `RWTexture2D`. The wipe adds
//     `framebufWipeEffectSource` (image 72) at raw binding 72. EfInverseBW and EfHueShift add the
//     sampled ALBEDO at raw binding 124 (`framebufAlbedo_Sampled`, a `Texture2D`).
//   - set 1, the engine's global uniform `ConstantBuffer<ShGlobalUniform>` at raw binding 0 - every
//     effect except EfCrtDecode declares it; the shaders read `time`, `frameId` and the render size
//     from it exactly as the legacy descriptor sets provide.
//   The module builds per-effect layouts with the engine's raw bindings as NVRHI slots (UAV offset
//   0, SRV offset 124, constant-buffer offset 0), the mechanism RhiRtComposePass and
//   RhiRtGodRaysPass use.
//
// One deliberate difference from the legacy host: the wipe's `EffectWipe` builds a three-set
// pipeline layout and binds the engine's blue noise at its set 2 (EffectWipe.h:47-53, :137-147),
// but the shipped `EfWipe.comp.spv` declares no set 2 (measured with `spirv-dis`): its
// `DESC_SET_RANDOM` reaches only the hash-based `rnd16` through `effect_getRandomSample`
// (EfCommon.hlsli:115-121), the `blueNoiseTextures` image `Random.hlsli` declares is never loaded,
// and the compiler removes it. This module builds the blob's own two-set shape, so it needs no
// engine blue-noise object at all.
//
// The push constants are the legacy host's structs, byte for byte (EffectSimple.h:115-122,
// EffectSimple_Instances.h, EffectWipe.h:31-37): `transitionType` (0 = fading in, 1 = fading out;
// `EfSimple.inl`'s getProgress returns `1 - progress` for 1), `transitionBeginTime`,
// `transitionDuration`, then the effect's custom members at offset 12:
//   - color tint: 4 floats (intensity, r, g, b)         block 28
//   - waves:      3 floats (amplitude, speed, multX)    block 24
//   - chromatic:  1 float  (intensity)                  block 16
//   - the effects with no custom members (inverseBW, hueShift, distortedSides, radialBlur, CRT):
//     the legacy host's struct carries an empty member that pads the block to 16 bytes, which is
//     what the pass pushes too; the shader declares the first 12 bytes only.
//   - the wipe: its own 16-byte `WipePush_BT` (stripWidthInPixels, startFrameId, beginTime,
//     endTime) without any transition member.
// The CRT blobs' push blocks are optimized out (their entry points declare no push constant), so
// the bytes the legacy host pushes for them are unread; the pass still mirrors the host's push to
// keep the pipeline-layout shape identical.
//
// The transition state is the legacy `EffectSimple::Setup` logic, kept per effect in this module:
// a rising `isActive` starts an "in" fade with the params' `transitionDurationIn` at the frame's
// time, a falling edge starts an "out" fade with `transitionDurationOut`, and the effect is
// dispatched while it is active or while an out-fade is still running. The early-outs are the
// legacy ones: a null params pointer turns the effect off (no fade), a chromatic intensity <= 0 or
// a waves amplitude <= 0 does the same (EffectSimple_Instances.h:61, :210), and the CRT pair runs
// only while `pCRT != nullptr && pCRT->isActive`. The time the module book-keeps with is the
// uniform's `time` (`ShGlobalUniform::time`), the same float the shaders compare their
// `transitionBeginTime` against (`EfSimple.inl:43-59`), which is what the legacy host's
// `(float)currentFrameTime` is filled from (VulkanDevice.cpp:174, :1151).
//
// The target and the state contract: the effects ping-pong between images 29 and 30 - each
// dispatch reads the current one and writes the other, exactly the engine's
// `EffectBase::Dispatch` return-a-flipped-index scheme. The module wraps both images per slot,
// announces the engine's resting state (VK_IMAGE_LAYOUT_GENERAL, NVRHI's UnorderedAccess) on every
// list and requires UnorderedAccess on the image each dispatch just wrote, so the next dispatch's
// read is ordered (the same-state UAV barrier the engine's per-dispatch `BarrierMultiple` would
// be). The chain always leaves its result in image 29, because that is what the 2D UI draws into
// and what the present samples (`RhiRtComposePass::GetUpscaledTexture`): when an odd number of
// dispatches ends with the result in image 30, the module copies 30 -> 29 and moves both images
// back to UnorderedAccess - the same copy-and-restore pair the skeleton records after the FSR
// upscale (NvrhiFrameSkeleton.cpp:1001-1007, RhiFsrPass.h documents why). The two sampled-ALBEDO
// effects move ALBEDO to the shader-resource state for their dispatch and the module restores it
// to UnorderedAccess afterwards, so the next frame's primary UAV write finds the layout it
// announces.
//
// The wipe is inert in this game, and the module keeps its path documented: the game passes no
// `pWipe` (gl_vidsdl.c:2147-2154 fills none) and no `effectWipeIsUsed` (the `QrInstanceCreateInfo`
// aggregate never sets it, so image 72 stays 1x1, Framebuffers.cpp:631-637), which is exactly the
// legacy's double gate (EffectWipe.h:56-68). The module ports the wipe's timing, early-outs,
// push-constant state and dispatch; the one thing it does not port is the `beginNow` step that
// captures the previous swapchain image into image 72 (EffectWipe.h:86-132), because the module is
// never given the skeleton's swapchain wraps. A `beginNow` request is skipped with a one-shot
// warning instead; with the game's params the branch is unreachable.
//
// What the module deliberately does not record: the separate `Bloom` and `Sharpening` classes
// (`rt_bloom` and `rt_sharpen`, both default 0 in this game, VulkanDevice.cpp:1153-1165) - a
// recorded deferral; DLSS (out of scope, as in RhiFsrPass); and the legacy `BlitForEffects` step
// that precedes the chain (VulkanDevice.cpp:1144), whose upscale-sized copy the RHI path already
// performs where it is needed (the FSR 30 -> 29 copy) and whose `pPixelizedRenderSize` downscale
// the RHI path does not implement yet.
//
// Host contract:
//  - `Create` takes the RHI device, the host's frame model and the shader folder. `wipeIsUsed` is
//    the library-config flag the legacy `EffectWipe` takes; the wipe dispatch is gated on it, so
//    this game's false keeps the wipe inert even if a caller ever set `pWipe`.
//  - the host records `Render` once per traced frame between the upscaler (the FSR copy or
//    `RhiRtComposePass::RenderTaaU`) and the UI, and `RenderPostUi` after the UI and before the
//    present, both on the frame context's open list of the same slot, with the same extent and
//    uniform arguments.
//  - `ReleaseTargets` drops every per-slot wrap and set; call it before `Framebuffers::
//    PrepareForSize` destroys the engine images, next to the other passes' `ReleaseTargets`.
//
// The pass is a no-op until Create succeeded and while an input is missing (no framebuffers, no
// uniform, a zero or mismatched upscaled size); every early return is quiet after the first
// warning. It is not thread-safe: each call uses the per-slot target of the frameIndex it is
// given, which is the engine's single-threaded per-slot frame model (RhiFrameContext).
class RhiPostEffectPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    RhiPostEffectPass();
    ~RhiPostEffectPass();

    RhiPostEffectPass(const RhiPostEffectPass &other) = delete;
    RhiPostEffectPass(RhiPostEffectPass &&other) noexcept = delete;
    RhiPostEffectPass &operator=(const RhiPostEffectPass &other) = delete;
    RhiPostEffectPass &operator=(RhiPostEffectPass &&other) noexcept = delete;

    // 'pDevice' is the RHI device; 'pFrameContext' is the host's frame model
    // (RHI/RhiFrameContext.h) that owns the retire queue every replaced wrap and set goes through.
    // 'pShaderFolderPath' is the folder the engine blobs are loaded from, with the trailing
    // separator; the thirteen `Ef*.comp.spv` effect files (and, when 'wipeIsUsed' is set, the
    // wipe's blobs) are read from it. 'wipeIsUsed' is the engine instance's effectWipeIsUsed. None
    // is owned.
    // Returns false and leaves the pass unusable if a binding layout, a shader or a pipeline of the
    // nine non-wipe effects cannot be created; the wipe's own pieces are soft failures that log a
    // warning and leave the wipe unavailable.
    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                const char *pShaderFolderPath,
                bool wipeIsUsed,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    // The pre-UI half of the chain (effects 1-7 of the class comment): one call per traced frame,
    // on the frame context's open command list of 'frameIndex', after the upscaler wrote image 29
    // and before the UI draws into it. 'pFramebuffers' is the engine's framebuffer registry;
    // 'renderWidth'/'renderHeight' are the render resolution (the sampled ALBEDO wrap's extent)
    // and 'upscaledWidth'/'upscaledHeight' the engine's upscaled size (the dispatch extent and the
    // extent of images 29/30). 'currentTime' is the uniform's `time`, 'pUniformBuffer' the
    // engine's global uniform as the static constant-buffer wrap the world/compose passes take,
    // and 'params' the frame's `drawInfo.postEffectParams` block. All of it is borrowed for the
    // duration of the call.
    void Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                const Framebuffers *pFramebuffers,
                uint32_t renderWidth,
                uint32_t renderHeight,
                uint32_t upscaledWidth,
                uint32_t upscaledHeight,
                float currentTime,
                nvrhi::IBuffer *pUniformBuffer,
                const QrDrawFramePostEffectsParams &params);

    // The post-UI half of the chain (effects 8-9): one call per traced frame, on the same list,
    // after the UI block and before the present. The arguments are `Render`'s and have to be the
    // same values; 'frameId' is the engine's frame counter (`VulkanDevice::frameId`), the value
    // the legacy wipe carries as its `startFrameId` (VulkanDevice.cpp:1212). The call is a no-op
    // while no wipe and no CRT is requested; the image the present samples is left in 29 either
    // way, and the two images are left in UnorderedAccess.
    void RenderPostUi(nvrhi::ICommandList *pCommandList,
                      uint32_t frameIndex,
                      const Framebuffers *pFramebuffers,
                      uint32_t renderWidth,
                      uint32_t renderHeight,
                      uint32_t upscaledWidth,
                      uint32_t upscaledHeight,
                      float currentTime,
                      nvrhi::IBuffer *pUniformBuffer,
                      const QrDrawFramePostEffectsParams &params,
                      uint32_t frameId);

    // Drops every slot's image wraps and the sets over them, and retires them through the frame
    // context's queue. The caller has to call it before the engine destroys its framebuffer images
    // (the Framebuffers::PrepareForSize path) - otherwise the wraps reference destroyed VkImages.
    // The next entry point re-reads the handles and re-wraps, so the pass survives a resize
    // without a second Create.
    void ReleaseTargets();

private:
    // The chain's effects, in the legacy recording order (the class comment's table). The two CRT
    // steps are separate ids because they are separate blobs with separate pipelines, and the wipe
    // is one because its push block is its own.
    enum EffectId : uint32_t
    {
        EFFECT_COLOR_TINT = 0,
        EFFECT_INVERSE_BW,
        EFFECT_HUE_SHIFT,
        EFFECT_CHROMATIC_ABERRATION,
        EFFECT_DISTORTED_SIDES,
        EFFECT_WAVES,
        EFFECT_RADIAL_BLUR,
        EFFECT_WIPE,
        EFFECT_CRT_DEMODULATE_ENCODE,
        EFFECT_CRT_DECODE,
        EFFECT_SHARPEN,
        EFFECT_GAMEPLAY_FEEDBACK,
        EFFECT_VIGNETTE,
        EFFECT_FILM_GRAIN,
        EFFECT_COUNT,
    };

    // One engine image of a slot's target, in the order the slot stores it.
    enum ImageSlot : uint32_t
    {
        IMAGE_PING = 0,      // 29 UPSCALED_PING  - the upscaler's output, the present's source
        IMAGE_PONG,          // 30 UPSCALED_PONG  - the ping-pong partner and the FSR output
        IMAGE_ALBEDO,        // 0  ALBEDO         - the sampled read of inverseBW/hueShift
        IMAGE_WIPE_SOURCE,   // 72               - the wipe's capture source (only while wipeIsUsed)
        IMAGE_COUNT,
    };

    // The legacy `EffectSimple::Setup` state, per effect (EffectSimple.h:58-81).
    struct Transition
    {
        bool isCurrentlyActive = false;
        uint32_t transitionType = 0; // 0 - in, 1 - out
        float transitionBeginTime = 0.0f;
        float transitionDuration = 0.0f;
    };

    // One effect of the chain: the loaded blob, the two `isSourcePing` specializations (index 0
    // reads the pong role, index 1 the ping role - the legacy `pipelines[isSourcePing]`), the two
    // pipelines over them and the transition state.
    struct Effect
    {
        nvrhi::ShaderHandle shader;
        nvrhi::ShaderHandle specializedShaders[2];
        nvrhi::ComputePipelineHandle pipelines[2];
        Transition transition;
    };

    // One engine frame slot: the wraps of the four images and the sets over them. The two upscaled
    // images are not swapped engine images, but the slot's shape is per-slot like every sibling
    // module's and the ALBEDO wrap follows the engine's swap permutation. The handles are what
    // Render received, not VkImages, because they are what the change detection compares.
    struct Target
    {
        uint64_t imageHandles[IMAGE_COUNT] = {};
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t upscaledWidth = 0;
        uint32_t upscaledHeight = 0;
        nvrhi::TextureHandle textures[IMAGE_COUNT];

        // Set 0 of the three shapes: the ping/pong pair, the pair plus the sampled ALBEDO, and the
        // pair plus the wipe source.
        nvrhi::BindingSetHandle simpleSet;
        nvrhi::BindingSetHandle albedoSet;
        nvrhi::BindingSetHandle wipeSet;

        // Set 1: the engine's global uniform. Kept per slot with the rest under the same
        // pointer-change rule the sibling modules use.
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::BindingSetHandle uniformSet;
    };

    bool LoadShader(const char *pFileName, nvrhi::ShaderHandle &result);

    // The shared preamble of the two entry points: validates the arguments, resolves the slot's
    // four image handles and extents, retires and re-wraps them when the engine re-created its
    // framebuffers or the resolution changed, and prepares the sets. Returns the slot's target, or
    // null when the call cannot be recorded - which the caller treats as a silent skip.
    Target *PrepareFrame(uint32_t frameIndex,
                         const Framebuffers *pFramebuffers,
                         uint32_t renderWidth,
                         uint32_t renderHeight,
                         uint32_t upscaledWidth,
                         uint32_t upscaledHeight,
                         nvrhi::IBuffer *pUniformBuffer);

    bool PrepareFramebufferSets(Target &target);
    bool PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer);

    // The legacy `EffectSimple::Setup` (EffectSimple.h:58-81): updates 'effect's transition state
    // and answers whether the effect has to be dispatched on this frame.
    bool SetupSimple(EffectId effect, float currentTime, bool isActive,
                     float transitionDurationIn, float transitionDurationOut);

    // The legacy `SetupNull` (EffectSimple.h:52-56): a null params pointer turns the effect off
    // without a fade-out.
    void SetupNull(EffectId effect);

    // One `setComputeState` + `setPushConstants` + `dispatch` of one effect, with the sets of the
    // effect's measured shape and the engine's dispatch formula, followed by the same-state
    // UnorderedAccess requirement on the image the dispatch wrote. Returns false (and records
    // nothing) when an input of the effect is missing.
    bool DispatchEffect(nvrhi::ICommandList *pCommandList,
                        Target &target,
                        EffectId effect,
                        bool sourceIsPing,
                        const void *pPushConstant,
                        uint32_t pushConstantSize,
                        uint32_t groupsX,
                        uint32_t groupsY);

    // The legacy wipe's `Setup` (EffectWipe.h:56-135) and its dispatch, ported together because
    // the push state persists across frames. Returns true when a wipe dispatch was recorded -
    // which flips the image roles like every other effect.
    bool RecordWipe(nvrhi::ICommandList *pCommandList,
                    Target &target,
                    const QrPostEffectWipe &params,
                    uint32_t upscaledWidth,
                    uint32_t upscaledHeight,
                    float currentTime,
                    uint32_t currentFrameId,
                    bool sourceIsPing);

    // The engine's resting layout for one of the slot's images: announces UnorderedAccess and
    // queues the same-state requirement (NVRHI's same-state UAV barrier), the pair RhiRtComposePass
    // documents for its per-iteration hand-offs.
    void AnnounceImage(nvrhi::ICommandList *pCommandList, Target &target, ImageSlot image) const;

    // The 30 -> 29 copy of the odd-dispatch case, with both images restored to UnorderedAccess -
    // the pair the skeleton records after the FSR upscale.
    void CopyPongToPing(nvrhi::ICommandList *pCommandList, Target &target) const;

    void ReleaseFramebufferSets(Target &target);
    void ReleaseTarget(Target &target);

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;

    // Not owned: the host's frame model, which outlives this module.
    rhi::RhiFrameContext *frameContext = nullptr;

    // The library-config gate of the wipe (EffectWipe.h:44-45) and whether the wipe's soft pieces
    // could be created.
    bool wipeIsUsed = false;
    bool wipeAvailable = false;

    // The fourteen effects; every one but the wipe is a hard requirement of Create.
    Effect effects[EFFECT_COUNT];

    // The layouts of the module: the three set-0 shapes, the uniform set and the five
    // push-constant layouts the effects' block sizes call for (16, 20, 24, 28 and 44 bytes). Every
    // layout is Compute visibility and the pinned backend keeps the order the pipelines add them in,
    // which is the shader's set numbering.
    nvrhi::BindingLayoutHandle simpleFramebufferLayout;
    nvrhi::BindingLayoutHandle albedoFramebufferLayout;
    nvrhi::BindingLayoutHandle wipeFramebufferLayout;
    nvrhi::BindingLayoutHandle uniformLayout;
    nvrhi::BindingLayoutHandle pushConstant16Layout;
    nvrhi::BindingLayoutHandle pushConstant20Layout;
    nvrhi::BindingLayoutHandle pushConstant24Layout;
    nvrhi::BindingLayoutHandle pushConstant28Layout;
    nvrhi::BindingLayoutHandle pushConstant44Layout;

    // The wipe's persistent push state, exactly the legacy `EffectWipe::push` member: begin/end
    // and the start frame survive across frames until a new `beginNow` overwrites them. The layout
    // is the blob's `WipePush_BT` (EffectWipe.h:31-37): no transition member, the fields at 0/4/8/12.
    struct WipePush
    {
        uint32_t stripWidthInPixels = 0;
        uint32_t startFrameId = 0;
        float beginTime = 0.0f;
        float endTime = 0.0f;
    } wipePush;
    static_assert(sizeof(WipePush) == 16);
    static_assert(offsetof(WipePush, stripWidthInPixels) == 0);
    static_assert(offsetof(WipePush, startFrameId) == 4);
    static_assert(offsetof(WipePush, beginTime) == 8);
    static_assert(offsetof(WipePush, endTime) == 12);

    // One entry per engine frame slot (MAX_FRAMES_IN_FLIGHT, Common.h:31).
    Target targets[MAX_FRAMES_IN_FLIGHT];

    // One-shot warnings for the inputs that can legitimately be missing for a few frames or are a
    // permanent host-side mistake.
    bool warnedMissingFramebuffers = false;
    bool warnedMissingUniform = false;
    bool warnedUnexpectedSize = false;
    bool warnedFailedSet = false;
    bool warnedMissingAlbedo = false;
    bool warnedMissingWipe = false;
    bool warnedWipeCapture = false;

    bool created = false;
};

}
