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

#include "RhiCloudShadowBinding.h"
#include "RhiFrameContext.h"
#include "RhiResources.h"

#include <utility>

namespace qray::rhi
{

bool RhiCloudShadowBinding::Create(nvrhi::IDevice *pDevice, RhiFrameContext *pFrames,
                                    nvrhi::ShaderType visibility, uint32_t pushConstantSize)
{
    device = pDevice;
    frames = pFrames;
    if (device == nullptr || frames == nullptr)
    {
        return false;
    }
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = visibility;
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0).setSamplerOffset(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(1));
    if (pushConstantSize != 0)
    {
        desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, pushConstantSize));
    }
    layout = device->createBindingLayout(desc);

    nvrhi::TextureDesc textureDesc;
    textureDesc.dimension = nvrhi::TextureDimension::Texture3D;
    textureDesc.format = nvrhi::Format::R16_FLOAT;
    textureDesc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    textureDesc.keepInitialState = true;
    fallbackTexture = createTexture(device, textureDesc, "Cloud shadow fallback");
    fallbackSampler = createSampler(device, nvrhi::SamplerDesc().setMinFilter(true).setMagFilter(true)
        .setMipFilter(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp), "Cloud shadow fallback sampler");
    if (layout == nullptr || fallbackTexture == nullptr || fallbackSampler == nullptr)
    {
        return false;
    }
    return SetTexture(nullptr, nullptr);
}

bool RhiCloudShadowBinding::SetTexture(nvrhi::ITexture *pTexture, nvrhi::ISampler *pSampler)
{
    if (pTexture == nullptr || pSampler == nullptr)
    {
        pTexture = fallbackTexture.Get();
        pSampler = fallbackSampler.Get();
    }
    if (set != nullptr && texture.Get() == pTexture && sampler.Get() == pSampler)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, pTexture));
    desc.addItem(nvrhi::BindingSetItem::Sampler(1, pSampler));
    auto next = device->createBindingSet(desc, layout);
    if (next == nullptr)
    {
        return false;
    }
    frames->Retire(set);
    set = std::move(next);
    texture = pTexture;
    sampler = pSampler;
    return true;
}

}
