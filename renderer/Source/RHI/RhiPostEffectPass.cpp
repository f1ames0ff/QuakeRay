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

#include "RhiPostEffectPass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiTextureSource.h"

#include "../Framebuffers.h"
#include "../Generated/ShaderCommonC.h"
#include "../Generated/ShaderCommonCFramebuf.h"
#include "../Utils.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>

using namespace qray;

namespace
{

// The ten engine blobs, by the file names ShaderManager loads them under (ShaderManager.cpp:87-96).
// Each is the same module the legacy effect objects read through ShaderManager, so the RHI and the
// legacy renderer dispatch byte-identical shaders.
const char *const COLOR_TINT_SHADER_FILE_NAME = "EfColorTint.comp.spv";
const char *const INVERSE_BW_SHADER_FILE_NAME = "EfInverseBW.comp.spv";
const char *const HUE_SHIFT_SHADER_FILE_NAME = "EfHueShift.comp.spv";
const char *const CHROMATIC_ABERRATION_SHADER_FILE_NAME = "EfChromaticAberration.comp.spv";
const char *const DISTORTED_SIDES_SHADER_FILE_NAME = "EfDistortedSides.comp.spv";
const char *const WAVES_SHADER_FILE_NAME = "EfWaves.comp.spv";
const char *const RADIAL_BLUR_SHADER_FILE_NAME = "EfRadialBlur.comp.spv";
const char *const WIPE_SHADER_FILE_NAME = "EfWipe.comp.spv";
const char *const CRT_DEMODULATE_ENCODE_SHADER_FILE_NAME = "EfCrtDemodulateEncode.comp.spv";
const char *const CRT_DECODE_SHADER_FILE_NAME = "EfCrtDecode.comp.spv";

// `[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]` in every blob (the
// header of EfSimple.inl:25); the legacy host dispatches with `Utils::GetWorkGroupCount(size, 16)`
// (EffectBase.h:130-131), the `1 + ceil(size / 16)` formula (Utils.cpp:319-328).
static_assert(COMPUTE_EFFECT_GROUP_SIZE_X == 16 && COMPUTE_EFFECT_GROUP_SIZE_Y == 16,
              "the module's dispatch formula is written for the engine's 16 x 16 effect workgroup");
constexpr uint32_t EFFECT_GROUP_SIZE = COMPUTE_EFFECT_GROUP_SIZE_X;

// The one specialization constant of the effect blobs: `isSourcePing`, the constant the shaders
// select their source image with (`EfSimple.inl:27`, `EfSimple.hlsli:34`). The legacy host creates
// `pipelines[isSourcePing]` (EffectBase::Dispatch, EffectBase.h:139) and flips the source role
// after every dispatch, which is what the module's `sourceIsPing` mirrors.
constexpr uint32_t SPEC_CONSTANT_IS_SOURCE_PING = 0;

// The engine's raw bindings of the framebuffers set (measured with `spirv-dis` over the shipped
// `renderer\Build\Ef*.comp.spv`; the shader sources spell the same numbers through the generated
// header): the storage images sit at raw binding = image index, the sampled image at
// 124 + image index. The layout offsets turn a raw binding into an NVRHI slot (RhiPipeline.h), so
// the storage items need offset 0 and the sampled ALBEDO item the offset 124.
constexpr uint32_t FRAMEBUFFER_UAV_OFFSET = 0;
constexpr uint32_t FRAMEBUFFER_SRV_OFFSET = 124;
static_assert(FB_IMAGE_INDEX_UPSCALED_PING == 29 && FB_IMAGE_INDEX_UPSCALED_PONG == 30 &&
              FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE == 72 && FB_IMAGE_INDEX_ALBEDO == 0,
              "the module's raw-binding table is written for the engine's image indices");
static_assert(FRAMEBUFFER_SRV_OFFSET + FB_IMAGE_INDEX_ALBEDO == 124,
              "the sampled ALBEDO is at raw binding 124 in every effect blob");

// The engine's global uniform set (raw binding 0, the `BINDING_GLOBAL_UNIFORM` of
// Generated/ShaderCommonC.h:27); the constant-buffer offset 0 keeps the NVRHI slot equal to the
// raw binding, the mechanism RhiRtComposePass uses for the same set.
constexpr uint32_t FRAMEBUFFER_UNIFORM_OFFSET = 0;

// The module blobs' block sizes and member offsets, byte for byte the legacy host's structs
// (EffectSimple.h:115-122 / EffectSimple_Instances.h / EffectWipe.h:31-37):
//   struct { uint32_t transitionType; float transitionBeginTime; float transitionDuration;
//            PUSH_CONST custom; } push;
// The shader declares the first three members at 0/4/8 and the custom ones from offset 12
// (`EffectSimplePush_BT` in every blob, measured with `spirv-dis`); the effects without custom
// members get the legacy host's 16-byte struct - the empty member occupies one byte and the struct
// pads to 16 - so the pass pushes the same block the legacy device pushes through
// `vkCmdPushConstants`.
struct EffectTransitionPush
{
    uint32_t transitionType; // 0 - in, 1 - out (EfSimple.inl's getProgress)
    float transitionBeginTime;
    float transitionDuration;
};
static_assert(sizeof(EffectTransitionPush) == 12);
static_assert(offsetof(EffectTransitionPush, transitionType) == 0);
static_assert(offsetof(EffectTransitionPush, transitionBeginTime) == 4);
static_assert(offsetof(EffectTransitionPush, transitionDuration) == 8);

struct EffectBasePush
{
    EffectTransitionPush transition;
    uint32_t padding; // the legacy host's empty custom member and the struct's tail padding, zero
};
static_assert(sizeof(EffectBasePush) == 16);

struct EffectColorTintPush
{
    EffectTransitionPush transition;
    float intensity;
    float r, g, b;
};
static_assert(sizeof(EffectColorTintPush) == 28);
static_assert(offsetof(EffectColorTintPush, intensity) == 12);

struct EffectWavesPush
{
    EffectTransitionPush transition;
    float amplitude;
    float speed;
    float multX;
};
static_assert(sizeof(EffectWavesPush) == 24);
static_assert(offsetof(EffectWavesPush, amplitude) == 12);
static_assert(offsetof(EffectWavesPush, speed) == 16);
static_assert(offsetof(EffectWavesPush, multX) == 20);

struct EffectChromaticAberrationPush
{
    EffectTransitionPush transition;
    float intensity;
};
static_assert(sizeof(EffectChromaticAberrationPush) == 16);
static_assert(offsetof(EffectChromaticAberrationPush, intensity) == 12);

// The wipe's own push block is the header's `RhiPostEffectPass::WipePush` (the legacy
// `EffectWipe::PushConst`, EffectWipe.h:31-37, with no transition member), asserted there.

void LogMessage(const RhiPostEffectPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

// One specialized pipeline of one effect, exactly the legacy `EffectBase::Dispatch` selection:
// `createShaderSpecialization` for the blob's `isSourcePing` and a compute pipeline over the
// effect's layouts in descriptor-set order. The layouts are the pipeline's whole shape - the
// pinned backend places them into descriptor sets in the order they are added
// (vulkan-resource-bindings.cpp:1090-1099) and skips a PushConstants item when it builds the
// Vulkan set layout (vulkan-resource-bindings.cpp:90-94, the shape RhiRtGodRaysPass documents), so
// the list order is the shader's set numbering and no set is bound for the push constants.
nvrhi::ComputePipelineHandle CreateEffectPipeline(nvrhi::IDevice *device,
                                                  nvrhi::IShader *shader,
                                                  bool sourceIsPing,
                                                  nvrhi::ShaderHandle &specializedShaderResult,
                                                  std::initializer_list<nvrhi::IBindingLayout *> layouts,
                                                  std::string_view debugName)
{
    specializedShaderResult = nullptr;

    if (device == nullptr || shader == nullptr)
    {
        return nullptr;
    }

    const nvrhi::ShaderSpecialization specialization =
        nvrhi::ShaderSpecialization::UInt32(SPEC_CONSTANT_IS_SOURCE_PING, sourceIsPing ? 1u : 0u);

    specializedShaderResult = device->createShaderSpecialization(shader, &specialization, 1);
    if (specializedShaderResult == nullptr)
    {
        return nullptr;
    }

    nvrhi::ComputePipelineDesc desc;
    desc.setComputeShader(specializedShaderResult);

    for (nvrhi::IBindingLayout *pLayout : layouts)
    {
        if (pLayout == nullptr)
        {
            return nullptr;
        }

        desc.addBindingLayout(pLayout);
    }

    return rhi::createComputePipeline(device, desc, debugName);
}

}

RhiPostEffectPass::RhiPostEffectPass() = default;

RhiPostEffectPass::~RhiPostEffectPass()
{
    if (device != nullptr)
    {
        // The wraps reference engine images; the host destroys the pass while it can still idle the
        // device (VulkanDevice does that before the skeleton as well), so the handles are dropped
        // directly instead of through the frame context's retire queue - the shape RhiFsrPass uses.
        device->waitForIdle();
    }

    for (Effect &effect : effects)
    {
        effect.pipelines[0] = nullptr;
        effect.pipelines[1] = nullptr;
        effect.specializedShaders[0] = nullptr;
        effect.specializedShaders[1] = nullptr;
        effect.shader = nullptr;
    }

    for (Target &target : targets)
    {
        for (nvrhi::TextureHandle &texture : target.textures)
        {
            texture = nullptr;
        }

        target.simpleSet = nullptr;
        target.albedoSet = nullptr;
        target.wipeSet = nullptr;
        target.uniformSet = nullptr;
        target.uniformBuffer = nullptr;
    }

    simpleFramebufferLayout = nullptr;
    albedoFramebufferLayout = nullptr;
    wipeFramebufferLayout = nullptr;
    uniformLayout = nullptr;
    pushConstant16Layout = nullptr;
    pushConstant24Layout = nullptr;
    pushConstant28Layout = nullptr;
}

bool RhiPostEffectPass::Create(nvrhi::IDevice *pDevice,
                               rhi::RhiFrameContext *pFrameContext,
                               const char *pShaderFolderPath,
                               bool wipeIsUsed,
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
    this->wipeIsUsed = wipeIsUsed;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the post-effect pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the post-effect pass needs the frame context of the RHI layer");
        return false;
    }

