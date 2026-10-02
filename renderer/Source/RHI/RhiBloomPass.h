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

#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

#include <nvrhi/nvrhi.h>

#include "../Common.h"

namespace qray
{

class Tonemapping;

namespace rhi
{
class RhiFrameContext;
}

class RhiBloomPass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    static constexpr uint32_t MAX_LEVELS = 7;
    static constexpr uint32_t GROUP_SIZE = 16;
    static constexpr uint32_t PUSH_SIZE = 16;

    struct Settings
    {
        float intensity;
        float threshold;
        float knee;
        float scatter;
        float radius;
        uint32_t quality;
    };

    RhiBloomPass();
    ~RhiBloomPass();

    RhiBloomPass(const RhiBloomPass &other) = delete;
    RhiBloomPass(RhiBloomPass &&other) noexcept = delete;
    RhiBloomPass &operator=(const RhiBloomPass &other) = delete;
    RhiBloomPass &operator=(RhiBloomPass &&other) noexcept = delete;

    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                const Tonemapping *pTonemapping,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    void Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                nvrhi::ITexture *pHdrSource,
                uint32_t width,
                uint32_t height,
                const Settings &settings);

    nvrhi::ITexture *GetResultTexture(uint32_t frameIndex) const;

    void ReleaseTargets();

private:
    struct Level
    {
        nvrhi::TextureHandle texture;
        nvrhi::BindingSetHandle srvSet;
        nvrhi::BindingSetHandle uavSet;
    };

    struct Target
    {
        uint32_t width = 0;
        uint32_t height = 0;
        bool quarterBase = false;
        uint32_t downCount = 0;
        Level down[MAX_LEVELS];
        Level up[MAX_LEVELS];
        Level scratch;
        Level result;
        nvrhi::ITexture *sourceTexture = nullptr;
        nvrhi::BindingSetHandle sourceSet;
    };

    bool PrepareTonemappingSet(uint32_t frameIndex);
    bool PrepareSourceSet(Target &target, nvrhi::ITexture *pSource);

    bool CreateTarget(Target &target, uint32_t width, uint32_t height, bool quarterBase);
    bool CreateLevel(Level &level, uint32_t width, uint32_t height, const std::string &name);

    void DispatchDownsample(nvrhi::ICommandList *pCommandList,
                            uint32_t frameIndex,
                            nvrhi::IBindingSet *pSourceSet,
                            nvrhi::IBindingSet *pDestinationSet,
                            uint32_t destinationWidth,
                            uint32_t destinationHeight,
                            bool extract,
                            float threshold,
                            float knee);

    void DispatchUpsample(nvrhi::ICommandList *pCommandList,
                          uint32_t frameIndex,
                          nvrhi::IBindingSet *pSourceSet,
                          nvrhi::IBindingSet *pCoarseSet,
                          nvrhi::IBindingSet *pDestinationSet,
                          uint32_t destinationWidth,
                          uint32_t destinationHeight,
                          bool hasSource,
                          float scatter);

    void RecordDispatch(nvrhi::ICommandList *pCommandList,
                        nvrhi::IComputePipeline *pPipeline,
                        std::initializer_list<nvrhi::IBindingSet *> sets,
                        uint32_t destinationWidth,
                        uint32_t destinationHeight,
                        const void *pPushData,
                        uint32_t pushSize);

    void ReleaseLevel(Level &level);
    void ReleaseTarget(Target &target);
    void ClearLevel(Level &level);
    void ClearTarget(Target &target);

    nvrhi::IDevice *device = nullptr;
    PrintFunction print;
    std::string shaderFolderPath;
    rhi::RhiFrameContext *frameContext = nullptr;
    const Tonemapping *tonemapping = nullptr;

    nvrhi::ShaderHandle downsampleShader;
    nvrhi::ShaderHandle upsampleShader;

    nvrhi::BindingLayoutHandle sourceLayout;
    nvrhi::BindingLayoutHandle secondSourceLayout;
    nvrhi::BindingLayoutHandle tonemappingLayout;
    nvrhi::BindingLayoutHandle destinationLayout;
    nvrhi::BindingLayoutHandle pushConstantLayout;
    nvrhi::BindingLayoutHandle emptyLayout;

    nvrhi::ComputePipelineHandle downsamplePipeline;
    nvrhi::ComputePipelineHandle upsamplePipeline;

    nvrhi::BindingSetHandle emptySet;
    nvrhi::SamplerHandle sampler;

    nvrhi::BufferHandle tonemappingBuffers[MAX_FRAMES_IN_FLIGHT];
    nvrhi::BindingSetHandle tonemappingSets[MAX_FRAMES_IN_FLIGHT];
    uint64_t tonemappingHandles[MAX_FRAMES_IN_FLIGHT] = {};

    Target targets[MAX_FRAMES_IN_FLIGHT];

    bool warnedBadSource = false;
    bool warnedBadTonemapping = false;

    bool created = false;
};

}
