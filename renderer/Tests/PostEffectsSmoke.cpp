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

#include "VulkanTestContext.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiExposureHistory.h"
#include "Generated/ShaderCommonC.h"
#include "Generated/ShaderCommonCFramebuf.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>

using namespace qray;

namespace
{

uint32_t UavBinding(FramebufferImageIndex image)
{
    return ShFramebuffers_Bindings[image];
}

uint32_t SrvBinding(FramebufferImageIndex image)
{
    return ShFramebuffers_Sampled_Bindings[image];
}

uint32_t SamplerBinding(FramebufferImageIndex image)
{
    const uint32_t binding = ShFramebuffers_Sampler_Bindings[image];
    Require(binding != FB_SAMPLER_INVALID_BINDING, "generated framebuffer sampler binding");
    return binding;
}

nvrhi::BindingLayoutHandle Layout(nvrhi::IDevice *device, std::initializer_list<nvrhi::BindingLayoutItem> items)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0)
        .setUnorderedAccessViewOffset(0).setConstantBufferOffset(0).setSamplerOffset(0));
    for (auto item : items) desc.addItem(item);
    auto layout = device->createBindingLayout(desc);
    Require(layout != nullptr, "create test layout");
    return layout;
}

nvrhi::ComputePipelineHandle Pipeline(nvrhi::IDevice *device, const std::string &path,
    std::initializer_list<nvrhi::IBindingLayout *> layouts)
{
    auto shader = rhi::loadShader(device, path, nvrhi::ShaderType::Compute, path);
    Require(shader != nullptr, "load " + path);
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    for (auto layout : layouts) desc.addBindingLayout(layout);
    auto pipeline = device->createComputePipeline(desc);
    Require(pipeline != nullptr, "create " + path);
    return pipeline;
}

float Half(uint16_t bits)
{
    const unsigned exponent = (bits >> 10) & 31;
    const float value = exponent == 0 ? std::ldexp(float(bits & 1023), -24)
        : exponent == 31 ? std::bit_cast<float>(uint32_t(0x7f800000 | ((bits & 1023) << 13)))
        : std::ldexp(1.0f + float(bits & 1023) / 1024.0f, int(exponent) - 15);
    return bits & 0x8000 ? -value : value;
}

float UnsignedFloat(uint32_t bits, unsigned mantissaBits)
{
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1u);
    const uint32_t exponent = bits >> mantissaBits;
    return exponent == 0 ? std::ldexp(float(mantissa), -14 - int(mantissaBits))
        : exponent == 31 ? std::numeric_limits<float>::infinity()
        : std::ldexp(1.0f + float(mantissa) / float(1u << mantissaBits), int(exponent) - 15);
}

