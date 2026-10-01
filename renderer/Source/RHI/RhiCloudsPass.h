#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/nvrhi.h>

#include "../Common.h"

namespace qray
{

namespace rhi
{
class RhiFrameContext;
}

class RhiCloudsPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    static constexpr uint32_t LAYER_CUBEMAP_SIZE = 1024;
    static constexpr uint32_t LAYER_CUBEMAP_FACE_COUNT = 6;

    static constexpr uint32_t SHADOW_VOLUME_SIZE = 1024;
    static constexpr uint32_t SHADOW_VOLUME_SLICES = 8;
    static constexpr float SHADOW_VOLUME_EXTENT_PER_ALTITUDE = 12.0f;

    static constexpr uint32_t LAYER_UAV_SLOT = 3;
    static constexpr uint32_t LAYER_PARAMS_CB_SLOT = 1;
    static constexpr uint32_t LAYER_SHADOW_SRV_SLOT = 5;
    static constexpr uint32_t LAYER_SHADOW_SAMPLER_SLOT = 6;

    static constexpr uint32_t SHADOW_UAV_SLOT = 0;
    static constexpr uint32_t SHADOW_PARAMS_CB_SLOT = 1;

    struct LayerParams
    {
        float faceBasis[18][4];
        float sunDirection[4];
        float skyColor[4];
        float skyParams[4];
        float cloudColor[4];
        float cloudParams[4];
        float sunDiscColor[4];
        float cloudLayer[4];
        float cloudMarch[4];
        float cloudAnchor[4];
        float cloudShadowPlacement[4];
    };

    struct ShadowParams
    {
        float sunDirection[4];
        float cloudLayer[4];
        float cloudMarch[4];
        float mapProjection[4];
    };

    RhiCloudsPass();
    ~RhiCloudsPass();

    RhiCloudsPass(const RhiCloudsPass &other) = delete;
    RhiCloudsPass(RhiCloudsPass &&other) noexcept = delete;
    RhiCloudsPass &operator=(const RhiCloudsPass &other) = delete;
    RhiCloudsPass &operator=(RhiCloudsPass &&other) noexcept = delete;

    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    bool Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                const LayerParams &params,
                const ShadowParams &shadowParams,
                uint32_t quality = 2);

    static std::array<float, 4> MakeShadowPlacement(const LayerParams &params);

    nvrhi::ITexture *GetLayerTexture() const { return layerTexture.Get(); }
    nvrhi::ISampler *GetLayerSampler() const { return layerSampler.Get(); }
    nvrhi::ITexture *GetShadowTexture() const { return shadowTexture.Get(); }
    nvrhi::ISampler *GetShadowSampler() const { return shadowSampler.Get(); }
    const float *GetShadowPlacement() const { return shadowPlacement; }

private:
    bool SetQuality(uint32_t quality);

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;

    rhi::RhiFrameContext *frameContext = nullptr;

    nvrhi::ShaderHandle layerShader;
    nvrhi::ShaderHandle shadowShader;

    nvrhi::BindingLayoutHandle layerLayout;
    nvrhi::BindingLayoutHandle shadowLayout;
    nvrhi::ComputePipelineHandle layerPipeline;
    nvrhi::ComputePipelineHandle shadowPipeline;

    nvrhi::TextureHandle layerTexture;
    nvrhi::SamplerHandle layerSampler;
    nvrhi::TextureHandle shadowTexture;
    nvrhi::SamplerHandle shadowSampler;

    nvrhi::BufferHandle layerParamsBuffers[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BufferHandle shadowParamsBuffers[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle layerSets[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle shadowSets[MAX_FRAMES_IN_FLIGHT];

    LayerParams lastLayerParams = {};
    ShadowParams lastShadowParams = {};
    float shadowPlacement[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    bool created = false;
    bool layerValid = false;
    bool shadowValid = false;
    uint32_t quality = 2;
};

static_assert(sizeof(RhiCloudsPass::LayerParams) == 448,
              "CmSkyCloudsParams_BT is 28 float4");
static_assert(offsetof(RhiCloudsPass::LayerParams, faceBasis) == 0, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, sunDirection) == 288, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, skyColor) == 304, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, skyParams) == 320, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudColor) == 336, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudParams) == 352, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, sunDiscColor) == 368, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudLayer) == 384, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudMarch) == 400, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudAnchor) == 416, "measured member offset");
static_assert(offsetof(RhiCloudsPass::LayerParams, cloudShadowPlacement) == 432, "measured member offset");

static_assert(sizeof(RhiCloudsPass::ShadowParams) == 64,
              "CmCloudShadowParams_BT is 4 float4");

}
