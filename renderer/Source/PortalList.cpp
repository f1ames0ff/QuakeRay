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

#include "PortalList.h"

#include "QrException.h"

#include "Generated/ShaderCommonC.h"

static_assert(sizeof(qray::ShPortalInstance) % 16 == 0);
static_assert(qray::detail::PORTAL_LIST_BITCOUNT == PORTAL_MAX_COUNT);

qray::PortalList::PortalList(VkDevice _device, std::shared_ptr<MemoryAllocator> _allocator) :
    device(_device),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    buffer = std::make_shared<AutoBuffer>(std::move(_allocator));
    buffer->Create(
        PORTAL_MAX_COUNT * sizeof(ShPortalInstance),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        "Portals buffer");

    CreateDescriptors();
}

qray::PortalList::~PortalList()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
}

void qray::PortalList::Upload(uint32_t frameIndex, const QrPortalUploadInfo &info)
{
    if (info.portalIndex >= PORTAL_MAX_COUNT)
    {
        throw QrException(QR_WRONG_ARGUMENT, "Portal index must be in [0, 62]");
    }

    if (uploadedIndices.test(info.portalIndex))
    {
        throw QrException(QR_WRONG_ARGUMENT, "Portal with such index was already uploaded in this frame");
    }

    ShPortalInstance instance = {};

    memcpy(instance.inPosition, info.inPosition.data, 3 * sizeof(float));
    memcpy(instance.outPosition, info.outPosition.data, 3 * sizeof(float));
    memcpy(instance.outDirection, info.outDirection.data, 3 * sizeof(float));
    memcpy(instance.outUp, info.outUp.data, 3 * sizeof(float));

    auto *frameInstances = static_cast<ShPortalInstance *>(buffer->GetMapped(frameIndex));
    memcpy(&frameInstances[info.portalIndex], &instance, sizeof(ShPortalInstance));
}

VkBuffer qray::PortalList::GetStagingBuffer(uint32_t frameIndex)
{
    return buffer->GetStaging(frameIndex);
}

VkBuffer qray::PortalList::GetDeviceLocalBuffer() const
{
    return buffer->GetDeviceLocal();
}

VkDeviceSize qray::PortalList::GetBufferSize() const
{
    return buffer->GetSize();
}

void qray::PortalList::ResetUploads()
{
    uploadedIndices.reset();
}

VkDescriptorSet qray::PortalList::GetDescSet(uint32_t frameIndex) const
{
    return descSet;
}

VkDescriptorSetLayout qray::PortalList::GetDescSetLayout() const
{
    return descSetLayout;
}

void qray::PortalList::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = BINDING_PORTAL_INSTANCES;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Portals Desc set layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Portals Desc pool");

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descSetLayout;

    r = vkAllocateDescriptorSets(device, &allocInfo, &descSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Portals Desc set");

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = buffer->GetDeviceLocal();
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet wrt = {};
    wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wrt.dstSet = descSet;
    wrt.dstBinding = BINDING_PORTAL_INSTANCES;
    wrt.dstArrayElement = 0;
    wrt.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wrt.descriptorCount = 1;
    wrt.pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(device, 1, &wrt, 0, nullptr);
}
