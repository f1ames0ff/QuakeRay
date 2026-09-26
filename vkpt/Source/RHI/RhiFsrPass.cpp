#include "RhiFsrPass.h"

#include "RhiFrameContext.h"
#include "RhiTextureSource.h"

#include "../FSR.h"
#include "../Framebuffers.h"
#include "../Generated/ShaderCommonCFramebuf.h"
#include "../RenderResolutionHelper.h"

#include <cstring>
#include <memory>
#include <string>

using namespace vkpt;

namespace
{

void LogMessage(const RhiFsrPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

// The engine images of the module, in the slot order of RhiFsrPass::ImageSlot: the three inputs of
// `FidelityFX::FSR::Apply` and the output it writes (FSR.cpp:392-410). The sizes come from the
// generated flags: 12/28/31 are render-sized, 30 carries UPSCALED_SIZE
// (ShFramebuffers_Flags[30], Generated/ShaderCommonCFramebuf.cpp), and the module checks exactly
// that split in PrepareTarget.
constexpr uint32_t FSR_IMAGE_COUNT = 4;
constexpr FramebufferImageIndex FSR_IMAGES[FSR_IMAGE_COUNT] =
{
    FB_IMAGE_INDEX_FINAL,          // 28  color   B10G11R11_UFLOAT_PACK32  render size
    FB_IMAGE_INDEX_DEPTH_NDC,      // 12  depth   R32_SFLOAT               render size
    FB_IMAGE_INDEX_MOTION_DLSS,    // 31  motion  R16G16_SFLOAT            render size
    FB_IMAGE_INDEX_UPSCALED_PONG,  // 30  output  B10G11R11_UFLOAT_PACK32  upscaled size
};
static_assert(FSR_IMAGE_COUNT == 4, "RhiFsrPass::Target is sized by the four images");
static_assert(FSR_IMAGES[RhiFsrPass::IMAGE_OUTPUT] == FB_IMAGE_INDEX_UPSCALED_PONG,
              "the module's output slot has to be the image FSR::Apply writes");

const char *const FSR_IMAGE_DEBUG_NAMES[FSR_IMAGE_COUNT] =
{
    "FINAL",
    "DEPTH_NDC",
    "MOTION_DLSS",
    "UPSCALED_PONG",
};

}

RhiFsrPass::RhiFsrPass() = default;

RhiFsrPass::~RhiFsrPass()
{
    if (device != nullptr)
    {
        // The wraps reference engine images; the host destroys the pass while it can still idle the
        // device (VulkanDevice does that before the skeleton as well), so nothing has to go through
        // a retire queue here.
        device->waitForIdle();
    }

    for (Target &target : targets)
    {
        target.finalTexture = nullptr;
        target.depthNdcTexture = nullptr;
        target.motionTexture = nullptr;
        target.outputTexture = nullptr;
        target.valid = false;
    }
}

bool RhiFsrPass::Create(nvrhi::IDevice *pDevice,
                        rhi::RhiFrameContext *pFrameContext,
                        FidelityFX::FSR *pFsr,
                        PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    device = pDevice;
    print = std::move(pfnPrint);
    frameContext = pFrameContext;
    fsr = pFsr;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the FSR pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the FSR pass needs the frame context of the RHI layer");
        return false;
    }

    if (fsr == nullptr)
    {
        LogMessage(print, "Warning: RHI: the FSR pass needs the engine's FidelityFX FSR object");
        return false;
    }

    // No Vulkan object of its own: the FFX context, the render/upscale sizes and the jitter phase
    // belong to the engine object, and a missing FSR provider in the DLL only surfaces when Apply
    // is called (the technique may be switched after Create).
    created = true;
    return true;
}

