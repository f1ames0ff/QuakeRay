// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_LIGHT_SOURCES 1

#include "ShaderCommonHLSLFunc.hlsli"

float4 getTextureSampleLod(uint textureIndex, const float2 texCoord, float lod)
{
    return float4(1.0, 1.0, 1.0, 1.0);
}

int2 getTextureSize(uint textureIndex, uint mipLevel)
{
    return int2(1, 1);
}

#include "Light.hlsli"

struct DtalPerfParams
{
    uint lightCount;
    uint samplesPerThread;
    uint seed;
    uint padding;
};

[[vk::binding(0, 2)]] ConstantBuffer<DtalPerfParams> probeParams;
[[vk::binding(1, 2)]] RWStructuredBuffer<float> probeOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightCount = max(probeParams.lightCount, 1u);
    const float2 golden = float2(0.6180339887, 0.3819660113);
    float2 rnd = frac(float2(float(dispatchThreadId.x), float(dispatchThreadId.x * 3u)) * 0.001 + float(probeParams.seed));
    float acc = 0.0;

    for (uint s = 0; s < probeParams.samplesPerThread; s++)
    {
        const uint index = (dispatchThreadId.x + s * 17u) % lightCount;
        const TexturedAreaLight light = decodeAsTexturedAreaLight(lightSources[index]);
        const LightSample sample = sampleTexturedAreaLight(light, float3(0.0, 0.0, 0.0), rnd);
        acc += sample.color.x + sample.dw + sample.position.y;
        rnd = frac(rnd + golden);
    }

    probeOutput[dispatchThreadId.x] = acc;
}
