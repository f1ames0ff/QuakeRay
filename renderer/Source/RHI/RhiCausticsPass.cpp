#include "RhiCausticsPass.h"

#include <cstring>

#include "../Framebuffers.h"
#include "../Generated/ShaderCommonC.h"
#include "../Generated/ShaderCommonCFramebuf.h"
#include "../Utils.h"
#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"
#include "RhiTextureSource.h"
#include "RhiTextureTable.h"

using namespace qray;

namespace
{
constexpr uint32_t CAUSTICS_GROUP_SIZE = 8;
constexpr uint32_t CAUSTICS_MAX_RESOLUTION = 512;
constexpr uint32_t CAUSTICS_PHOTON_STRIDE = 48;
constexpr uint32_t CAUSTICS_PARAMS_STRIDE = 64;
constexpr uint32_t CAUSTICS_FRAMEBUFFER_SRV_OFFSET = 124;
constexpr uint32_t CAUSTICS_IMAGE_COUNT = 3;
constexpr uint32_t CAUSTICS_VERTEX_DATA_BINDING_COUNT = 7;

const uint32_t CAUSTICS_VERTEX_DATA_BINDINGS[CAUSTICS_VERTEX_DATA_BINDING_COUNT] =
{
    BINDING_VERTEX_BUFFER_STATIC,
    BINDING_VERTEX_BUFFER_DYNAMIC,
    BINDING_INDEX_BUFFER_STATIC,
    BINDING_INDEX_BUFFER_DYNAMIC,
    BINDING_GEOMETRY_INSTANCES,
    BINDING_PREV_POSITIONS_BUFFER_DYNAMIC,
    BINDING_PREV_INDEX_BUFFER_DYNAMIC,
};

const FramebufferImageIndex CAUSTICS_IMAGES[CAUSTICS_IMAGE_COUNT] =
{
    FB_IMAGE_INDEX_FINAL,
    FB_IMAGE_INDEX_ALBEDO,
    FB_IMAGE_INDEX_SURFACE_POSITION,
};

void LogMessage(const qray::RhiCausticsPass::PrintFunction &print, const std::string &message)
{
    if (print != nullptr)
    {
        print(message.c_str());
    }
}

bool LoadShader(nvrhi::IDevice *device,
                const std::string &folder,
                const char *name,
                nvrhi::ShaderHandle &shader,
                const qray::RhiCausticsPass::PrintFunction &print)
{
    if (device == nullptr)
    {
        return false;
    }

    const std::string path = folder + name;
    shader = qray::rhi::loadShader(device, path, nvrhi::ShaderType::Compute, path);

    if (shader == nullptr)
    {
        LogMessage(print, std::string("Warning: RHI: failed to load ") + path + ", the caustics pass is skipped");
        return false;
    }

    return true;
}
}