bool RhiFsrPass::Render(nvrhi::ICommandList *pCommandList,
                        uint32_t frameIndex,
                        const Framebuffers *pFramebuffers,
                        const RenderResolutionHelper &renderResolution,
                        RgFloat2D jitterOffset,
                        float timeDelta,
                        float nearPlane,
                        float farPlane,
                        float fovVerticalRad,
                        bool resetAccumulation)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }

    if (pFramebuffers == nullptr)
    {
        if (!warnedMissingFramebuffers)
        {
            warnedMissingFramebuffers = true;
            LogMessage(print, "Warning: RHI: the FSR pass got no engine framebuffers, the upscale is skipped");
        }
        return false;
    }

    // The resolution the host sized the framebuffers and the uniform for. `GetResolutionState` is
    // the same four values `FSR::Apply` receives through the helper (FSR.cpp:404-418); a zero one
    // means the frame has no usable size yet and neither the images nor the dispatch can exist.
    const ResolutionState resolutionState = renderResolution.GetResolutionState();
    if (resolutionState.renderWidth == 0 || resolutionState.renderHeight == 0 ||
        resolutionState.upscaledWidth == 0 || resolutionState.upscaledHeight == 0)
    {
        return false;
    }

    Target &target = targets[frameIndex];
    if (!PrepareTarget(frameIndex, target, *pFramebuffers, resolutionState))
    {
        return false;
    }

    // The interop discipline (the class comment): declare the engine's resting state for all four
    // wraps, queue the same-state UAV requirement for each and flush every barrier NVRHI still
    // holds pending on this list - before the native FFX call records its own barriers.
    AnnounceAndFlushStates(pCommandList, target);

    // The open VkCommandBuffer of the very list NVRHI records to (vulkan-commandlist.cpp:50-58).
    // Fetched after the flush so the engine's own barriers are recorded after every NVRHI barrier
    // of the frame.
    const VkCommandBuffer nativeCmdBuffer = static_cast<VkCommandBuffer>(
        pCommandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);

    if (nativeCmdBuffer == VK_NULL_HANDLE)
    {
        if (!warnedNoNativeCommandBuffer)
        {
            warnedNoNativeCommandBuffer = true;
            LogMessage(print, "Warning: RHI: the FSR pass got no native command buffer, the upscale is skipped");
        }
        return false;
    }

    // `FSR::Apply` takes `const std::shared_ptr<Framebuffers>&` although it only dereferences it -
    // it reads the images through `GetImageHandles`/`GetImage` and never stores or copies the
    // pointer (FSR.cpp:399-439) - so the module passes a non-owning alias of the registry it was
    // handed: the aliasing constructor of an empty shared_ptr allocates no control block and
    // carries no ownership, and the engine object stays alive through the host for the call's
    // duration.
    const std::shared_ptr<Framebuffers> framebuffersAlias{
        std::shared_ptr<Framebuffers>{}, const_cast<Framebuffers *>(pFramebuffers)};

    const FramebufferImageIndex output = fsr->Apply(
        nativeCmdBuffer, frameIndex, framebuffersAlias, renderResolution,
        jitterOffset, timeDelta, nearPlane, farPlane, fovVerticalRad, resetAccumulation);

    if (output != FB_IMAGE_INDEX_UPSCALED_PONG)
    {
        // `Apply` answered FINAL, which happens in two sub-cases that its return value cannot tell
        // apart:
        //  (a) the engine object has no context (the technique is not FSR, or the DLL has no
        //      provider - FSR.cpp:379-388): nothing was recorded, all four images are still in the
        //      GENERAL this module announced;
        //  (b) the context existed and `ffxDispatch` failed (FSR.cpp:430-437): the forward barrier
        //      was already recorded, so the three inputs are in SHADER_READ_ONLY_OPTIMAL and the
        //      output in GENERAL, and the engine's early return skips the backwards barrier that
        //      would have returned them.
        // The DLL-level provider query separates the two: with no provider at all no context can
        // have existed (RecreateContext returns before creating one), while a provider plus the
        // host's technique gate means the context existed and the forward barrier ran. In (b) the
        // inputs are declared to be where the engine left them and the UnorderedAccess requirement
        // then queues their return to GENERAL; in (a) everything is a same-state no-op. The queued
        // transitions are recorded by the next state set - the TAAU fallback's dispatch, the
        // present's - or at the list's close (vulkan-commandlist.cpp:74-80). The cost of the
        // assumption is the mis-wiring case (the host calls Render although the technique is not
        // FSR, with a provider still in the DLL): the declaration would then be wrong; the
        // class comment's host contract forbids that call, and a `HasContext` accessor on
        // `FidelityFX::FSR` would make the choice exact instead of inferred.
        nvrhi::ITexture *const inputs[3] =
        {
            target.finalTexture.Get(),
            target.depthNdcTexture.Get(),
            target.motionTexture.Get(),
        };

        const bool providerAvailable =
            FidelityFX::FSR::IsUpscaleVersionAvailable(RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2) ||
            FidelityFX::FSR::IsUpscaleVersionAvailable(RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3);

        if (providerAvailable)
        {
            for (nvrhi::ITexture *texture : inputs)
            {
                pCommandList->beginTrackingTextureState(texture, nvrhi::AllSubresources,
                                                        nvrhi::ResourceStates::ShaderResource);
            }
        }

        for (nvrhi::ITexture *texture : inputs)
        {
            pCommandList->setTextureState(texture, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::UnorderedAccess);
        }
        pCommandList->setTextureState(target.outputTexture, nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::UnorderedAccess);

        if (!warnedFailedDispatch)
        {
            warnedFailedDispatch = true;
            LogMessage(print, "Warning: RHI: the FSR upscale produced no output, the TAAU path has to cover the frame");
        }
        return false;
    }

    // Here the four images are physically VK_IMAGE_LAYOUT_GENERAL again: the engine's backwards
    // barrier returned the inputs and left the output there (FSR.cpp:439), which is exactly the
    // UnorderedAccess this module's wraps claimed before the call. No correction is needed on this
    // path.
    return true;
}

