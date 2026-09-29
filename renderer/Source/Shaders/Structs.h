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

#ifndef STRUCTS_H_
#define STRUCTS_H_

struct ShTriangle
{
    mat3    positions;
    mat3    prevPositions;
    mat3    normals;
    mat3x2  layerTexCoord[3];
    vec4    materialColors[3];
    uvec3   materials[3];
    uint    vertexColors[3];
    uint    geometryInstanceFlags;
    vec4    tangent;
    float   geomRoughness;
    float   geomEmission;
    float   geomMetallicity;
    uint    portalIndex;
    uint    cluster;
    uint    lightStyleIndices;
};

struct ShPayload
{
    vec2    baryCoords;
    uint    instIdAndIndex;
    uint    geomAndPrimIndex;
};

struct ShPayloadShadow
{
    uint    isShadowed;
};

struct ShHitInfo
{
    vec3    albedo;
    float   metallic;
    vec3    normal;
    float   roughness;
    vec3    normalGeom;
    float   emission;
    vec3    hitPosition;
    uint    instCustomIndex;
    uint    geometryInstanceFlags;
    uint    portalIndex;
    uint    cluster;
};

#endif