namespace qray
{

RhiCausticsPass::RhiCausticsPass() = default;

RhiCausticsPass::~RhiCausticsPass()
{
    if (device != nullptr)
    {
        device->waitForIdle();
    }

    for (Target &target : targets)
    {
        target.framebufferSet = nullptr;
        target.uniformSet = nullptr;
        target.vertexDataSet = nullptr;

        for (nvrhi::TextureHandle &texture : target.engineTextures)
        {
            texture = nullptr;
        }
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        paramsSets[i] = nullptr;
        compositeSets[i] = nullptr;
        paramsBuffers[i] = nullptr;
        photonBuffers[i] = nullptr;
    }

    traceShader = nullptr;
    compositeShader = nullptr;
    tracePipeline = nullptr;
    compositePipeline = nullptr;
    tlasLayout = nullptr;
    uniformLayout = nullptr;
    vertexDataLayout = nullptr;
    traceParamsLayout = nullptr;
    compositeParamsLayout = nullptr;
    framebufferLayout = nullptr;
}

bool RhiCausticsPass::Create(nvrhi::IDevice *pDevice,
                             rhi::RhiFrameContext *pFrameContext,
                             rhi::RhiTextureTable *table,
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
    textureTable = table;

    if (device == nullptr)
    {
        LogMessage(print, "Warning: RHI: the caustics pass needs an RHI device");
        return false;
    }

    if (frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the caustics pass needs the frame context of the RHI layer");
        return false;
    }

    if (textureTable == nullptr || textureTable->GetLayout() == nullptr || textureTable->GetTable() == nullptr)
    {
        LogMessage(print, "Warning: RHI: the caustics pass needs the bindless texture table");
        return false;
    }

    if (!LoadShader(device, shaderFolderPath, "CmCaustics.comp.spv", traceShader, print) ||
        !LoadShader(device, shaderFolderPath, "CmCausticsComposite.comp.spv", compositeShader, print))
    {
        return false;
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::All;
        desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(BINDING_ACCELERATION_STRUCTURE_MAIN));

        tlasLayout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM));

        uniformLayout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0));

        for (uint32_t i = 0; i < CAUSTICS_VERTEX_DATA_BINDING_COUNT; i++)
        {
            desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(CAUSTICS_VERTEX_DATA_BINDINGS[i]));
        }

        vertexDataLayout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));

        traceParamsLayout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));

        compositeParamsLayout = device->createBindingLayout(desc);
    }
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(CAUSTICS_FRAMEBUFFER_SRV_OFFSET)
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(FB_IMAGE_INDEX_FINAL));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(FB_IMAGE_INDEX_ALBEDO));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(FB_IMAGE_INDEX_SURFACE_POSITION));

        framebufferLayout = device->createBindingLayout(desc);
    }

    if (tlasLayout == nullptr || uniformLayout == nullptr || vertexDataLayout == nullptr ||
        traceParamsLayout == nullptr || compositeParamsLayout == nullptr || framebufferLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a caustics pass binding layout");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(traceShader);
        desc.addBindingLayout(tlasLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(vertexDataLayout);
        desc.addBindingLayout(textureTable->GetLayout());
        desc.addBindingLayout(traceParamsLayout);

        tracePipeline = rhi::createComputePipeline(device, desc, "RhiCausticsPass trace");
    }
    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(compositeShader);
        desc.addBindingLayout(framebufferLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(compositeParamsLayout);

        compositePipeline = rhi::createComputePipeline(device, desc, "RhiCausticsPass composite");
    }

    if (tracePipeline == nullptr || compositePipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a caustics pass compute pipeline");
        return false;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = CAUSTICS_PARAMS_STRIDE;
            desc.structStride = CAUSTICS_PARAMS_STRIDE;
            desc.initialState = nvrhi::ResourceStates::CopyDest;
            desc.keepInitialState = true;

            paramsBuffers[i] = rhi::createBuffer(device, desc, "RhiCausticsPass params " + std::to_string(i));
        }
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = uint64_t(CAUSTICS_MAX_RESOLUTION) * CAUSTICS_MAX_RESOLUTION * CAUSTICS_PHOTON_STRIDE;
            desc.structStride = CAUSTICS_PHOTON_STRIDE;
            desc.canHaveUAVs = true;
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.keepInitialState = true;

            photonBuffers[i] = rhi::createBuffer(device, desc, "RhiCausticsPass photons " + std::to_string(i));
        }

        if (paramsBuffers[i] == nullptr || photonBuffers[i] == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create a caustics pass buffer");
            return false;
        }

        {
            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, paramsBuffers[i]));
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, photonBuffers[i]));

            paramsSets[i] = device->createBindingSet(setDesc, traceParamsLayout);
        }
        {
            nvrhi::BindingSetDesc setDesc;
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, paramsBuffers[i]));
            setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, photonBuffers[i]));

            compositeSets[i] = device->createBindingSet(setDesc, compositeParamsLayout);
        }

        if (paramsSets[i] == nullptr || compositeSets[i] == nullptr)
        {
            LogMessage(print, "Warning: RHI: failed to create a caustics pass binding set");
            return false;
        }
    }

    created = true;
    return true;
}

