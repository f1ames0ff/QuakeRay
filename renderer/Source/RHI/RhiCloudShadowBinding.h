#pragma once

#include <nvrhi/nvrhi.h>

namespace qray::rhi
{

class RhiFrameContext;

class RhiCloudShadowBinding
{
public:
    bool Create(nvrhi::IDevice *device, RhiFrameContext *frames, nvrhi::ShaderType visibility,
                uint32_t pushConstantSize = 0);
    bool SetTexture(nvrhi::ITexture *texture, nvrhi::ISampler *sampler);
    nvrhi::IBindingLayout *GetLayout() const { return layout.Get(); }
    nvrhi::IBindingSet *GetSet() const { return set.Get(); }

private:
    nvrhi::IDevice *device = nullptr;
    RhiFrameContext *frames = nullptr;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::BindingSetHandle set;
    nvrhi::TextureHandle fallbackTexture;
    nvrhi::SamplerHandle fallbackSampler;
    nvrhi::TextureHandle texture;
    nvrhi::SamplerHandle sampler;
};

}
