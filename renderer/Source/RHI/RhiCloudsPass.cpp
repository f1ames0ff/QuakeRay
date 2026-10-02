// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#include "RhiCloudsPass.h"

#include "../Utils.h"
#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"

#include <algorithm>
#include <cstring>

namespace qray
{

namespace
{

constexpr const char *LAYER_SHADER_FILE_NAME = "CmSkyClouds.comp.spv";
constexpr const char *SHADOW_SHADER_FILE_NAME = "CmCloudShadow.comp.spv";

constexpr uint32_t THREAD_GROUP_SIZE = 16;

void LogMessage(const RhiCloudsPass::PrintFunction &print, const char *pMessage)
{
    if (print)
    {
        print(pMessage);
    }
}

nvrhi::TextureHandle CreateLayerTexture(nvrhi::IDevice *device, const char *pDebugName,
                                       uint32_t size = RhiCloudsPass::LAYER_CUBEMAP_SIZE)
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::TextureCube;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.width = size;
    desc.height = size;
    desc.arraySize = RhiCloudsPass::LAYER_CUBEMAP_FACE_COUNT;
    desc.mipLevels = 1;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    desc.keepInitialState = true;

    return rhi::createTexture(device, desc, pDebugName);
}

nvrhi::TextureHandle CreateShadowTexture(nvrhi::IDevice *device, const char *pDebugName,
                                        uint32_t size = RhiCloudsPass::SHADOW_VOLUME_SIZE)
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture3D;
    desc.format = nvrhi::Format::R16_FLOAT;
    desc.width = size;
    desc.height = size;
    desc.depth = RhiCloudsPass::SHADOW_VOLUME_SLICES;
    desc.mipLevels = 1;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    desc.keepInitialState = true;

    return rhi::createTexture(device, desc, pDebugName);
}

nvrhi::SamplerHandle CreateCloudSampler(nvrhi::IDevice *device, const char *pDebugName)
{
    const nvrhi::SamplerDesc desc = nvrhi::SamplerDesc()
        .setMinFilter(true)
        .setMagFilter(true)
        .setMipFilter(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);

    return rhi::createSampler(device, desc, pDebugName);
}

nvrhi::BindingSetHandle CreateLayerSet(nvrhi::IDevice *device,
                                       nvrhi::IBindingLayout *pLayout,
                                       nvrhi::ITexture *pLayer,
                                       nvrhi::IBuffer *pParamsBuffer,
                                       nvrhi::ITexture *pShadow,
                                       nvrhi::ISampler *pShadowSampler)
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        RhiCloudsPass::LAYER_UAV_SLOT, pLayer, nvrhi::Format::RGBA16_FLOAT,
        nvrhi::TextureSubresourceSet(0, 1, 0, RhiCloudsPass::LAYER_CUBEMAP_FACE_COUNT),
        nvrhi::TextureDimension::Texture2DArray));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
        RhiCloudsPass::LAYER_PARAMS_CB_SLOT, pParamsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
        RhiCloudsPass::LAYER_SHADOW_SRV_SLOT, pShadow));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(
        RhiCloudsPass::LAYER_SHADOW_SAMPLER_SLOT, pShadowSampler));

    return device->createBindingSet(setDesc, pLayout);
}

nvrhi::BindingSetHandle CreateShadowSet(nvrhi::IDevice *device,
                                        nvrhi::IBindingLayout *pLayout,
                                        nvrhi::ITexture *pShadow,
                                        nvrhi::IBuffer *pParamsBuffer)
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        RhiCloudsPass::SHADOW_UAV_SLOT, pShadow, nvrhi::Format::R16_FLOAT,
        nvrhi::TextureSubresourceSet(), nvrhi::TextureDimension::Texture3D));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
        RhiCloudsPass::SHADOW_PARAMS_CB_SLOT, pParamsBuffer));

    return device->createBindingSet(setDesc, pLayout);
}

}

RhiCloudsPass::RhiCloudsPass() = default;

RhiCloudsPass::~RhiCloudsPass() = default;