void RhiCausticsPass::ReleaseFramebufferTarget(Target &target)
{
    for (nvrhi::TextureHandle &texture : target.engineTextures)
    {
        if (texture != nullptr)
        {
            frameContext->Retire(texture);
            texture = nullptr;
        }
    }

    if (target.framebufferSet != nullptr)
    {
        frameContext->Retire(target.framebufferSet);
        target.framebufferSet = nullptr;
    }

    if (target.uniformSet != nullptr)
    {
        frameContext->Retire(target.uniformSet);
        target.uniformSet = nullptr;
    }

    target.uniformBuffer = nullptr;

    if (target.vertexDataSet != nullptr)
    {
        frameContext->Retire(target.vertexDataSet);
        target.vertexDataSet = nullptr;
    }

    if (target.tlasSet != nullptr)
    {
        frameContext->Retire(target.tlasSet);
        target.tlasSet = nullptr;
    }

    target.topLevel = nullptr;
    target.width = 0;
    target.height = 0;
    std::memset(target.imageHandles, 0, sizeof(target.imageHandles));
}

RhiCausticsPass::Target *RhiCausticsPass::PrepareFrame(uint32_t frameIndex,
                                                       const Framebuffers *pFramebuffers,
                                                       uint32_t width,
                                                       uint32_t height,
                                                       nvrhi::IBuffer *pUniformBuffer)
{
    if (pFramebuffers == nullptr || pUniformBuffer == nullptr)
    {
        if (!warnedMissingInputs)
        {
            warnedMissingInputs = true;
            LogMessage(print, "Warning: RHI: the caustics pass got no framebuffers or no global uniform, the pass is skipped");
        }
        return nullptr;
    }

    Target &target = targets[frameIndex];

    const ResolutionState resolutionState = { width, height, 0, 0 };

    uint64_t imageHandles[CAUSTICS_IMAGE_COUNT] = {};
    uint64_t imageViews[CAUSTICS_IMAGE_COUNT] = {};
    VkFormat imageFormats[CAUSTICS_IMAGE_COUNT] = {};
    VkExtent2D imageExtents[CAUSTICS_IMAGE_COUNT] = {};

    for (uint32_t i = 0; i < CAUSTICS_IMAGE_COUNT; i++)
    {
        const auto [image, view, format, extent] =
            pFramebuffers->GetImageHandles(CAUSTICS_IMAGES[i], frameIndex, resolutionState);

        imageHandles[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image));
        imageViews[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view));
        imageFormats[i] = format;
        imageExtents[i] = extent;

        if (imageHandles[i] == 0)
        {
            return nullptr;
        }
    }

    bool framebuffersChanged = target.width != width || target.height != height;

    for (uint32_t i = 0; i < CAUSTICS_IMAGE_COUNT; i++)
    {
        framebuffersChanged = framebuffersChanged || target.imageHandles[i] != imageHandles[i];
    }

    if (framebuffersChanged)
    {
        ReleaseFramebufferTarget(target);

        for (uint32_t i = 0; i < CAUSTICS_IMAGE_COUNT; i++)
        {
            const std::string debugName = std::string("RhiCausticsPass ") +
                                          ShFramebuffers_DebugNames[CAUSTICS_IMAGES[i]] +
                                          " frame " + std::to_string(frameIndex);

            target.engineTextures[i] = rhi::wrapEngineStorageImage(
                device, imageHandles[i], imageViews[i], imageFormats[i],
                imageExtents[i].width, imageExtents[i].height, debugName);

            if (target.engineTextures[i] == nullptr)
            {
                LogMessage(print, "Warning: RHI: failed to wrap a caustics pass framebuffer image");
                ReleaseFramebufferTarget(target);
                return nullptr;
            }
        }

        std::memcpy(target.imageHandles, imageHandles, sizeof(target.imageHandles));
        target.width = width;
        target.height = height;
    }

    if (!PrepareFramebufferSets(target) || !PrepareUniformSet(target, pUniformBuffer))
    {
        return nullptr;
    }

    return &target;
}

