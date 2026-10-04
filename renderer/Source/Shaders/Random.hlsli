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


#ifndef RANDOM_HLSLI_
#define RANDOM_HLSLI_
#define RANDOM_SALT_DIFF_BOUNCE(bounceIndex) (8 + (bounceIndex))
#define RANDOM_SALT_SPEC_BOUNCE(bounceIndex) (12 + (bounceIndex))
#define RANDOM_SALT_POSTEFFECT 16
#define RANDOM_SALT_LIGHT_POINT 20
#define RANDOM_SALT_LIGHT_GRID_BASE 24
#define RANDOM_SALT_INITIAL_RESERVOIRS_BASE 48
#define RANDOM_SALT_LIGHT_CHOOSE_DIRECT_BASE 72
#define RANDOM_SALT_LIGHT_CHOOSE_INDIRECT_BASE 96
#define RANDOM_SALT_RESAMPLE_INDIRECT_BASE 132

float2 sampleDisk(float radius, float u1, float u2)
{
    u1 *= 0.99;
    u2 *= 0.99;

    const float r = radius * sqrt(u1);
    const float phi = 2 * M_PI * u2;

    return float2(
        r * cos(phi),
        r * sin(phi)
    );

}

float3 sampleTriangle(const float3 p0, const float3 p1, const float3 p2, float u1, float u2)
{
    u1 *= 0.99;
    u2 *= 0.99;

    float beta = 1 - sqrt(u1);
    float gamma = (1 - beta) * u2;
    float alpha = 1 - beta - gamma;

    return alpha * p0 + beta * p1 + gamma * p2;
}

float3 sampleHemisphere(float u1, float u2, out float oneOverPdf)
{
    u1 *= 0.99;
    u2 *= 0.99;

    const float r = sqrt(u1);
    const float phi = 2 * M_PI * u2;

    const float z = sqrt(1 - u1);

    oneOverPdf = M_PI / max(z, 0.1);

    return float3(
        r * cos(phi),
        r * sin(phi),
        z
    );
}

float3 sampleSphere(float u1, float u2)
{
    u1 *= 0.99;
    u2 *= 0.99;

    u1 = 2 * u1 - 1;
    u2 = 2 * u2 - 1;

    const float d = 1 - (abs(u1) + abs(u2));
    const float r = 1 - abs(d);

    const float phi = r == 0 ? 0.0 : M_PI / 4 * ((abs(u2) - abs(u1)) / r + 1);
    const float f = r * sqrt(2 - r * r);

    return float3(
        f * sign(u1) * cos(phi),
        f * sign(u2) * sin(phi),
        sign(d) * (1 - r * r));

}

void revisedONB(const float3 n, out float3 b1, out float3 b2)
{
    if(n.z < 0.0)
    {
        const float a = 1.0f / (1.0f - n.z);
        const float b = n.x * n.y * a;

        b1 = float3(1.0f - n.x * n.x * a, -b, n.x);
        b2 = float3(b, n.y * n.y * a - 1.0f, -n.y);
    }
    else
    {
        const float a = 1.0f / (1.0f + n.z);
        const float b = -n.x * n.y * a;

        b1 = float3(1.0f - n.x * n.x * a, b, -n.x);
        b2 = float3(b, 1.0f - n.y * n.y * a, -n.y);
    }
}

void frisvadONB(const float3 n, out float3 b1, out float3 b2)
{
    if(n.z < -0.9999999)
    {
        b1 = float3( 0.0, -1.0, 0.0);
        b2 = float3( -1.0, 0.0, 0.0);

        return;
    }

    const float a = 1.0 / (1.0 + n.z);
    const float b = -n.x * n.y * a;

    b1 = float3(1.0 - n.x * n.x * a, b, n.x);
    b2 = float3(b, 1.0 - n.y * n.y * a, -n.y);
}

float3x3 getONB(const float3 n)
{
    float3 b1;
    float3 b2;
    revisedONB(n, b1, b2);

    return transpose(float3x3(b1, b2, n));
}

