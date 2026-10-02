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

#include "RhiBloomPass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"

#include "../Generated/ShaderCommonC.h"
#include "../Tonemapping.h"
#include "../Utils.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace qray;

namespace
{

const char *const DOWNSAMPLE_SHADER_FILE_NAME = "CmBloomDownsample.comp.spv";
const char *const UPSAMPLE_SHADER_FILE_NAME = "CmBloomUpsample.comp.spv";

constexpr uint32_t BLOOM_SRV_SLOT = 0;
constexpr uint32_t BLOOM_SAMPLER_SLOT = 1;
constexpr uint32_t BLOOM_DESTINATION_SLOT = 0;

struct BloomDownsamplePush
{
    uint32_t extract;
    float threshold;
    float knee;
    float padding;
};

struct BloomUpsamplePush
{
    float scatter;
    uint32_t hasSource;
    float padding0;
    float padding1;
};

void LogMessage(const RhiBloomPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

uint32_t QualityToMaxLevels(uint32_t quality)
{
    if (quality == 0)
    {
        return 5;
    }

    if (quality == 1)
    {
        return 6;
    }

    return 7;
}

uint32_t ComputeLevelCount(float radius, uint32_t width, uint32_t height, uint32_t maxLevels)
{
    const float sanitizedRadius = radius == radius ? std::min(std::max(radius, 0.0f), 1.0f) : 0.04f;
    const float characteristic = std::max(sanitizedRadius * static_cast<float>(std::max(width, height)), 1.0f);
    const uint32_t levels = 1 + static_cast<uint32_t>(std::ceil(std::log2(characteristic)));

    return std::min(std::max(levels, 2u), maxLevels);
}

nvrhi::BindingLayoutHandle CreateSourceLayout(nvrhi::IDevice *device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                               .setShaderResourceOffset(0)
                               .setSamplerOffset(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(BLOOM_SRV_SLOT));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(BLOOM_SAMPLER_SLOT));

    return device->createBindingLayout(desc);
}

}

RhiBloomPass::RhiBloomPass() = default;

RhiBloomPass::~RhiBloomPass()
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

    emptySet = nullptr;
    sampler = nullptr;
    upsamplePipeline = nullptr;
    downsamplePipeline = nullptr;
    emptyLayout = nullptr;
    pushConstantLayout = nullptr;
    destinationLayout = nullptr;
    tonemappingLayout = nullptr;
    secondSourceLayout = nullptr;
    sourceLayout = nullptr;
    upsampleShader = nullptr;
    downsampleShader = nullptr;
}

