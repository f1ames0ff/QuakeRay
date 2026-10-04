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





#ifndef SPHERICAL_HARMONICS_HLSLI_
#define SPHERICAL_HARMONICS_HLSLI_
struct SH
{
    float4 r;
    float4 g;
    float4 b;
};

SH newSH()
{
    SH sh;
    sh.r = sh.g = sh.b = (float4)0.0;

    return sh;
}

SH irradianceToSH(const float3 color, const float3 dir)
{
    const float4 shBasis = float4(
        0.282095,
        0.488603 * dir.y,
        0.488603 * dir.z,
        0.488603 * dir.x
    );

    SH sh;

    sh.r = shBasis * color.r;
    sh.g = shBasis * color.g;
    sh.b = shBasis * color.b;

    return sh;
}

float3 SHToIrradiance(const SH sh, const float3 normal)
{
    float A_hat_0 = 3.141593;
    float A_hat_1 = 2.094395;

    float Y_0_0 = 0.282095;
    float Y_1m1 = 0.488603 * normal.y;
    float Y_1_0 = 0.488603 * normal.z;
    float Y_1_1 = 0.488603 * normal.x;

    float3 L_0_0 = float3(sh.r[0], sh.g[0], sh.b[0]);
    float3 L_1m1 = float3(sh.r[1], sh.g[1], sh.b[1]);
    float3 L_1_0 = float3(sh.r[2], sh.g[2], sh.b[2]);
    float3 L_1_1 = float3(sh.r[3], sh.g[3], sh.b[3]);

    return A_hat_0 * L_0_0 * Y_0_0 +
           A_hat_1 * L_1m1 * Y_1m1 +
           A_hat_1 * L_1_0 * Y_1_0 +
           A_hat_1 * L_1_1 * Y_1_1;
}

float3 getSHColor(const SH sh)
{
    return float3(
        sh.r[0] / 0.282095,
        sh.g[0] / 0.282095,
        sh.b[0] / 0.282095
    );
}

void accumulateSH(inout SH x, const SH y, const float a)
{
    x.r += y.r * a;
    x.g += y.g * a;
    x.b += y.b * a;
}

SH mixSH(const SH x, const SH y, const float a)
{
    SH sh;
    sh.r = lerp(x.r, y.r, a);
    sh.g = lerp(x.g, y.g, a);
    sh.b = lerp(x.b, y.b, a);

    return sh;
}

void multiplySH(inout SH x, float a)
{
    x.r *= a;
    x.g *= a;
    x.b *= a;
}

#endif
