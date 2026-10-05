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

#include "Common.h"
#include "ShaderManager.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"
#include "MemoryAllocator.h"
#include "Buffer.h"

namespace qray
{

class Tonemapping : public IShaderDependency
{
public:
    Tonemapping(
        VkDevice device,
        std::shared_ptr<Framebuffers> framebuffers,
        const std::shared_ptr<const ShaderManager> &shaderManager,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<MemoryAllocator> &allocator);
    ~Tonemapping() override;

    Tonemapping(const Tonemapping &other) = delete;
    Tonemapping(Tonemapping &&other) noexcept = delete;
    Tonemapping &operator=(const Tonemapping &other) = delete;
    Tonemapping &operator=(Tonemapping &&other) noexcept = delete;

    void PrepareExposureParams(
        uint32_t frameIndex,
        const std::shared_ptr<const GlobalUniform> &uniform,
        float exposureBias, float tonemapPower, uint32_t tonemapType,
        const QrDrawFrameTonemappingParams &params);

    VkBuffer GetBuffer(uint32_t frameIndex) const;

    uint32_t GetElementSize() const;

    void SetAvgLuminance(uint32_t frameIndex, float avgLuminance);

    void OnShaderReload(const ShaderManager *shaderManager) override;

private:
    void CreateTonemappingBuffer(const std::shared_ptr<MemoryAllocator> &allocator);

private:
    Buffer tmBuffer[MAX_FRAMES_IN_FLIGHT];

    void *mappedTmBuffer[MAX_FRAMES_IN_FLIGHT] = {};
    bool exposureReady = false;
    float previousTime = -1.0f;
};

}