std::vector<float> ReadTexture(nvrhi::IDevice *device, nvrhi::ITexture *texture)
{
    auto desc = texture->getDesc();
    desc.isUAV = false;
    desc.keepInitialState = false;
    auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    Require(staging != nullptr, "create readback texture");
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "wait for texture");
    size_t pitch = 0;
    auto data = static_cast<const uint8_t *>(device->mapStagingTexture(staging,
        nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    Require(data != nullptr, "map texture");
    std::vector<float> values(size_t(desc.width) * desc.height);
    for (uint32_t y = 0; y < desc.height; y++)
        for (uint32_t x = 0; x < desc.width; x++)
        {
            if (desc.format == nvrhi::Format::RGBA16_FLOAT)
                values[size_t(y) * desc.width + x] = Half(reinterpret_cast<const uint16_t *>(data + y * pitch)[x * 4]);
            else
                values[size_t(y) * desc.width + x] = UnsignedFloat(
                    reinterpret_cast<const uint32_t *>(data + y * pitch)[x] & 0x7ffu, 6);
        }
    device->unmapStagingTexture(staging);
    return values;
}

std::vector<float> ReadTextureRGBA(nvrhi::IDevice *device, nvrhi::ITexture *texture)
{
    auto desc = texture->getDesc();
    Require(desc.format == nvrhi::Format::RGBA16_FLOAT || desc.format == nvrhi::Format::R11G11B10_FLOAT,
        "supported color readback format");
    desc.isUAV = false;
    desc.keepInitialState = false;
    auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    Require(staging != nullptr, "create readback texture");
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "wait for texture");
    size_t pitch = 0;
    auto data = static_cast<const uint8_t *>(device->mapStagingTexture(staging,
        nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    Require(data != nullptr, "map texture");
    std::vector<float> values(size_t(desc.width) * desc.height * 4);
    for (uint32_t y = 0; y < desc.height; y++)
        for (uint32_t x = 0; x < desc.width; x++)
        {
            const size_t pixel = (size_t(y) * desc.width + x) * 4;
            if (desc.format == nvrhi::Format::RGBA16_FLOAT)
            {
                for (uint32_t c = 0; c < 4; c++)
                    values[pixel + c] = Half(reinterpret_cast<const uint16_t *>(data + y * pitch)[x * 4 + c]);
            }
            else
            {
                const uint32_t bits = reinterpret_cast<const uint32_t *>(data + y * pitch)[x];
                values[pixel] = UnsignedFloat(bits & 0x7ffu, 6);
                values[pixel + 1] = UnsignedFloat((bits >> 11) & 0x7ffu, 6);
                values[pixel + 2] = UnsignedFloat(bits >> 22, 5);
            }
        }
    device->unmapStagingTexture(staging);
    return values;
}

class ExposureProbe
{
public:
    ExposureProbe(nvrhi::IDevice *dev, const std::string &shaders) : device(dev)
    {
        histogramLayout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_PRE_FINAL))});
        emptyLayout = Layout(device, {});
        uniformLayout = Layout(device, {nvrhi::BindingLayoutItem::ConstantBuffer(0)});
        exposureLayout = Layout(device, {nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)});
        histogram = Pipeline(device, shaders + "/CmLuminanceHistogram.comp.spv",
            {histogramLayout, uniformLayout, exposureLayout});
        average = Pipeline(device, shaders + "/CmLuminanceAvg.comp.spv",
            {emptyLayout, uniformLayout, exposureLayout});
        emptySet = device->createBindingSet(nvrhi::BindingSetDesc(), emptyLayout);

        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(ShGlobalUniform);
        desc.isConstantBuffer = true;
        desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        desc.keepInitialState = true;
        uniform = device->createBuffer(desc);
        uniformSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
            nvrhi::BindingSetItem::ConstantBuffer(0, uniform)), uniformLayout);
        desc = {};
        desc.byteSize = sizeof(ShTonemapping);
        desc.structStride = sizeof(ShTonemapping);
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        for (size_t i = 0; i < exposure.size(); i++)
        {
            exposure[i] = device->createBuffer(desc);
            exposureSets[i] = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
                nvrhi::BindingSetItem::StructuredBuffer_UAV(0, exposure[i])), exposureLayout);
            Require(exposure[i] != nullptr && exposureSets[i] != nullptr, "create exposure slot");
        }
        desc = {};
        desc.byteSize = sizeof(ShTonemapping);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        readback = device->createBuffer(desc);
        Require(uniform != nullptr && uniformSet != nullptr && readback != nullptr && emptySet != nullptr,
            "create exposure probe resources");
    }

    ShTonemapping Run(uint32_t width, uint32_t height, std::vector<float> &pixels,
        bool reset, float dt, float low = 0.0f, float high = 100.0f)
    {
        nvrhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        auto input = device->createTexture(desc);
        auto inputSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
            nvrhi::BindingSetItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_PRE_FINAL), input)), histogramLayout);
        Require(input != nullptr && inputSet != nullptr, "create exposure input");

        ShGlobalUniform frame{};
        frame.renderWidth = width;
        frame.renderHeight = height;
        ShTonemapping settings{};
        settings.tmExposureSpeedUp = 3.0f;
        settings.tmExposureSpeedDown = 1.0f;
        settings.tmLowPercentile = low;
        settings.tmHighPercentile = high;
        settings.tmMinLuminance = 0.0001f;
        settings.tmMaxLuminance = 64.0f;
        settings.tmNoiseStops = -12.0f;
        settings.tmDynRangeStops = 7.0f;
        settings.tmNoiseBlend = 0.5f;
        settings.tmSlopeBlurSigma = 12.0f;
        settings.frameTime = dt;
        settings.resetCurve = reset ? 1u : 0u;

        const uint32_t slot = serial % uint32_t(exposure.size());
        const uint32_t previous = (serial + uint32_t(exposure.size()) - 1) % uint32_t(exposure.size());
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->writeTexture(input, 0, 0, pixels.data(), size_t(width) * sizeof(float) * 4);
        cmd->writeBuffer(uniform, &frame, sizeof(frame));
        if (reset || serial == 0)
            cmd->writeBuffer(exposure[slot], &settings, sizeof(settings));
        else
        {
            cmd->writeBuffer(exposure[slot], &settings, offsetof(ShTonemapping, histogram));
            rhi::transferExposureHistory(cmd, exposure[previous], exposure[slot]);
        }
        nvrhi::ComputeState state;
        state.pipeline = histogram;
        state.addBindingSet(inputSet).addBindingSet(uniformSet).addBindingSet(exposureSets[slot]);
        cmd->setComputeState(state);
        cmd->dispatch((width + 15) / 16, (height + 15) / 16);
        cmd->setBufferState(exposure[slot], nvrhi::ResourceStates::UnorderedAccess);
        state = {};
        state.pipeline = average;
        state.addBindingSet(emptySet).addBindingSet(uniformSet).addBindingSet(exposureSets[slot]);
        cmd->setComputeState(state);
        cmd->dispatch(1, 1);
        cmd->copyBuffer(readback, 0, exposure[slot], 0, sizeof(settings));
        cmd->close();
        device->executeCommandList(cmd);
        Require(device->waitForIdle(), "wait for exposure");
        auto data = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
        Require(data != nullptr, "map exposure");
        ShTonemapping output{};
        std::memcpy(&output, data, sizeof(output));
        device->unmapBuffer(readback);
        serial++;
        Require(std::isfinite(output.avgLuminance) && output.avgLuminance > 0.0f, "finite exposure");
        for (float v : output.curve) Require(std::isfinite(v), "finite tone curve");
        for (auto bin : output.histogram) Require(bin == 0, "histogram cleared");
        return output;
    }

private:
    nvrhi::IDevice *device;
    nvrhi::BindingLayoutHandle histogramLayout, emptyLayout, uniformLayout, exposureLayout;
    nvrhi::ComputePipelineHandle histogram, average;
    nvrhi::BufferHandle uniform, readback;
    std::array<nvrhi::BufferHandle, 3> exposure;
    std::array<nvrhi::BindingSetHandle, 3> exposureSets;
    nvrhi::BindingSetHandle emptySet, uniformSet;
    uint32_t serial = 0;
};