bool RhiCloudsPass::Create(nvrhi::IDevice *pDevice,
                           rhi::RhiFrameContext *pFrameContext,
                           const char *pShaderFolderPath,
                           PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    if (pDevice == nullptr || pShaderFolderPath == nullptr || pShaderFolderPath[0] == '\0')
    {
        return false;
    }

    if (pFrameContext == nullptr || !pFrameContext->IsCreated())
    {
        return false;
    }

    device = pDevice;
    frameContext = pFrameContext;
    print = std::move(pfnPrint);
    shaderFolderPath = pShaderFolderPath;

    layerShader = rhi::loadShader(device, shaderFolderPath + LAYER_SHADER_FILE_NAME,
                                  nvrhi::ShaderType::Compute, LAYER_SHADER_FILE_NAME);
    if (layerShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to load the cloud layer shader");
        return false;
    }

    shadowShader = rhi::loadShader(device, shaderFolderPath + SHADOW_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, SHADOW_SHADER_FILE_NAME);
    if (shadowShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to load the cloud shadow shader");
        return false;
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0)
                                   .setConstantBufferOffset(0)
                                   .setSamplerOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(LAYER_PARAMS_CB_SLOT));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(LAYER_UAV_SLOT));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(LAYER_SHADOW_SRV_SLOT));
        desc.addItem(nvrhi::BindingLayoutItem::Sampler(LAYER_SHADOW_SAMPLER_SLOT));

        layerLayout = device->createBindingLayout(desc);
        if (layerLayout == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud layer binding layout");
            return false;
        }
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0)
                                   .setConstantBufferOffset(0)
                                   .setSamplerOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(SHADOW_UAV_SLOT));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(SHADOW_PARAMS_CB_SLOT));

        shadowLayout = device->createBindingLayout(desc);
        if (shadowLayout == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud shadow binding layout");
            return false;
        }
    }

    layerTexture = CreateLayerTexture(device, "RhiCloudsPass layer cubemap (RGBA16F 1024, 6x1)");
    shadowTexture = CreateShadowTexture(device, "RhiCloudsPass shadow volume (R16F 1024x1024x8)");
    if (layerTexture == nullptr || shadowTexture == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the cloud images");
        return false;
    }

    layerSampler = CreateCloudSampler(device, "RhiCloudsPass layer sampler (linear, clamp)");
    shadowSampler = CreateCloudSampler(device, "RhiCloudsPass shadow sampler (linear, clamp)");
    if (layerSampler == nullptr || shadowSampler == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the cloud samplers");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.CS = layerShader;
        desc.bindingLayouts.push_back(layerLayout);

        layerPipeline = rhi::createComputePipeline(device, desc, "RhiCloudsPass layer compute");
        if (layerPipeline == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud layer pipeline");
            return false;
        }
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.CS = shadowShader;
        desc.bindingLayouts.push_back(shadowLayout);

        shadowPipeline = rhi::createComputePipeline(device, desc, "RhiCloudsPass shadow compute");
        if (shadowPipeline == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud shadow pipeline");
            return false;
        }
    }

    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++)
    {
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(LayerParams);
            desc.isConstantBuffer = true;
            desc.initialState = nvrhi::ResourceStates::CopyDest;
            desc.keepInitialState = true;

            layerParamsBuffers[frameIndex] = rhi::createBuffer(device, desc, "RhiCloudsPass layer params");
        }

        {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(ShadowParams);
            desc.isConstantBuffer = true;
            desc.initialState = nvrhi::ResourceStates::CopyDest;
            desc.keepInitialState = true;

            shadowParamsBuffers[frameIndex] = rhi::createBuffer(device, desc, "RhiCloudsPass shadow params");
        }

        if (layerParamsBuffers[frameIndex] == nullptr || shadowParamsBuffers[frameIndex] == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud params buffers");
            return false;
        }

        layerSets[frameIndex] = CreateLayerSet(device, layerLayout, layerTexture,
                                               layerParamsBuffers[frameIndex], shadowTexture, shadowSampler);
        shadowSets[frameIndex] = CreateShadowSet(device, shadowLayout, shadowTexture,
                                                 shadowParamsBuffers[frameIndex]);
        if (layerSets[frameIndex] == nullptr || shadowSets[frameIndex] == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create the cloud binding sets");
            return false;
        }
    }

    created = true;

    return true;
}

