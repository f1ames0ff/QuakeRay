#include "VulkanTestContext.h"
#include "RHI/RhiCloudsPass.h"
#include "RHI/RhiProceduralSkyPass.h"
#include "RHI/RhiCloudShadowBinding.h"
#include "RHI/RhiFrameContext.h"
#include "RHI/RhiPipeline.h"
#include "Generated/ShaderCommonC.h"
#include "Matrix.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

float Half(uint16_t bits)
{
    const unsigned exponent = (bits >> 10) & 31;
    float value = exponent == 0 ? std::ldexp(float(bits & 1023), -24)
        : exponent == 31 ? std::bit_cast<float>(uint32_t(0x7f800000 | ((bits & 1023) << 13)))
        : std::ldexp(1.0f + float(bits & 1023) / 1024.0f, int(exponent) - 15);
    return bits & 0x8000 ? -value : value;
}

void FillBasis(qray::RhiProceduralSkyPass::Params &p)
{
    constexpr float pi = 3.14159265358979323846f;
    const float angles[6][2] = {{0,pi/2},{0,-pi/2},{-pi/2,0},{pi/2,0},{0,0},{0,pi}};
    const float origin[3] = {};
    for (int f = 0; f < 6; ++f)
    {
        float view[16];
        qray::Matrix::GetViewMatrix(view, origin, angles[f][0], angles[f][1], 0);
        for (int b = 0; b < 3; ++b)
            for (int axis = 0; axis < 3; ++axis) p.faceBasis[f * 3 + b][axis] = view[b + axis * 4];
    }
}

std::vector<float> ReadCube(nvrhi::IDevice *device, nvrhi::ITexture *texture, uint32_t mip = 0, uint32_t crop = 0)
{
    auto desc = texture->getDesc();
    const uint32_t fullWidth = std::max(desc.width >> mip, 1u);
    const uint32_t fullHeight = std::max(desc.height >> mip, 1u);
    const uint32_t width = crop ? std::min(crop, fullWidth) : fullWidth;
    const uint32_t height = crop ? std::min(crop, fullHeight) : fullHeight;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = 1;
    desc.isUAV = false;
    desc.isRenderTarget = false;
    desc.keepInitialState = false;
    auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    Require(staging != nullptr, "create cube readback");
    auto cmd = device->createCommandList();
    cmd->open();
    for (uint32_t f = 0; f < 6; ++f)
    {
        auto to = nvrhi::TextureSlice().setArraySlice(f);
        auto from = nvrhi::TextureSlice().setArraySlice(f).setMipLevel(mip)
            .setOrigin((fullWidth - width) / 2, (fullHeight - height) / 2).setSize(width, height, 1);
        cmd->copyTexture(staging, to, texture, from);
    }
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    std::vector<float> values(size_t(width) * height * 6 * 4);
    for (uint32_t f = 0; f < 6; ++f)
    {
        size_t pitch = 0;
        const auto data = static_cast<const uint8_t *>(device->mapStagingTexture(staging,
            nvrhi::TextureSlice().setArraySlice(f), nvrhi::CpuAccessMode::Read, &pitch));
        Require(data != nullptr, "map cube readback");
        for (uint32_t y = 0; y < height; ++y)
        {
            const auto row = reinterpret_cast<const uint16_t *>(data + y * pitch);
            for (uint32_t x = 0; x < width * 4; ++x)
                values[(size_t(f) * height + y) * width * 4 + x] = Half(row[x]);
        }
        device->unmapStagingTexture(staging);
    }
    return values;
}

void CheckClear(const std::vector<float> &values, const float color[4])
{
    for (size_t i = 0; i < values.size(); ++i)
        Require(std::isfinite(values[i]) && std::abs(values[i] - (i % 4 == 3 ? 1.0f : color[i % 4])) < 0.001f,
                "clear sky has an incomplete face or mip");
}

void CheckLayer(const std::vector<float> &values)
{
    for (float v : values) Require(std::isfinite(v), "non-finite cloud layer");
    for (size_t i = values.size() * 5 / 6 + 3; i < values.size(); i += 4)
        Require(values[i] == 1.0f, "lower cloud face was not written transparent");
}

