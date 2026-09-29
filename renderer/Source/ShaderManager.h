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

#include <list>
#include <string>

#include "Common.h"
#include "Containers.h"
#include "IShaderDependency.h"
#include "UserFunction.h"

namespace qray
{

class ShaderManager
{
public:
    explicit ShaderManager(VkDevice device, const char *pShaderFolderPath, std::shared_ptr<UserFileLoad> userFileLoad);
    ~ShaderManager();

    ShaderManager(const ShaderManager& other) = delete;
    ShaderManager(ShaderManager&& other) noexcept = delete;
    ShaderManager& operator=(const ShaderManager& other) = delete;
    ShaderManager& operator=(ShaderManager&& other) noexcept = delete;

    void ReloadShaders();

    VkShaderModule GetShaderModule(const char *name) const;
    VkShaderStageFlagBits GetModuleStage(const char *name) const;
    VkPipelineShaderStageCreateInfo GetStageInfo(const char *name) const;

    void Subscribe(std::shared_ptr<IShaderDependency> subscriber);
    void Unsubscribe(const IShaderDependency *subscriber);

private:
    struct ShaderModule
    {
        VkShaderModule module;
        VkShaderStageFlagBits shaderStage;
    };

private:
    static VkShaderStageFlagBits GetStageByExtension(const char *name);

    VkShaderModule LoadModule(const char *path);
    VkShaderModule LoadModuleFromFile(const char *path);
    VkShaderModule LoadModuleFromMemory(const uint32_t *pCode, uint32_t codeSize);
    void LoadShaderModules();
    void UnloadShaderModules();

    void NotifySubscribersAboutReload();

private:
    VkDevice device;
    std::shared_ptr<UserFileLoad> userFileLoad;
    std::string shaderFolderPath;

    rgl::unordered_map<std::string, ShaderModule> modules;

    std::list<std::weak_ptr<IShaderDependency>> subscribers;
};

}
