#include "NearDof.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::binding(1, 0)]] Texture2D<float4> depthTexture;
[[vk::binding(2, 0)]] Texture2D<float4> surface;
[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> output;

struct NearDofPush
{
    float strength;
    float focusDistance;
    float maxRadius;
    float axisFactor;
};

[[vk::push_constant]] ConstantBuffer<NearDofPush> push;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
    {
        return;
    }
    output[id.xy] = float4(nearDofFilter(source, depthTexture, surface, int2(id.xy), push.axisFactor,
                                        push.strength, push.focusDistance, push.maxRadius), 1.0);
}