    // Set 0 of the effects that only touch the upscaled pair: `RWTexture2D` at raw binding 29 and
    // 30 (measured; Generated/ShaderCommonHLSL.hlsli:605-606 spells the same two bindings).
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(FRAMEBUFFER_SRV_OFFSET)
                                   .setUnorderedAccessViewOffset(FRAMEBUFFER_UAV_OFFSET));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING)));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG)));

        simpleFramebufferLayout = device->createBindingLayout(desc);
    }

    // Set 0 of EfInverseBW and EfHueShift: the same pair plus the sampled ALBEDO at raw binding
    // 124 (`framebufAlbedo_Sampled`, a `Texture2D`; ShaderCommonHLSL.hlsli:708).
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(FRAMEBUFFER_SRV_OFFSET)
                                   .setUnorderedAccessViewOffset(FRAMEBUFFER_UAV_OFFSET));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING)));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG)));
        // The sampled ALBEDO at raw binding 124: with the SRV offset 124 the slot is the image
        // index itself.
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_ALBEDO)));

        albedoFramebufferLayout = device->createBindingLayout(desc);
    }

    // Set 0 of the wipe: the pair plus `framebufWipeEffectSource` (image 72) as a `RWTexture2D`
    // (ShaderCommonHLSL.hlsli:653); the wipe reads the previous screen from it.
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(FRAMEBUFFER_SRV_OFFSET)
                                   .setUnorderedAccessViewOffset(FRAMEBUFFER_UAV_OFFSET));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING)));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG)));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE)));

        wipeFramebufferLayout = device->createBindingLayout(desc);
    }

    // Set 1: `ConstantBuffer<ShGlobalUniform>` at raw binding 0. The constant-buffer offset 0
    // keeps the item's slot equal to the raw binding (RhiPipeline.h's rule).
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(FRAMEBUFFER_UNIFORM_OFFSET));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM));

        uniformLayout = device->createBindingLayout(desc);
    }

    // The three push-constant layouts the measured block sizes call for: 16 bytes for the effects
    // without custom members and for the chromatic aberration (one float) and the wipe (its own
    // four-member block), 24 for the waves, 28 for the colour tint. No set is bound for them.
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(EffectBasePush)));
        pushConstant16Layout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(EffectWavesPush)));
        pushConstant24Layout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(EffectColorTintPush)));
        pushConstant28Layout = device->createBindingLayout(desc);
    }

    if (simpleFramebufferLayout == nullptr || albedoFramebufferLayout == nullptr ||
        wipeFramebufferLayout == nullptr || uniformLayout == nullptr ||
        pushConstant16Layout == nullptr ||
        pushConstant24Layout == nullptr || pushConstant28Layout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a post-effect pass binding layout");
        return false;
    }

    // The wipe's soft pieces: the game's `effectWipeIsUsed` is the library-config gate the legacy
    // `EffectWipe` takes (EffectWipe.h:44-45), and this game leaves it false (gl_vidsdl.c:1443-1486
    // sets no such field), so the wipe's shader and pipelines are not built at all. A caller that
    // sets the flag gets the same soft-failure policy: the wipe logs and stays unavailable while
    // the rest of the chain works.
    if (wipeIsUsed)
    {
        nvrhi::ShaderHandle wipeShader;
        LoadShader(WIPE_SHADER_FILE_NAME, wipeShader);

        if (wipeShader != nullptr)
        {
            Effect &wipe = effects[EFFECT_WIPE];
            wipe.shader = std::move(wipeShader);

            for (uint32_t sourceIsPing = 0; sourceIsPing < 2; sourceIsPing++)
            {
                const std::string pipelineName =
                    std::string("RhiPostEffectPass wipe ") + (sourceIsPing != 0 ? "(ping source)" : "(pong source)");

                wipe.pipelines[sourceIsPing] = CreateEffectPipeline(
                    device, wipe.shader, sourceIsPing != 0, wipe.specializedShaders[sourceIsPing],
                    { wipeFramebufferLayout, uniformLayout, pushConstant16Layout },
                    pipelineName);
            }

            wipeAvailable = wipe.pipelines[0] != nullptr && wipe.pipelines[1] != nullptr;
        }

        if (!wipeAvailable)
        {
            LogMessage(print, "Warning: RHI: the post-effect wipe is unavailable, the wipe path stays inert");
        }
    }

    // The nine effects that are always available: the blob, its two specializations and the two
    // pipelines over the effect's measured set shape. Every one of them is a hard requirement -
    // a missing blob means the engine's shader folder is incomplete.
    struct EffectDesc
    {
        EffectId id;
        const char *debugName;
        const char *fileName;
        uint32_t framebufferKind; // FB_SIMPLE / FB_ALBEDO
        uint32_t pushConstantSize;
        bool usesUniform;
    };

    constexpr uint32_t FB_SIMPLE = 0;
    constexpr uint32_t FB_ALBEDO = 1;

    const EffectDesc descs[] =
    {
        { EFFECT_COLOR_TINT, "color tint", COLOR_TINT_SHADER_FILE_NAME, FB_SIMPLE, 28, true },
        { EFFECT_INVERSE_BW, "inverse black and white", INVERSE_BW_SHADER_FILE_NAME, FB_ALBEDO, 16, true },
        { EFFECT_HUE_SHIFT, "hue shift", HUE_SHIFT_SHADER_FILE_NAME, FB_ALBEDO, 16, true },
        { EFFECT_CHROMATIC_ABERRATION, "chromatic aberration", CHROMATIC_ABERRATION_SHADER_FILE_NAME, FB_SIMPLE, 16, true },
        { EFFECT_DISTORTED_SIDES, "distorted sides", DISTORTED_SIDES_SHADER_FILE_NAME, FB_SIMPLE, 16, true },
        { EFFECT_WAVES, "waves", WAVES_SHADER_FILE_NAME, FB_SIMPLE, 24, true },
        { EFFECT_RADIAL_BLUR, "radial blur", RADIAL_BLUR_SHADER_FILE_NAME, FB_SIMPLE, 16, true },
        { EFFECT_CRT_DEMODULATE_ENCODE, "CRT demodulate/encode", CRT_DEMODULATE_ENCODE_SHADER_FILE_NAME, FB_SIMPLE, 16, true },
        // EfCrtDecode's entry point declares neither the push block nor set 1 (measured), so its
        // list stops after set 0 and the push-constant layout; the module mirrors the legacy host's
        // unread push anyway (the layout is still added, the bytes are still pushed).
        { EFFECT_CRT_DECODE, "CRT decode", CRT_DECODE_SHADER_FILE_NAME, FB_SIMPLE, 16, false },
    };

    static_assert(std::size(descs) == EFFECT_COUNT - 1,
                  "the non-wipe effects of the chain have to cover every EffectId but the wipe");

    for (const EffectDesc &desc : descs)
    {
        Effect &effect = effects[desc.id];

        if (!LoadShader(desc.fileName, effect.shader))
        {
            return false;
        }

        nvrhi::IBindingLayout *framebufferLayout =
            desc.framebufferKind == FB_ALBEDO ? albedoFramebufferLayout.Get() : simpleFramebufferLayout.Get();

        nvrhi::IBindingLayout *pushConstantLayout =
            desc.pushConstantSize == 28 ? pushConstant28Layout.Get() :
            desc.pushConstantSize == 24 ? pushConstant24Layout.Get() : pushConstant16Layout.Get();

        for (uint32_t sourceIsPing = 0; sourceIsPing < 2; sourceIsPing++)
        {
            const std::string pipelineName = std::string("RhiPostEffectPass ") + desc.debugName +
                                             (sourceIsPing != 0 ? " (ping source)" : " (pong source)");

            // The shader's set order: set 0 the framebuffers, set 1 the uniform (the CRT decode
            // blob has no set 1), then the descriptor-less push-constant layout.
            if (desc.usesUniform)
            {
                effect.pipelines[sourceIsPing] = CreateEffectPipeline(
                    device, effect.shader, sourceIsPing != 0, effect.specializedShaders[sourceIsPing],
                    { framebufferLayout, uniformLayout, pushConstantLayout }, pipelineName);
            }
            else
            {
                effect.pipelines[sourceIsPing] = CreateEffectPipeline(
                    device, effect.shader, sourceIsPing != 0, effect.specializedShaders[sourceIsPing],
                    { framebufferLayout, pushConstantLayout }, pipelineName);
            }

            if (effect.pipelines[sourceIsPing] == nullptr)
            {
                LogMessage(print, std::string("Warning: RHI: failed to create the post-effect pipeline of \"") +
                                      desc.debugName + "\"");
                return false;
            }
        }
    }

    created = true;
    return true;
}

