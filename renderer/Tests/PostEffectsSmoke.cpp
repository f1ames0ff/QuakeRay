#include "VulkanTestContext.h"
#include "RHI/RhiPipeline.h"
#include "RHI/RhiExposureHistory.h"
#include "Generated/ShaderCommonC.h"
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>

using namespace qray;

namespace
{

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

class ExposureProbe
{
public:
    ExposureProbe(nvrhi::IDevice *dev, const std::string &shaders) : device(dev)
    {
        histogramLayout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(151)});
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
            nvrhi::BindingSetItem::Texture_SRV(151, input)), histogramLayout);
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
    auto images = Layout(device, {nvrhi::BindingLayoutItem::Texture_UAV(29),
        nvrhi::BindingLayoutItem::Texture_UAV(30)});
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
            .addItem(nvrhi::BindingSetItem::Texture_UAV(29, ping))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(30, pong)), images);
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

void CheckStreak(nvrhi::IDevice *device, const std::string &shaders)
{
    auto sourceLayout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(1)});
    auto highlightsLayout = Layout(device, {nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1)});
    auto exposureLayout = Layout(device, {nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)});
    auto destinationLayout = Layout(device, {nvrhi::BindingLayoutItem::Texture_UAV(0)});
    auto pushLayout = Layout(device, {nvrhi::BindingLayoutItem::PushConstants(0, 16)});
    auto pipeline = Pipeline(device, shaders + "/CmLensFlare.comp.spv",
        {sourceLayout, highlightsLayout, exposureLayout, destinationLayout, pushLayout});
    nvrhi::TextureDesc desc;
    desc.width = desc.height = 32;
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    auto source = device->createTexture(desc);
    auto bright = device->createTexture(desc);
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto output = device->createTexture(desc);
    auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = bufferDesc.structStride = sizeof(ShTonemapping);
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;
    auto exposure = device->createBuffer(bufferDesc);
    auto sourceSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
        .addItem(nvrhi::BindingSetItem::Sampler(1, sampler)), sourceLayout);
    auto highlightsSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, bright)), highlightsLayout);
    auto exposureSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, exposure)), exposureLayout);
    auto destinationSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
        nvrhi::BindingSetItem::Texture_UAV(0, output)), destinationLayout);
    auto pixels = Image(32, 32, 0.0f);
    auto impulse = pixels;
    for (size_t channel = 0; channel < 3; channel++) impulse[(16 * 32 + 16) * 4 + channel] = 100.0f;
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->writeTexture(source, 0, 0, pixels.data(), 32 * sizeof(float) * 4);
    cmd->writeTexture(bright, 0, 0, impulse.data(), 32 * sizeof(float) * 4);
    ShTonemapping tm{};
    cmd->writeBuffer(exposure, &tm, sizeof(tm));
    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.addBindingSet(sourceSet).addBindingSet(highlightsSet).addBindingSet(exposureSet).addBindingSet(destinationSet);
    cmd->setComputeState(state);
    const uint32_t push[4] = {4, 0, 0, 0};
    cmd->setPushConstants(push, sizeof(push));
    cmd->dispatch(2, 2);
    cmd->close();
    device->executeCommandList(cmd);
    auto values = ReadTexture(device, output);
    Require(values[16 * 32 + 12] > 0.07f && values[16 * 32 + 20] > 0.07f,
        "streak spreads horizontally");
    Require(values[12 * 32 + 16] == 0.0f && values[20 * 32 + 16] == 0.0f,
        "streak does not spread vertically");
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
            CheckStreak(device, argv[1]);
            CheckLocalExposure(device, argv[2]);
        }
        Require(device->waitForIdle(), "finish GPU tests");
        Require(gpu.errors.load() == 0, "Vulkan validation errors");
        std::cout << "PASS: histogram, adaptation, local exposure, vignette and streak\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
