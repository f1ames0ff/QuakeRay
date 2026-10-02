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

#include "RhiLensFlarePass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"

#include "../Generated/ShaderCommonC.h"
#include "../Tonemapping.h"
#include "../Utils.h"

#include <algorithm>
#include <string>

using namespace qray;

namespace
{

const char *const FLARE_SHADER_FILE_NAME = "CmLensFlare.comp.spv";

constexpr uint32_t FLARE_SRV_SLOT = 0;
constexpr uint32_t FLARE_SAMPLER_SLOT = 1;
constexpr uint32_t FLARE_DESTINATION_SLOT = 0;

constexpr uint32_t FLARE_PASS_EXTRACT = 0;
constexpr uint32_t FLARE_PASS_WIDE = 1;
constexpr uint32_t FLARE_PASS_COMPOSITE = 2;

constexpr float FLARE_KNEE = 0.5f;

struct LensFlarePush
{
    uint32_t passMode;
    float threshold;
    float knee;
    float padding;
};

void LogMessage(const RhiLensFlarePass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

nvrhi::BindingLayoutHandle CreateSourceLayout(nvrhi::IDevice *device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                               .setShaderResourceOffset(0)
                               .setSamplerOffset(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(FLARE_SRV_SLOT));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(FLARE_SAMPLER_SLOT));

    return device->createBindingLayout(desc);
}

}

RhiLensFlarePass::RhiLensFlarePass() = default;

RhiLensFlarePass::~RhiLensFlarePass()
{
    if (device != nullptr)
    {
        device->waitForIdle();
    }

    for (Target &target : targets)
    {
        ClearTarget(target);
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        tonemappingSets[i] = nullptr;
        tonemappingBuffers[i] = nullptr;
        tonemappingHandles[i] = 0;
    }

    sampler = nullptr;
    pipeline = nullptr;
    pushConstantLayout = nullptr;
    destinationLayout = nullptr;
    tonemappingLayout = nullptr;
    secondSourceLayout = nullptr;
    sourceLayout = nullptr;
    flareShader = nullptr;
}

bool RhiLensFlarePass::Create(nvrhi::IDevice *pDevice,
                              rhi::RhiFrameContext *pFrameContext,
                              const Tonemapping *pTonemapping,
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
    tonemapping = pTonemapping;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the lens flare pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the lens flare pass needs the frame context of the RHI layer");
        return false;
    }

    if (pTonemapping == nullptr || pTonemapping->GetElementSize() == 0)
    {
        LogMessage(print, "Warning: RHI: the lens flare pass needs the engine tonemapping buffer");
        return false;
    }

    flareShader = rhi::loadShader(device, shaderFolderPath + FLARE_SHADER_FILE_NAME,
                                  nvrhi::ShaderType::Compute, FLARE_SHADER_FILE_NAME);
    if (flareShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the lens flare pass shader \"" +
                              shaderFolderPath + FLARE_SHADER_FILE_NAME + "\"");
        return false;
    }

    sourceLayout = CreateSourceLayout(device);
    secondSourceLayout = CreateSourceLayout(device);

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(FLARE_DESTINATION_SLOT));

        destinationLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(BINDING_LUM_HISTOGRAM));

        tonemappingLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, PUSH_SIZE));

        pushConstantLayout = device->createBindingLayout(desc);
    }

    if (sourceLayout == nullptr || secondSourceLayout == nullptr || destinationLayout == nullptr ||
        tonemappingLayout == nullptr || pushConstantLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a lens flare pass binding layout");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(flareShader);
        desc.addBindingLayout(sourceLayout);
        desc.addBindingLayout(secondSourceLayout);
        desc.addBindingLayout(tonemappingLayout);
        desc.addBindingLayout(destinationLayout);
        desc.addBindingLayout(pushConstantLayout);

        pipeline = rhi::createComputePipeline(device, desc, "RhiLensFlarePass compute");
    }

    if (pipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the lens flare pass compute pipeline");
        return false;
    }

    {
        const nvrhi::SamplerDesc desc = nvrhi::SamplerDesc()
            .setMinFilter(true)
            .setMagFilter(true)
            .setMipFilter(false)
            .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);

        sampler = rhi::createSampler(device, desc, "RhiLensFlarePass sampler");

        if (sampler == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the lens flare pass sampler");
            return false;
        }
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!PrepareTonemappingSet(i))
        {
            return false;
        }
    }

    created = true;
    return true;
}

