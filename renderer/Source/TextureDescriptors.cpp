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

#include "TextureDescriptors.h"
#include "Const.h"

using namespace qray;

TextureDescriptors::TextureDescriptors(VkDevice _device, std::shared_ptr<SamplerManager> _samplerManager, uint32_t _maxTextureCount, uint32_t _bindingIndex, uint32_t _samplerBindingIndex)
    : device(_device)
    , samplerManager(std::move(_samplerManager))
    , bindingIndex(_bindingIndex)
    , samplerBindingIndex(_samplerBindingIndex)
    , descPool(VK_NULL_HANDLE)
    , descLayout(VK_NULL_HANDLE)
    , descSets{}
    , emptyTextureImageView(VK_NULL_HANDLE)
    , emptyTextureImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
    , currentWriteCount(0)
{
    writeImageInfos.resize(_maxTextureCount);
    writeSamplerInfos.resize(_maxTextureCount);
    writeInfos.resize(_maxTextureCount * 2);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        writeCache[i].resize(_maxTextureCount);
    }

    CreateDescriptors(_maxTextureCount);
}

TextureDescriptors::~TextureDescriptors()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
}

VkDescriptorSet TextureDescriptors::GetDescSet(uint32_t frameIndex) const
{
    return descSets[frameIndex];
}

VkDescriptorSetLayout TextureDescriptors::GetDescSetLayout() const
{
    return descLayout;
}

void TextureDescriptors::SetEmptyTextureInfo(VkImageView view)
{
    emptyTextureImageView = view;
    emptyTextureImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void TextureDescriptors::CreateDescriptors(uint32_t maxTextureCount)
{
    const VkDescriptorSetLayoutBinding bindings[2] =
    {
        { bindingIndex,        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxTextureCount, VK_SHADER_STAGE_ALL, nullptr },
        { samplerBindingIndex, VK_DESCRIPTOR_TYPE_SAMPLER,       maxTextureCount, VK_SHADER_STAGE_ALL, nullptr },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descLayout);
    VK_CHECKERROR(result);

    SET_DEBUG_NAME(device, descLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Textures Desc set layout");

    const VkDescriptorPoolSize poolSizes[2] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxTextureCount * MAX_FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_SAMPLER,       maxTextureCount * MAX_FRAMES_IN_FLIGHT },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(result);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Textures Desc pool");

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descLayout;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        result = vkAllocateDescriptorSets(device, &setInfo, &descSets[i]);
        VK_CHECKERROR(result);

        SET_DEBUG_NAME(device, descSets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, "Textures desc set");
    }
}

bool TextureDescriptors::IsCached(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle)
{
    const UpdatedDescCache &cached = writeCache[frameIndex][textureIndex];
    return cached.view == view && cached.samplerHandle == samplerHandle;
}

void TextureDescriptors::AddToCache(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle)
{
    writeCache[frameIndex][textureIndex] = { view, samplerHandle };
}

void TextureDescriptors::ResetCache(uint32_t frameIndex, uint32_t textureIndex)
{
    writeCache[frameIndex][textureIndex] = { VK_NULL_HANDLE, SamplerManager::Handle() };
}

void qray::TextureDescriptors::ResetAllCache(uint32_t frameIndex)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        for (auto &cached : writeCache[i])
        {
            cached.view = VK_NULL_HANDLE;
            cached.samplerHandle = SamplerManager::Handle();
        }
    }
}

void TextureDescriptors::UpdateTextureDesc(uint32_t frameIndex, uint32_t textureIndex, VkImageView view, SamplerManager::Handle samplerHandle)
{
    assert(view != VK_NULL_HANDLE);

    if (IsCached(frameIndex, textureIndex, view, samplerHandle))
    {
        return;
    }

    if (currentWriteCount >= writeImageInfos.size())
    {
        FlushDescWrites();
    }

    const uint32_t writeIndex = currentWriteCount;
    currentWriteCount++;

    VkDescriptorImageInfo &imageInfo = writeImageInfos[writeIndex];
    imageInfo.sampler = VK_NULL_HANDLE;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo &samplerInfo = writeSamplerInfos[writeIndex];
    samplerInfo.sampler = samplerManager->GetSampler(samplerHandle);
    samplerInfo.imageView = VK_NULL_HANDLE;
    samplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkWriteDescriptorSet &viewWrite = writeInfos[writeIndex * 2];
    viewWrite = {};
    viewWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    viewWrite.dstSet = descSets[frameIndex];
    viewWrite.dstBinding = bindingIndex;
    viewWrite.dstArrayElement = textureIndex;
    viewWrite.descriptorCount = 1;
    viewWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    viewWrite.pImageInfo = &imageInfo;

    VkWriteDescriptorSet &samplerWrite = writeInfos[writeIndex * 2 + 1];
    samplerWrite = {};
    samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    samplerWrite.dstSet = descSets[frameIndex];
    samplerWrite.dstBinding = samplerBindingIndex;
    samplerWrite.dstArrayElement = textureIndex;
    samplerWrite.descriptorCount = 1;
    samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerWrite.pImageInfo = &samplerInfo;

    AddToCache(frameIndex, textureIndex, view, samplerHandle);
}

void TextureDescriptors::ResetTextureDesc(uint32_t frameIndex, uint32_t textureIndex)
{
    assert(emptyTextureImageView != VK_NULL_HANDLE &&
           emptyTextureImageLayout != VK_IMAGE_LAYOUT_UNDEFINED);

    UpdateTextureDesc(frameIndex, textureIndex,
                      emptyTextureImageView, SamplerManager::Handle(QR_SAMPLER_FILTER_NEAREST, QR_SAMPLER_ADDRESS_MODE_REPEAT, QR_SAMPLER_ADDRESS_MODE_REPEAT, 0));
}

void TextureDescriptors::FlushDescWrites()
{
    vkUpdateDescriptorSets(device, currentWriteCount * 2, writeInfos.data(), 0, nullptr);
    currentWriteCount = 0;
}
