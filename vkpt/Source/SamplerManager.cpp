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

#include "SamplerManager.h"

#include <cmath>
#include <string>

#include "RgException.h"
#include "RHI/RhiTextureTable.h"

using namespace vkpt;

namespace
{
    constexpr uint32_t FILTER_LINEAR = 1u << 0;
    constexpr uint32_t FILTER_NEAREST = 2u << 0;
    constexpr uint32_t FILTER_MASK = 3u << 0;

    constexpr uint32_t ADDRESS_MODE_U_REPEAT = 1u << 2;
    constexpr uint32_t ADDRESS_MODE_U_MIRRORED_REPEAT = 2u << 2;
    constexpr uint32_t ADDRESS_MODE_U_CLAMP_TO_EDGE = 3u << 2;
    constexpr uint32_t ADDRESS_MODE_U_CLAMP_TO_BORDER = 4u << 2;
    constexpr uint32_t ADDRESS_MODE_U_MIRROR_CLAMP_TO_EDGE = 5u << 2;
    constexpr uint32_t ADDRESS_MODE_U_MASK = 7u << 2;

    constexpr uint32_t ADDRESS_MODE_V_REPEAT = 1u << 5;
    constexpr uint32_t ADDRESS_MODE_V_MIRRORED_REPEAT = 2u << 5;
    constexpr uint32_t ADDRESS_MODE_V_CLAMP_TO_EDGE = 3u << 5;
    constexpr uint32_t ADDRESS_MODE_V_CLAMP_TO_BORDER = 4u << 5;
    constexpr uint32_t ADDRESS_MODE_V_MIRROR_CLAMP_TO_EDGE = 5u << 5;
    constexpr uint32_t ADDRESS_MODE_V_MASK = 7u << 5;

    constexpr uint32_t FORCE_LOWEST_MIP_BOOL = 1u << 8;

    VkFilter RgFilterToVk(RgSamplerFilter filter)
    {
        switch (filter)
        {
            case RG_SAMPLER_FILTER_LINEAR: return VK_FILTER_LINEAR;
            case RG_SAMPLER_FILTER_NEAREST: return VK_FILTER_NEAREST;
            default: assert(0); return VK_FILTER_NEAREST;
        }
    }

