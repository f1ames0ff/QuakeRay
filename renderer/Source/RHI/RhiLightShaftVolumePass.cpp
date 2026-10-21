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

#include "RhiLightShaftVolumePass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"

#include <cstring>
#include <string>

namespace qray
{

namespace
{

constexpr const char *VOLUME_SHADER_FILE_NAME = "CmLightShaftVolume.comp.spv";
constexpr const char *PREFIX_SHADER_FILE_NAME = "CmLightShaftPrefix.comp.spv";

void LogMessage(const RhiLightShaftVolumePass::PrintFunction &print, const char *pMessage)
{
    if (print)
    {
        print(pMessage);
    }
}

}

RhiLightShaftVolumePass::RhiLightShaftVolumePass() = default;

RhiLightShaftVolumePass::~RhiLightShaftVolumePass() = default;

bool RhiLightShaftVolumePass::Create(nvrhi::IDevice *pDevice,
                                     rhi::RhiFrameContext *pFrameContext,
                                     const char *pShaderFolderPath,
                                     PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    device = pDevice;
    frameContext = pFrameContext;
    shaderFolderPath = pShaderFolderPath != nullptr ? pShaderFolderPath : "";
    print = std::move(pfnPrint);

    if (device == nullptr || frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the light-shaft volume pass needs a device and frame context");
        return false;
    }

    volumeShader = rhi::loadShader(device, shaderFolderPath + VOLUME_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass shader");
    prefixShader = rhi::loadShader(device, shaderFolderPath + PREFIX_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass prefix shader");
    if (volumeShader == nullptr || prefixShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to load the light-shaft volume shader");
        return false;
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::All;
        desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        tlasLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        paramsLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        uniformLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        volumeLayout = device->createBindingLayout(desc);
    }

    sampler = rhi::createSampler(device, nvrhi::SamplerDesc()
                                             .setMinFilter(true)
                                             .setMagFilter(true)
                                             .setMipFilter(true)
                                             .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp),
                                 "RhiLightShaftVolumePass sampler");

    if (tlasLayout == nullptr || paramsLayout == nullptr || uniformLayout == nullptr ||
        volumeLayout == nullptr || sampler == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a light-shaft volume binding layout");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(volumeShader);
        desc.addBindingLayout(tlasLayout);
        desc.addBindingLayout(paramsLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(volumeLayout);
        volumePipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass");
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(prefixShader);
        desc.addBindingLayout(volumeLayout);
        desc.addBindingLayout(uniformLayout);
        prefixPipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass prefix");
    }

    if (volumePipeline == nullptr || prefixPipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the light-shaft volume pipeline");
        return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!CreateVolumeTarget(targets[i], i))
        {
            LogMessage(print, "Warning: RHI: failed to create a light-shaft volume target");
            return false;
        }
    }

    created = true;
    return true;
}

bool RhiLightShaftVolumePass::CreateVolumeTarget(Target &target, uint32_t frameIndex)
{
    nvrhi::TextureDesc textureDesc;
    textureDesc.dimension = nvrhi::TextureDimension::Texture3D;
    textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
    textureDesc.width = VOLUME_SIZE_X;
    textureDesc.height = VOLUME_SIZE_Y;
    textureDesc.depth = VOLUME_SIZE_Z;
    textureDesc.mipLevels = 1;
    textureDesc.isUAV = true;
    textureDesc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    textureDesc.keepInitialState = true;

    const std::string debugName = "RhiLightShaftVolumePass slot " + std::to_string(frameIndex);
    target.texture = rhi::createTexture(device, textureDesc, debugName);
    if (target.texture == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc uavSetDesc;
    uavSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        0, target.texture, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(),
        nvrhi::TextureDimension::Texture3D));
    target.uavSet = device->createBindingSet(uavSetDesc, volumeLayout);
    if (target.uavSet == nullptr)
    {
        return false;
    }

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.isVolatile = false;
    bufferDesc.keepInitialState = true;
    bufferDesc.debugName = "RhiLightShaftVolumePass params";
    bufferDesc.byteSize = sizeof(Params);
    target.paramsBuffer = rhi::createBuffer(device, bufferDesc, bufferDesc.debugName);
    if (target.paramsBuffer == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc paramsSetDesc;
    paramsSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, target.paramsBuffer));
    target.paramsSet = device->createBindingSet(paramsSetDesc, paramsLayout);

    return target.paramsSet != nullptr;
}

bool RhiLightShaftVolumePass::PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer)
{
    if (target.uniformSet != nullptr && target.uniformBuffer == pUniformBuffer)
    {
        return true;
    }

    if (pUniformBuffer == nullptr)
    {
        return false;
    }

    if (target.uniformSet != nullptr)
    {
        frameContext->Retire(target.uniformSet);
    }
    target.uniformSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, pUniformBuffer));
    target.uniformSet = device->createBindingSet(setDesc, uniformLayout);
    if (target.uniformSet == nullptr)
    {
        return false;
    }

    target.uniformBuffer = pUniformBuffer;
    return true;
}

bool RhiLightShaftVolumePass::PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel)
{
    if (pTopLevel == nullptr)
    {
        return false;
    }

    if (target.tlasSet != nullptr && target.topLevel == pTopLevel)
    {
        return true;
    }

    if (target.tlasSet != nullptr)
    {
        frameContext->Retire(target.tlasSet);
    }
    target.tlasSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, pTopLevel));
    target.tlasSet = device->createBindingSet(setDesc, tlasLayout);
    if (target.tlasSet == nullptr)
    {
        return false;
    }

    target.topLevel = pTopLevel;
    return true;
}

bool RhiLightShaftVolumePass::Render(nvrhi::ICommandList *pCommandList,
                                     uint32_t frameIndex,
                                     nvrhi::IBuffer *pUniformBuffer,
                                     nvrhi::rt::IAccelStruct *pTopLevel,
                                     const Params &params)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }

    Target &target = targets[frameIndex];
    if (target.texture == nullptr || target.uavSet == nullptr || target.paramsSet == nullptr)
    {
        return false;
    }

    if (!PrepareUniformSet(target, pUniformBuffer) || !PrepareTlasSet(target, pTopLevel))
    {
        return false;
    }

    rhi::writeBuffer(pCommandList, target.paramsBuffer, &params, sizeof(params));
    pCommandList->setTextureState(target.texture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);

    nvrhi::ComputeState state;
    state.setPipeline(volumePipeline);
    state.addBindingSet(target.tlasSet);
    state.addBindingSet(target.paramsSet);
    state.addBindingSet(target.uniformSet);
    state.addBindingSet(target.uavSet);
    pCommandList->setComputeState(state);

    const uint32_t groupsX = (VOLUME_SIZE_X + GROUP_SIZE_X - 1) / GROUP_SIZE_X;
    const uint32_t groupsY = (VOLUME_SIZE_Y + GROUP_SIZE_Y - 1) / GROUP_SIZE_Y;
    pCommandList->dispatch(groupsX, groupsY, VOLUME_SIZE_Z);

    pCommandList->commitBarriers();

    nvrhi::ComputeState prefixState;
    prefixState.setPipeline(prefixPipeline);
    prefixState.addBindingSet(target.uavSet);
    prefixState.addBindingSet(target.uniformSet);
    pCommandList->setComputeState(prefixState);
    pCommandList->dispatch((VOLUME_SIZE_X + 7) / 8, (VOLUME_SIZE_Y + 7) / 8, 1);

    pCommandList->setTextureState(target.texture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);

    return true;
}

nvrhi::ITexture *RhiLightShaftVolumePass::GetTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return targets[frameIndex].texture;
}

}
