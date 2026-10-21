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

// Independent implementation of a frustum froxel lighting/prefix pass and its history
// resources; no reference renderer source is copied.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/vulkan.h>

#include "../Common.h"

namespace qray
{

namespace rhi
{
class RhiFrameContext;
}

class RhiLightShaftVolumePass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    static constexpr uint32_t MAX_LIGHT_SHAFTS = 64;
    static constexpr uint32_t VOLUME_SIZE_X = 160;
    static constexpr uint32_t VOLUME_SIZE_Y = 88;
    static constexpr uint32_t VOLUME_SIZE_Z = 64;
    static constexpr uint32_t GROUP_SIZE_X = 4;
    static constexpr uint32_t GROUP_SIZE_Y = 4;

    struct LightShaft
    {
        float position[4];
        float direction[4];
        float color[4];
        float cone[4];
        float tangentU[4];
        float tangentV[4];
    };

    struct Params
    {
        uint32_t lightShaftCount;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
        float worldCenter[4];
        float worldHalfSizeInv[4];
        LightShaft lightShafts[MAX_LIGHT_SHAFTS];
    };

    RhiLightShaftVolumePass();
    ~RhiLightShaftVolumePass();

    RhiLightShaftVolumePass(const RhiLightShaftVolumePass &other) = delete;
    RhiLightShaftVolumePass(RhiLightShaftVolumePass &&other) noexcept = delete;
    RhiLightShaftVolumePass &operator=(const RhiLightShaftVolumePass &other) = delete;
    RhiLightShaftVolumePass &operator=(RhiLightShaftVolumePass &&other) noexcept = delete;

    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    bool Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                nvrhi::IBuffer *pUniformBuffer,
                nvrhi::rt::IAccelStruct *pTopLevel,
                const Params &params);

    nvrhi::ITexture *GetTexture(uint32_t frameIndex) const;
    nvrhi::ISampler *GetSampler() const { return sampler; }

private:
    struct Target
    {
        nvrhi::TextureHandle texture;
        nvrhi::BindingSetHandle uavSet;
        nvrhi::BindingSetHandle paramsSet;
        nvrhi::BindingSetHandle uniformSet;
        nvrhi::BindingSetHandle tlasSet;
        nvrhi::BufferHandle paramsBuffer;
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::rt::IAccelStruct *topLevel = nullptr;
    };

    bool PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer);
    bool PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel);
    bool CreateVolumeTarget(Target &target, uint32_t frameIndex);

    nvrhi::IDevice *device = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;
    std::string shaderFolderPath;
    PrintFunction print;
    bool created = false;

    nvrhi::ShaderHandle volumeShader;
    nvrhi::BindingLayoutHandle tlasLayout;
    nvrhi::BindingLayoutHandle paramsLayout;
    nvrhi::BindingLayoutHandle uniformLayout;
    nvrhi::BindingLayoutHandle volumeLayout;
    nvrhi::ShaderHandle prefixShader;
    nvrhi::ComputePipelineHandle volumePipeline;
    nvrhi::ComputePipelineHandle prefixPipeline;
    nvrhi::SamplerHandle sampler;

    Target targets[MAX_FRAMES_IN_FLIGHT];
};

static_assert(sizeof(RhiLightShaftVolumePass::Params) == 6192,
              "LightShaftVolumeParams_BT is 6192 bytes");

}