bool RhiBloomPass::Create(nvrhi::IDevice *pDevice,
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
        LogMessage(print, "Warning: RHI: the bloom pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the bloom pass needs the frame context of the RHI layer");
        return false;
    }

    if (pTonemapping == nullptr || pTonemapping->GetElementSize() == 0)
    {
        LogMessage(print, "Warning: RHI: the bloom pass needs the engine tonemapping buffer");
        return false;
    }

    downsampleShader = rhi::loadShader(device, shaderFolderPath + DOWNSAMPLE_SHADER_FILE_NAME,
                                       nvrhi::ShaderType::Compute, DOWNSAMPLE_SHADER_FILE_NAME);
    if (downsampleShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the bloom pass shader \"" +
                              shaderFolderPath + DOWNSAMPLE_SHADER_FILE_NAME + "\"");
        return false;
    }

    upsampleShader = rhi::loadShader(device, shaderFolderPath + UPSAMPLE_SHADER_FILE_NAME,
                                     nvrhi::ShaderType::Compute, UPSAMPLE_SHADER_FILE_NAME);
    if (upsampleShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: cannot load the bloom pass shader \"" +
                              shaderFolderPath + UPSAMPLE_SHADER_FILE_NAME + "\"");
        return false;
    }

    sourceLayout = CreateSourceLayout(device);
    secondSourceLayout = CreateSourceLayout(device);

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(BLOOM_DESTINATION_SLOT));

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

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;

        emptyLayout = device->createBindingLayout(desc);
    }

    if (sourceLayout == nullptr || secondSourceLayout == nullptr || destinationLayout == nullptr ||
        tonemappingLayout == nullptr || pushConstantLayout == nullptr || emptyLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a bloom pass binding layout");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(downsampleShader);
        desc.addBindingLayout(sourceLayout);
        desc.addBindingLayout(emptyLayout);
        desc.addBindingLayout(tonemappingLayout);
        desc.addBindingLayout(destinationLayout);
        desc.addBindingLayout(pushConstantLayout);

        downsamplePipeline = rhi::createComputePipeline(device, desc, "RhiBloomPass downsample");
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(upsampleShader);
        desc.addBindingLayout(sourceLayout);
        desc.addBindingLayout(secondSourceLayout);
        desc.addBindingLayout(tonemappingLayout);
        desc.addBindingLayout(destinationLayout);
        desc.addBindingLayout(pushConstantLayout);

        upsamplePipeline = rhi::createComputePipeline(device, desc, "RhiBloomPass upsample");
    }

    if (downsamplePipeline == nullptr || upsamplePipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a bloom pass compute pipeline");
        return false;
    }

    {
        const nvrhi::SamplerDesc desc = nvrhi::SamplerDesc()
            .setMinFilter(true)
            .setMagFilter(true)
            .setMipFilter(false)
            .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);

        sampler = rhi::createSampler(device, desc, "RhiBloomPass sampler");

        if (sampler == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the bloom pass sampler");
            return false;
        }
    }

    emptySet = device->createBindingSet(nvrhi::BindingSetDesc(), emptyLayout);
    if (emptySet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the bloom pass empty binding set");
        return false;
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

bool RhiBloomPass::PrepareTonemappingSet(uint32_t frameIndex)
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
            LogMessage(print, "Warning: RHI: the bloom pass got no engine tonemapping buffer");
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
    desc.debugName = "RhiBloomPass tonemapping wrap " + std::to_string(frameIndex);

    tonemappingBuffers[frameIndex] = device->createHandleForNativeBuffer(
        nvrhi::ObjectTypes::VK_Buffer, nvrhi::Object(bufferHandle), desc);

    if (tonemappingBuffers[frameIndex] == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to wrap the engine tonemapping buffer for the bloom pass");
        return false;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_LUM_HISTOGRAM, tonemappingBuffers[frameIndex]));

    tonemappingSets[frameIndex] = device->createBindingSet(setDesc, tonemappingLayout);
    if (tonemappingSets[frameIndex] == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the bloom pass tonemapping binding set");
        return false;
    }

    tonemappingHandles[frameIndex] = bufferHandle;
    return true;
}

bool RhiBloomPass::PrepareSourceSet(Target &target, nvrhi::ITexture *pSource)
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
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(BLOOM_SRV_SLOT, pSource));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(BLOOM_SAMPLER_SLOT, sampler));

    target.sourceSet = device->createBindingSet(setDesc, sourceLayout);
    if (target.sourceSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the bloom pass source binding set");
        return false;
    }

    target.sourceTexture = pSource;
    return true;
}