bool RhiLensFlarePass::PrepareTonemappingSet(uint32_t frameIndex)
{
    if (tonemapping == nullptr)
    {
        return false;
    }

    const uint64_t bufferHandle =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tonemapping->GetBuffer(frameIndex)));

    if (bufferHandle == 0)
    {
        if (!warnedBadTonemapping)
        {
            warnedBadTonemapping = true;
            LogMessage(print, "Warning: RHI: the lens flare pass got no engine tonemapping buffer");
        }
        return false;
    }

    if (tonemappingSets[frameIndex] != nullptr && tonemappingHandles[frameIndex] == bufferHandle)
    {
        return true;
    }

    if (tonemappingSets[frameIndex] != nullptr)
    {
        frameContext->Retire(tonemappingSets[frameIndex]);
        tonemappingSets[frameIndex] = nullptr;
        tonemappingBuffers[frameIndex] = nullptr;
    }

    const uint32_t elementSize = tonemapping->GetElementSize();
    if (elementSize == 0)
    {
        return false;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = elementSize;
    desc.structStride = elementSize;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.debugName = "RhiLensFlarePass tonemapping wrap " + std::to_string(frameIndex);

    tonemappingBuffers[frameIndex] = device->createHandleForNativeBuffer(
        nvrhi::ObjectTypes::VK_Buffer, nvrhi::Object(bufferHandle), desc);

    if (tonemappingBuffers[frameIndex] == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to wrap the engine tonemapping buffer for the lens flare pass");
        return false;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LUM_HISTOGRAM, tonemappingBuffers[frameIndex]));

    tonemappingSets[frameIndex] = device->createBindingSet(setDesc, tonemappingLayout);
    if (tonemappingSets[frameIndex] == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the lens flare pass tonemapping binding set");
        return false;
    }

    tonemappingHandles[frameIndex] = bufferHandle;
    return true;
}

bool RhiLensFlarePass::PrepareSourceSet(Target &target, nvrhi::ITexture *pSource)
{
    if (target.sourceSet != nullptr && target.sourceTexture == pSource)
    {
        return true;
    }

    if (target.sourceSet != nullptr)
    {
        frameContext->Retire(target.sourceSet);
        target.sourceSet = nullptr;
        target.sourceTexture = nullptr;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(FLARE_SRV_SLOT, pSource));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(FLARE_SAMPLER_SLOT, sampler));

    target.sourceSet = device->createBindingSet(setDesc, sourceLayout);
    if (target.sourceSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the lens flare pass source binding set");
        return false;
    }

    target.sourceTexture = pSource;
    return true;
}

void RhiLensFlarePass::Render(nvrhi::ICommandList *pCommandList,
                              uint32_t frameIndex,
                              nvrhi::ITexture *pHdrSource,
                              uint32_t width,
                              uint32_t height,
                              const Settings &settings)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    if (!(settings.intensity > 0.0f))
    {
        return;
    }

    if (pHdrSource == nullptr || width == 0 || height == 0)
    {
        return;
    }

    const nvrhi::TextureDesc &sourceDesc = pHdrSource->getDesc();

    if (sourceDesc.dimension != nvrhi::TextureDimension::Texture2D ||
        sourceDesc.format != nvrhi::Format::R11G11B10_FLOAT ||
        sourceDesc.width != width || sourceDesc.height != height)
    {
        if (!warnedBadSource)
        {
            warnedBadSource = true;
            LogMessage(print, "Warning: RHI: the lens flare pass needs the render-sized r11g11b10f bloom input");
        }
        return;
    }

    Target &target = targets[frameIndex];

    if (target.width != width || target.height != height)
    {
        ReleaseTarget(target);

        if (!CreateTarget(target, width, height))
        {
            LogMessage(print, "Warning: RHI: failed to create the lens flare pass targets");
            return;
        }
    }

    if (!PrepareTonemappingSet(frameIndex) || !PrepareSourceSet(target, pHdrSource))
    {
        return;
    }

    pCommandList->beginTrackingTextureState(pHdrSource, nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);

    DispatchPass(pCommandList, frameIndex, pipeline, target.sourceSet, target.wide.srvSet, target.bright.uavSet,
                 target.bright.handle->getDesc().width, target.bright.handle->getDesc().height,
                 FLARE_PASS_EXTRACT, settings.threshold, FLARE_KNEE);

    DispatchPass(pCommandList, frameIndex, pipeline, target.bright.srvSet, target.result.srvSet, target.wide.uavSet,
                 target.wide.handle->getDesc().width, target.wide.handle->getDesc().height,
                 FLARE_PASS_WIDE, settings.threshold, FLARE_KNEE);

    DispatchPass(pCommandList, frameIndex, pipeline, target.bright.srvSet, target.wide.srvSet, target.result.uavSet,
                 target.result.handle->getDesc().width, target.result.handle->getDesc().height,
                 FLARE_PASS_COMPOSITE, settings.threshold, FLARE_KNEE);

    pCommandList->setTextureState(pHdrSource, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

bool RhiLensFlarePass::CreateTarget(Target &target, uint32_t width, uint32_t height)
{
    const uint32_t halfWidth = std::max(1u, (width + 1) / 2);
    const uint32_t halfHeight = std::max(1u, (height + 1) / 2);
    const uint32_t quarterWidth = std::max(1u, (halfWidth + 1) / 2);
    const uint32_t quarterHeight = std::max(1u, (halfHeight + 1) / 2);
    const uint32_t wideWidth = std::max(1u, (quarterWidth + WIDE_DIVISOR - 1) / WIDE_DIVISOR);
    const uint32_t wideHeight = std::max(1u, (quarterHeight + WIDE_DIVISOR - 1) / WIDE_DIVISOR);

    if (!CreateTexture(target.bright, quarterWidth, quarterHeight, "RhiLensFlarePass bright"))
    {
        return false;
    }

    if (!CreateTexture(target.wide, wideWidth, wideHeight, "RhiLensFlarePass wide"))
    {
        return false;
    }

    if (!CreateTexture(target.result, quarterWidth, quarterHeight, "RhiLensFlarePass result"))
    {
        return false;
    }

    target.width = width;
    target.height = height;

    return true;
}

bool RhiLensFlarePass::CreateTexture(Texture &texture, uint32_t width, uint32_t height, const std::string &name)
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = 1;
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;

    texture.handle = rhi::createTexture(device, desc, name);
    if (texture.handle == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc srvSetDesc;
    srvSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(FLARE_SRV_SLOT, texture.handle));
    srvSetDesc.addItem(nvrhi::BindingSetItem::Sampler(FLARE_SAMPLER_SLOT, sampler));

    texture.srvSet = device->createBindingSet(srvSetDesc, sourceLayout);
    if (texture.srvSet == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc uavSetDesc;
    uavSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(FLARE_DESTINATION_SLOT, texture.handle));

    texture.uavSet = device->createBindingSet(uavSetDesc, destinationLayout);
    if (texture.uavSet == nullptr)
    {
        return false;
    }

    return true;
}

