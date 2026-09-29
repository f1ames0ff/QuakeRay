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



#ifndef STRUCTS_HLSLI_
#define STRUCTS_HLSLI_
struct ShTriangle
{
    float3x3 positions;
    float3x3 prevPositions;
    float3x3 normals;
    float2x3 layerTexCoord[3];
    float4   materialColors[3];
    uint3    materials[3];
    uint     vertexColors[3];
    uint     geometryInstanceFlags;
    float4   tangent;
    float     geomRoughness;
    float     geomEmission;
    float     geomMetallicity;
    uint     portalIndex;
    uint     cluster;
    uint     lightStyleIndices;
};

struct ShPayload
{
    float2 baryCoords;
    uint   instIdAndIndex;
    uint   geomAndPrimIndex;
};

struct ShPayloadShadow
{
    uint isShadowed;
};

struct ShHitInfo
{
    float3 albedo;
    float  metallic;
    float3 normal;
    float  roughness;
    float3 normalGeom;
    float  emission;
    float3 hitPosition;
    uint   instCustomIndex;
    uint   geometryInstanceFlags;
    uint   portalIndex;
    uint   cluster;
};

#endif
