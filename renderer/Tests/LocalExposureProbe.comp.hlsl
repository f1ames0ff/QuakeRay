#include "LocalExposure.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> source;
[[vk::binding(1, 0)]] SamplerState sourceSampler;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> output;

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
    const float2 inverseSize = 1.0 / float2(width, height);
    const float2 uv = (float2(id.xy) + 0.5) * inverseSize;
    const float correction = localExposureCorrection(source, sourceSampler, uv, inverseSize, 0.5);
    output[id.xy] = float4(correction, correction, correction, 1.0);
}