void RhiBloomPass::Render(nvrhi::ICommandList *pCommandList,
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
            LogMessage(print, "Warning: RHI: the bloom pass needs the render-sized r11g11b10f bloom input");
        }
        return;
    }

    Target &target = targets[frameIndex];
    const bool quarterBase = settings.quality == 0;

    if (target.width != width || target.height != height || target.quarterBase != quarterBase)
    {
        ReleaseTarget(target);

        if (!CreateTarget(target, width, height, quarterBase))
        {
            LogMessage(print, "Warning: RHI: failed to create the bloom pass targets");
            return;
        }
    }

    if (!PrepareTonemappingSet(frameIndex) || !PrepareSourceSet(target, pHdrSource))
    {
        return;
    }

    const uint32_t maxLevels = QualityToMaxLevels(settings.quality);
    const uint32_t requestedLevels = ComputeLevelCount(settings.radius, width, height, maxLevels);
    const uint32_t levelCount = std::min(std::max(requestedLevels, 1u), target.downCount);

    pCommandList->beginTrackingTextureState(pHdrSource, nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);

    if (quarterBase)
    {
        DispatchDownsample(pCommandList, frameIndex, target.sourceSet, target.scratch.uavSet,
                           target.scratch.texture->getDesc().width, target.scratch.texture->getDesc().height,
                           true, settings.threshold, settings.knee);
        DispatchDownsample(pCommandList, frameIndex, target.scratch.srvSet, target.down[0].uavSet,
                           target.down[0].texture->getDesc().width, target.down[0].texture->getDesc().height,
                           false, settings.threshold, settings.knee);
    }
    else
    {
        DispatchDownsample(pCommandList, frameIndex, target.sourceSet, target.down[0].uavSet,
                           target.down[0].texture->getDesc().width, target.down[0].texture->getDesc().height,
                           true, settings.threshold, settings.knee);
    }

    for (uint32_t i = 1; i < levelCount; i++)
    {
        DispatchDownsample(pCommandList, frameIndex, target.down[i - 1].srvSet, target.down[i].uavSet,
                           target.down[i].texture->getDesc().width, target.down[i].texture->getDesc().height,
                           false, settings.threshold, settings.knee);
    }

    if (levelCount == 1)
    {
        DispatchUpsample(pCommandList, frameIndex, target.down[0].srvSet, target.down[0].srvSet,
                         target.result.uavSet, target.result.texture->getDesc().width,
                         target.result.texture->getDesc().height, !quarterBase, 0.0f);
    }
    else
    {
        for (int32_t i = static_cast<int32_t>(levelCount) - 2; i >= 0; i--)
        {
            nvrhi::IBindingSet *pCoarseSet = i == static_cast<int32_t>(levelCount) - 2
                ? target.down[levelCount - 1].srvSet
                : target.up[i + 1].srvSet;

            nvrhi::IBindingSet *pDestinationSet = nullptr;
            uint32_t destinationWidth = 0;
            uint32_t destinationHeight = 0;

            if (i == 0)
            {
                if (quarterBase)
                {
                    pDestinationSet = target.up[0].uavSet;
                    destinationWidth = target.up[0].texture->getDesc().width;
                    destinationHeight = target.up[0].texture->getDesc().height;
                }
                else
                {
                    pDestinationSet = target.result.uavSet;
                    destinationWidth = target.result.texture->getDesc().width;
                    destinationHeight = target.result.texture->getDesc().height;
                }
            }
            else
            {
                pDestinationSet = target.up[i].uavSet;
                destinationWidth = target.up[i].texture->getDesc().width;
                destinationHeight = target.up[i].texture->getDesc().height;
            }

            DispatchUpsample(pCommandList, frameIndex, target.down[i].srvSet, pCoarseSet, pDestinationSet,
                             destinationWidth, destinationHeight, true, settings.scatter);
        }

        if (quarterBase)
        {
            DispatchUpsample(pCommandList, frameIndex, target.down[0].srvSet, target.up[0].srvSet,
                             target.result.uavSet, target.result.texture->getDesc().width,
                             target.result.texture->getDesc().height, false, 0.0f);
        }
    }

    pCommandList->setTextureState(pHdrSource, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

bool RhiBloomPass::CreateTarget(Target &target, uint32_t width, uint32_t height, bool quarterBase)
{
    const uint32_t halfWidth = std::max(1u, (width + 1) / 2);
    const uint32_t halfHeight = std::max(1u, (height + 1) / 2);
    const uint32_t baseWidth = quarterBase ? std::max(1u, (halfWidth + 1) / 2) : halfWidth;
    const uint32_t baseHeight = quarterBase ? std::max(1u, (halfHeight + 1) / 2) : halfHeight;

    uint32_t sizesX[MAX_LEVELS] = {};
    uint32_t sizesY[MAX_LEVELS] = {};
    uint32_t count = 0;
    uint32_t levelWidth = baseWidth;
    uint32_t levelHeight = baseHeight;

    while (count < MAX_LEVELS)
    {
        sizesX[count] = levelWidth;
        sizesY[count] = levelHeight;
        count++;

        if (levelWidth == 1 && levelHeight == 1)
        {
            break;
        }

        levelWidth = std::max(1u, (levelWidth + 1) / 2);
        levelHeight = std::max(1u, (levelHeight + 1) / 2);
    }

    for (uint32_t i = 0; i < count; i++)
    {
        if (!CreateLevel(target.down[i], sizesX[i], sizesY[i], "RhiBloomPass down " + std::to_string(i)))
        {
            return false;
        }
    }

    target.downCount = count;

    if (quarterBase)
    {
        if (!CreateLevel(target.scratch, halfWidth, halfHeight, "RhiBloomPass scratch"))
        {
            return false;
        }
    }

    for (uint32_t i = 0; i + 1 < count; i++)
    {
        if (i == 0 && !quarterBase)
        {
            continue;
        }

        if (!CreateLevel(target.up[i], sizesX[i], sizesY[i], "RhiBloomPass up " + std::to_string(i)))
        {
            return false;
        }
    }

    if (!CreateLevel(target.result, halfWidth, halfHeight, "RhiBloomPass result"))
    {
        return false;
    }

    target.width = width;
    target.height = height;
    target.quarterBase = quarterBase;

    return true;
}

bool RhiBloomPass::CreateLevel(Level &level, uint32_t width, uint32_t height, const std::string &name)
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

    level.texture = rhi::createTexture(device, desc, name);
    if (level.texture == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc srvSetDesc;
    srvSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(BLOOM_SRV_SLOT, level.texture));
    srvSetDesc.addItem(nvrhi::BindingSetItem::Sampler(BLOOM_SAMPLER_SLOT, sampler));

    level.srvSet = device->createBindingSet(srvSetDesc, sourceLayout);
    if (level.srvSet == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc uavSetDesc;
    uavSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(BLOOM_DESTINATION_SLOT, level.texture));

    level.uavSet = device->createBindingSet(uavSetDesc, destinationLayout);
    if (level.uavSet == nullptr)
    {
        return false;
    }

    return true;
}