void RhiPostEffectPass::Render(nvrhi::ICommandList *pCommandList,
                               uint32_t frameIndex,
                               const Framebuffers *pFramebuffers,
                               uint32_t renderWidth,
                               uint32_t renderHeight,
                               uint32_t upscaledWidth,
                               uint32_t upscaledHeight,
                               float currentTime,
                               nvrhi::IBuffer *pUniformBuffer,
                               const QrDrawFramePostEffectsParams &params)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    Target *pTarget = PrepareFrame(frameIndex, pFramebuffers, renderWidth, renderHeight,
                                   upscaledWidth, upscaledHeight, pUniformBuffer);
    if (pTarget == nullptr)
    {
        return;
    }

    Target &target = *pTarget;

    // The upscaler wrote image 29 on this very list through its own wrap; the announcement says
    // where the engine leaves the image (GENERAL, NVRHI's UnorderedAccess) and the requirement is
    // the same-state UAV barrier that orders that write before the first effect's read - the
    // barrier the legacy `EffectBase::Dispatch` records over its source every dispatch
    // (EffectBase.h:148-152). Image 30 is announced with it for the odd-dispatch hand-offs.
    AnnounceImage(pCommandList, target, IMAGE_PING);
    AnnounceImage(pCommandList, target, IMAGE_PONG);

    // The sampled ALBEDO of the inverse-BW/hue-shift effects: after the compose it rests in the
    // same GENERAL state. The announcement is bookkeeping only - the image is bound as an SRV and
    // the binding's own UAV -> SRV transition is the barrier that orders the primary's
    // checkerboard write before the read. It is made once, here, and not per dispatch: the first
    // of the two dispatches moves the image to the read-only state through the binding, and a
    // second announcement would claim a GENERAL the image no longer is (the second dispatch binds
    // the same set object, so NVRHI does not re-apply the set's requirements and the claim would
    // never be corrected). The restore at the end of the chain names the read-only state the
    // tracker then really holds.
    pCommandList->beginTrackingTextureState(target.textures[IMAGE_ALBEDO], nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);

    // The source role of the first effect: the upscale output lives in the ping image. The role
    // flips after every recorded dispatch, exactly the legacy `isSourcePing` bookkeeping.
    bool sourceIsPing = true;

    // The legacy `args.width`/`args.height` of this chain are the upscaled size
    // (VulkanDevice.cpp:1151) and every dispatch is `Utils::GetWorkGroupCount(size, 16)`.
    const uint32_t groupsX = Utils::GetWorkGroupCount(upscaledWidth, EFFECT_GROUP_SIZE);
    const uint32_t groupsY = Utils::GetWorkGroupCount(upscaledHeight, EFFECT_GROUP_SIZE);

    // Whether one of the two ALBEDO-sampling effects was dispatched: their set moves ALBEDO to the
    // read-only state and the module returns it to the engine's resting state below.
    bool albedoSampled = false;

    // 1. The colour tint (`effectColorTint->Setup(args, pColorTint)`, VulkanDevice.cpp:1166-1169):
    // the custom members are written whenever the params exist (EffectSimple_Instances.h:129-142),
    // the setup decides whether the effect runs.
    if (params.pColorTint != nullptr)
    {
        if (SetupSimple(EFFECT_COLOR_TINT, currentTime, params.pColorTint->isActive,
                        params.pColorTint->transitionDurationIn, params.pColorTint->transitionDurationOut))
        {
            EffectColorTintPush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_COLOR_TINT].transition.transitionType,
                effects[EFFECT_COLOR_TINT].transition.transitionBeginTime,
                effects[EFFECT_COLOR_TINT].transition.transitionDuration,
            };
            push.intensity = params.pColorTint->intensity;
            push.r = params.pColorTint->color.data[0];
            push.g = params.pColorTint->color.data[1];
            push.b = params.pColorTint->color.data[2];

            if (DispatchEffect(pCommandList, target, EFFECT_COLOR_TINT, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
            }
        }
    }
    else
    {
        SetupNull(EFFECT_COLOR_TINT);
    }

    // 2. The inverse black and white (`effectInverseBW`, :1170-1173).
    if (params.pInverseBlackAndWhite != nullptr)
    {
        if (SetupSimple(EFFECT_INVERSE_BW, currentTime, params.pInverseBlackAndWhite->isActive,
                        params.pInverseBlackAndWhite->transitionDurationIn,
                        params.pInverseBlackAndWhite->transitionDurationOut))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_INVERSE_BW].transition.transitionType,
                effects[EFFECT_INVERSE_BW].transition.transitionBeginTime,
                effects[EFFECT_INVERSE_BW].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_INVERSE_BW, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
                albedoSampled = true;
            }
        }
    }
    else
    {
        SetupNull(EFFECT_INVERSE_BW);
    }

    // 3. The hue shift (`effectHueShift`, :1174-1177).
    if (params.pHueShift != nullptr)
    {
        if (SetupSimple(EFFECT_HUE_SHIFT, currentTime, params.pHueShift->isActive,
                        params.pHueShift->transitionDurationIn, params.pHueShift->transitionDurationOut))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_HUE_SHIFT].transition.transitionType,
                effects[EFFECT_HUE_SHIFT].transition.transitionBeginTime,
                effects[EFFECT_HUE_SHIFT].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_HUE_SHIFT, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
                albedoSampled = true;
            }
        }
    }
    else
    {
        SetupNull(EFFECT_HUE_SHIFT);
    }

    // 4. The chromatic aberration (`effectChromaticAberration`, :1178-1181): a null pointer or a
    // non-positive intensity turns it off without a fade (EffectSimple_Instances.h:59-68).
    if (params.pChromaticAberration == nullptr || params.pChromaticAberration->intensity <= 0.0f)
    {
        SetupNull(EFFECT_CHROMATIC_ABERRATION);
    }
    else if (SetupSimple(EFFECT_CHROMATIC_ABERRATION, currentTime, params.pChromaticAberration->isActive,
                         params.pChromaticAberration->transitionDurationIn,
                         params.pChromaticAberration->transitionDurationOut))
    {
        EffectChromaticAberrationPush push{};
        push.transition = EffectTransitionPush{
            effects[EFFECT_CHROMATIC_ABERRATION].transition.transitionType,
            effects[EFFECT_CHROMATIC_ABERRATION].transition.transitionBeginTime,
            effects[EFFECT_CHROMATIC_ABERRATION].transition.transitionDuration,
        };
        push.intensity = params.pChromaticAberration->intensity;

        if (DispatchEffect(pCommandList, target, EFFECT_CHROMATIC_ABERRATION, sourceIsPing,
                           &push, sizeof(push), groupsX, groupsY))
        {
            sourceIsPing = !sourceIsPing;
        }
    }

    // 5. The distorted sides (`effectDistortedSides`, :1182-1185).
    if (params.pDistortedSides != nullptr)
    {
        if (SetupSimple(EFFECT_DISTORTED_SIDES, currentTime, params.pDistortedSides->isActive,
                        params.pDistortedSides->transitionDurationIn,
                        params.pDistortedSides->transitionDurationOut))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_DISTORTED_SIDES].transition.transitionType,
                effects[EFFECT_DISTORTED_SIDES].transition.transitionBeginTime,
                effects[EFFECT_DISTORTED_SIDES].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_DISTORTED_SIDES, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
            }
        }
    }
    else
    {
        SetupNull(EFFECT_DISTORTED_SIDES);
    }

    // 6. The underwater waves (`effectWaves`, :1186-1189): a null pointer or a non-positive
    // amplitude turns it off without a fade (EffectSimple_Instances.h:208-222); the game gates the
    // pointer on `r_waterwarp == 1` and the activation on the camera's media
    // (gl_vidsdl.c:2100-2107, :2150).
    if (params.pWaves == nullptr || params.pWaves->amplitude <= 0.0f)
    {
        SetupNull(EFFECT_WAVES);
    }
    else if (SetupSimple(EFFECT_WAVES, currentTime, params.pWaves->isActive,
                         params.pWaves->transitionDurationIn, params.pWaves->transitionDurationOut))
    {
        EffectWavesPush push{};
        push.transition = EffectTransitionPush{
            effects[EFFECT_WAVES].transition.transitionType,
            effects[EFFECT_WAVES].transition.transitionBeginTime,
            effects[EFFECT_WAVES].transition.transitionDuration,
        };
        push.amplitude = params.pWaves->amplitude;
        push.speed = params.pWaves->speed;
        push.multX = params.pWaves->xMultiplier;

        if (DispatchEffect(pCommandList, target, EFFECT_WAVES, sourceIsPing,
                           &push, sizeof(push), groupsX, groupsY))
        {
            sourceIsPing = !sourceIsPing;
        }
    }

    // 7. The radial blur (`effectRadialBlur`, :1190-1193).
    if (params.pRadialBlur != nullptr)
    {
        if (SetupSimple(EFFECT_RADIAL_BLUR, currentTime, params.pRadialBlur->isActive,
                        params.pRadialBlur->transitionDurationIn,
                        params.pRadialBlur->transitionDurationOut))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_RADIAL_BLUR].transition.transitionType,
                effects[EFFECT_RADIAL_BLUR].transition.transitionBeginTime,
                effects[EFFECT_RADIAL_BLUR].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_RADIAL_BLUR, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
            }
        }
    }
    else
    {
        SetupNull(EFFECT_RADIAL_BLUR);
    }

    // The two ALBEDO-sampling effects left the image in the read-only state their SRV requires.
    // ALBEDO's resting state is the engine's GENERAL, and the compose announces exactly that for
    // its own wrap every frame, so the module returns it here - the restore discipline the compose
    // applies to its sampled reads (RhiRtComposePass.h's CHAIN_RESTORE list).
    if (albedoSampled)
    {
        pCommandList->setTextureState(target.textures[IMAGE_ALBEDO], nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::UnorderedAccess);
    }

    // The chain always leaves its result in image 29 for the UI and the present. An even number of
    // dispatches ends in the ping image already; an odd one ends in the pong image and is copied
    // back, the pair the skeleton records after the FSR upscale (NvrhiFrameSkeleton.cpp:1001-1007).
    if (!sourceIsPing)
    {
        CopyPongToPing(pCommandList, target);
    }
}