std::vector<float> Image(uint32_t width, uint32_t height, float value)
{
    auto pixels = std::vector<float>(size_t(width) * height * 4, value);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 1.0f;
    return pixels;
}

void CheckVignette(nvrhi::IDevice *device, const std::string &shaders)
{
    auto images = Layout(device, {nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING)),
        nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG))});
    auto emptyLayout = Layout(device, {});
    auto empty = device->createBindingSet(nvrhi::BindingSetDesc(), emptyLayout);
    auto pushLayout = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 28)});
    auto pipeline = Pipeline(device, shaders + "/EfVignette.comp.spv", {images, emptyLayout, pushLayout});
    for (const auto size : {std::array<uint32_t, 2>{65, 37}, {37, 65}, {1, 1}})
    {
        nvrhi::TextureDesc desc;
        desc.width = size[0];
        desc.height = size[1];
        desc.format = nvrhi::Format::R11G11B10_FLOAT;
        desc.isUAV = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        auto ping = device->createTexture(desc);
        auto pong = device->createTexture(desc);
        auto set = device->createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING), ping))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG), pong)), images);
        Require(set != nullptr, "create vignette set");
        for (float intensity : {0.0f, 0.5f})
        {
            const std::array<uint32_t, 7> push = {0, 0, 0, std::bit_cast<uint32_t>(intensity),
                std::bit_cast<uint32_t>(0.45f), std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(0.35f)};
            auto cmd = device->createCommandList();
            cmd->open();
            cmd->clearTextureFloat(pong, nvrhi::AllSubresources, nvrhi::Color(0.5f));
            nvrhi::ComputeState state;
            state.pipeline = pipeline;
            state.addBindingSet(set).addBindingSet(empty);
            cmd->setComputeState(state);
            cmd->setPushConstants(push.data(), sizeof(push));
            cmd->dispatch((size[0] + 15) / 16, (size[1] + 15) / 16);
            cmd->close();
            device->executeCommandList(cmd);
            auto values = ReadTexture(device, ping);
            for (float v : values) Require(std::isfinite(v) && v > 0.0f && v <= 0.5f, "bounded vignette");
            Require(std::abs(values[(size_t(size[1] / 2) * size[0]) + size[0] / 2] - 0.5f) < 0.01f,
                "vignette preserves center");
            if (intensity == 0.0f)
                for (float v : values) Require(std::abs(v - 0.5f) < 0.01f, "vignette zero is identity");
            else if (size[0] > 1)
                Require(values.front() < 0.35f && std::abs(values.front() - values.back()) < 0.01f,
                    "vignette dims symmetric corners");
        }
    }
}

void CheckColorCompositing(nvrhi::IDevice *device, const std::string &probes)
{
    auto layout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_UAV(2)});
    auto pushLayout = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 16)});
    auto pipeline = Pipeline(device, probes + "/ColorCompositingProbe.comp.spv", {layout, pushLayout});
    nvrhi::TextureDesc desc;
    desc.width = 17;
    desc.height = 9;
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    auto scene = device->createTexture(desc);
    auto bloom = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto output = device->createTexture(desc);
    auto set = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, scene))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, bloom))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(2, output)), layout);
    Require(scene != nullptr && bloom != nullptr && output != nullptr && set != nullptr,
        "create color compositing probe");

    const auto run = [&](uint32_t mode, nvrhi::Color sceneColor, nvrhi::Color bloomColor = nvrhi::Color(0.0f),
                         float bloomStrength = 0.0f, bool thresholded = true) {
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(scene, nvrhi::AllSubresources, sceneColor);
        cmd->clearTextureFloat(bloom, nvrhi::AllSubresources, bloomColor);
        nvrhi::ComputeState state;
        state.pipeline = pipeline;
        state.addBindingSet(set);
        cmd->setComputeState(state);
        const uint32_t push[4] = {mode, thresholded ? 1u : 0u,
            std::bit_cast<uint32_t>(bloomStrength), 0};
        cmd->setPushConstants(push, sizeof(push));
        cmd->dispatch(2, 1);
        cmd->close();
        device->executeCommandList(cmd);
        auto values = ReadTextureRGBA(device, output);
        for (size_t i = 0; i < values.size(); i++)
            Require(std::isfinite(values[i]) && values[i] >= 0.0f, "finite color compositing output");
        return std::array<float, 3>{values[0], values[1], values[2]};
    };
    const auto matches = [](const std::array<float, 3> &value, const std::array<float, 3> &expected,
                            const std::string &name) {
        for (size_t channel = 0; channel < 3; channel++)
            Require(std::abs(value[channel] - expected[channel]) < 0.002f * std::max(1.0f, expected[channel]), name);
    };

    for (float peak : {0.25f, 0.5f, 4.0f, 4096.0f})
    {
        const nvrhi::Color input(peak, peak * 0.25f, peak * 0.0625f, 1.0f);
        const float bounded = std::min(peak, 1.0f);
        matches(run(0, input), {bounded, bounded * 0.25f, bounded * 0.0625f}, "display gamut preserves hue");
        const float radiance = std::min(peak, 256.0f);
        matches(run(1, input), {radiance, radiance * 0.25f, radiance * 0.0625f}, "radiance bound preserves hue");
        const auto shoulder = run(2, input);
        Require(shoulder[0] <= 1.0f && shoulder[0] > 0.0f, "bounded highlight shoulder");
        matches(shoulder, {shoulder[0], shoulder[0] * 0.25f, shoulder[0] * 0.0625f},
            "tone shoulder preserves hue");
        if (peak <= 0.6f)
            matches(shoulder, {peak, peak * 0.25f, peak * 0.0625f}, "tone shoulder leaves midtones unchanged");
    }
    matches(run(4, nvrhi::Color(120000.0f, 30000.0f, 7500.0f, 1.0f)),
        {60000.0f, 15000.0f, 3750.0f}, "HDR storage bound preserves hue");
    matches(run(0, nvrhi::Color(std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f, 1.0f)),
        {0.0f, 0.0f, 0.0f}, "invalid display color is black");
    matches(run(1, nvrhi::Color(std::numeric_limits<float>::infinity(), 1.0f, 0.0f, 1.0f)),
        {0.0f, 0.0f, 0.0f}, "invalid radiance input is black");
    const nvrhi::Color base(0.3f, 0.2f, 0.1f, 1.0f);
    const nvrhi::Color halo(2.0f, 0.5f, 0.25f, 1.0f);
    matches(run(3, base, halo, 0.2f), {0.7f, 0.3f, 0.15f}, "bloom adds HDR energy without alpha");
    matches(run(3, base, halo), {0.3f, 0.2f, 0.1f}, "disabled bloom leaves scene unchanged");
    matches(run(3, base, base, 0.2f, false),
        {0.3f, 0.2f, 0.1f}, "thresholdless bloom conserves uniform scene energy");
    matches(run(3, base, nvrhi::Color(0.0f), 0.2f, false),
        {0.24f, 0.16f, 0.08f}, "thresholdless bloom redistributes rather than adds scene energy");
    matches(run(5, base, nvrhi::Color(1.0f, 0.15f, 0.1f, 1.0f), 0.5f),
        {0.3f, 0.115f, 0.055f}, "tint filters scene instead of adding a pale layer");
    matches(run(5, nvrhi::Color(0.0f), halo, 1.0f),
        {0.0f, 0.0f, 0.0f}, "tint cannot lift black");
    matches(run(5, base, nvrhi::Color(1.0f), 1.0f),
        {0.3f, 0.2f, 0.1f}, "neutral filter is identity");
}

