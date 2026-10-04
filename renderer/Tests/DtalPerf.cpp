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
#include "Generated/ShaderCommonC.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct PerfCase
{
    const char *name;
    uint32_t threads;
    uint32_t lights;
    uint32_t samplesPerThread;
};

struct CaseResult
{
    std::string name;
    double milliseconds;
};

const PerfCase kCases[] =
{
    { "small",  16384,   256,   256 },
    { "medium", 262144,  4096,  128 },
    { "large",  524288,  16384, 256 },
    { "huge",   1048576, 65536, 256 },
};

qray::ShLightEncoded MakeLight()
{
    qray::ShLightEncoded light = {};
    light.lightType = LIGHT_TYPE_TEXTURED_AREA;

    for (int i = 0; i < 3; i++)
    {
        light.color[i] = 1.0f;
        light.data_7[i] = (i == 2) ? 1.0f : 0.0f;
    }

    light.data_0[0] = 0.0f; light.data_0[1] = 0.0f; light.data_0[2] = 1.0f; light.data_0[3] = 0.0f;
    light.data_1[0] = 1.0f; light.data_1[1] = 0.0f; light.data_1[2] = 1.0f; light.data_1[3] = 1.0f;
    light.data_2[0] = 1.0f; light.data_2[1] = 1.0f; light.data_2[2] = 1.0f; light.data_2[3] = 8.0f;

    float *const uvSlots[4] = { light.data_3, light.data_4, light.data_5, light.data_6 };

    for (int i = 0; i < 8; i++)
    {
        const float angle = float(i) / 8.0f * 6.28318530718f;
        float *slot = uvSlots[i >> 1] + (i & 1) * 2;
        slot[0] = 0.5f + 0.4f * std::cos(angle);
        slot[1] = 0.5f + 0.4f * std::sin(angle);
    }

    light.data_7[3] = 1.0f;
    return light;
}

double RunCase(VulkanTestContext &gpu, nvrhi::IShader *shader, const PerfCase &perfCase, float &checksum)
{
    nvrhi::IDevice *device = gpu.context.GetDevice();

    const uint32_t stride = uint32_t(sizeof(qray::ShLightEncoded));
    const std::vector<qray::ShLightEncoded> lights(perfCase.lights, MakeLight());
    const std::vector<uint8_t> zeros(4096);
    const uint32_t params[4] = { perfCase.lights, perfCase.samplesPerThread, 1u, 0u };

    nvrhi::BufferDesc desc;
    desc.byteSize = 4096;
    desc.isConstantBuffer = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto uniformBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = 16;
    desc.isConstantBuffer = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto paramsBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = uint64_t(perfCase.lights) * stride;
    desc.structStride = stride;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto lightsBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = uint64_t(perfCase.threads) * sizeof(float);
    desc.structStride = sizeof(float);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    auto outputBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = sizeof(float);
    desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto readbackBuffer = device->createBuffer(desc);

    Require(uniformBuffer != nullptr && paramsBuffer != nullptr && lightsBuffer != nullptr &&
            outputBuffer != nullptr && readbackBuffer != nullptr, "create perf buffers");

    nvrhi::BufferDesc dummyDesc;
    dummyDesc.byteSize = 64;
    dummyDesc.structStride = 4;
    dummyDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    dummyDesc.keepInitialState = true;

    std::vector<nvrhi::BufferHandle> lightDummies;

    for (int i = 0; i < 7; i++)
    {
        lightDummies.push_back(device->createBuffer(dummyDesc));
        Require(lightDummies.back() != nullptr, "create perf light dummy");
    }

    dummyDesc.canHaveUAVs = true;
    dummyDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto lightStatsDummy = device->createBuffer(dummyDesc);
    Require(lightStatsDummy != nullptr, "create perf light stats dummy");

    nvrhi::VulkanBindingOffsets zeroOffsets;
    zeroOffsets.shaderResource = 0;
    zeroOffsets.sampler = 0;
    zeroOffsets.constantBuffer = 0;
    zeroOffsets.unorderedAccess = 0;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    auto uniformLayout = device->createBindingLayout(layoutDesc);
    auto uniformSet = device->createBindingSet(
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, uniformBuffer)), uniformLayout);

    layoutDesc = {};
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    auto lightLayout = device->createBindingLayout(layoutDesc);
    auto lightSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, lightsBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, lightDummies[0]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, lightDummies[1]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, lightDummies[2]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightDummies[3]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightDummies[4]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(6, lightStatsDummy))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, lightDummies[5]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, lightDummies[6])), lightLayout);

    layoutDesc = {};
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    auto probeLayout = device->createBindingLayout(layoutDesc);
    auto probeSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, paramsBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, outputBuffer)), probeLayout);

    Require(uniformLayout != nullptr && uniformSet != nullptr && lightLayout != nullptr && lightSet != nullptr &&
            probeLayout != nullptr && probeSet != nullptr, "create perf bindings");

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = shader;
    pipelineDesc.bindingLayouts.push_back(uniformLayout.Get());
    pipelineDesc.bindingLayouts.push_back(lightLayout.Get());
    pipelineDesc.bindingLayouts.push_back(probeLayout.Get());
    auto pipeline = device->createComputePipeline(pipelineDesc);
    Require(pipeline != nullptr, "create perf pipeline");

    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings.push_back(uniformSet.Get());
    state.bindings.push_back(lightSet.Get());
    state.bindings.push_back(probeSet.Get());

    {
        auto setup = device->createCommandList();
        setup->open();
        setup->writeBuffer(uniformBuffer, zeros.data(), zeros.size());
        setup->writeBuffer(paramsBuffer, params, sizeof(params));
        setup->writeBuffer(lightsBuffer, lights.data(), lights.size() * stride);
        setup->close();
        device->executeCommandList(setup);
        device->waitForIdle();
    }

    const uint32_t groups = (perfCase.threads + 63) / 64;
    std::vector<double> timings;

    for (int iteration = 0; iteration < 4; iteration++)
    {
        auto timer = device->createTimerQuery();
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->setComputeState(state);
        cmd->beginTimerQuery(timer);
        cmd->dispatch(groups, 1, 1);
        cmd->endTimerQuery(timer);
        cmd->copyBuffer(readbackBuffer, 0, outputBuffer, 0, sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();

        if (iteration > 0)
        {
            timings.push_back(device->getTimerQueryTime(timer) * 1000.0);
        }
    }

    std::sort(timings.begin(), timings.end());

    const void *data = device->mapBuffer(readbackBuffer, nvrhi::CpuAccessMode::Read);
    Require(data != nullptr, "map perf readback");
    std::memcpy(&checksum, data, sizeof(float));
    device->unmapBuffer(readbackBuffer);

    return timings[timings.size() / 2];
}