void RhiPostEffectPass::RenderPostUi(nvrhi::ICommandList *pCommandList,
                                     uint32_t frameIndex,
                                     const Framebuffers *pFramebuffers,
                                     uint32_t renderWidth,
                                     uint32_t renderHeight,
                                     uint32_t upscaledWidth,
                                     uint32_t upscaledHeight,
                                     float currentTime,
                                     nvrhi::IBuffer *pUniformBuffer,
                                     const QrDrawFramePostEffectsParams &params,
                                     uint32_t frameId)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    Target *pTarget = PrepareFrame(frameIndex, pFramebuffers, renderWidth, renderHeight,
                                   upscaledWidth, upscaledHeight, pUniformBuffer);
    if (pTarget == nullptr)
    {
        return;
    }

    Target &target = *pTarget;

    // `Render` left the result in image 29 (the UI has drawn over it since); the same announcement
    // and same-state requirement as there order the UI's render-target write before the wipe's
    // read of the image.
    AnnounceImage(pCommandList, target, IMAGE_PING);
    AnnounceImage(pCommandList, target, IMAGE_PONG);

    bool sourceIsPing = true;

    const uint32_t groupsX = Utils::GetWorkGroupCount(upscaledWidth, EFFECT_GROUP_SIZE);
    const uint32_t groupsY = Utils::GetWorkGroupCount(upscaledHeight, EFFECT_GROUP_SIZE);

    // 8. The wipe (`effectWipe->Setup(args, pWipe, swapchain, frameId)`, :1212-1215). The path is
    // inert for this game - `pWipe` is never filled and `effectWipeIsUsed` is never set - but it is
    // ported to the legacy shape, so a caller that uses the feature gets the same early-outs.
    if (params.pWipe != nullptr && wipeAvailable)
    {
        if (RecordWipe(pCommandList, target, *params.pWipe, upscaledWidth, upscaledHeight,
                       currentTime, frameId, sourceIsPing))
        {
            sourceIsPing = !sourceIsPing;
        }
    }

    // 9. The CRT pair (`:1216-1223`): only while `pCRT != nullptr && pCRT->isActive`, with the
    // legacy host's `Setup(args)` = `Setup(args, true, 0, 0)` (EffectSimple_Instances.h:177-191).
    if (params.pCRT != nullptr && params.pCRT->isActive)
    {
        if (SetupSimple(EFFECT_CRT_DEMODULATE_ENCODE, currentTime, true, 0.0f, 0.0f))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_CRT_DEMODULATE_ENCODE].transition.transitionType,
                effects[EFFECT_CRT_DEMODULATE_ENCODE].transition.transitionBeginTime,
                effects[EFFECT_CRT_DEMODULATE_ENCODE].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_CRT_DEMODULATE_ENCODE, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
            }
        }

        if (SetupSimple(EFFECT_CRT_DECODE, currentTime, true, 0.0f, 0.0f))
        {
            EffectBasePush push{};
            push.transition = EffectTransitionPush{
                effects[EFFECT_CRT_DECODE].transition.transitionType,
                effects[EFFECT_CRT_DECODE].transition.transitionBeginTime,
                effects[EFFECT_CRT_DECODE].transition.transitionDuration,
            };

            if (DispatchEffect(pCommandList, target, EFFECT_CRT_DECODE, sourceIsPing,
                               &push, sizeof(push), groupsX, groupsY))
            {
                sourceIsPing = !sourceIsPing;
            }
        }
    }

    if (!sourceIsPing)
    {
        CopyPongToPing(pCommandList, target);
    }
}