bool RhiCausticsPass::PrepareFramebufferSets(Target &target)
{
    if (target.framebufferSet != nullptr)
    {
        return true;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(FB_IMAGE_INDEX_FINAL,
                                                       target.engineTextures[0].Get()));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(FB_IMAGE_INDEX_ALBEDO,
                                                       target.engineTextures[1].Get()));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(FB_IMAGE_INDEX_SURFACE_POSITION,
                                                       target.engineTextures[2].Get()));

    target.framebufferSet = device->createBindingSet(setDesc, framebufferLayout);

    if (target.framebufferSet == nullptr)
    {
        if (!warnedBadTable)
        {
            warnedBadTable = true;
            LogMessage(print, "Warning: RHI: failed to create the caustics pass framebuffer set");
        }
        return false;
    }

    return true;
}

bool RhiCausticsPass::PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer)
{
    if (target.uniformSet != nullptr && target.uniformBuffer == pUniformBuffer)
    {
        return true;
    }

    if (target.uniformSet != nullptr)
    {
        frameContext->Retire(target.uniformSet);
        target.uniformSet = nullptr;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM, pUniformBuffer));

    target.uniformSet = device->createBindingSet(setDesc, uniformLayout);

    if (target.uniformSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the caustics pass uniform set");
        return false;
    }

    target.uniformBuffer = pUniformBuffer;
    return true;
}

bool RhiCausticsPass::PrepareVertexDataSet(Target &target, const RhiRtPrimaryPass::VertexData &vertexData)
{
    nvrhi::IBuffer *buffers[CAUSTICS_VERTEX_DATA_BINDING_COUNT] =
    {
        vertexData.staticVertices,
        vertexData.dynamicVertices,
        vertexData.staticIndices,
        vertexData.dynamicIndices,
        vertexData.geometryInstances,
        vertexData.dynamicVerticesPrev,
        vertexData.prevDynamicIndices,
    };

    bool changed = target.vertexDataSet == nullptr;

    for (uint32_t i = 0; i < CAUSTICS_VERTEX_DATA_BINDING_COUNT && !changed; i++)
    {
        changed = target.vertexDataBuffers[i] != buffers[i];
    }

    if (!changed)
    {
        return true;
    }

    if (!vertexData.IsComplete())
    {
        return false;
    }

    if (target.vertexDataSet != nullptr)
    {
        frameContext->Retire(target.vertexDataSet);
        target.vertexDataSet = nullptr;
    }

    nvrhi::BindingSetDesc setDesc;

    for (uint32_t i = 0; i < CAUSTICS_VERTEX_DATA_BINDING_COUNT; i++)
    {
        setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            CAUSTICS_VERTEX_DATA_BINDINGS[i], buffers[i]));
    }

    target.vertexDataSet = device->createBindingSet(setDesc, vertexDataLayout);

    if (target.vertexDataSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the caustics pass vertex-data set");
        return false;
    }

    for (uint32_t i = 0; i < CAUSTICS_VERTEX_DATA_BINDING_COUNT; i++)
    {
        target.vertexDataBuffers[i] = buffers[i];
    }

    return true;
}

bool RhiCausticsPass::PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel)
{
    if (target.tlasSet != nullptr && target.topLevel == pTopLevel)
    {
        return true;
    }

    if (target.tlasSet != nullptr)
    {
        frameContext->Retire(target.tlasSet);
        target.tlasSet = nullptr;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(BINDING_ACCELERATION_STRUCTURE_MAIN,
                                                                 pTopLevel));

    target.tlasSet = device->createBindingSet(setDesc, tlasLayout);

    if (target.tlasSet == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the caustics pass TLAS set");
        return false;
    }

    target.topLevel = pTopLevel;
    return true;
}

