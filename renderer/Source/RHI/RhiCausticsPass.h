#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/vulkan.h>

#include "../Common.h"

#include "RhiRtPrimaryPass.h"

namespace qray
{

class Framebuffers;

namespace rhi
{
class RhiFrameContext;
class RhiTextureTable;
}

class RhiCausticsPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    struct Params
    {
        float sunDirection[4] = {};
        float sunColor[4] = {};
        float gridMinAndTexel[4] = {};
        uint32_t gridSize[4] = {};
    };

    RhiCausticsPass();
    ~RhiCausticsPass();

    RhiCausticsPass(const RhiCausticsPass &other) = delete;
    RhiCausticsPass(RhiCausticsPass &&other) noexcept = delete;
    RhiCausticsPass &operator=(const RhiCausticsPass &other) = delete;
    RhiCausticsPass &operator=(RhiCausticsPass &&other) noexcept = delete;

    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                rhi::RhiTextureTable *pTextureTable,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    void RenderTrace(nvrhi::ICommandList *pCommandList,
                     uint32_t frameIndex,
                     uint32_t width,
                     uint32_t height,
                     nvrhi::IBuffer *pUniformBuffer,
                     nvrhi::rt::IAccelStruct *pTopLevel,
                     const RhiRtPrimaryPass::VertexData &vertexData,
                     const Params &params);

    void RenderDiagnostics(nvrhi::ICommandList *pCommandList,
                           uint32_t frameIndex,
                           const Framebuffers *pFramebuffers,
                           uint32_t width,
                           uint32_t height,
                           nvrhi::IBuffer *pUniformBuffer);

    nvrhi::IBuffer *GetCellBuffer(uint32_t frameIndex) const;
    nvrhi::IBuffer *GetParamsBuffer(uint32_t frameIndex) const;
    bool HasTraceResult(uint32_t frameIndex) const;

    void ReleaseTargets();

private:
    static constexpr uint32_t CAUSTICS_IMAGE_COUNT = 3;

    struct Target
    {
        uint64_t imageHandles[CAUSTICS_IMAGE_COUNT] = {};
        uint32_t width = 0;
        uint32_t height = 0;
        nvrhi::TextureHandle engineTextures[CAUSTICS_IMAGE_COUNT];
        nvrhi::BindingSetHandle framebufferSet;
        nvrhi::BindingSetHandle uniformSet;
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::BindingSetHandle vertexDataSet;
        const nvrhi::IBuffer *vertexDataBuffers[7] = {};
        nvrhi::BindingSetHandle tlasSet;
        nvrhi::rt::IAccelStruct *topLevel = nullptr;
    };

    void ReleaseFramebufferTarget(Target &target);
    Target *PrepareFrame(uint32_t frameIndex,
                         const Framebuffers *pFramebuffers,
                         uint32_t width,
                         uint32_t height,
                         nvrhi::IBuffer *pUniformBuffer);
    bool PrepareFramebufferSets(Target &target);
    bool PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer);
    bool PrepareVertexDataSet(Target &target, const RhiRtPrimaryPass::VertexData &vertexData);
    bool PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel);

    nvrhi::IDevice *device = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;
    rhi::RhiTextureTable *textureTable = nullptr;
    std::string shaderFolderPath;
    PrintFunction print;

    nvrhi::ShaderHandle traceShader;
    nvrhi::ShaderHandle compositeShader;

    nvrhi::BindingLayoutHandle tlasLayout;
    nvrhi::BindingLayoutHandle uniformLayout;
    nvrhi::BindingLayoutHandle vertexDataLayout;
    nvrhi::BindingLayoutHandle traceParamsLayout;
    nvrhi::BindingLayoutHandle compositeParamsLayout;
    nvrhi::BindingLayoutHandle framebufferLayout;

    nvrhi::ComputePipelineHandle tracePipeline;
    nvrhi::ComputePipelineHandle compositePipeline;

    nvrhi::BufferHandle paramsBuffers[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle paramsSets[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle cellBuffers[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle compositeSets[MAX_FRAMES_IN_FLIGHT];

    Target targets[MAX_FRAMES_IN_FLIGHT];

    bool traceValid[MAX_FRAMES_IN_FLIGHT] = {};

    bool created = false;
    bool warnedMissingInputs = false;
    bool warnedBadTable = false;
    bool warnedSkip = false;
};

}