void CheckGameplayColor(nvrhi::IDevice *device, const std::string &shaders)
{
    auto images = Layout(device, {nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING)),
        nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG))});
    auto uniforms = Layout(device, {nvrhi::BindingLayoutItem::ConstantBuffer(0)});
    auto feedbackPush = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 44)});
    auto tintPush = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 28)});
    auto feedback = Pipeline(device, shaders + "/EfGameplayFeedback.comp.spv", {images, uniforms, feedbackPush});
    auto tint = Pipeline(device, shaders + "/EfColorTint.comp.spv", {images, uniforms, tintPush});
    const uint32_t width = 33;
    const uint32_t height = 17;
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::R11G11B10_FLOAT;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    auto ping = device->createTexture(desc);
    auto pong = device->createTexture(desc);
    auto imageSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING), ping))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG), pong)), images);
    nvrhi::BufferDesc buffer;
    buffer.byteSize = sizeof(ShGlobalUniform);
    buffer.isConstantBuffer = true;
    buffer.initialState = nvrhi::ResourceStates::ConstantBuffer;
    buffer.keepInitialState = true;
    auto uniform = device->createBuffer(buffer);
    auto uniformSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, uniform)), uniforms);
    Require(ping != nullptr && pong != nullptr && imageSet != nullptr && uniform != nullptr && uniformSet != nullptr,
        "create gameplay color targets");
    const auto run = [&](bool legacyTint, float base, float damage, float pickup) {
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(pong, nvrhi::AllSubresources, nvrhi::Color(base));
        ShGlobalUniform frame{};
        frame.time = 0.25f;
        cmd->writeBuffer(uniform, &frame, sizeof(frame));
        nvrhi::ComputeState state;
        state.pipeline = legacyTint ? tint : feedback;
        state.addBindingSet(imageSet).addBindingSet(uniformSet);
        cmd->setComputeState(state);
        const uint32_t push[11] = {0, 0, 0, std::bit_cast<uint32_t>(damage), 0,
            std::bit_cast<uint32_t>(pickup), std::bit_cast<uint32_t>(0.14f), 0,
            std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(0.831373f), std::bit_cast<uint32_t>(0.482353f)};
        const uint32_t legacyPush[7] = {0, 0, 0, std::bit_cast<uint32_t>(damage),
            std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(0.15f), std::bit_cast<uint32_t>(0.1f)};
        cmd->setPushConstants(legacyTint ? legacyPush : push, legacyTint ? sizeof(legacyPush) : sizeof(push));
        cmd->dispatch((width + 15) / 16, (height + 15) / 16);
        cmd->close();
        device->executeCommandList(cmd);
        auto values = ReadTextureRGBA(device, ping);
        for (size_t i = 0; i < values.size(); i++)
            Require(std::isfinite(values[i]) && values[i] >= 0.0f && values[i] <= 1.0f, "bounded gameplay color");
        return values;
    };
    const auto at = [&](const std::vector<float> &values, uint32_t x, uint32_t y, uint32_t channel) {
        return values[(size_t(y) * width + x) * 4 + channel];
    };
    auto values = run(false, 0.5f, 0.0f, 0.0f);
    Require(at(values, 0, 0, 0) == 0.5f && at(values, 0, 0, 1) == 0.5f && at(values, 0, 0, 2) == 0.5f,
        "disabled gameplay feedback is identity");
    values = run(false, 0.5f, 1.0f, 0.0f);
    Require(at(values, 0, 0, 0) == 0.5f && std::abs(at(values, 0, 0, 1) - 0.449f) < 0.01f &&
        std::abs(at(values, 0, 0, 2) - 0.446f) < 0.01f, "damage is a blood-red transmission filter");
    Require(at(values, width / 2, height / 2, 1) == 0.5f, "damage protects the centre");
    values = run(false, 0.5f, 0.0f, 1.0f);
    Require(at(values, width / 2, height - 1, 0) == 0.5f &&
        at(values, width / 2, height - 1, 1) < 0.46f && at(values, width / 2, height - 1, 2) < 0.32f,
        "pickup warms the bottom without a white veil");
    Require(at(values, width / 2, 0, 1) == 0.5f, "pickup protects the top");
    values = run(false, 0.0f, 1.0f, 1.0f);
    Require(at(values, 0, height - 1, 0) == 0.0f && at(values, 0, height - 1, 1) == 0.0f &&
        at(values, 0, height - 1, 2) == 0.0f, "gameplay colors leave black black");
    values = run(true, 0.5f, 1.0f, 0.0f);
    Require(at(values, 0, 0, 0) == 0.5f && at(values, 0, 0, 1) < 0.35f && at(values, 0, 0, 2) < 0.35f,
        "legacy color tint filters instead of replacing scene color");
    values = run(true, 0.0f, 1.0f, 0.0f);
    Require(at(values, 0, 0, 0) == 0.0f, "legacy tint cannot lift black");
}