void RhiCausticsPass::Render(nvrhi::ICommandList *pCommandList,
                             uint32_t frameIndex,
                             const Framebuffers *pFramebuffers,
                             uint32_t width,
                             uint32_t height,
                             nvrhi::IBuffer *pUniformBuffer,
                             nvrhi::rt::IAccelStruct *pTopLevel,
                             const RhiRtPrimaryPass::VertexData &vertexData,
                             const Params &params)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    if (width == 0 || height == 0 || pTopLevel == nullptr || params.gridSize[0] == 0 ||
        params.gridSize[0] > CAUSTICS_MAX_RESOLUTION)
    {
        if (!warnedSkip)
        {
            warnedSkip = true;
            LogMessage(print, std::string("RHI: caustics: skipped, width ") + std::to_string(width) +
                                   ", height " + std::to_string(height) +
                                   ", resolution " + std::to_string(params.gridSize[0]) +
                                   ", tlas " + (pTopLevel != nullptr ? "yes" : "no"));
        }
        return;
    }

    Target *pTarget = PrepareFrame(frameIndex, pFramebuffers, width, height, pUniformBuffer);
    if (pTarget == nullptr)
    {
        return;
    }

    Target &target = *pTarget;

    if (!PrepareTlasSet(target, pTopLevel))
    {
        return;
    }

    if (!PrepareVertexDataSet(target, vertexData))
    {
        if (!warnedSkip)
        {
            warnedSkip = true;
            LogMessage(print, "RHI: caustics: skipped, the vertex-data buffers are incomplete");
        }
        return;
    }

    uint8_t paramsBytes[CAUSTICS_PARAMS_STRIDE] = {};
    std::memcpy(paramsBytes, &params, sizeof(params));
    rhi::writeBuffer(pCommandList, paramsBuffers[frameIndex], paramsBytes, sizeof(paramsBytes));

    for (uint32_t i = 0; i < CAUSTICS_IMAGE_COUNT; i++)
    {
        pCommandList->beginTrackingTextureState(target.engineTextures[i], nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);
    }

    pCommandList->clearBufferUInt(photonBuffers[frameIndex], 0);

    {
        nvrhi::ComputeState state;
        state.setPipeline(tracePipeline);
        state.addBindingSet(target.tlasSet);
        state.addBindingSet(target.uniformSet);
        state.addBindingSet(target.vertexDataSet);
        state.addBindingSet(textureTable->GetTable());
        state.addBindingSet(paramsSets[frameIndex]);
        pCommandList->setComputeState(state);

        const uint32_t groups = Utils::GetWorkGroupCount(params.gridSize[0], CAUSTICS_GROUP_SIZE);
        pCommandList->dispatch(groups, groups, 1);
    }

    {
        nvrhi::ComputeState state;
        state.setPipeline(compositePipeline);
        state.addBindingSet(target.framebufferSet);
        state.addBindingSet(target.uniformSet);
        state.addBindingSet(compositeSets[frameIndex]);
        pCommandList->setComputeState(state);

        pCommandList->dispatch(Utils::GetWorkGroupCount(width, CAUSTICS_GROUP_SIZE),
                               Utils::GetWorkGroupCount(height, CAUSTICS_GROUP_SIZE), 1);
    }

    for (uint32_t i = 1; i < CAUSTICS_IMAGE_COUNT; i++)
    {
        pCommandList->setTextureState(target.engineTextures[i], nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::UnorderedAccess);
    }

    pCommandList->setTextureState(target.engineTextures[0], nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);
    pCommandList->setTextureState(target.engineTextures[0], nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
}

void RhiCausticsPass::ReleaseTargets()
{
    for (Target &target : targets)
    {
        ReleaseFramebufferTarget(target);
    }
}

}
