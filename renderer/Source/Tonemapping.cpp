// Copyright (c) 2026 QuakeRay contributors
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

        resetRequired[i] = true;
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
                                        float exposureBias, float contrast)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    ShTonemapping *tm = static_cast<ShTonemapping *>(mappedTmBuffer[frameIndex]);

    if (tm == nullptr)
    {
        return;
    }

    tm->tmExposureBias      = exposureBias;
    tm->tmExposureSpeedDown = 1.0f;
    tm->tmExposureSpeedUp   = 1.0f;
    tm->tmLowPercentile     = 70.0f;
    tm->tmHighPercentile    = 90.0f;
    tm->tmMinLuminance      = 0.02f;
    tm->tmMaxLuminance      = 1.0f;
    tm->tmNoiseBlend        = 0.5f;
    tm->tmNoiseStops        = -12.0f;
    tm->tmDynRangeStops     = 7.0f;
    tm->tmReinhard          = contrast;
    tm->tmKneeStart         = 0.6f;
    tm->tmWhitePoint        = 10.0f;
    tm->tmSlopeBlurSigma    = 12.0f;
    tm->frameTime           = uniform->GetData()->timeDelta;
    tm->resetCurve          = resetRequired[frameIndex] ? 1u : 0u;

    const float kneeStart      = tm->tmKneeStart;
    const float kneeWhitePoint = tm->tmWhitePoint;
    const float kneeW = (kneeStart * (kneeStart - 2.0f) + kneeWhitePoint) / (kneeWhitePoint - 1.0f);

    tm->kneeW = kneeW;
    tm->kneeA = -kneeStart * kneeStart;
    tm->kneeB = kneeW - 2.0f * kneeStart;

    resetRequired[frameIndex] = false;
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
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "Tonemapping buffer");
    }
}