nvrhi::ITexture *RhiFsrPass::GetOutputTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    const Target &target = targets[frameIndex];
    return target.valid ? target.outputTexture.Get() : nullptr;
}

void RhiFsrPass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseTarget(target);
    }
}

bool RhiFsrPass::PrepareTarget(uint32_t frameIndex,
                               Target &target,
                               const Framebuffers &framebuffers,
                               const ResolutionState &resolutionState)
{
    // The four engine images of this slot, with the extents their size classes predict.
    // `GetImageHandles` resolves the engine's per-slot swap inside (Framebuffers.cpp:33-53), so the
    // handles are the ones `FSR::Apply` will resolve again for the barriers and the descriptors.
    VkImage images[IMAGE_COUNT] = {};
    VkImageView views[IMAGE_COUNT] = {};
    VkFormat formats[IMAGE_COUNT] = {};
    VkExtent2D extents[IMAGE_COUNT] = {};

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        const auto [image, view, format, extent] =
            framebuffers.GetImageHandles(FSR_IMAGES[i], frameIndex, resolutionState);

        images[i] = image;
        views[i] = view;
        formats[i] = format;
        extents[i] = extent;

        if (image == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED)
        {
            if (!warnedMissingImages)
            {
                warnedMissingImages = true;
                LogMessage(print, std::string("Warning: RHI: the FSR pass got no usable engine image \"") +
                                      FSR_IMAGE_DEBUG_NAMES[i] + "\", the upscale is skipped");
            }
            target.valid = false;
            return false;
        }
    }

    // The size classes: FINAL/DEPTH_NDC/MOTION_DLSS are render-sized, the output upscaled-sized.
    // `GetImageHandles` derives the extent from the same ResolutionState and the generated flags
    // (Framebuffers::GetFramebufSize), so this check is the module's assertion that its table names
    // the images the engine's flags put in those classes; a mismatch is an engine-side size-class
    // change the module has to follow, not a frame error.
    const VkExtent2D expected[IMAGE_COUNT] =
    {
        { resolutionState.renderWidth, resolutionState.renderHeight },
        { resolutionState.renderWidth, resolutionState.renderHeight },
        { resolutionState.renderWidth, resolutionState.renderHeight },
        { resolutionState.upscaledWidth, resolutionState.upscaledHeight },
    };

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        if (extents[i].width != expected[i].width || extents[i].height != expected[i].height)
        {
            if (!warnedUnexpectedSize)
            {
                warnedUnexpectedSize = true;
                LogMessage(print, std::string("Warning: RHI: the FSR pass got \"") +
                                      FSR_IMAGE_DEBUG_NAMES[i] +
                                      "\" of an extent its size class does not predict, the upscale is skipped");
            }
            target.valid = false;
            return false;
        }
    }

    uint64_t imageHandles[IMAGE_COUNT] = {};
    uint64_t viewHandles[IMAGE_COUNT] = {};
    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        // The view handles are not consumed by the wrappers (NVRHI builds its own views from the
        // desc); they are kept in the slots for call-site symmetry with the sibling modules.
        imageHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(images[i]));
        viewHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(views[i]));
    }

    bool targetChanged = !target.valid ||
                         target.width != resolutionState.renderWidth ||
                         target.height != resolutionState.renderHeight ||
                         target.upscaledWidth != resolutionState.upscaledWidth ||
                         target.upscaledHeight != resolutionState.upscaledHeight;

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        targetChanged = targetChanged || target.imageHandles[i] != imageHandles[i];
    }

    if (targetChanged)
    {
        // The engine re-created its framebuffers (a resize or a format change) or the resolution
        // changed: retire the old wraps and rebuild. Retiring first is safe even if the rebuild
        // below fails - the next frame retries, and the pass stays skipped until it succeeds.
        ReleaseTarget(target);

        const std::string frameTag = std::to_string(frameIndex);

        // The three inputs are the engine's GENERAL-resting storage images, so they take
        // wrapEngineStorageImage like every other RHI module's framebuffer images. The output takes
        // wrapEngineRenderTarget: FSR writes it through its own Vulkan pipeline, but the host's
        // follow-up is a copy (a copy source needs no flag) or a direct sample, and a later UI
        // increment may bind it as a colour attachment, which NVRHI's validation requires
        // isRenderTarget for (validation-device.cpp:719-727); wrapEngineRenderTarget carries both
        // flags. Both helpers document their state contracts in RhiTextureSource.h.
        target.finalTexture = rhi::wrapEngineStorageImage(
            device, imageHandles[IMAGE_FINAL], viewHandles[IMAGE_FINAL], formats[IMAGE_FINAL],
            extents[IMAGE_FINAL].width, extents[IMAGE_FINAL].height,
            "RhiFsrPass " + std::string(FSR_IMAGE_DEBUG_NAMES[IMAGE_FINAL]) + " frame " + frameTag);
        target.depthNdcTexture = rhi::wrapEngineStorageImage(
            device, imageHandles[IMAGE_DEPTH_NDC], viewHandles[IMAGE_DEPTH_NDC], formats[IMAGE_DEPTH_NDC],
            extents[IMAGE_DEPTH_NDC].width, extents[IMAGE_DEPTH_NDC].height,
            "RhiFsrPass " + std::string(FSR_IMAGE_DEBUG_NAMES[IMAGE_DEPTH_NDC]) + " frame " + frameTag);
        target.motionTexture = rhi::wrapEngineStorageImage(
            device, imageHandles[IMAGE_MOTION], viewHandles[IMAGE_MOTION], formats[IMAGE_MOTION],
            extents[IMAGE_MOTION].width, extents[IMAGE_MOTION].height,
            "RhiFsrPass " + std::string(FSR_IMAGE_DEBUG_NAMES[IMAGE_MOTION]) + " frame " + frameTag);
        target.outputTexture = rhi::wrapEngineRenderTarget(
            device, imageHandles[IMAGE_OUTPUT], viewHandles[IMAGE_OUTPUT], formats[IMAGE_OUTPUT],
            extents[IMAGE_OUTPUT].width, extents[IMAGE_OUTPUT].height,
            "RhiFsrPass " + std::string(FSR_IMAGE_DEBUG_NAMES[IMAGE_OUTPUT]) + " frame " + frameTag);

        if (target.finalTexture == nullptr || target.depthNdcTexture == nullptr ||
            target.motionTexture == nullptr || target.outputTexture == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to wrap an engine image of the FSR pass");
            ReleaseTarget(target);
            return false;
        }

        std::memcpy(target.imageHandles, imageHandles, sizeof(target.imageHandles));
        target.width = resolutionState.renderWidth;
        target.height = resolutionState.renderHeight;
        target.upscaledWidth = resolutionState.upscaledWidth;
        target.upscaledHeight = resolutionState.upscaledHeight;
        target.valid = true;
    }

    return true;
}

