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

#include "Tonemapping.h"

#include <cstring>
#include <algorithm>
#include <cmath>

#include "Generated/ShaderCommonC.h"

using namespace qray;

Tonemapping::Tonemapping(
    VkDevice _device,
    std::shared_ptr<Framebuffers> _framebuffers,
    const std::shared_ptr<const ShaderManager> &_shaderManager,
    const std::shared_ptr<const GlobalUniform> &_uniform,
    const std::shared_ptr<MemoryAllocator> &_allocator)
{
    CreateTonemappingBuffer(_allocator);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        mappedTmBuffer[i] = tmBuffer[i].Map();

        if (mappedTmBuffer[i] != nullptr)
        {
            memset(mappedTmBuffer[i], 0, sizeof(ShTonemapping));
        }

    }
}

Tonemapping::~Tonemapping()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (tmBuffer[i].IsMapped())
        {
            tmBuffer[i].TryUnmap();
        }

        tmBuffer[i].Destroy();
    }
}

void Tonemapping::PrepareExposureParams(uint32_t frameIndex, const std::shared_ptr<const GlobalUniform> &uniform,
                                        float exposureBias, float tonemapPower, uint32_t tonemapType,
                                        const QrDrawFrameTonemappingParams &params)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    ShTonemapping *tm = static_cast<ShTonemapping *>(mappedTmBuffer[frameIndex]);

    if (tm == nullptr)
    {
        return;
    }

    const auto bounded = [](float value, float minimum, float maximum, float fallback)
    {
        return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
    };

    tm->tmExposureBias = bounded(exposureBias, -16.0f, 16.0f, 0.0f);
    tm->tmExposureSpeedDown = params.exposureSpeedDown > 0.0f
        ? bounded(params.exposureSpeedDown, 0.01f, 20.0f, 1.0f) : 1.0f;
    tm->tmExposureSpeedUp = params.exposureSpeedUp > 0.0f
        ? bounded(params.exposureSpeedUp, 0.01f, 20.0f, 3.0f) : 3.0f;
    tm->tmLowPercentile = bounded(params.exposureLowPercentile, 0.0f, 99.0f, 70.0f);
    tm->tmHighPercentile = bounded(params.exposureHighPercentile, tm->tmLowPercentile + 1.0f, 100.0f, 90.0f);
    if (params.exposureHighPercentile <= 0.0f)
    {
        tm->tmLowPercentile = 70.0f;
        tm->tmHighPercentile = 90.0f;
    }
    tm->tmMinLuminance = params.minAdaptedLuminance > 0.0f
        ? bounded(params.minAdaptedLuminance, 1e-4f, 60000.0f, 0.02f) : 0.02f;
    tm->tmMaxLuminance = params.maxAdaptedLuminance > 0.0f
        ? bounded(params.maxAdaptedLuminance, tm->tmMinLuminance, 60000.0f, 1.0f)
        : std::max(tm->tmMinLuminance, 1.0f);
    tm->tmNoiseBlend        = 0.5f;
    tm->tmNoiseStops        = -12.0f;
    tm->tmDynRangeStops     = 7.0f;
    tm->tonemapPower        = bounded(tonemapPower, 0.0f, 1.0f, 0.9f);
    tm->tonemapType         = std::min(tonemapType, 4u);
    tm->tmKneeStart         = 0.6f;
    tm->tmWhitePoint        = 10.0f;
    tm->tmSlopeBlurSigma    = 12.0f;
    const auto *frame = uniform->GetData();
    tm->frameTime = bounded(frame->timeDelta, 0.0f, 0.25f, 0.0f);
    float cameraDeltaSquared = 0.0f;
    for (uint32_t axis = 0; axis < 3; axis++)
    {
        const float delta = frame->cameraPosition[axis] - frame->cameraPositionPrev[axis];
        cameraDeltaSquared += delta * delta;
    }
    tm->resetCurve = !exposureReady || frame->restirParams[2] != 0 || frame->time < previousTime ||
                     frame->timeDelta > 1.0f || cameraDeltaSquared > 10000.0f ? 1u : 0u;

    const float kneeStart      = tm->tmKneeStart;
    const float kneeWhitePoint = tm->tmWhitePoint;
    const float kneeW = (kneeStart * (kneeStart - 2.0f) + kneeWhitePoint) / (kneeWhitePoint - 1.0f);

    tm->kneeW = kneeW;
    tm->kneeA = -kneeStart * kneeStart;
    tm->kneeB = kneeW - 2.0f * kneeStart;

    exposureReady = true;
    previousTime = frame->time;
}

VkBuffer Tonemapping::GetBuffer(uint32_t frameIndex) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    return tmBuffer[frameIndex].GetBuffer();
}

uint32_t Tonemapping::GetElementSize() const
{
    return static_cast<uint32_t>(sizeof(ShTonemapping));
}

void Tonemapping::SetAvgLuminance(uint32_t frameIndex, float avgLuminance)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    if (mappedTmBuffer[frameIndex] != nullptr)
    {
        static_cast<ShTonemapping *>(mappedTmBuffer[frameIndex])->avgLuminance = avgLuminance;
    }
}

void Tonemapping::OnShaderReload(const ShaderManager *shaderManager)
{
}

void Tonemapping::CreateTonemappingBuffer(const std::shared_ptr<MemoryAllocator> &allocator)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        tmBuffer[i].Init(
            allocator,
            sizeof(ShTonemapping),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "Tonemapping buffer");
    }
}