bool RhiCloudsPass::SetQuality(uint32_t requestedQuality)
{
    requestedQuality = std::min(requestedQuality, uint32_t(QR_SKY_CLOUDS_MAX_QUALITY));
    if (quality == requestedQuality)
    {
        return true;
    }

    constexpr uint32_t layerSizes[QR_SKY_CLOUDS_MAX_QUALITY + 1] = { 384, 512, 1024, 2048 };
    constexpr uint32_t shadowSizes[QR_SKY_CLOUDS_MAX_QUALITY + 1] = { 512, 1024, 1024, 2048 };
    auto nextLayer = CreateLayerTexture(device, "RhiCloudsPass layer cubemap", layerSizes[requestedQuality]);
    auto nextShadow = CreateShadowTexture(device, "RhiCloudsPass shadow volume", shadowSizes[requestedQuality]);
    if (nextLayer == nullptr || nextShadow == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetHandle nextLayerSets[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle nextShadowSets[MAX_FRAMES_IN_FLIGHT];
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
    {
        nextLayerSets[frame] = CreateLayerSet(device, layerLayout, nextLayer,
                                              layerParamsBuffers[frame], nextShadow, shadowSampler);
        nextShadowSets[frame] = CreateShadowSet(device, shadowLayout, nextShadow,
                                                shadowParamsBuffers[frame]);
        if (nextLayerSets[frame] == nullptr || nextShadowSets[frame] == nullptr)
        {
            return false;
        }
    }

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
    {
        frameContext->Retire(layerSets[frame]);
        frameContext->Retire(shadowSets[frame]);
        layerSets[frame] = std::move(nextLayerSets[frame]);
        shadowSets[frame] = std::move(nextShadowSets[frame]);
    }
    frameContext->Retire(layerTexture);
    frameContext->Retire(shadowTexture);
    layerTexture = std::move(nextLayer);
    shadowTexture = std::move(nextShadow);
    quality = requestedQuality;
    layerValid = false;
    shadowValid = false;
    return true;
}

uint32_t RhiCloudsPass::GetViewSteps(uint32_t quality)
{
    constexpr uint32_t steps[QR_SKY_CLOUDS_MAX_QUALITY + 1] = { 32, 40, 48, 56 };
    return steps[std::min(quality, uint32_t(QR_SKY_CLOUDS_MAX_QUALITY))];
}

float RhiCloudsPass::GetWindSpeed(float setting, float altitude)
{
    return setting * altitude / 1400.0f;
}

std::array<float, 4> RhiCloudsPass::MakeShadowPlacement(const LayerParams &params)
{
    const float extent = std::max(params.cloudLayer[0] * SHADOW_VOLUME_EXTENT_PER_ALTITUDE, 1.0f);
    const bool enabled = params.sunDirection[3] > 0.5f && params.sunDirection[2] > 0.05f;
    return { enabled ? 1.0f : 0.0f, params.cloudAnchor[0] - extent * 0.5f,
             params.cloudAnchor[1] - extent * 0.5f, extent };
}

bool RhiCloudsPass::Render(nvrhi::ICommandList *pCommandList,
                           uint32_t frameIndex,
                           const LayerParams &params,
                           const ShadowParams &shadowParams,
                           uint32_t requestedQuality)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }

    if (params.cloudParams[3] <= 0.5f || params.skyParams[1] <= 0.0f)
    {
        shadowPlacement[0] = 0.0f;
        return false;
    }
    if (!SetQuality(requestedQuality))
    {
        return false;
    }

    const auto placement = MakeShadowPlacement(params);
    std::memcpy(shadowPlacement, placement.data(), sizeof(shadowPlacement));

    ShadowParams shadow = shadowParams;
    shadow.mapProjection[0] = shadowPlacement[1];
    shadow.mapProjection[1] = shadowPlacement[2];
    shadow.mapProjection[2] = shadowPlacement[3];
    shadow.mapProjection[3] = float(shadowTexture->getDesc().width);
    shadow.cloudMarch[3] = params.cloudAnchor[2] + params.cloudLayer[0];

    LayerParams layer = params;
    std::memcpy(layer.cloudShadowPlacement, shadowPlacement, sizeof(shadowPlacement));
    if (layer.cloudParams[2] == 0.0f)
    {
        layer.cloudColor[3] = 0.0f;
        shadow.cloudMarch[0] = 0.0f;
    }
    if (shadowPlacement[0] <= 0.5f)
    {
        layer.sunDirection[3] = 0.0f;
        shadowValid = false;
    }
    if (layerValid && std::memcmp(&layer, &lastLayerParams, sizeof(layer)) == 0 &&
        std::memcmp(&shadow, &lastShadowParams, sizeof(shadow)) == 0)
    {
        return false;
    }

    if (shadowPlacement[0] > 0.5f && (!shadowValid ||
        std::memcmp(&shadow, &lastShadowParams, sizeof(shadow)) != 0))
    {
        rhi::writeBuffer(pCommandList, shadowParamsBuffers[frameIndex], &shadow, sizeof(shadow));
        nvrhi::ComputeState state;
        state.pipeline = shadowPipeline;
        state.addBindingSet(shadowSets[frameIndex]);
        pCommandList->setComputeState(state);

        const uint32_t groups = Utils::GetWorkGroupCount(shadowTexture->getDesc().width, THREAD_GROUP_SIZE);
        pCommandList->dispatch(groups, groups, 1);
        shadowValid = true;
    }

    pCommandList->setTextureState(shadowTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);

    rhi::writeBuffer(pCommandList, layerParamsBuffers[frameIndex], &layer, sizeof(layer));

    {
        nvrhi::ComputeState state;
        state.pipeline = layerPipeline;
        state.addBindingSet(layerSets[frameIndex]);
        pCommandList->setComputeState(state);

        const uint32_t groups = Utils::GetWorkGroupCount(layerTexture->getDesc().width, THREAD_GROUP_SIZE);
        pCommandList->dispatch(groups, groups, LAYER_CUBEMAP_FACE_COUNT);
    }

    pCommandList->setTextureState(layerTexture, nvrhi::AllSubresources,
                                   nvrhi::ResourceStates::NonPixelShaderResource);
    lastLayerParams = layer;
    lastShadowParams = shadow;
    layerValid = true;
    return true;
}

}