    VkSamplerAddressMode RgAddressModeToVk(RgSamplerAddressMode addressMode)
    {
        switch (addressMode)
        {
            case RG_SAMPLER_ADDRESS_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case RG_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case RG_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case RG_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            case RG_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            default: assert(0); return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    nvrhi::SamplerAddressMode VkAddressModeToNvrhi(VkSamplerAddressMode addressMode)
    {
        switch (addressMode)
        {
            case VK_SAMPLER_ADDRESS_MODE_REPEAT: return nvrhi::SamplerAddressMode::Repeat;
            case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return nvrhi::SamplerAddressMode::MirroredRepeat;
            case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return nvrhi::SamplerAddressMode::ClampToEdge;
            case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return nvrhi::SamplerAddressMode::ClampToBorder;
            case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return nvrhi::SamplerAddressMode::MirrorClampToEdge;
            default: assert(0); return nvrhi::SamplerAddressMode::Repeat;
        }
    }

    bool IsLinearFilter(VkFilter filter)
    {
        assert(filter == VK_FILTER_NEAREST || filter == VK_FILTER_LINEAR);
        return filter == VK_FILTER_LINEAR;
    }

    nvrhi::SamplerDesc VkSamplerInfoToNvrhiDesc(const VkSamplerCreateInfo &info)
    {
        nvrhi::SamplerDesc desc;
        desc.minFilter = IsLinearFilter(info.minFilter);
        desc.magFilter = IsLinearFilter(info.magFilter);
        desc.mipFilter = info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
        desc.addressU = VkAddressModeToNvrhi(info.addressModeU);
        desc.addressV = VkAddressModeToNvrhi(info.addressModeV);
        desc.addressW = VkAddressModeToNvrhi(info.addressModeW);
        desc.mipBias = info.mipLodBias;
        desc.maxAnisotropy = info.maxAnisotropy;
        desc.reductionType = nvrhi::SamplerReductionType::Standard;
        desc.borderColor = nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f);
        return desc;
    }

    uint32_t SwapFilterInIndex(uint32_t srcIndex, RgSamplerFilter newFilter)
    {
        if (srcIndex & FORCE_LOWEST_MIP_BOOL)
        {
            return FORCE_LOWEST_MIP_BOOL;
        }

        uint32_t index = srcIndex & (~FILTER_MASK);

        switch (newFilter)
        {
            case RG_SAMPLER_FILTER_NEAREST: index |= FILTER_NEAREST; break;
            case RG_SAMPLER_FILTER_LINEAR: index |= FILTER_LINEAR; break;
            default: assert(0); break;
        }

        assert(index != 0);
        return index;
    }
}

SamplerManager::SamplerManager(
    VkDevice _device, uint32_t _anisotropy, bool _forceMinificationFilterLinear,
    rhi::RhiTextureTable *pRhiTextureTable)
    : device(_device)
    , mipLodBias(0.0f)
    , anisotropy(_anisotropy)
    , forceMinificationFilterLinear(_forceMinificationFilterLinear)
    , rhiTextureTable(pRhiTextureTable)
{
    CreateAllSamplers(anisotropy, mipLodBias);
}

SamplerManager::~SamplerManager()
{
    for (auto &pair : samplers)
    {
        vkDestroySampler(device, pair.second, nullptr);
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        for (const VkSampler sampler : samplersToDelete[i])
        {
            vkDestroySampler(device, sampler, nullptr);
        }

        samplersToDelete[i].clear();
    }

    samplers.clear();
}

void SamplerManager::SetRhiTextureTable(rhi::RhiTextureTable *pTable)
{
    rhiTextureTable = pTable;
}

void vkpt::SamplerManager::CreateAllSamplers(uint32_t _anisotropy, float _mipLodBias)
{
    assert(samplers.empty());
    assert(_anisotropy == 0 || _anisotropy == 2 || _anisotropy == 4 || _anisotropy == 8 || _anisotropy == 16);

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.mipLodBias = _mipLodBias;
    info.anisotropyEnable = _anisotropy > 0 ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = _anisotropy;
    info.compareEnable = VK_FALSE;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    constexpr VkFilter filters[] =
    {
        VK_FILTER_NEAREST,
        VK_FILTER_LINEAR,
    };

    constexpr VkSamplerAddressMode addressModes[] =
    {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
    };

    for (const VkFilter filter : filters)
    {
        for (const VkSamplerAddressMode addressModeU : addressModes)
        {
            for (const VkSamplerAddressMode addressModeV : addressModes)
            {
                info.minFilter = forceMinificationFilterLinear ? VK_FILTER_LINEAR : filter;
                info.magFilter = filter;
                info.addressModeU = addressModeU;
                info.addressModeV = addressModeV;

                const uint32_t index = ToIndex(filter, addressModeU, addressModeV, false);

                VkSampler sampler = VK_NULL_HANDLE;
                VK_CHECKERROR(vkCreateSampler(device, &info, nullptr, &sampler));

                assert(samplers.find(index) == samplers.end());
                samplers[index] = sampler;

                if (rhiTextureTable != nullptr)
                {
                    rhiTextureTable->SetSamplerDesc(index, VkSamplerInfoToNvrhiDesc(info));
                }
            }
        }
    }

    info.minFilter = VK_FILTER_LINEAR;
    info.magFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.minLod = VK_LOD_CLAMP_NONE - 1;

    const uint32_t lowestMipIndex = ToIndex(info.minFilter, info.addressModeU, info.addressModeV, true);

    VkSampler lowestMipSampler = VK_NULL_HANDLE;
    VK_CHECKERROR(vkCreateSampler(device, &info, nullptr, &lowestMipSampler));

    assert(samplers.find(lowestMipIndex) == samplers.end());
    samplers[lowestMipIndex] = lowestMipSampler;

    if (rhiTextureTable != nullptr)
    {
        rhiTextureTable->SetSamplerDesc(lowestMipIndex, VkSamplerInfoToNvrhiDesc(info));
    }
}

void vkpt::SamplerManager::AddAllSamplersToDestroy(uint32_t frameIndex)
{
    for (auto &pair : samplers)
    {
        samplersToDelete[frameIndex].push_back(pair.second);
    }

    samplers.clear();
}

void vkpt::SamplerManager::PrepareForFrame(uint32_t frameIndex)
{
    for (const VkSampler sampler : samplersToDelete[frameIndex])
    {
        vkDestroySampler(device, sampler, nullptr);
    }

    samplersToDelete[frameIndex].clear();
}

VkSampler SamplerManager::GetSampler(
    RgSamplerFilter filter, RgSamplerAddressMode addressModeU, RgSamplerAddressMode addressModeV, bool forceLowestMip) const
{
    const uint32_t index = ToIndex(filter, addressModeU, addressModeV, forceLowestMip);

    const auto f = samplers.find(index);

    if (f != samplers.end())
    {
        return f->second;
    }

    throw RgException(RG_WRONG_MATERIAL_PARAMETER,
                      "Wrong RgSamplerFilter(" + std::to_string(filter) +
                      ") or RgSamplerAddressMode (U: " + std::to_string(addressModeU) +
                      ", V: " + std::to_string(addressModeV) + ") value");
}

VkSampler vkpt::SamplerManager::GetSampler(const Handle &handle) const
{
    assert(handle.internalIndex != 0);

    const auto f = samplers.find(handle.internalIndex);

    if (f == samplers.end())
    {
        assert(0);
        return VK_NULL_HANDLE;
    }

    return f->second;
}

bool vkpt::SamplerManager::TryChangeMipLodBias(uint32_t frameIndex, float newMipLodBias)
{
    constexpr float delta = 0.025f;

    if (std::abs(newMipLodBias - mipLodBias) < delta)
    {
        return false;
    }

    AddAllSamplersToDestroy(frameIndex);
    CreateAllSamplers(anisotropy, newMipLodBias);

    if (rhiTextureTable != nullptr)
    {
        rhiTextureTable->RebuildSamplers();
    }

    mipLodBias = newMipLodBias;
    return true;
}

uint32_t SamplerManager::ToIndex(
    RgSamplerFilter filter, RgSamplerAddressMode addressModeU, RgSamplerAddressMode addressModeV, bool forceLowestMip)
{
    return ToIndex(RgFilterToVk(filter), RgAddressModeToVk(addressModeU), RgAddressModeToVk(addressModeV), forceLowestMip);
}

uint32_t SamplerManager::ToIndex(
    VkFilter filter, VkSamplerAddressMode addressModeU, VkSamplerAddressMode addressModeV, bool forceLowestMip)
{
    if (forceLowestMip)
    {
        return FORCE_LOWEST_MIP_BOOL;
    }

    uint32_t index = 0;

    switch (filter)
    {
        case VK_FILTER_NEAREST: index |= FILTER_NEAREST; break;
        case VK_FILTER_LINEAR: index |= FILTER_LINEAR; break;
        default: assert(0); break;
    }

    switch (addressModeU)
    {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: index |= ADDRESS_MODE_U_REPEAT; break;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: index |= ADDRESS_MODE_U_MIRRORED_REPEAT; break;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: index |= ADDRESS_MODE_U_CLAMP_TO_EDGE; break;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: index |= ADDRESS_MODE_U_CLAMP_TO_BORDER; break;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: index |= ADDRESS_MODE_U_MIRROR_CLAMP_TO_EDGE; break;
        default: assert(0); break;
    }

    switch (addressModeV)
    {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: index |= ADDRESS_MODE_V_REPEAT; break;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: index |= ADDRESS_MODE_V_MIRRORED_REPEAT; break;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: index |= ADDRESS_MODE_V_CLAMP_TO_EDGE; break;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: index |= ADDRESS_MODE_V_CLAMP_TO_BORDER; break;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: index |= ADDRESS_MODE_V_MIRROR_CLAMP_TO_EDGE; break;
        default: assert(0); break;
    }

    assert(index != 0);
    return index;
}

vkpt::SamplerManager::Handle::Handle()
    : internalIndex(0)
    , hasDynamicSamplerFilter(false)
{
}

vkpt::SamplerManager::Handle::Handle(
    RgSamplerFilter filter, RgSamplerAddressMode addressModeU, RgSamplerAddressMode addressModeV, RgMaterialCreateFlags flags)
    : internalIndex(ToIndex(filter, addressModeU, addressModeV, (flags & RG_MATERIAL_CREATE_FORCE_LOWEST_MIP_BIT) != 0))
    , hasDynamicSamplerFilter((flags & RG_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT) != 0)
{
}

bool vkpt::SamplerManager::Handle::SetIfHasDynamicSamplerFilter(RgSamplerFilter newDynamicSamplerFilter)
{
    if (!hasDynamicSamplerFilter)
    {
        return false;
    }

    internalIndex = SwapFilterInIndex(internalIndex, newDynamicSamplerFilter);
    return true;
}