float3 sampleOrientedHemisphere(const float3 n, float u1, float u2, out float oneOverPdf)
{


    float a = 1 - 2 * u1;
    float b = sqrt(1 - a * a);
    float phi = 2 * M_PI * u2;

    a *= 0.98;
    b *= 0.98;

    float3 r = float3(
        n.x + b * cos(phi),
        n.y + b * sin(phi),
        n.z + a
    );
    r = normalize(r);

    float z = dot(r, n);
    oneOverPdf = M_PI * safePositiveRcp(z);

    return r;
}

uint packRandomSeed(uint textureIndex, uint2 offset)
{
    return
        (textureIndex << (BLUE_NOISE_TEXTURE_SIZE_POW * 2)) |
        (offset.y     << (BLUE_NOISE_TEXTURE_SIZE_POW    )) |
        offset.x;
}

void unpackRandomSeed(uint seed, out uint textureIndex, out uint2 offset)
{
    textureIndex = seed >> (BLUE_NOISE_TEXTURE_SIZE_POW * 2);
    offset.y     = (seed >> BLUE_NOISE_TEXTURE_SIZE_POW) & (BLUE_NOISE_TEXTURE_SIZE - 1);
    offset.x     = seed                                  & (BLUE_NOISE_TEXTURE_SIZE - 1);
}


#ifdef DESC_SET_RANDOM
[[vk::binding(BINDING_BLUE_NOISE, DESC_SET_RANDOM)]]
Texture2DArray<float4> blueNoiseTextures;

#if BLUE_NOISE_TEXTURE_SIZE_POW * 2 > 31
    #error BLUE_NOISE_TEXTURE_SIZE_POW must be lower, around 6-8
#endif

float4 rndBlueNoise8(uint seed, uint salt)
{
    uint texIndex;
    uint2 offset;
    unpackRandomSeed(seed, texIndex, offset);

    texIndex = (texIndex + salt) % BLUE_NOISE_TEXTURE_COUNT;

    return blueNoiseTextures.Load(int4((int)offset.x, (int)offset.y, (int)texIndex, 0));
}
#endif


uint wellonsLowBias32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float rnd16(uint seed, uint salt)
{
    uint rnd = wellonsLowBias32(seed + salt);
    return
        (float)(rnd & 0x0000FFFF) / (float)UINT16_MAX;
}

float2 rnd16_2(uint seed, uint salt)
{
    uint rnd = wellonsLowBias32(seed + salt);
    return float2(
        (float)(rnd & 0x0000FFFF) / (float)UINT16_MAX,
        (float)((rnd & 0xFFFF0000) >> 16) / (float)UINT16_MAX);
}

float rnd24(uint seed, uint salt)
{
    uint rnd = wellonsLowBias32(seed + salt);
    return (float)(rnd >> 8) * (1.0 / 16777216.0);
}

float4 rnd8_4(uint seed, uint salt)
{
    uint rnd = wellonsLowBias32(seed + salt);
    return float4(
        (float)(rnd & 0x000000FF) / (float)UINT8_MAX,
        (float)((rnd & 0x0000FF00) >> 8 ) / (float)UINT8_MAX,
        (float)((rnd & 0x00FF0000) >> 16) / (float)UINT8_MAX,
        (float)((rnd & 0xFF000000) >> 24) / (float)UINT8_MAX);
}

uint3 murmurHash33(uint3 src) {
    const uint M = 0x5bd1e995u;
    uint3 h = uint3(1190494759u, 2147483647u, 3559788179u);
    src *= M; src ^= src >> 24u; src *= M;
    h *= M; h ^= src.x; h *= M; h ^= src.y; h *= M; h ^= src.z;
    h ^= h >> 13u; h *= M; h ^= h >> 15u;
    return h;
}

uint getRandomSeed(const int2 pix, uint frameIndex)
{
    uint3 hash = murmurHash33(uint3((uint)pix.x, (uint)pix.y, frameIndex));

    uint2 offset = uint2(
        hash.x % BLUE_NOISE_TEXTURE_SIZE,
        hash.y % BLUE_NOISE_TEXTURE_SIZE
    );
    uint texIndex = hash.z % BLUE_NOISE_TEXTURE_COUNT;

    return packRandomSeed(texIndex, offset);
}

#endif