void WriteBaseline(const std::filesystem::path &path, const std::vector<CaseResult> &results)
{
    std::ofstream file(path);
    Require(file.is_open(), "open baseline for writing: " + path.string());
    file << "# dtal perf baseline: case milliseconds\n";
    for (const CaseResult &result : results)
    {
        file << result.name << ' ' << result.milliseconds << '\n';
    }
}

std::map<std::string, double> ReadBaseline(const std::filesystem::path &path)
{
    std::ifstream file(path);
    Require(file.is_open(), "open baseline for reading: " + path.string());

    std::map<std::string, double> baseline;
    std::string line;

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        double milliseconds = 0.0;

        if (stream >> name >> milliseconds)
        {
            baseline[name] = milliseconds;
        }
    }

    return baseline;
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::string shaderPath = argc > 1 ? argv[1] : "DtalPerf.comp.spv";
        std::filesystem::path baselinePath = argc > 2 ? argv[2] : "DtalPerf.baseline.txt";
        bool update = false;
        double tolerance = 0.2;

        for (int i = 1; i < argc; i++)
        {
            const std::string argument = argv[i];
            if (argument == "--update")
            {
                update = true;
            }
            else if (argument == "--tolerance" && i + 1 < argc)
            {
                tolerance = std::stod(argv[++i]);
            }
        }

        VulkanTestContext gpu;
        nvrhi::IDevice *device = gpu.context.GetDevice();
        auto shader = qray::rhi::loadShader(device, shaderPath, nvrhi::ShaderType::Compute, "Dtal perf probe");
        Require(shader != nullptr, "load " + shaderPath);

        std::vector<CaseResult> results;

        for (const PerfCase &perfCase : kCases)
        {
            float checksum = 0.0f;
            const double milliseconds = RunCase(gpu, shader, perfCase, checksum);
            Require(std::isfinite(checksum), "non-finite perf checksum");
            std::cout << "dtal " << perfCase.name << ": " << milliseconds << " ms ("
                      << perfCase.threads << " threads, " << perfCase.lights << " lights, "
                      << perfCase.samplesPerThread << " samples/thread, "
                      << double(perfCase.threads) * perfCase.samplesPerThread / 1e6 << "M samples)\n";
            results.push_back({ perfCase.name, milliseconds });
        }

        Require(gpu.errors == 0, "Vulkan/NVRHI validation errors=" + std::to_string(gpu.errors.load()));

        if (update || !std::filesystem::exists(baselinePath))
        {
            WriteBaseline(baselinePath, results);
            std::cout << "baseline written to " << baselinePath.string() << '\n';
            std::cout << "Dtal perf passed (baseline)\n";
            return 0;
        }

        const std::map<std::string, double> baseline = ReadBaseline(baselinePath);
        bool failed = false;

        for (const CaseResult &result : results)
        {
            const auto it = baseline.find(result.name);
            if (it == baseline.end())
            {
                std::cerr << "baseline has no case '" << result.name << "'\n";
                failed = true;
                continue;
            }

            const double ratio = result.milliseconds / it->second;
            const bool regression = ratio > 1.0 + tolerance;
            std::cout << "dtal " << result.name << ": " << result.milliseconds << " ms vs baseline "
                      << it->second << " ms (" << (ratio * 100.0 - 100.0) << "%)"
                      << (regression ? " REGRESSION" : "") << '\n';

            if (regression)
            {
                failed = true;
            }
        }

        if (failed)
        {
            std::cerr << "Dtal perf failed; run with --update to accept the new numbers\n";
            return 1;
        }

        std::cout << "Dtal perf passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