float SampleTopFace(const std::vector<float> &image, float x, float y)
{
    constexpr uint32_t side = qray::RhiProceduralSkyPass::CUBEMAP_SIZE;
    const uint32_t x0 = uint32_t(std::floor(x));
    const uint32_t y0 = uint32_t(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;
    auto at = [&](uint32_t px, uint32_t py)
    {
        return image[(size_t(4) * side * side + size_t(py) * side + px) * 4];
    };
    const float top = std::lerp(at(x0, y0), at(x0 + 1, y0), fx);
    const float bottom = std::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx);
    return std::lerp(top, bottom, fy);
}

std::array<float, 3> PickShadowColumn(nvrhi::IDevice *device, nvrhi::ITexture *texture, const float *placement)
{
    auto desc = texture->getDesc();
    const uint32_t side = desc.width;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.depth = 1;
    desc.isUAV = false;
    desc.keepInitialState = false;
    auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    Require(staging != nullptr, "create shadow readback");
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice().setSize(side, side, 1));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    size_t pitch = 0;
    const auto data = static_cast<const uint8_t *>(device->mapStagingTexture(staging,
        nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    Require(data != nullptr, "map shadow readback");
    for (uint32_t y = side / 10; y < side * 9 / 10; ++y)
    {
        const auto row = reinterpret_cast<const uint16_t *>(data + y * pitch);
        for (uint32_t x = side / 10; x < side * 9 / 10; ++x)
        {
            const float tau = Half(row[x]);
            if (tau > 0.5f && tau < 2.0f)
            {
                std::array<float, 3> result = {placement[1] + (float(x) + 0.5f) / side * placement[3],
                    placement[2] + (float(y) + 0.5f) / side * placement[3], tau};
                device->unmapStagingTexture(staging);
                return result;
            }
        }
    }
    device->unmapStagingTexture(staging);
    throw std::runtime_error("shadow volume has no cloud optical depth");
}

std::array<float, 16> Probe(nvrhi::IDevice *device, qray::rhi::RhiFrameContext &frames,
    qray::RhiCloudsPass &clouds, qray::RhiProceduralSkyPass &sky,
    const qray::ShGlobalUniform &uniform, const std::string &shaderPath, bool withPush)
{
    qray::rhi::RhiCloudShadowBinding shadow;
    Require(shadow.Create(device, &frames, nvrhi::ShaderType::Compute, withPush ? 4 : 0), "create shadow sampling layout");
    Require(shadow.SetTexture(clouds.GetShadowTexture(), clouds.GetShadowSampler()), "bind shadow volume");
    auto shader = qray::rhi::loadShader(device, shaderPath, nvrhi::ShaderType::Compute, "Clouds probe");
    Require(shader != nullptr, "load clouds probe");
    nvrhi::BufferDesc desc;
    desc.byteSize = sizeof(uniform);
    desc.isConstantBuffer = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto params = device->createBuffer(desc);
    desc = {};
    desc.byteSize = 64;
    desc.structStride = 16;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    auto output = device->createBuffer(desc);
    desc.structStride = 0;
    desc.canHaveUAVs = false;
    desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    auto readback = device->createBuffer(desc);
    Require(params != nullptr && output != nullptr && readback != nullptr, "create probe buffers");

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    auto emptyLayout = device->createBindingLayout(layoutDesc);
    auto empty = device->createBindingSet(nvrhi::BindingSetDesc(), emptyLayout);
    layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    auto outputLayout = device->createBindingLayout(layoutDesc);
    auto outputSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output)), outputLayout);
    layoutDesc.bindings.clear();
    layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    auto paramsLayout = device->createBindingLayout(layoutDesc);
    auto paramsSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(
        nvrhi::BindingSetItem::ConstantBuffer(0, params)), paramsLayout);
    layoutDesc.bindings.clear();
    layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0).setSamplerOffset(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(2));
    auto skyLayout = device->createBindingLayout(layoutDesc);
    auto skySet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, sky.GetCubemapTexture()))
        .addItem(nvrhi::BindingSetItem::Sampler(2, sky.GetCubemapSampler())), skyLayout);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = shader;
    nvrhi::ComputeState state;
    const uint32_t count = withPush ? 9 : 13;
    const uint32_t shadowSlot = withPush ? 5 : 12;
    for (uint32_t i = 0; i < count; ++i)
    {
        pipelineDesc.bindingLayouts.push_back(i == 0 ? outputLayout.Get() : i == 2 ? paramsLayout.Get()
            : i == 8 ? skyLayout.Get() : i == shadowSlot ? shadow.GetLayout() : emptyLayout.Get());
        state.bindings.push_back(i == 0 ? outputSet.Get() : i == 2 ? paramsSet.Get()
            : i == 8 ? skySet.Get() : i == shadowSlot ? shadow.GetSet() : empty.Get());
    }
    auto pipeline = device->createComputePipeline(pipelineDesc);
    Require(pipeline != nullptr, "create cloud probe pipeline");
    state.pipeline = pipeline;
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->writeBuffer(params, &uniform, sizeof(uniform));
    cmd->setComputeState(state);
    const uint32_t index = 2;
    if (withPush) cmd->setPushConstants(&index, sizeof(index));
    cmd->dispatch(1, 1, 1);
    cmd->copyBuffer(readback, 0, output, 0, 64);
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    auto data = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
    Require(data != nullptr, "map probe result");
    std::array<float, 16> result;
    std::memcpy(result.data(), data, 64);
    device->unmapBuffer(readback);
    for (float v : result) Require(std::isfinite(v), "non-finite motion or shadow lookup");
    return result;
}

