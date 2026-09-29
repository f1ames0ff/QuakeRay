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

#include "ShaderManager.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "QrException.h"

using namespace qray;

namespace
{
    struct ShaderDefinition
    {
        const char *name = nullptr;
        const char *filename = nullptr;
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;
    };

    ShaderDefinition G_SHADERS[] =
    {
        {"CLuminanceHistogram",     "CmLuminanceHistogram.comp.spv"        },
        {"CLuminanceAvg",           "CmLuminanceAvg.comp.spv"              },
        {"CVertexPreprocess",       "CmVertexPreprocess.comp.spv"          },
        {"VertDecal",               "RsDecal.vert.spv"                     },
        {"FragDecal",               "RsDecal.frag.spv"                     },
    };

    struct ShaderExtension
    {
        const char *suffix;
        VkShaderStageFlagBits stage;
    };

    constexpr ShaderExtension G_SHADER_EXTENSIONS[] =
    {
        {".vert.spv",   VK_SHADER_STAGE_VERTEX_BIT},
        {".frag.spv",   VK_SHADER_STAGE_FRAGMENT_BIT},
        {".comp.spv",   VK_SHADER_STAGE_COMPUTE_BIT},
        {".rgen.spv",   VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {".rahit.spv",  VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
        {".rchit.spv",  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {".rmiss.spv",  VK_SHADER_STAGE_MISS_BIT_KHR},
        {".rcall.spv",  VK_SHADER_STAGE_CALLABLE_BIT_KHR},
        {".rint.spv",   VK_SHADER_STAGE_INTERSECTION_BIT_KHR},
        {".tesc.spv",   VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT},
        {".tese.spv",   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT},
        {".mesh.spv",   VK_SHADER_STAGE_MESH_BIT_NV},
        {".task.spv",   VK_SHADER_STAGE_TASK_BIT_NV},
    };
}

ShaderManager::ShaderManager(VkDevice _device, const char *_pShaderFolderPath, std::shared_ptr<UserFileLoad> _userFileLoad)
    : device(_device)
    , userFileLoad(std::move(_userFileLoad))
    , shaderFolderPath(_pShaderFolderPath)
{
    LoadShaderModules();
}

ShaderManager::~ShaderManager()
{
    UnloadShaderModules();
}

void ShaderManager::ReloadShaders()
{
    vkDeviceWaitIdle(device);

    UnloadShaderModules();
    LoadShaderModules();

    NotifySubscribersAboutReload();

    vkDeviceWaitIdle(device);
}

void ShaderManager::LoadShaderModules()
{
    for (ShaderDefinition &shader : G_SHADERS)
    {
        assert(shader.filename != nullptr);
        assert(shader.name != nullptr);

        if (shader.stage == VK_SHADER_STAGE_ALL)
        {
            shader.stage = GetStageByExtension(shader.filename);
        }

        const std::string path = shaderFolderPath + shader.filename;
        const VkShaderModule module = LoadModule(path.c_str());

        SET_DEBUG_NAME(device, module, VK_OBJECT_TYPE_SHADER_MODULE, shader.name);

        modules[shader.name] = { module, shader.stage };
    }
}

void ShaderManager::UnloadShaderModules()
{
    for (auto &entry : modules)
    {
        vkDestroyShaderModule(device, entry.second.module, nullptr);
    }

    modules.clear();
}

VkShaderModule ShaderManager::GetShaderModule(const char *name) const
{
    const auto it = modules.find(name);
    return it != modules.end() ? it->second.module : VK_NULL_HANDLE;
}

VkShaderStageFlagBits ShaderManager::GetModuleStage(const char *name) const
{
    const auto it = modules.find(name);
    return it != modules.end() ? it->second.shaderStage : static_cast<VkShaderStageFlagBits>(0);
}

VkPipelineShaderStageCreateInfo ShaderManager::GetStageInfo(const char *name) const
{
    const auto it = modules.find(name);

    if (it == modules.end())
    {
        throw QrException(QR_WRONG_ARGUMENT, std::string("Can't find loaded shader with name \"") + name + "\"");
    }

    VkPipelineShaderStageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.module = it->second.module;
    info.stage = it->second.shaderStage;
    info.pName = "main";

    return info;
}

VkShaderModule ShaderManager::LoadModule(const char *path)
{
    if (!userFileLoad->Exists())
    {
        return LoadModuleFromFile(path);
    }

    auto fileHandle = userFileLoad->Open(path);

    if (!fileHandle.Contains())
    {
        throw QrException(QR_WRONG_ARGUMENT, std::string("Can't load shader file \"") + path + "\" using user's file load function");
    }

    return LoadModuleFromMemory(static_cast<const uint32_t *>(fileHandle.pData), fileHandle.dataSize);
}

VkShaderModule ShaderManager::LoadModuleFromFile(const char *path)
{
    std::ifstream shaderFile(path, std::ios::binary);
    std::vector<uint8_t> shaderSource(std::istreambuf_iterator<char>(shaderFile), {});

    if (shaderSource.empty())
    {
        throw QrException(QR_WRONG_ARGUMENT, std::string("Can't find shader file: \"") + path + "\"");
    }

    return LoadModuleFromMemory(reinterpret_cast<const uint32_t *>(shaderSource.data()), static_cast<uint32_t>(shaderSource.size()));
}

VkShaderModule ShaderManager::LoadModuleFromMemory(const uint32_t *pCode, uint32_t codeSize)
{
    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = codeSize;
    moduleInfo.pCode = pCode;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule);
    VK_CHECKERROR(r);

    return shaderModule;
}

VkShaderStageFlagBits ShaderManager::GetStageByExtension(const char *name)
{
    for (const ShaderExtension &extension : G_SHADER_EXTENSIONS)
    {
        if (std::strstr(name, extension.suffix) != nullptr)
        {
            return extension.stage;
        }
    }

    assert(0);
    return VK_SHADER_STAGE_ALL;
}

void ShaderManager::Subscribe(std::shared_ptr<IShaderDependency> subscriber)
{
    subscribers.emplace_back(subscriber);
}

void ShaderManager::Unsubscribe(const IShaderDependency *subscriber)
{
    subscribers.remove_if([subscriber](const std::weak_ptr<IShaderDependency> &weakSubscriber)
    {
        if (const auto sharedSubscriber = weakSubscriber.lock())
        {
            return sharedSubscriber.get() == subscriber;
        }

        return true;
    });
}

void ShaderManager::NotifySubscribersAboutReload()
{
    for (auto &weakSubscriber : subscribers)
    {
        if (auto sharedSubscriber = weakSubscriber.lock())
        {
            sharedSubscriber->OnShaderReload(this);
        }
    }
}
