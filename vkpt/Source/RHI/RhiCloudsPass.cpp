#include "RhiCloudsPass.h"

#include "../Utils.h"
#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"

#include <algorithm>
#include <cstring>

namespace vkpt
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

nvrhi::TextureHandle CreateLayerTexture(nvrhi::IDevice *device, const char *pDebugName)
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::TextureCube;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.width = RhiCloudsPass::LAYER_CUBEMAP_SIZE;
    desc.height = RhiCloudsPass::LAYER_CUBEMAP_SIZE;
    desc.arraySize = RhiCloudsPass::LAYER_CUBEMAP_FACE_COUNT;
    desc.mipLevels = 1;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    desc.keepInitialState = true;

    return rhi::createTexture(device, desc, pDebugName);
}

nvrhi::TextureHandle CreateShadowTexture(nvrhi::IDevice *device, const char *pDebugName)
{
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture3D;
    desc.format = nvrhi::Format::R16_FLOAT;
    desc.width = RhiCloudsPass::SHADOW_VOLUME_SIZE;
    desc.height = RhiCloudsPass::SHADOW_VOLUME_SIZE;
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
        nvrhi::TextureSubresourceSet(), nvrhi::TextureDimension::Texture2DArray));
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

            layerParamsBuffers[frameIndex] = rhi::createBuffer(device, desc, "RhiCloudsPass layer params");
        }

        {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(ShadowParams);
            desc.isConstantBuffer = true;
            desc.initialState = nvrhi::ResourceStates::CopyDest;

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

void RhiCloudsPass::Render(nvrhi::ICommandList *pCommandList,
                           uint32_t frameIndex,
                           const LayerParams &params,
                           const ShadowParams &shadowParams)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    const float extent = std::max(params.cloudLayer[0] * SHADOW_VOLUME_EXTENT_PER_ALTITUDE, 1.0f);

    shadowPlacement[0] = 1.0f;
    shadowPlacement[1] = params.cloudAnchor[0] - extent * 0.5f;
    shadowPlacement[2] = params.cloudAnchor[1] - extent * 0.5f;
    shadowPlacement[3] = extent;

    ShadowParams shadow = shadowParams;
    shadow.mapProjection[0] = shadowPlacement[1];
    shadow.mapProjection[1] = shadowPlacement[2];
    shadow.mapProjection[2] = extent;
    shadow.mapProjection[3] = (float)SHADOW_VOLUME_SIZE;
    shadow.cloudMarch[3] = params.cloudAnchor[2] + params.cloudLayer[0];

    rhi::writeBuffer(pCommandList, shadowParamsBuffers[frameIndex], &shadow, sizeof(shadow));

    {
        nvrhi::ComputeState state;
        state.pipeline = shadowPipeline;
        state.addBindingSet(shadowSets[frameIndex]);
        pCommandList->setComputeState(state);

        const uint32_t groups = Utils::GetWorkGroupCount(SHADOW_VOLUME_SIZE, THREAD_GROUP_SIZE);
        pCommandList->dispatch(groups, groups, 1);
    }

    pCommandList->setTextureState(shadowTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);

    LayerParams layer = params;
    std::memcpy(layer.cloudShadowPlacement, shadowPlacement, sizeof(shadowPlacement));

    rhi::writeBuffer(pCommandList, layerParamsBuffers[frameIndex], &layer, sizeof(layer));

    {
        nvrhi::ComputeState state;
        state.pipeline = layerPipeline;
        state.addBindingSet(layerSets[frameIndex]);
        pCommandList->setComputeState(state);

        const uint32_t groups = Utils::GetWorkGroupCount(LAYER_CUBEMAP_SIZE, THREAD_GROUP_SIZE);
        pCommandList->dispatch(groups, groups, LAYER_CUBEMAP_FACE_COUNT);
    }

    pCommandList->setTextureState(layerTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);
}

}