void RhiPostEffectPass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseTarget(target);
    }
}

RhiPostEffectPass::Target *RhiPostEffectPass::PrepareFrame(uint32_t frameIndex,
                                                           const Framebuffers *pFramebuffers,
                                                           uint32_t renderWidth,
                                                           uint32_t renderHeight,
                                                           uint32_t upscaledWidth,
                                                           uint32_t upscaledHeight,
                                                           nvrhi::IBuffer *pUniformBuffer)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    if (pFramebuffers == nullptr)
    {
        if (!warnedMissingFramebuffers)
        {
            warnedMissingFramebuffers = true;
            LogMessage(print, "Warning: RHI: the post-effect pass got no engine framebuffers, the chain is skipped");
        }
        return nullptr;
    }

    if (pUniformBuffer == nullptr)
    {
        if (!warnedMissingUniform)
        {
            warnedMissingUniform = true;
            LogMessage(print, "Warning: RHI: the post-effect pass got no global uniform, the chain is skipped");
        }
        return nullptr;
    }

    if (upscaledWidth == 0 || upscaledHeight == 0)
    {
        return nullptr;
    }

    Target &target = targets[frameIndex];

    // The four images of this slot: the upscaled pair the chain ping-pongs between, the sampled
    // ALBEDO and - only while the wipe is used - its capture source. `GetImageHandles` resolves
    // the engine's per-slot swap; the 4-tuple overload supplies each image's extent, which has to
    // match the module's expectation - 29/30 and 72 are upscaled-sized
    // (`FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_UPSCALED_SIZE`), ALBEDO is render-sized
    // (Framebuffers.cpp:628-692). A mismatch is a wrongly sized wrap and the frame is skipped.
    const ResolutionState resolutionState = { renderWidth, renderHeight, upscaledWidth, upscaledHeight };

    const FramebufferImageIndex images[IMAGE_COUNT] =
    {
        FB_IMAGE_INDEX_UPSCALED_PING,
        FB_IMAGE_INDEX_UPSCALED_PONG,
        FB_IMAGE_INDEX_ALBEDO,
        FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE,
    };

    uint64_t imageHandles[IMAGE_COUNT] = {};
    uint64_t imageViews[IMAGE_COUNT] = {};
    VkFormat imageFormats[IMAGE_COUNT] = {};
    VkExtent2D imageExtents[IMAGE_COUNT] = {};

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        if (i == IMAGE_WIPE_SOURCE && !wipeAvailable)
        {
            // The wipe never runs, so its 1x1 image (`effectWipeIsUsed` false,
            // Framebuffers.cpp:631-637) is not wrapped and the wipe set is never built.
            continue;
        }

        const auto [image, view, format, extent] =
            pFramebuffers->GetImageHandles(images[i], frameIndex, resolutionState);

        imageHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image));
        imageViews[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view));
        imageFormats[i] = format;
        imageExtents[i] = extent;

        if (imageHandles[i] == 0)
        {
            if (!warnedMissingFramebuffers)
            {
                warnedMissingFramebuffers = true;
                LogMessage(print, "Warning: RHI: the post-effect pass got no framebuffer image, the chain is skipped");
            }
            return nullptr;
        }
    }

    const VkExtent2D expectedExtents[IMAGE_COUNT] =
    {
        { upscaledWidth, upscaledHeight },
        { upscaledWidth, upscaledHeight },
        { renderWidth, renderHeight },
        { upscaledWidth, upscaledHeight },
    };

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        if (i == IMAGE_WIPE_SOURCE && !wipeAvailable)
        {
            continue;
        }

        if (imageExtents[i].width != expectedExtents[i].width ||
            imageExtents[i].height != expectedExtents[i].height)
        {
            if (!warnedUnexpectedSize)
            {
                warnedUnexpectedSize = true;
                LogMessage(print, std::string("Warning: RHI: the post-effect pass got \"") +
                                      ShFramebuffers_DebugNames[images[i]] +
                                      "\", which is not sized as the module expects; the chain is skipped");
            }
            return nullptr;
        }
    }

    bool framebuffersChanged = target.renderWidth != renderWidth || target.renderHeight != renderHeight ||
                               target.upscaledWidth != upscaledWidth || target.upscaledHeight != upscaledHeight;

    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
    {
        framebuffersChanged = framebuffersChanged || target.imageHandles[i] != imageHandles[i];
    }

    if (framebuffersChanged)
    {
        // The engine re-created its framebuffers (a resize or a format change) or the resolution
        // changed: the wraps and the sets over them are retired and rebuilt. Retiring first is
        // safe even if the rebuild below fails - the next frame retries and the chain is skipped
        // until it succeeds.
        ReleaseTarget(target);

        for (uint32_t i = 0; i < IMAGE_COUNT; i++)
        {
            if (imageHandles[i] == 0)
            {
                continue;
            }

            const std::string debugName = std::string("RhiPostEffectPass ") +
                                          ShFramebuffers_DebugNames[images[i]] +
                                          " frame " + std::to_string(frameIndex);

            // Every one of the four is an engine framebuffer image the engine leaves in
            // VK_IMAGE_LAYOUT_GENERAL and that the passes read and write through storage images,
            // so each takes the storage-image wrap (isUAV, the flag the validation device requires
            // for a Texture_UAV binding, and isShaderResource for the sampled ALBEDO use). The
            // wrap keeps no state between command lists, hence the per-list announcements
            // (RhiTextureSource.h).
            target.textures[i] = rhi::wrapEngineStorageImage(
                device, imageHandles[i], imageViews[i], imageFormats[i],
                imageExtents[i].width, imageExtents[i].height, debugName);

            if (target.textures[i] == nullptr)
            {
                LogMessage(print, "Warning: RHI: failed to wrap a post-effect pass framebuffer image");
                ReleaseTarget(target);
                return nullptr;
            }
        }

        std::memcpy(target.imageHandles, imageHandles, sizeof(target.imageHandles));
        target.renderWidth = renderWidth;
        target.renderHeight = renderHeight;
        target.upscaledWidth = upscaledWidth;
        target.upscaledHeight = upscaledHeight;
    }

    if (!PrepareFramebufferSets(target) || !PrepareUniformSet(target, pUniformBuffer))
    {
        return nullptr;
    }

    return &target;
}

