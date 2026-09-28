// Copyright (c) 2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Tonemapping.h"

#include <cstring>

#include "Generated/ShaderCommonC.h"

vkpt::Tonemapping::Tonemapping(
    VkDevice _device,
    std::shared_ptr<Framebuffers> _framebuffers,
    const std::shared_ptr<const ShaderManager> &_shaderManager,
    const std::shared_ptr<const GlobalUniform> &_uniform,
    const std::shared_ptr<MemoryAllocator> &_allocator)
{
    CreateTonemappingBuffer(_allocator);

    // map once per frame slot; host only writes the params region, GPU owns the
    // rest of the struct (histogram/curve/adaptedLuminance)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        mappedTmBuffer[i] = tmBuffer[i].Map();
        if (mappedTmBuffer[i])
        {
            memset(mappedTmBuffer[i], 0, sizeof(ShTonemapping));
        }
        resetRequired[i] = true;
    }
}

vkpt::Tonemapping::~Tonemapping()
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

void vkpt::Tonemapping::PrepareExposureParams(uint32_t frameIndex, const std::shared_ptr<const GlobalUniform> &uniform,
                                              float exposureBias, float contrast)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    // Write tone mapper params from the host. Only the params prefix of the
    // buffer is touched here; the histogram/curve state lives past it and is
    // owned by the GPU. Each frame slot has its own copy so the host never
    // overwrites params that an in-flight frame's GPU still reads.
    if (mappedTmBuffer[frameIndex])
    {
        ShTonemapping *tm = static_cast<ShTonemapping *>(mappedTmBuffer[frameIndex]);

        tm->tmExposureBias     = exposureBias;
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

        // Piecewise knee (tone_mapping.c): y(x) = (w*x+a)/(x+b) with
        //   y(kneeStart)=kneeStart, dy/dx(kneeStart)=1, y(kneeWhitePoint)=whitePoint
        const float kneeStart      = tm->tmKneeStart;
        const float kneeWhitePoint = tm->tmWhitePoint;
        const float kneeW = (kneeStart * (kneeStart - 2.0f) + kneeWhitePoint) / (kneeWhitePoint - 1.0f);
        const float kneeA = -kneeStart * kneeStart;
        const float kneeB = kneeW - 2.0f * kneeStart;
        tm->kneeW = kneeW;
        tm->kneeA = kneeA;
        tm->kneeB = kneeB;

        resetRequired[frameIndex] = false;
    }
}

VkBuffer vkpt::Tonemapping::GetBuffer(uint32_t frameIndex) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    return tmBuffer[frameIndex].GetBuffer();
}

uint32_t vkpt::Tonemapping::GetElementSize() const
{
    return static_cast<uint32_t>(sizeof(ShTonemapping));
}

void vkpt::Tonemapping::SetAvgLuminance(uint32_t frameIndex, float avgLuminance)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    // Same host-visible mapping the params prefix is written through (CalculateExposure), so the
    // write is coherent and per-slot: the slot's fence keeps the GPU from reading it in flight.
    if (mappedTmBuffer[frameIndex] != nullptr)
    {
        static_cast<ShTonemapping *>(mappedTmBuffer[frameIndex])->avgLuminance = avgLuminance;
    }
}

void vkpt::Tonemapping::OnShaderReload(const ShaderManager *shaderManager)
{
}

void vkpt::Tonemapping::CreateTonemappingBuffer(const std::shared_ptr<MemoryAllocator> &allocator)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        tmBuffer[i].Init(
            allocator,
            sizeof(ShTonemapping),
            // The RHI layer wraps this buffer through a native handle, and NVRHI queries the buffer
            // device address on every wrap - vulkan-buffer.cpp:215-220 - so the usage bit is
            // mandatory there. The memory is address-capable regardless: Buffer::Init always
            // allocates through AllocType::WITH_ADDRESS_QUERY (Buffer.cpp:67,
            // MemoryAllocator.cpp:269-277). Same reason the collector's geometry buffers carry it
            // (RasterizedDataCollector.cpp:72-81).
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "Tonemapping buffer");
    }
}