int main(int argc, char **argv)
{
    try
    {
        Require(argc >= 3, "usage: qray_clouds_smoke shader-folder probe-folder [capture-folder]");
        VulkanTestContext gpu;
        {
            auto device = gpu.context.GetDevice();
            qray::rhi::RhiFrameContext frames;
            Require(frames.Create(device, 2), "create frame context");
            auto print = [](const char *s) { std::cerr << s << '\n'; };
            const std::string shaders = std::filesystem::absolute(argv[1]).string() + "/";
            const std::string probes = std::filesystem::absolute(argv[2]).string() + "/";
            qray::RhiCloudsPass clouds;
            Require(clouds.Create(device, &frames, shaders.c_str(), print), "create cloud passes");
            qray::RhiProceduralSkyPass sky;
            Require(sky.Create(device, &frames, shaders.c_str(), clouds.GetLayerTexture(), clouds.GetLayerSampler(), print), "create sky pass");
            qray::RhiProceduralSkyPass::Params p{};
            FillBasis(p);
            p.skyTint[0] = 0.125f;
            p.skyTint[1] = 0.25f;
            p.skyTint[2] = 0.5f;
            p.skyParams[0] = 1;
            p.sunDirection[2] = 1;
            frames.BeginSlot(0);
            sky.Render(frames.GetCommandList(0), 0, p);
            frames.EndSlot(0);
            CheckClear(ReadCube(device, sky.GetCubemapTexture()), p.skyTint);
            for (uint32_t mip = 0; mip <= 10; ++mip)
                CheckClear(ReadCube(device, sky.GetEnvironmentTexture(), mip), p.skyTint);
            std::cout << "Clear sky: all 6 faces and 11 mips passed\n";

            p.skyParams[1] = 1;
            p.skyParams[2] = 6;
            p.skyParams[3] = 0.025f;
            p.cloudColor[0] = p.cloudColor[1] = p.cloudColor[2] = 1;
            p.cloudParams[0] = 0.4f;
            p.cloudParams[1] = 0.8f;
            p.cloudParams[2] = 1;
            p.cloudParams[3] = 1;
            p.sunDiscColor[0] = p.sunDiscColor[1] = p.sunDiscColor[2] = 1;
            p.sunDirection[0] = 0.3f;
            p.sunDirection[1] = 0.4f;
            p.sunDirection[2] = std::sqrt(0.75f);
            p.sunDirection[3] = 1;
            qray::RhiCloudsPass::LayerParams layer{};
            std::memcpy(&layer, &p, sizeof(p));
            layer.cloudLayer[0] = 140000;
            layer.cloudLayer[1] = 90000;
            layer.cloudLayer[2] = layer.cloudLayer[3] = 1;
            layer.cloudMarch[0] = 48;
            layer.cloudMarch[2] = 0.35f;
            layer.cloudMarch[3] = 0.75f;
            layer.cloudAnchor[0] = 123;
            layer.cloudAnchor[1] = 456;
            layer.cloudAnchor[2] = 78;
            qray::RhiCloudsPass::ShadowParams shadow{};
            std::memcpy(shadow.sunDirection, p.sunDirection, sizeof(shadow.sunDirection));
            shadow.sunDirection[3] = layer.cloudLayer[0];
            shadow.cloudLayer[0] = layer.cloudLayer[1];
            shadow.cloudLayer[1] = p.cloudParams[0];
            shadow.cloudLayer[2] = p.cloudParams[1];
            shadow.cloudLayer[3] = layer.cloudMarch[2];
            shadow.cloudMarch[1] = layer.cloudParams[2];
            auto render = [&](uint32_t quality, uint32_t slot)
            {
                frames.BeginSlot(slot);
                bool changed = clouds.Render(frames.GetCommandList(slot), slot, layer, shadow, quality);
                Require(sky.SetCloudLayer(clouds.GetLayerTexture(), clouds.GetLayerSampler()), "rebind resized layer");
                sky.Render(frames.GetCommandList(slot), slot, p, changed);
                frames.EndSlot(slot);
                return changed;
            };
            Require(render(2, 1), "first cloud frame must render");
            auto values = ReadCube(device, clouds.GetLayerTexture());
            CheckLayer(values);
            size_t covered = 0;
            for (size_t i = 3; i < values.size(); i += 4) if (values[i] < 0.95f) ++covered;
            Require(covered > 1000 && covered < 3 * 1024 * 1024, "clouds are empty or extend below the horizon");
            std::cout << "Clouds: " << covered << " covered texels; lower face transparent\n";
            if (argc > 3)
            {
                std::filesystem::create_directories(argv[3]);
                auto image = ReadCube(device, sky.GetCubemapTexture());
                std::ofstream out(std::string(argv[3]) + "/sky.bin", std::ios::binary);
                out.write(reinterpret_cast<const char *>(image.data()), image.size() * sizeof(float));
            }
            Require(!render(2, 0), "identical cloud parameters must reuse the result");
            layer.cloudParams[2] = shadow.cloudMarch[1] = p.cloudParams[2] = 0;
            Require(render(2, 1), "stopping the wind must update the layer");
            layer.cloudColor[3] = p.cloudColor[3] = shadow.cloudMarch[0] = 30;
            Require(!render(2, 0), "a stopped wind must ignore the clock");
            layer.cloudAnchor[0] += 1000;
            Require(render(2, 1), "camera translation must update a stationary layer");
            std::cout << "Stationary cache, clock freeze and camera translation passed\n";

            const uint32_t qualities[] = {1, 3, 4, 99, 2};
            const uint32_t sizes[] = {512, 2048, 2048, 2048, 1024};
            for (uint32_t i = 0; i < 5; ++i)
            {
                const bool changed = render(qualities[i], i % 2);
                Require(changed || qualities[i] > QR_SKY_CLOUDS_MAX_QUALITY, "quality change must render");
                Require(clouds.GetLayerTexture()->getDesc().width == sizes[i], "quality did not change layer resolution");
                Require(clouds.GetShadowTexture()->getDesc().width == (qualities[i] >= 3 ? 2048u : 1024u), "quality did not change shadow resolution");
                CheckLayer(ReadCube(device, clouds.GetLayerTexture(), 0, 16));
                std::cout << "Quality " << std::min(qualities[i], uint32_t(QR_SKY_CLOUDS_MAX_QUALITY))
                          << " (requested " << qualities[i] << "): " << sizes[i] << " pixels, fresh sky binding passed\n";
            }

            qray::ShGlobalUniform uniform{};
            uniform.skyType = SKY_TYPE_PROCEDURAL;
            uniform.worldUpVector[2] = 1;
            const auto placement = qray::RhiCloudsPass::MakeShadowPlacement(layer);
            std::memcpy(uniform.cloudShadowPlacement, placement.data(), 16);
            std::memcpy(uniform.cameraPosition, layer.cloudAnchor, 12);
            const auto column = PickShadowColumn(device, clouds.GetShadowTexture(), placement.data());
            const float baseHeight = uniform.cameraPosition[2] + layer.cloudLayer[0];
            uniform.cameraPosition[0] = column[0] + p.sunDirection[0] * baseHeight / p.sunDirection[2];
            uniform.cameraPosition[1] = column[1] + p.sunDirection[1] * baseHeight / p.sunDirection[2];
            std::memcpy(uniform.cameraPositionPrev, uniform.cameraPosition, 12);
            uniform.cloudLayerMotion[0] = 1000;
            uniform.cloudLayerMotion[1] = layer.cloudLayer[0];
            uniform.cloudLayerMotion[2] = layer.cloudLayer[1];
            uniform.cloudLayerMotion[3] = 1;
            uniform.timeDelta = 0.02f;
            uniform.view[0] = uniform.view[5] = uniform.view[10] = uniform.view[15] = 1;
            std::memcpy(uniform.viewPrev, uniform.view, 64);
            uniform.projection[0] = uniform.projection[5] = uniform.projection[10] = uniform.projection[11] = 1;
            std::memcpy(uniform.projectionPrev, uniform.projection, 64);
            auto probe = [&](bool push = false)
            {
                return Probe(device, frames, clouds, sky, uniform, probes + (push ? "CloudsPush.comp.spv" : "CloudsProbe.comp.spv"), push);
            };
            auto result = probe();
            const float centre = layer.cloudLayer[0] + 0.3f * layer.cloudLayer[1];
            Require(std::abs(result[0]) < 1e-6 && std::abs(result[1]) < 1e-6, "stationary camera has infinite-sky motion");
            Require(std::abs(result[2] - 0.5f * uniform.timeDelta * 1000 * 30 / centre) < 1e-6 &&
                    std::abs(result[3] - 0.5f * uniform.timeDelta * 1000 * 12 / centre) < 1e-6, "cloud wind motion disagrees with projected displacement");
            Require(result[4] >= 0 && result[4] <= result[5] + 1e-4f && result[5] <= 1 &&
                    std::abs(result[6] - 1) < 1e-4 && std::abs(result[7] - 1) < 1e-4, "shadow height interpolation or map fade is wrong");
            Require(result[4] < 0.98f && std::abs(result[4] - std::exp(-column[2])) < 0.003f,
                    "world sun does not read the baked cloud optical depth");
            std::cout << "Sun shadow below/inside/above layer: " << result[4] << '/' << result[5] << '/' << result[6] << '\n';
            auto pushResult = probe(true);
            Require(pushResult[15] == 2 && std::abs(pushResult[4] - result[4]) < 1e-4f,
                    "god-rays shadow layout lost its volume or push constants");

            const float litVisibility = result[4];
            layer.sunDirection[3] = p.sunDirection[3] = 0;
            layer.cloudParams[1] *= 2;
            p.cloudParams[1] = layer.cloudParams[1];
            shadow.cloudLayer[2] = layer.cloudParams[1];
            Require(render(2, 0), "turning off the sun must update the layer");
            uniform.cloudShadowPlacement[0] = 0;
            Require(probe()[4] == 1, "sunless clouds retain a world shadow");
            layer.sunDirection[3] = p.sunDirection[3] = 1;
            Require(render(2, 1), "turning on the sun must update the layer");
            uniform.cloudShadowPlacement[0] = 1;
            Require(probe()[4] < litVisibility * 0.9f, "sun activation reused a shadow from before the density edit");
            std::cout << "Sun toggle and density edit rebuilt the shadow volume\n";
            uniform.cloudLayerMotion[0] = 0;
            uniform.cameraPositionPrev[0] -= 100;
            uniform.cameraPositionPrev[1] += 50;
            result = probe();
            Require(std::abs(result[2] - 50 / centre) < 1e-6 && std::abs(result[3] + 25 / centre) < 1e-6,
                    "stationary clouds lost camera parallax");
            uniform.cloudLayerMotion[1] = uniform.cloudLayerMotion[2] = 0;
            uniform.cloudShadowPlacement[0] = 0;
            uniform.cloudLayerMotion[0] = 0.3f;
            result = probe();
            Require(std::abs(result[2] - uniform.timeDelta * 0.3f / 2) < 1e-6, "flat-cloud motion disagrees with planar drift");
            uniform.cloudLayerMotion[3] = 0;
            result = probe();
            Require(std::abs(result[2]) < 1e-6 && std::abs(result[3]) < 1e-6 && result[4] == 1 && result[5] == 1,
                    "disabled clouds retain motion or shadow");
            std::cout << "Wind, parallax, flat clouds, disabled shadow and god-rays push constants passed\n";

            std::memcpy(uniform.cameraPositionPrev, uniform.cameraPosition, 12);
            uniform.cloudLayerMotion[3] = 1;
            const float windCases[][3] = {{0.3f, 140000, 90000}, {4, 10000, 5000}, {-0.7f, 1400, 900}};
            const float directions[][3] = {{0, 0, 1}, {0.6f, 0.2f, 1}, {1, 0.5f, 0.2f}};
            for (const auto &setting : windCases)
            {
                const auto wind = qray::RhiCloudsPass::GetWindSpeeds(setting[0], setting[1], setting[2]);
                for (const auto &direction : directions)
                {
                    std::memcpy(uniform.worldUpVector, direction, 12);
                    uniform.cloudLayerMotion[0] = wind.volume;
                    uniform.cloudLayerMotion[1] = setting[1];
                    uniform.cloudLayerMotion[2] = setting[2];
                    const auto volumeMotion = probe();
                    uniform.cloudLayerMotion[0] = wind.flat;
                    uniform.cloudLayerMotion[1] = uniform.cloudLayerMotion[2] = 0;
                    const auto flatMotion = probe();
                    Require(std::abs(volumeMotion[2] - flatMotion[2]) < 1e-6 &&
                            std::abs(volumeMotion[3] - flatMotion[3]) < 1e-6,
                            "flat and volumetric wind speeds disagree under perspective projection");
                }
            }
            std::cout << "Flat/volume wind agreement passed at zenith and grazing angles for three layer scales\n";

            p.skyTint[3] = 1;
            p.cloudParams[3] = 1;
            layer.cloudParams[3] = 1;
            Require(!render(0, 0), "flat quality recorded a volume march");
            const auto flatSky = ReadCube(device, sky.GetEnvironmentTexture(), 0, 16);
            for (size_t i = 0; i < flatSky.size(); ++i)
                Require(std::isfinite(flatSky[i]) && flatSky[i] >= (i % 4 == 3 ? 0.99f : p.skyTint[i % 4] - 0.001f),
                        "flat sky contains an unrendered face");

            auto flatParams = p;
            flatParams.sunDirection[3] = 0;
            flatParams.skyTint[0] = flatParams.skyTint[1] = flatParams.skyTint[2] = 0;
            flatParams.cloudParams[0] = 0.35f;
            flatParams.cloudParams[1] = 0.8f;
            const auto visibleWind = qray::RhiCloudsPass::GetWindSpeeds(4, 140000, 90000);
            flatParams.cloudParams[2] = visibleWind.flat;
            flatParams.cloudColor[3] = 0;
            frames.BeginSlot(1);
            sky.Render(frames.GetCommandList(1), 1, flatParams);
            frames.EndSlot(1);
            const auto first = ReadCube(device, sky.GetCubemapTexture());
            flatParams.cloudColor[3] = 1;
            frames.BeginSlot(0);
            sky.Render(frames.GetCommandList(0), 0, flatParams);
            frames.EndSlot(0);
            const auto second = ReadCube(device, sky.GetCubemapTexture());
            const float centreHeight = 140000 + 0.3f * 90000;
            const float dx = visibleWind.volume * 30 / centreHeight * qray::RhiProceduralSkyPass::CUBEMAP_SIZE / 2;
            const float dy = visibleWind.volume * 12 / centreHeight * qray::RhiProceduralSkyPass::CUBEMAP_SIZE / 2;
            double totalError = 0;
            float maxError = 0;
            size_t samples = 0;
            constexpr uint32_t side = qray::RhiProceduralSkyPass::CUBEMAP_SIZE;
            for (uint32_t y = 128; y < 896; y += 32)
            {
                for (uint32_t x = 128; x < 896; x += 32)
                {
                    const float expected = SampleTopFace(first, float(x) + dx, float(y) - dy);
                    const float actual = second[(size_t(4) * side * side + size_t(y) * side + x) * 4];
                    const float error = std::abs(actual - expected);
                    maxError = std::max(maxError, error);
                    totalError += error;
                    ++samples;
                }
            }
            Require(totalError / samples < 0.01 && maxError < 0.05f,
                    "visible flat-cloud animation does not follow the volumetric wind displacement");
            std::cout << "Flat-mask GPU advection: mean error=" << totalError / samples << "; max=" << maxError << '\n';
            p.skyParams[1] = layer.skyParams[1] = 0;
            Require(!render(2, 1), "zero opacity recorded a volume march");
            CheckClear(ReadCube(device, sky.GetEnvironmentTexture(), 0, 16), p.skyTint);
            p.skyParams[1] = layer.skyParams[1] = 1;
            p.cloudParams[3] = layer.cloudParams[3] = 0;
            Require(!render(2, 0), "disabled clouds recorded a volume march");
            CheckClear(ReadCube(device, sky.GetEnvironmentTexture(), 0, 16), p.skyTint);
            frames.WaitForIdle();
        }
        Require(gpu.errors == 0, "Vulkan/NVRHI validation errors=" + std::to_string(gpu.errors.load()));
        std::cout << "Cloud GPU regression passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