void CheckFilmGrain(nvrhi::IDevice *device, const std::string &shaders)
{
    auto images = Layout(device, {nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING)),
        nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG))});
    auto uniforms = Layout(device, {nvrhi::BindingLayoutItem::ConstantBuffer(0)});
    auto pushLayout = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 20)});
    auto pipeline = Pipeline(device, shaders + "/EfFilmGrain.comp.spv", {images, uniforms, pushLayout});
    const uint32_t width = 64;
    const uint32_t height = 48;
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::R11G11B10_FLOAT;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    auto ping = device->createTexture(desc);
    auto pong = device->createTexture(desc);
    auto imageSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING), ping))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PONG), pong)), images);
    nvrhi::BufferDesc buffer;
    buffer.byteSize = sizeof(ShGlobalUniform);
    buffer.isConstantBuffer = true;
    buffer.initialState = nvrhi::ResourceStates::ConstantBuffer;
    buffer.keepInitialState = true;
    auto uniform = device->createBuffer(buffer);
    auto uniformSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, uniform)), uniforms);
    Require(ping != nullptr && pong != nullptr && imageSet != nullptr && uniform != nullptr && uniformSet != nullptr,
        "create film grain targets");
    const auto run = [&](float base, float intensity, uint32_t frameId) {
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(pong, nvrhi::AllSubresources, nvrhi::Color(base));
        ShGlobalUniform frame{};
        frame.frameId = frameId;
        cmd->writeBuffer(uniform, &frame, sizeof(frame));
        nvrhi::ComputeState state;
        state.pipeline = pipeline;
        state.addBindingSet(imageSet).addBindingSet(uniformSet);
        cmd->setComputeState(state);
        const uint32_t push[5] = {0, 0, 0, std::bit_cast<uint32_t>(intensity), std::bit_cast<uint32_t>(1.6f)};
        cmd->setPushConstants(push, sizeof(push));
        cmd->dispatch((width + 15) / 16, (height + 15) / 16);
        cmd->close();
        device->executeCommandList(cmd);
        auto values = ReadTextureRGBA(device, ping);
        for (float v : values) Require(std::isfinite(v) && v >= 0.0f && v <= 1.0f, "bounded film grain");
        return values;
    };
    auto flat = run(0.5f, 0.0f, 1);
    for (size_t i = 0; i < flat.size(); i += 4)
        Require(flat[i] == 0.5f && flat[i + 1] == 0.5f && flat[i + 2] == 0.5f, "zero film grain is identity");
    auto grained = run(0.5f, 0.6f, 1);
    double sum = 0.0;
    float maxDelta = 0.0f;
    for (size_t i = 0; i < grained.size(); i += 4)
    {
        const float delta = grained[i] - 0.5f;
        sum += delta;
        maxDelta = std::max(maxDelta, std::abs(delta));
        Require(std::abs(delta - (grained[i + 1] - 0.5f)) < 0.01f &&
            std::abs(delta - (grained[i + 2] - 0.5f)) < 0.01f, "film grain is monochrome");
    }
    Require(maxDelta > 0.005f, "film grain is visible");
    Require(std::abs(sum / double(grained.size() / 4)) < 0.01, "film grain preserves the mean colour");
    auto other = run(0.5f, 0.6f, 2);
    bool differs = false;
    for (size_t i = 0; i < grained.size() && !differs; i++) differs = grained[i] != other[i];
    Require(differs, "film grain is a new one every frame");
    auto strong = run(0.5f, 1.0f, 1);
    float strongDelta = 0.0f;
    for (size_t i = 0; i < strong.size(); i += 4)
        strongDelta = std::max(strongDelta, std::abs(strong[i] - 0.5f));
    Require(strongDelta > maxDelta, "film grain scales with its strength");
    auto black = run(0.0f, 1.0f, 1);
    for (float v : black) Require(v == 0.0f, "film grain leaves black black");
}

