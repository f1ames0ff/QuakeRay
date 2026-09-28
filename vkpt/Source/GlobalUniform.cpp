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

#include "GlobalUniform.h"

#include "Generated/ShaderCommonC.h"

using namespace vkpt;

GlobalUniform::GlobalUniform(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator)
    : device(_device)
    , uniformData(std::make_shared<ShGlobalUniform>())
    , uniformBuffer(std::make_shared<AutoBuffer>(_device, _allocator))
    , descPool(VK_NULL_HANDLE)
    , descSetLayout(VK_NULL_HANDLE)
    , descSet(VK_NULL_HANDLE)
{
    uniformBuffer->Create(sizeof(ShGlobalUniform),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          "Uniform buffer");

    CreateDescriptors();
}

GlobalUniform::~GlobalUniform()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
}

void GlobalUniform::CreateDescriptors()
{
    const VkDescriptorSetLayoutBinding uniformBinding =
    {
        BINDING_GLOBAL_UNIFORM, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL
    };

    const VkDescriptorSetLayoutCreateInfo layoutInfo =
    {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &uniformBinding
    };

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Uniform Desc set layout");

    const VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT };

    const VkDescriptorPoolCreateInfo poolInfo =
    {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, MAX_FRAMES_IN_FLIGHT, 1, &poolSize
    };

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Uniform Desc pool");

    const VkDescriptorSetAllocateInfo allocInfo =
    {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descPool, 1, &descSetLayout
    };

    r = vkAllocateDescriptorSets(device, &allocInfo, &descSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Uniform Desc set");

    const VkDescriptorBufferInfo bufferInfo = { uniformBuffer->GetDeviceLocal(), 0, VK_WHOLE_SIZE };

    const VkWriteDescriptorSet write =
    {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, BINDING_GLOBAL_UNIFORM, 0, 1,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

ShGlobalUniform *GlobalUniform::GetData()
{
    return uniformData.get();
}

const ShGlobalUniform *GlobalUniform::GetData() const
{
    return uniformData.get();
}

VkBuffer GlobalUniform::GetBuffer() const
{
    return uniformBuffer->GetDeviceLocal();
}

VkDescriptorSet GlobalUniform::GetDescSet(uint32_t frameIndex) const
{
    return descSet;
}

VkDescriptorSetLayout GlobalUniform::GetDescSetLayout() const
{
    return descSetLayout;
}