bool RhiPostEffectPass::PrepareFramebufferSets(Target &target)
{
    // Set 0 of the seven effects that only touch the upscaled pair (and of the CRT decode). One
    // set object serves them all: the shader reads the role its pipeline's `isSourcePing` selects
    // and writes the other, and the module orders every hand-off with its own same-state UAV
    // requirement - the discipline RhiRtComposePass documents for repeated sets.
    if (target.simpleSet == nullptr)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING), target.textures[IMAGE_PING]));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG), target.textures[IMAGE_PONG]));

        target.simpleSet = device->createBindingSet(setDesc, simpleFramebufferLayout);

        if (target.simpleSet == nullptr)
        {
            if (!warnedFailedSet)
            {
                warnedFailedSet = true;
                LogMessage(print, "Warning: RHI: failed to create the post-effect framebuffer set, the chain is skipped");
            }
            return false;
        }
    }

    // Set 0 of the two ALBEDO-sampling effects. Its items are the same wraps, so the two effects
    // and the simple-set effects share NVRHI's transitions of the upscaled images.
    if (target.albedoSet == nullptr)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING), target.textures[IMAGE_PING]));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG), target.textures[IMAGE_PONG]));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_ALBEDO),
            target.textures[IMAGE_ALBEDO]));

        target.albedoSet = device->createBindingSet(setDesc, albedoFramebufferLayout);

        if (target.albedoSet == nullptr)
        {
            // Only the two ALBEDO-sampling effects miss out; the rest of the chain still runs.
            if (!warnedMissingAlbedo)
            {
                warnedMissingAlbedo = true;
                LogMessage(print, "Warning: RHI: failed to create the post-effect ALBEDO set, the inverse-BW and hue-shift effects are skipped");
            }
        }
    }

    // Set 0 of the wipe, only while its path exists.
    if (wipeAvailable && target.wipeSet == nullptr)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PING), target.textures[IMAGE_PING]));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_UPSCALED_PONG), target.textures[IMAGE_PONG]));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            static_cast<uint32_t>(FB_IMAGE_INDEX_WIPE_EFFECT_SOURCE), target.textures[IMAGE_WIPE_SOURCE]));

        target.wipeSet = device->createBindingSet(setDesc, wipeFramebufferLayout);

        if (target.wipeSet == nullptr)
        {
            if (!warnedMissingWipe)
            {
                warnedMissingWipe = true;
                LogMessage(print, "Warning: RHI: failed to create the post-effect wipe set, the wipe is skipped");
            }
        }
    }

    return true;
}