void CheckNearDof(nvrhi::IDevice *device, const std::string &probes)
{
    auto layout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_UAV(3)});
    auto pushLayout = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 16)});
    auto pipeline = Pipeline(device, probes + "/NearDofProbe.comp.spv", {layout, pushLayout});
    const auto run = [&](uint32_t width, uint32_t height, const std::vector<float> &pixels,
                         const std::vector<float> &depths, const std::vector<float> &surfaces,
                         float strength, float focus = 8.0f) {
        nvrhi::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = nvrhi::Format::RGBA32_FLOAT;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        auto source = device->createTexture(desc);
        auto depth = device->createTexture(desc);
        auto surface = device->createTexture(desc);
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        auto output = device->createTexture(desc);
        auto set = device->createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(1, depth))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(2, surface))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(3, output)), layout);
        Require(source != nullptr && depth != nullptr && surface != nullptr && output != nullptr && set != nullptr,
            "create near DOF targets");
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->writeTexture(source, 0, 0, pixels.data(), width * sizeof(float) * 4);
        cmd->writeTexture(depth, 0, 0, depths.data(), width * sizeof(float) * 4);
        cmd->writeTexture(surface, 0, 0, surfaces.data(), width * sizeof(float) * 4);
        nvrhi::ComputeState state;
        state.pipeline = pipeline;
        state.addBindingSet(set);
        cmd->setComputeState(state);
        const float push[4] = {strength, focus, 6.0f, 1.0f};
        cmd->setPushConstants(push, sizeof(push));
        cmd->dispatch((width + 15) / 16, (height + 15) / 16);
        cmd->close();
        device->executeCommandList(cmd);
        auto values = ReadTextureRGBA(device, output);
        for (float value : values)
            Require(std::isfinite(value) && value >= 0.0f, "finite near DOF output");
        return values;
    };
    const auto metadata = [](uint32_t width, uint32_t height, uint32_t flags) {
        auto values = Image(width, height, 0.0f);
        for (size_t i = 3; i < values.size(); i += 4)
            values[i] = std::bit_cast<float>(flags);
        return values;
    };
    for (const auto size : {std::array<uint32_t, 2>{1, 1}, {17, 9}, {32, 32}})
    {
        const auto values = run(size[0], size[1], Image(size[0], size[1], 0.5f),
            Image(size[0], size[1], 2.0f), metadata(size[0], size[1], INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON), 1.0f);
        for (size_t i = 0; i < values.size(); i += 4)
            Require(std::abs(values[i] - 0.5f) < 0.001f, "near DOF conserves constant color at edges and tiny sizes");
    }
    const uint32_t size = 32;
    const size_t center = (16 * size + 16) * 4;
    auto pixels = Image(size, size, 0.0f);
    pixels[center] = 1.0f;
    pixels[center + 1] = 0.25f;
    pixels[center + 2] = 0.125f;
    auto depths = Image(size, size, 2.0f);
    auto surfaces = metadata(size, size, INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON);
    auto off = run(size, size, pixels, depths, surfaces, 0.0f);
    Require(off[center] == 1.0f && off[center + 4] == 0.0f, "zero near DOF is identity");
    auto subtle = run(size, size, pixels, depths, surfaces, 0.25f);
    auto strong = run(size, size, pixels, depths, surfaces, 1.0f);
    Require(subtle[center] < 0.95f && subtle[center] > strong[center] && strong[center + 4] > 0.0f,
        "near weapon blur grows with strength");
    Require(std::abs(strong[center + 1] / strong[center] - 0.25f) < 0.001f &&
        std::abs(strong[center + 2] / strong[center] - 0.125f) < 0.001f, "near DOF preserves channel ratios");
    auto scaled = run(size, size, pixels, Image(size, size, 1.0f), surfaces, 0.25f, 4.0f);
    Require(std::abs(scaled[center] - subtle[center]) < 0.001f, "near DOF is invariant to weapon scale");
    auto farther = run(size, size, pixels, Image(size, size, 5.0f), surfaces, 0.25f);
    Require(farther[center] > subtle[center], "farther weapon parts are sharper");
    auto focused = run(size, size, pixels, Image(size, size, 8.0f), surfaces, 1.0f);
    Require(focused[center] == 1.0f && focused[center + 4] == 0.0f, "focus plane and farther parts stay sharp");
    auto invalid = run(size, size, pixels, Image(size, size, std::numeric_limits<float>::quiet_NaN()), surfaces, 1.0f);
    Require(invalid[center] == 1.0f, "invalid depth cannot trigger DOF");
    auto world = run(size, size, pixels, depths, metadata(size, size, 0u), 1.0f);
    Require(world[center] == 1.0f && world[center + 4] == 0.0f, "near world geometry is never blurred");
    auto viewer = run(size, size, pixels, depths, metadata(size, size, INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER), 1.0f);
    Require(viewer[center] == 1.0f, "player body is not a first-person weapon");

    pixels = Image(size, size, 0.25f);
    surfaces = metadata(size, size, 0u);
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++)
        {
            const uint32_t cbx = ((x + y % 2) % 2) * (size / 2) + x / 2;
            if (x < 16)
                surfaces[(size_t(y) * size + cbx) * 4 + 3] = std::bit_cast<float>(uint32_t(INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON));
            else
                for (size_t channel = 0; channel < 3; channel++)
                    pixels[(size_t(y) * size + x) * 4 + channel] = 100.0f;
        }
    auto edge = run(size, size, pixels, depths, surfaces, 1.0f);
    Require(std::abs(edge[(16 * size + 15) * 4] - 0.25f) < 0.001f &&
        edge[(16 * size + 16) * 4] == 100.0f, "DOF cannot mix world color across the weapon silhouette");
    depths = Image(size, size, 2.0f);
    surfaces = metadata(size, size, INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON);
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 16; x < size; x++)
        {
            const uint32_t cbx = ((x + y % 2) % 2) * (size / 2) + x / 2;
            depths[(size_t(y) * size + cbx) * 4] = 10.0f;
        }
    auto focusEdge = run(size, size, pixels, depths, surfaces, 1.0f);
    Require(std::abs(focusEdge[(16 * size + 15) * 4] - 0.25f) < 0.001f &&
        focusEdge[(16 * size + 16) * 4] == 100.0f, "DOF cannot smear focused weapon parts into the near part");
}

