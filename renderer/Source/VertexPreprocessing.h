// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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
#include "ASManager.h"
#include "GlobalUniform.h"
#include "ShaderManager.h"

namespace qray
{

struct ShVertPreprocessing;

class VertexPreprocessing : public IShaderDependency
{
public:
    explicit VertexPreprocessing(
        VkDevice device,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<const ASManager> &asManager,
        const std::shared_ptr<const ShaderManager> &shaderManager);

    ~VertexPreprocessing();

    VertexPreprocessing(const VertexPreprocessing &other) = delete;
    VertexPreprocessing(VertexPreprocessing &&other) noexcept = delete;
    VertexPreprocessing & operator=(const VertexPreprocessing &other) = delete;
    VertexPreprocessing & operator=(VertexPreprocessing &&other) noexcept = delete;

    void Preprocess(
        VkCommandBuffer cmd, uint32_t frameIndex, uint32_t preprocMode,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<ASManager> &asManager,
        const ShVertPreprocessing &push);

    void OnShaderReload(const ShaderManager *shaderManager) override;

private:
    void CreatePipelineLayout(VkDescriptorSetLayout *pSetLayouts, uint32_t setLayoutCount);
    void CreatePipelines(const ShaderManager *shaderManager);
    void DestroyPipelines();

private:
    VkDevice device;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipelineOnlyDynamic;
    VkPipeline pipelineDynamicAndMovable;
    VkPipeline pipelineAll;
};

}
