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

#include "VertexPreprocessing.h"

#include <vector>

#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"

qray::VertexPreprocessing::VertexPreprocessing(
    VkDevice _device,
    const std::shared_ptr<const GlobalUniform> &_uniform,
    const std::shared_ptr<const ASManager> &_asManager,
    const std::shared_ptr<const ShaderManager> &_shaderManager)
    : device(_device)
{
    std::vector<VkDescriptorSetLayout> setLayouts =
    {
        _uniform->GetDescSetLayout(),
        _asManager->GetBuffersDescSetLayout()
    };

    CreatePipelineLayout(setLayouts.data(), setLayouts.size());
    CreatePipelines(_shaderManager.get());
}

qray::VertexPreprocessing::~VertexPreprocessing()
{
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    DestroyPipelines();
}

void qray::VertexPreprocessing::Preprocess(
    VkCommandBuffer cmd, uint32_t frameIndex, uint32_t preprocMode,
    const std::shared_ptr<const GlobalUniform> &uniform,
    const std::shared_ptr<ASManager> &asManager,
    const ShVertPreprocessing &push)
{
    CmdLabel label(cmd, "Vertex preprocessing");

    const bool onlyDynamic = preprocMode == VERT_PREPROC_MODE_ONLY_DYNAMIC;

    asManager->OnVertexPreprocessingBegin(cmd, frameIndex, onlyDynamic);

    VkPipeline pipeline =
        preprocMode == VERT_PREPROC_MODE_ALL ?
            pipelineAll :
        preprocMode == VERT_PREPROC_MODE_DYNAMIC_AND_MOVABLE ?
            pipelineDynamicAndMovable :
            pipelineOnlyDynamic;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    const VkDescriptorSet sets[] =
    {
        uniform->GetDescSet(frameIndex),
        asManager->GetBuffersDescSet(frameIndex)
    };
    const uint32_t setCount = sizeof(sets) / sizeof(sets[0]);

    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
        0, setCount, sets, 0, nullptr);

    vkCmdPushConstants(
        cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(ShVertPreprocessing), &push);

    vkCmdDispatch(cmd, push.tlasInstanceCount, 1, 1);

    asManager->OnVertexPreprocessingFinish(cmd, frameIndex, onlyDynamic);
}

void qray::VertexPreprocessing::OnShaderReload(const ShaderManager *shaderManager)
{
    DestroyPipelines();
    CreatePipelines(shaderManager);
}

void qray::VertexPreprocessing::CreatePipelineLayout(VkDescriptorSetLayout *pSetLayouts, uint32_t setLayoutCount)
{
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ShVertPreprocessing);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = setLayoutCount;
    layoutInfo.pSetLayouts = pSetLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Vertex preprocessing pipeline layout");
}

void qray::VertexPreprocessing::CreatePipelines(const ShaderManager *shaderManager)
{
    struct PipelineSpec
    {
        uint32_t mode;
        VkPipeline *pipeline;
        const char *debugName;
    };

    const PipelineSpec specs[] =
    {
        { VERT_PREPROC_MODE_ONLY_DYNAMIC,        &pipelineOnlyDynamic,       "Vertex only dynamic preprocessing pipeline"        },
        { VERT_PREPROC_MODE_DYNAMIC_AND_MOVABLE, &pipelineDynamicAndMovable, "Vertex movable/dynamic preprocessing pipeline"     },
        { VERT_PREPROC_MODE_ALL,                 &pipelineAll,               "Vertex static/movable/dynamic preprocessing pipeline" },
    };

    VkComputePipelineCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    plInfo.layout = pipelineLayout;
    plInfo.stage = shaderManager->GetStageInfo("CVertexPreprocess");

    for (const PipelineSpec &spec : specs)
    {
        uint32_t mode = spec.mode;

        VkSpecializationMapEntry specEntry = {};
        specEntry.constantID = 0;
        specEntry.offset = 0;
        specEntry.size = sizeof(uint32_t);

        VkSpecializationInfo specInfo = {};
        specInfo.mapEntryCount = 1;
        specInfo.pMapEntries = &specEntry;
        specInfo.dataSize = sizeof(uint32_t);
        specInfo.pData = &mode;

        plInfo.stage.pSpecializationInfo = &specInfo;

        VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, spec.pipeline);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, *spec.pipeline, VK_OBJECT_TYPE_PIPELINE, spec.debugName);
    }
}

void qray::VertexPreprocessing::DestroyPipelines()
{
    vkDestroyPipeline(device, pipelineOnlyDynamic, nullptr);
    vkDestroyPipeline(device, pipelineDynamicAndMovable, nullptr);
    vkDestroyPipeline(device, pipelineAll, nullptr);

    pipelineOnlyDynamic = VK_NULL_HANDLE;
    pipelineDynamicAndMovable = VK_NULL_HANDLE;
    pipelineAll = VK_NULL_HANDLE;
}