void CheckLocalExposure(nvrhi::IDevice *device, const std::string &probes)
{
    auto layout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(1), nvrhi::BindingLayoutItem::Texture_UAV(2)});
    auto pipeline = Pipeline(device, probes + "/LocalExposureProbe.comp.spv", {layout});
    nvrhi::TextureDesc desc;
    desc.width = desc.height = 32;
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    auto input = device->createTexture(desc);
    desc.isUAV = true;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto output = device->createTexture(desc);
    auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    auto set = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, input))
        .addItem(nvrhi::BindingSetItem::Sampler(1, sampler))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(2, output)), layout);
    for (bool split : {false, true})
    {
        auto pixels = Image(32, 32, 0.5f);
        if (split)
            for (size_t y = 0; y < 32; y++)
                for (size_t x = 0; x < 32; x++)
                    for (size_t channel = 0; channel < 3; channel++)
                        pixels[(y * 32 + x) * 4 + channel] = x < 16 ? 0.03125f : 16.0f;
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->writeTexture(input, 0, 0, pixels.data(), 32 * sizeof(float) * 4);
        nvrhi::ComputeState state;
        state.pipeline = pipeline;
        state.addBindingSet(set);
        cmd->setComputeState(state);
        cmd->dispatch(2, 2);
        cmd->close();
        device->executeCommandList(cmd);
        auto values = ReadTexture(device, output);
        for (float v : values) Require(std::isfinite(v) && std::abs(v) <= 1.0f, "bounded local EV");
        if (!split)
            for (float v : values) Require(std::abs(v) < 0.001f, "local exposure preserves constant scene");
        else
            Require(values[16 * 32 + 15] > 0.99f && values[16 * 32 + 16] < -0.99f,
                "local exposure preserves contrast boundary");
    }
}

void CheckTaauHistoryReset(nvrhi::IDevice *device, const std::string &shaderFolder)
{
    auto imageLayout = Layout(device, {
        nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING)),
        nvrhi::BindingLayoutItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY)),
        nvrhi::BindingLayoutItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_FINAL)),
        nvrhi::BindingLayoutItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_MOTION_DLSS)),
        nvrhi::BindingLayoutItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY_PREV)),
        nvrhi::BindingLayoutItem::Sampler(SamplerBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY_PREV)),
    });
    auto uniformLayout = Layout(device, {nvrhi::BindingLayoutItem::ConstantBuffer(0)});
    auto pipeline = Pipeline(device, shaderFolder + "/CmQ2TAAU.comp.spv", {imageLayout, uniformLayout});

    nvrhi::TextureDesc desc;
    desc.width = desc.height = 32;
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    auto current = device->createTexture(desc);
    desc.format = nvrhi::Format::RG16_FLOAT;
    auto motion = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    auto previous = device->createTexture(desc);
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto history = device->createTexture(desc);
    desc.format = nvrhi::Format::R11G11B10_FLOAT;
    auto output = device->createTexture(desc);
    Require(current && motion && previous && history && output, "create TAAU reset textures");

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    auto sampler = device->createSampler(samplerDesc);
    nvrhi::BindingSetDesc images;
    images.addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_UPSCALED_PING), output));
    images.addItem(nvrhi::BindingSetItem::Texture_UAV(UavBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY), history));
    images.addItem(nvrhi::BindingSetItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_FINAL), current));
    images.addItem(nvrhi::BindingSetItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_MOTION_DLSS), motion));
    images.addItem(nvrhi::BindingSetItem::Texture_SRV(SrvBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY_PREV), previous));
    images.addItem(nvrhi::BindingSetItem::Sampler(SamplerBinding(FB_IMAGE_INDEX_Q2_TAA_HISTORY_PREV), sampler));
    auto imageSet = device->createBindingSet(images, imageLayout);

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(ShGlobalUniform);
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    auto uniform = device->createBuffer(bufferDesc);
    nvrhi::BindingSetDesc uniforms;
    uniforms.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, uniform));
    auto uniformSet = device->createBindingSet(uniforms, uniformLayout);
    Require(imageSet && uniformSet, "create TAAU reset binding sets");

    auto pixels = Image(32, 32, 0.1f);
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            for (uint32_t channel = 0; channel < 3; channel++)
                pixels[(y * 32 + x) * 4 + channel] = (x + y) % 2 ? 0.9f : 0.1f;

    float values[2] = {};
    for (uint32_t reset = 0; reset < 2; reset++)
    {
        ShGlobalUniform frame = {};
        frame.renderWidth = frame.renderHeight = 32.0f;
        frame.upscaledRenderWidth = frame.upscaledRenderHeight = 32.0f;
        frame.restirParams[2] = reset;
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->writeTexture(current, 0, 0, pixels.data(), 32 * sizeof(float) * 4);
        cmd->clearTextureFloat(motion, nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, 0.f, 0.f));
        cmd->clearTextureFloat(previous, nvrhi::AllSubresources, nvrhi::Color(0.5f, 0.5f, 0.5f, 1.f));
        cmd->writeBuffer(uniform, &frame, sizeof(frame));
        nvrhi::ComputeState state;
        state.setPipeline(pipeline);
        state.addBindingSet(imageSet);
        state.addBindingSet(uniformSet);
        cmd->setComputeState(state);
        cmd->dispatch(2, 2);
        cmd->close();
        device->executeCommandList(cmd);
        const auto result = ReadTexture(device, output);
        values[reset] = result[16 * 32 + 16];
    }

    Require(values[0] > 0.4f, "TAAU normally reuses valid history");
    Require(std::abs(values[1] - 0.1f) < 0.005f, "UI-to-scene reset rejects previous TAAU history");
}

}