bool RhiPostEffectPass::PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer)
{
    if (target.uniformSet != nullptr && target.uniformBuffer == pUniformBuffer)
    {
        return true;
    }

    // The shader declares set 1 as ConstantBuffer<ShGlobalUniform>; the validation device refuses
    // such a binding on a buffer whose desc lacks isConstantBuffer (validation-device.cpp:1717-1723)
    // and on a volatile one (:1725-1730, which would also become a dynamic-offset binding the
    // static layout item cannot take). The check is the compose's and the god-rays'.
    if (!pUniformBuffer->getDesc().isConstantBuffer || pUniformBuffer->getDesc().isVolatile)
    {
        if (!warnedMissingUniform)
        {
            warnedMissingUniform = true;
            LogMessage(print, "Warning: RHI: the post-effect pass needs the global uniform as a static constant-buffer wrap");
        }
        return false;
    }

    if (target.uniformSet != nullptr)
    {
        frameContext->Retire(target.uniformSet);
    }
    target.uniformSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM, pUniformBuffer));

    target.uniformSet = device->createBindingSet(setDesc, uniformLayout);

    if (target.uniformSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the post-effect uniform set");
        return false;
    }

    target.uniformBuffer = pUniformBuffer;
    return true;
}

bool RhiPostEffectPass::SetupSimple(EffectId effect, float currentTime, bool isActive,
                                    float transitionDurationIn, float transitionDurationOut)
{
    // `EffectSimple::Setup` (EffectSimple.h:58-81), byte for byte: a rising edge starts an "in"
    // fade whose progress the shader computes from `globalUniform.time - transitionBeginTime`, a
    // falling edge an "out" fade, and the effect is dispatched while it is active or while its out
    // fade has not run out.
    Transition &transition = effects[effect].transition;

    const bool wasActivePreviously = transition.isCurrentlyActive;
    transition.isCurrentlyActive = isActive;

    if (!wasActivePreviously && transition.isCurrentlyActive)
    {
        transition.transitionType = 0;
        transition.transitionBeginTime = currentTime;
        transition.transitionDuration = transitionDurationIn;
    }
    else if (wasActivePreviously && !transition.isCurrentlyActive)
    {
        transition.transitionType = 1;
        transition.transitionBeginTime = currentTime;
        transition.transitionDuration = transitionDurationOut;
    }

    return
        transition.isCurrentlyActive ||
        (transition.transitionType == 1 && currentTime - transition.transitionBeginTime <= transition.transitionDuration);
}

void RhiPostEffectPass::SetupNull(EffectId effect)
{
    // `EffectSimple::SetupNull` (EffectSimple.h:52-56): the effect turns off immediately, with no
    // fade-out.
    effects[effect].transition.isCurrentlyActive = false;
}

