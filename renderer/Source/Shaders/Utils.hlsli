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





#ifndef UTILS_HLSLI_
#define UTILS_HLSLI_
#define M_PI        3.14159265358979323846
#define UINT32_MAX  0xFFFFFFFF
#define UINT16_MAX  65535
#define UINT8_MAX   255



float4 unpackLittleEndianUintColor(uint c)
{
    return float4(
         (c & 0x000000FF)        / 255.0,
        ((c & 0x0000FF00) >> 8)  / 255.0,
        ((c & 0x00FF0000) >> 16) / 255.0,
        ((c & 0xFF000000) >> 24) / 255.0
    );
}

uint packLittleEndianUintColor(const float4 c)
{
    return
        (uint(c.r * 255.0) & 0x000000FF)        |
        (uint(c.g * 255.0) & 0x000000FF) << 8   |
        (uint(c.b * 255.0) & 0x000000FF) << 16  |
        (uint(c.a * 255.0) & 0x000000FF) << 24  ;
}

float getLuminance(float3 c)
{
    return 0.2125 * c.r + 0.7154 * c.g + 0.0721 * c.b;
}

float lengthSquared(const float3 v)
{
    return dot(v, v);
}

float safePositiveRcp(float f)
{
    return f <= 0.0 ? 0.0 : 1.0 / f;
}

float square(float x)
{
    return x * x;
}



struct DirectionAndLength { float3 dir; float len; };

DirectionAndLength calcDirectionAndLength(const float3 start, const float3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = length(r.dir);
    r.dir /= r.len;

    return r;
}

DirectionAndLength calcDirectionAndLengthSafe(const float3 start, const float3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = max(length(r.dir), 0.001);
    r.dir /= r.len;

    return r;
}



#define ENCODE_NORMAL_N_PHI 1u << 16
#define ENCODE_NORMAL_N_THETA 1u << 16

uint encodeNormal(float3 n)
{
    const uint N_phi = ENCODE_NORMAL_N_PHI;
    const uint N_theta = ENCODE_NORMAL_N_THETA;

    float phi = acos(n.z);
    float theta = atan2(n.y, n.x);
    theta = theta < 0 ? theta + 2 * M_PI : theta;

    uint j = uint(round(phi * (N_phi - 1) / M_PI));
    uint k = uint(round(theta * N_theta / (2 * M_PI))) % N_theta;

    return (j << 16) | k;
}

float3 decodeNormal(uint _packed)
{
    const uint N_phi = ENCODE_NORMAL_N_PHI;
    const uint N_theta = ENCODE_NORMAL_N_THETA;

    uint j = _packed >> 16;
    uint k = _packed & 0xFFFF;

    float phi = j * M_PI / (N_phi - 1);
    float theta = k * 2 * M_PI / N_theta;

    return float3(
        sin(phi) * cos(theta),
        sin(phi) * sin(theta),
        cos(phi)
    );
}

float3 safeNormalize(const float3 v)
{
    const float len = length(v);
    return len > 0.001 ? v / len : float3(0, 1, 0);
}




#define ENCODE_E5B9G9R9_EXPONENT_BITS 5
#define ENCODE_E5B9G9R9_MANTISSA_BITS 9
#define ENCODE_E5B9G9R9_MAX_VALID_BIASED_EXP 31
#define ENCODE_E5B9G9R9_EXP_BIAS 15

#define ENCODE_E5B9G9R9_MANTISSA_VALUES (1u << 9)
#define ENCODE_E5B9G9R9_MANTISSA_MASK (ENCODE_E5B9G9R9_MANTISSA_VALUES - 1)
#define ENCODE_E5B9G9R9_SHAREDEXP_MAX 65408

uint encodeE5B9G9R9(float3 unpacked)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int Np2 = (int)(1u << N);
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    unpacked = clamp(unpacked, (float3)0.0, (float3)ENCODE_E5B9G9R9_SHAREDEXP_MAX);
    float max_c = max(unpacked.r, max(unpacked.g, unpacked.b));

    if (max_c == 0.0)
    {
        return 0;
    }

    int exp_shared_p = max(-B-1, int(floor(log2(max_c)))) + 1 + B;
    int max_s = int(round(max_c * exp2(-(exp_shared_p - B - N))));

    int exp_shared = max_s != Np2 ?
        exp_shared_p :
        exp_shared_p + 1;

    float s = exp2(-(exp_shared - B - N));
    uint3 rgb_s = (uint3)round(unpacked * s);

    return
        (exp_shared << (3 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.b    << (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.g    << (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.r);
}

float3 decodeE5B9G9R9(const uint _packed)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    int exp_shared = int(_packed >> (3 * ENCODE_E5B9G9R9_MANTISSA_BITS));
    float s = exp2(exp_shared - B - N);

    return s * float3(
        (float)((_packed                                       ) & ENCODE_E5B9G9R9_MANTISSA_MASK),
        (float)((_packed >> (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK),
        (float)((_packed >> (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK)
    );
}



#define TANGENT_HANDEDNESS_ENCODING_CONST 19
#define TANGENT_HANDEDNESS_ENCODING_THRESHOLD 3

float3 encodeTangent4(const float3 tangent, float handedness)
{
    const float h = (-handedness + 1.0) * 0.5;

    return tangent.xyz * (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST);
}

float4 decodeTangent4(const float3 _packed)
{
    const float isUnitLen = (float)(dot(_packed, _packed) < TANGENT_HANDEDNESS_ENCODING_THRESHOLD);
    const float handedness = isUnitLen * 2.0 - 1.0;

    const float h = (-handedness + 1.0) * 0.5;

    return float4(_packed / (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST), handedness);
}

#endif