void RhiBloomPass::DispatchDownsample(nvrhi::ICommandList *pCommandList,
                                      uint32_t frameIndex,
                                      nvrhi::IBindingSet *pSourceSet,
                                      nvrhi::IBindingSet *pDestinationSet,
                                      uint32_t destinationWidth,
                                      uint32_t destinationHeight,
                                      bool extract,
                                      float threshold,
                                      float knee)
{
    const BloomDownsamplePush push = { extract ? 1u : 0u, threshold, knee, 0.0f };

    RecordDispatch(pCommandList, downsamplePipeline,
                   { pSourceSet, emptySet, tonemappingSets[frameIndex], pDestinationSet },
                   destinationWidth, destinationHeight, &push, sizeof(push));
}

void RhiBloomPass::DispatchUpsample(nvrhi::ICommandList *pCommandList,
                                    uint32_t frameIndex,
                                    nvrhi::IBindingSet *pSourceSet,
                                    nvrhi::IBindingSet *pCoarseSet,
                                    nvrhi::IBindingSet *pDestinationSet,
                                    uint32_t destinationWidth,
                                    uint32_t destinationHeight,
                                    bool hasSource,
                                    float scatter)
{
    const BloomUpsamplePush push = { scatter, hasSource ? 1u : 0u, 0.0f, 0.0f };

    RecordDispatch(pCommandList, upsamplePipeline,
                   { pSourceSet, pCoarseSet, tonemappingSets[frameIndex], pDestinationSet },
                   destinationWidth, destinationHeight, &push, sizeof(push));
}

void RhiBloomPass::RecordDispatch(nvrhi::ICommandList *pCommandList,
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

nvrhi::ITexture *RhiBloomPass::GetResultTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return targets[frameIndex].result.texture.Get();
}

void RhiBloomPass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseTarget(target);
    }
}

void RhiBloomPass::ReleaseLevel(Level &level)
{
    if (frameContext != nullptr)
    {
        if (level.srvSet != nullptr)
        {
            frameContext->Retire(level.srvSet);
        }
        if (level.uavSet != nullptr)
        {
            frameContext->Retire(level.uavSet);
        }
        if (level.texture != nullptr)
        {
            frameContext->Retire(level.texture);
        }
    }

    level.srvSet = nullptr;
    level.uavSet = nullptr;
    level.texture = nullptr;
}

void RhiBloomPass::ReleaseTarget(Target &target)
{
    if (frameContext != nullptr && target.sourceSet != nullptr)
    {
        frameContext->Retire(target.sourceSet);
    }

    target.sourceSet = nullptr;
    target.sourceTexture = nullptr;

    for (Level &level : target.down)
    {
        ReleaseLevel(level);
    }

    for (Level &level : target.up)
    {
        ReleaseLevel(level);
    }

    ReleaseLevel(target.scratch);
    ReleaseLevel(target.result);

    target.width = 0;
    target.height = 0;
    target.quarterBase = false;
    target.downCount = 0;
}

void RhiBloomPass::ClearLevel(Level &level)
{
    level.srvSet = nullptr;
    level.uavSet = nullptr;
    level.texture = nullptr;
}

void RhiBloomPass::ClearTarget(Target &target)
{
    target.sourceSet = nullptr;
    target.sourceTexture = nullptr;

    for (Level &level : target.down)
    {
        ClearLevel(level);
    }

    for (Level &level : target.up)
    {
        ClearLevel(level);
    }

    ClearLevel(target.scratch);
    ClearLevel(target.result);

    target.width = 0;
    target.height = 0;
    target.quarterBase = false;
    target.downCount = 0;
}