bool RhiPostEffectPass::DispatchEffect(nvrhi::ICommandList *pCommandList,
                                       Target &target,
                                       EffectId effect,
                                       bool sourceIsPing,
                                       const void *pPushConstant,
                                       uint32_t pushConstantSize,
                                       uint32_t groupsX,
                                       uint32_t groupsY)
{
    Effect &state = effects[effect];

    nvrhi::IComputePipeline *pipeline = state.pipelines[sourceIsPing ? 1 : 0];
    if (pipeline == nullptr)
    {
        return false;
    }

    const bool usesUniform = effect != EFFECT_CRT_DECODE;

    nvrhi::IBindingSet *framebufferSet = nullptr;
    if (effect == EFFECT_INVERSE_BW || effect == EFFECT_HUE_SHIFT)
    {
        framebufferSet = target.albedoSet;
    }
    else if (effect == EFFECT_WIPE)
    {
        framebufferSet = target.wipeSet;
    }
    else
    {
        framebufferSet = target.simpleSet;
    }

    if (framebufferSet == nullptr || (usesUniform && target.uniformSet == nullptr))
    {
        return false;
    }

    // The announce of the wipe source, the only image this dispatch binds that the entry point has
    // not announced yet; the two upscaled images and ALBEDO were announced there.
    if (effect == EFFECT_WIPE)
    {
        pCommandList->beginTrackingTextureState(target.textures[IMAGE_WIPE_SOURCE], nvrhi::AllSubresources,
                                                nvrhi::ResourceStates::UnorderedAccess);
    }

    nvrhi::ComputeState computeState;
    computeState.setPipeline(pipeline);
    computeState.addBindingSet(framebufferSet);
    if (usesUniform)
    {
        computeState.addBindingSet(target.uniformSet);
    }

    pCommandList->setComputeState(computeState);

    // The write needs the pipeline's layout, which `setComputeState` just installed, so the push
    // follows the state - the order `setPushConstants` requires (nvrhi.h:3430-3440).
    pCommandList->setPushConstants(pPushConstant, pushConstantSize);

    pCommandList->dispatch(groupsX, groupsY, 1);

    // The dispatch wrote the image of the role opposite to its source. Queue the same-state
    // UnorderedAccess requirement on it: NVRHI turns it into the same-state UAV barrier that orders
    // this write before the next dispatch's read (state-tracking.cpp:188-207; the compose's
    // per-iteration discipline). The requirement is committed by the next `setComputeState`.
    pCommandList->setTextureState(target.textures[sourceIsPing ? IMAGE_PONG : IMAGE_PING],
                                  nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

    return true;
}

bool RhiPostEffectPass::RecordWipe(nvrhi::ICommandList *pCommandList,
                                   Target &target,
                                   const QrPostEffectWipe &params,
                                   uint32_t upscaledWidth,
                                   uint32_t upscaledHeight,
                                   float currentTime,
                                   uint32_t currentFrameId,
                                   bool sourceIsPing)
{
    // `EffectWipe::Setup` (EffectWipe.h:56-135): the persistent push state first, then the
    // early-outs.
    wipePush.stripWidthInPixels =
        static_cast<uint32_t>(static_cast<float>(upscaledWidth) * std::clamp(params.stripWidth, 0.0f, 1.0f));

    if (params.beginNow)
    {
        wipePush.startFrameId = currentFrameId;
        wipePush.beginTime = currentTime;
        wipePush.endTime = currentTime + params.duration;
    }

    if (wipePush.stripWidthInPixels == 0 ||
        wipePush.beginTime >= wipePush.endTime ||
        currentTime >= wipePush.endTime)
    {
        return false;
    }

    if (params.beginNow)
    {
        // The legacy `beginNow` step captures the previous swapchain image into image 72
        // (EffectWipe.h:86-132). The RHI module is never given the skeleton's swapchain wraps, so
        // the capture is not ported and the wipe is skipped instead of sampling an image the
        // module cannot fill. Unreachable with the game's params (`pWipe` is never filled).
        if (!warnedWipeCapture)
        {
            warnedWipeCapture = true;
            LogMessage(print, "Warning: RHI: the post-effect wipe asked for its beginNow capture, which the RHI module does not record; the wipe is skipped");
        }
        return false;
    }

    const uint32_t groupsX = Utils::GetWorkGroupCount(upscaledWidth, EFFECT_GROUP_SIZE);
    const uint32_t groupsY = Utils::GetWorkGroupCount(upscaledHeight, EFFECT_GROUP_SIZE);

    return DispatchEffect(pCommandList, target, EFFECT_WIPE, sourceIsPing,
                          &wipePush, sizeof(wipePush), groupsX, groupsY);
}

void RhiPostEffectPass::AnnounceImage(nvrhi::ICommandList *pCommandList, Target &target, ImageSlot image) const
{
    pCommandList->beginTrackingTextureState(target.textures[image], nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->setTextureState(target.textures[image], nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

void RhiPostEffectPass::CopyPongToPing(nvrhi::ICommandList *pCommandList, Target &target) const
{
    // NVRHI's `copyTexture` queues the destination's CopyDest and the source's CopySource
    // transition and commits them itself (vulkan-texture.cpp:415-479), so no barrier is recorded
    // here. Both images are then moved back to the engine's resting UnorderedAccess - the same
    // pair the skeleton records after the FSR upscale (NvrhiFrameSkeleton.cpp:1001-1007), and the
    // pair RhiFsrPass.h documents: without the restore the present's announcement of 29 would name
    // a state the image is not in.
    pCommandList->copyTexture(target.textures[IMAGE_PING], nvrhi::TextureSlice(),
                              target.textures[IMAGE_PONG], nvrhi::TextureSlice());

    pCommandList->setTextureState(target.textures[IMAGE_PING], nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->setTextureState(target.textures[IMAGE_PONG], nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

void RhiPostEffectPass::ReleaseFramebufferSets(Target &target)
{
    // Anything a recorded list may still reference has to go through the frame context's retire
    // queue: the sets reference the wraps of engine images the GPU may still be reading. The queue
    // takes its reference now, so the handles below can be cleared immediately.
    if (frameContext != nullptr)
    {
        if (target.simpleSet != nullptr)
        {
            frameContext->Retire(target.simpleSet);
        }
        if (target.albedoSet != nullptr)
        {
            frameContext->Retire(target.albedoSet);
        }
        if (target.wipeSet != nullptr)
        {
            frameContext->Retire(target.wipeSet);
        }
    }

    target.simpleSet = nullptr;
    target.albedoSet = nullptr;
    target.wipeSet = nullptr;
}

void RhiPostEffectPass::ReleaseTarget(Target &target)
{
    ReleaseFramebufferSets(target);

    if (frameContext != nullptr)
    {
        for (nvrhi::TextureHandle &texture : target.textures)
        {
            if (texture != nullptr)
            {
                frameContext->Retire(texture);
            }
        }

        if (target.uniformSet != nullptr)
        {
            frameContext->Retire(target.uniformSet);
        }
    }

    for (nvrhi::TextureHandle &texture : target.textures)
    {
        texture = nullptr;
    }

    target.uniformSet = nullptr;
    target.uniformBuffer = nullptr;

    std::memset(target.imageHandles, 0, sizeof(target.imageHandles));
    target.renderWidth = 0;
    target.renderHeight = 0;
    target.upscaledWidth = 0;
    target.upscaledHeight = 0;
}

bool RhiPostEffectPass::LoadShader(const char *pFileName, nvrhi::ShaderHandle &result)
{
    const std::string path = shaderFolderPath + pFileName;

    // The helper stays silent about a missing or unreadable blob, so that this class keeps its own
    // warning and its 'created == false' path (RhiPipeline.h).
    result = rhi::loadShader(device, path, nvrhi::ShaderType::Compute, pFileName);
    if (result == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the post-effect pass shader \"" + path + "\"");
        return false;
    }

    return true;
}