int main(int argc, char **argv)
{
    try
    {
        Require(argc == 3, "usage: qray_posteffects_smoke shader-folder probe-folder");
        VulkanTestContext gpu;
        auto device = gpu.context.GetDevice();
        {
            ExposureProbe probe(device, argv[1]);
            for (auto size : {std::array<uint32_t, 2>{17, 9}, {1, 1}, {127, 7}})
            {
                auto pixels = Image(size[0], size[1], 0.25f);
                auto tm = probe.Run(size[0], size[1], pixels, true, 1.0f / 60);
                Require(std::abs(tm.avgLuminance - 0.25f) < 0.015f, "constant histogram exposure");
                tm = probe.Run(size[0], size[1], pixels, true, 1.0f / 60, 70.0f, 90.0f);
                Require(std::abs(tm.avgLuminance - 0.25f) < 0.015f, "default percentiles on tiny views");
            }
            float at30 = 0.0f;
            float at60 = 0.0f;
            for (int fps : {30, 60})
            {
                auto pixels = Image(17, 9, 0.25f);
                probe.Run(17, 9, pixels, true, 1.0f / fps);
                pixels = Image(17, 9, 1.0f);
                ShTonemapping tm{};
                for (int i = 0; i < fps; i++) tm = probe.Run(17, 9, pixels, false, 1.0f / fps);
                Require(std::abs(tm.avgLuminance - std::exp2(-2.0f * std::exp(-3.0f))) < 0.02f,
                    "exposure adapts in EV over consecutive slots");
                if (fps == 30) at30 = tm.avgLuminance; else at60 = tm.avgLuminance;
            }
            Require(std::abs(at30 - at60) < 0.002f, "frame-rate independent exposure");
            auto darkPixels = Image(17, 9, 1.0f);
            probe.Run(17, 9, darkPixels, true, 1.0f / 60);
            darkPixels = Image(17, 9, 0.0625f);
            ShTonemapping dark{};
            for (int i = 0; i < 60; i++) dark = probe.Run(17, 9, darkPixels, false, 1.0f / 60);
            Require(std::abs(dark.avgLuminance - std::exp2(-4.0f * (1.0f - std::exp(-1.0f)))) < 0.015f,
                "slower adaptation into dark scene");
            auto reset = probe.Run(17, 9, darkPixels, true, 1.0f / 60);
            Require(std::abs(reset.avgLuminance - 0.0625f) < 0.005f, "camera-cut exposure reset");
            darkPixels = Image(17, 9, 1.0f);
            auto paused = probe.Run(17, 9, darkPixels, false, 0.0f);
            Require(std::abs(paused.avgLuminance - reset.avgLuminance) < 0.0001f, "zero-delta exposure freezes");
            auto pixels = Image(17, 9, 0.0f);
            probe.Run(17, 9, pixels, true, 1.0f / 60);
            pixels[0] = std::numeric_limits<float>::quiet_NaN();
            pixels[4] = std::numeric_limits<float>::infinity();
            probe.Run(17, 9, pixels, true, 1.0f / 60);
            pixels = Image(127, 7, 0.25f);
            for (size_t i = 0; i < 30; i++)
                for (size_t channel = 0; channel < 3; channel++) pixels[i * 4 + channel] = 100.0f;
            auto tm = probe.Run(127, 7, pixels, true, 1.0f / 60, 10.0f, 80.0f);
            Require(std::abs(tm.avgLuminance - 0.25f) < 0.015f, "percentiles reject bright outliers");
            CheckVignette(device, argv[1]);
            CheckFilmGrain(device, argv[1]);
            CheckColorCompositing(device, argv[2]);
            CheckGameplayColor(device, argv[1]);
            CheckNearDof(device, argv[2]);
            CheckLocalExposure(device, argv[2]);
            CheckTaauHistoryReset(device, argv[1]);
        }
        Require(device->waitForIdle(), "finish GPU tests");
        Require(gpu.errors.load() == 0, "Vulkan validation errors");
        std::cout << "PASS: histogram, adaptation, local exposure, vignette, film grain, color compositing, gameplay tint, near weapon DOF and TAAU history reset\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
