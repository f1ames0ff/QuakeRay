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

#ifndef LIGHT_SHAFTS_HLSLI_
#define LIGHT_SHAFTS_HLSLI_

#define MAX_LIGHT_SHAFTS 64

struct LightShaft_BT
{
    float4 positionRadius;
    float4 direction;
    float4 color;
    float4 cone;
    float4 tangentU;
    float4 tangentV;
};

struct LightShaftVolumeParams_BT
{
    uint lightShaftCount;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 worldCenter;
    float4 worldHalfSizeInv;
    LightShaft_BT lightShafts[MAX_LIGHT_SHAFTS];
};

#endif
