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

#ifndef UTILS_H_
#define UTILS_H_



#define M_PI        3.14159265358979323846
#define UINT32_MAX  0xFFFFFFFF
#define UINT16_MAX  65535
#define UINT8_MAX   255



vec4 unpackLittleEndianUintColor(uint c)
{
    return vec4(
         (c & 0x000000FF)        / 255.0,
        ((c & 0x0000FF00) >> 8)  / 255.0,
        ((c & 0x00FF0000) >> 16) / 255.0,
        ((c & 0xFF000000) >> 24) / 255.0
    );
}

uint packLittleEndianUintColor(const vec4 c)
{
    return
        (uint(c.r * 255.0) & 0x000000FF)        |
        (uint(c.g * 255.0) & 0x000000FF) << 8   |
        (uint(c.b * 255.0) & 0x000000FF) << 16  |
        (uint(c.a * 255.0) & 0x000000FF) << 24  ;
}

float getLuminance(vec3 c)
{
    return 0.2125 * c.r + 0.7154 * c.g + 0.0721 * c.b;
}

float saturate(float a)
{
    return clamp(a, 0.0, 1.0);
}

float lengthSquared(const vec3 v)
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



struct DirectionAndLength { vec3 dir; float len; };

DirectionAndLength calcDirectionAndLength(const vec3 start, const vec3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = length(r.dir);
    r.dir /= r.len;

    return r;
}

DirectionAndLength calcDirectionAndLengthSafe(const vec3 start, const vec3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = max(length(r.dir), 0.001);
    r.dir /= r.len;

    return r;
}



#define ENCODE_NORMAL_N_PHI 1 << 16
#define ENCODE_NORMAL_N_THETA 1 << 16

uint encodeNormal(vec3 n)
{
    const uint N_phi = ENCODE_NORMAL_N_PHI;
    const uint N_theta = ENCODE_NORMAL_N_THETA;

    float phi = acos(n.z);
	float theta = atan(n.y, n.x);
    theta = theta < 0 ? theta + 2 * M_PI : theta;

    uint j = uint(round(phi * (N_phi - 1) / M_PI));
    uint k = uint(round(theta * N_theta / (2 * M_PI))) % N_theta;

    return (j << 16) | k;
}

vec3 decodeNormal(uint _packed)
{
    const uint N_phi = ENCODE_NORMAL_N_PHI;
    const uint N_theta = ENCODE_NORMAL_N_THETA;

    uint j = _packed >> 16;
    uint k = _packed & 0xFFFF;

    float phi = j * M_PI / (N_phi - 1);
    float theta = k * 2 * M_PI / N_theta;

    return vec3(
        sin(phi) * cos(theta),
        sin(phi) * sin(theta),
        cos(phi)
    );
}

vec3 safeNormalize(const vec3 v)
{
    const float len = length(v);
    return len > 0.001 ? v / len : vec3(0, 1, 0);
}




#define ENCODE_E5B9G9R9_EXPONENT_BITS 5
#define ENCODE_E5B9G9R9_MANTISSA_BITS 9
#define ENCODE_E5B9G9R9_MAX_VALID_BIASED_EXP 31
#define ENCODE_E5B9G9R9_EXP_BIAS 15

#define ENCODE_E5B9G9R9_MANTISSA_VALUES (1 << 9)
#define ENCODE_E5B9G9R9_MANTISSA_MASK (ENCODE_E5B9G9R9_MANTISSA_VALUES - 1)
#define ENCODE_E5B9G9R9_SHAREDEXP_MAX 65408

uint encodeE5B9G9R9(vec3 unpacked)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int Np2 = 1 << N;
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    unpacked = clamp(unpacked, vec3(0.0), vec3(ENCODE_E5B9G9R9_SHAREDEXP_MAX));
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
    uvec3 rgb_s = uvec3(round(unpacked * s));

    return
        (exp_shared << (3 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.b    << (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.g    << (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.r);
}

vec3 decodeE5B9G9R9(const uint _packed)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    int exp_shared = int(_packed >> (3 * ENCODE_E5B9G9R9_MANTISSA_BITS));
    float s = exp2(exp_shared - B - N);

    return s * vec3(
        (_packed                                       ) & ENCODE_E5B9G9R9_MANTISSA_MASK,
        (_packed >> (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK,
        (_packed >> (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK
    );
}



#define TANGENT_HANDEDNESS_ENCODING_CONST 19
#define TANGENT_HANDEDNESS_ENCODING_THRESHOLD 3

vec3 encodeTangent4(const vec3 tangent, float handedness)
{
    const float h = (-handedness + 1.0) * 0.5;

    return tangent.xyz * (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST);
}

vec4 decodeTangent4(const vec3 _packed)
{
    const float isUnitLen = float(dot(_packed, _packed) < TANGENT_HANDEDNESS_ENCODING_THRESHOLD);
    const float handedness = isUnitLen * 2.0 - 1.0;

    const float h = (-handedness + 1.0) * 0.5;

    return vec4(_packed / (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST), handedness);
}

#endif