void RhiFsrPass::ReleaseTarget(Target &target)
{
    // std::addressof, because nvrhi::RefCountPtr overloads operator& to return the address of the
    // stored pointer (resource.h:307-310).
    nvrhi::TextureHandle *const textures[IMAGE_COUNT] =
    {
        std::addressof(target.finalTexture),
        std::addressof(target.depthNdcTexture),
        std::addressof(target.motionTexture),
        std::addressof(target.outputTexture),
    };

    if (frameContext != nullptr)
    {
        for (nvrhi::TextureHandle *texture : textures)
        {
            if (*texture != nullptr)
            {
                frameContext->Retire(*texture);
            }
        }
    }

    for (nvrhi::TextureHandle *texture : textures)
    {
        *texture = nullptr;
    }

    std::memset(target.imageHandles, 0, sizeof(target.imageHandles));
    target.width = 0;
    target.height = 0;
    target.upscaledWidth = 0;
    target.upscaledHeight = 0;
    target.valid = false;
}

void RhiFsrPass::AnnounceAndFlushStates(nvrhi::ICommandList *pCommandList, const Target &target) const
{
    nvrhi::ITexture *const textures[IMAGE_COUNT] =
    {
        target.finalTexture.Get(),
        target.depthNdcTexture.Get(),
        target.motionTexture.Get(),
        target.outputTexture.Get(),
    };

    for (nvrhi::ITexture *texture : textures)
    {
        // The engine leaves every framebuffer image in VK_IMAGE_LAYOUT_GENERAL (Framebuffers
        // creates each image and barriers it there immediately, Framebuffers.cpp:764-768) - NVRHI's
        // UnorderedAccess - and that is also the `oldLayout` the engine's forward barrier names for
        // all four (FSR.cpp:107-108). A native wrap keeps no state between command lists
        // (RhiTextureSource.h), so the true state is announced here.
        pCommandList->beginTrackingTextureState(texture, nvrhi::AllSubresources,
                                                nvrhi::ResourceStates::UnorderedAccess);
    }

    for (nvrhi::ITexture *texture : textures)
    {
        // The same-state UnorderedAccess requirement queues NVRHI's same-state UAV barrier
        // (state-tracking.cpp:184-207): a memory dependency from the earlier shader writes to the
        // FFX dispatch (UnorderedAccess maps to ALL_COMMANDS with SHADER_READ|SHADER_WRITE,
        // vulkan-constants.cpp:242-245), and, together with any transition an earlier pass still
        // holds pending, the material `commitBarriers` below flushes.
        pCommandList->setTextureState(texture, nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::UnorderedAccess);
    }

    // The flush, and the point of the whole sequence: NVRHI records the barriers it queued on this
    // list here, before the engine's own FFX barriers are recorded on the same native command
    // buffer. Without it a pending barrier - the compose's restore of the images it sampled and
    // wrote, FINAL and MOTION_DLSS among them - would be recorded after the FFX dispatch and the
    // engine's `oldLayout = GENERAL` would name a layout the image is not in. This is the A4.2b
    // mechanism (RhiRtDirectPass.cpp:1320-1332): "Into the fill's state and out of the
    // UnorderedAccess claim the wrap starts every list with, committed before the native fill: a
    // pending barrier would otherwise be flushed after it and the fill would run unordered against
    // the previous frame's raygen atomics it overwrites." `commitBarriers` is a no-op when nothing
    // is pending (vulkan-commandlist.cpp:296-304).
    pCommandList->commitBarriers();
}