void RhiLensFlarePass::DispatchPass(nvrhi::ICommandList *pCommandList,
                                    uint32_t frameIndex,
                                    nvrhi::IComputePipeline *pPipeline,
                                    nvrhi::IBindingSet *pSourceSet,
                                    nvrhi::IBindingSet *pSecondSet,
                                    nvrhi::IBindingSet *pDestinationSet,
                                    uint32_t destinationWidth,
                                    uint32_t destinationHeight,
                                    uint32_t passMode,
                                    float threshold,
                                    float knee)
{
    const LensFlarePush push = { passMode, threshold, knee, 0.0f };

    RecordDispatch(pCommandList, pPipeline,
                   { pSourceSet, pSecondSet, tonemappingSets[frameIndex], pDestinationSet },
                   destinationWidth, destinationHeight, &push, sizeof(push));
}

void RhiLensFlarePass::RecordDispatch(nvrhi::ICommandList *pCommandList,
                                      nvrhi::IComputePipeline *pPipeline,
                                      std::initializer_list<nvrhi::IBindingSet *> sets,
                                      uint32_t destinationWidth,
                                      uint32_t destinationHeight,
                                      const void *pPushData,
                                      uint32_t pushSize)
{
    nvrhi::ComputeState state;
    state.setPipeline(pPipeline);

    for (nvrhi::IBindingSet *pSet : sets)
    {
        state.addBindingSet(pSet);
    }

    pCommandList->setComputeState(state);

    if (pPushData != nullptr)
    {
        pCommandList->setPushConstants(pPushData, pushSize);
    }

    pCommandList->dispatch(Utils::GetWorkGroupCount(destinationWidth, GROUP_SIZE),
                           Utils::GetWorkGroupCount(destinationHeight, GROUP_SIZE), 1);
}

nvrhi::ITexture *RhiLensFlarePass::GetResultTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return targets[frameIndex].result.handle.Get();
}

void RhiLensFlarePass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseTarget(target);
    }
}

void RhiLensFlarePass::ReleaseTexture(Texture &texture)
{
    if (frameContext != nullptr)
    {
        if (texture.srvSet != nullptr)
        {
            frameContext->Retire(texture.srvSet);
        }
        if (texture.uavSet != nullptr)
        {
            frameContext->Retire(texture.uavSet);
        }
        if (texture.handle != nullptr)
        {
            frameContext->Retire(texture.handle);
        }
    }

    texture.srvSet = nullptr;
    texture.uavSet = nullptr;
    texture.handle = nullptr;
}

void RhiLensFlarePass::ReleaseTarget(Target &target)
{
    if (frameContext != nullptr && target.sourceSet != nullptr)
    {
        frameContext->Retire(target.sourceSet);
    }

    target.sourceSet = nullptr;
    target.sourceTexture = nullptr;

    ReleaseTexture(target.bright);
    ReleaseTexture(target.wide);
    ReleaseTexture(target.result);

    target.width = 0;
    target.height = 0;
}

void RhiLensFlarePass::ClearTexture(Texture &texture)
{
    texture.srvSet = nullptr;
    texture.uavSet = nullptr;
    texture.handle = nullptr;
}

void RhiLensFlarePass::ClearTarget(Target &target)
{
    target.sourceSet = nullptr;
    target.sourceTexture = nullptr;

    ClearTexture(target.bright);
    ClearTexture(target.wide);
    ClearTexture(target.result);

    target.width = 0;
    target.height = 0;
}
