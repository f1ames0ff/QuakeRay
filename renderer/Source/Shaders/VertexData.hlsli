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


#ifndef VERTEX_DATA_HLSLI_
#define VERTEX_DATA_HLSLI_
#ifdef DESC_SET_GLOBAL_UNIFORM
#ifdef DESC_SET_VERTEX_DATA

#ifdef VERTEX_BUFFER_WRITEABLE
[[vk::binding(BINDING_VERTEX_BUFFER_STATIC, DESC_SET_VERTEX_DATA)]] RWStructuredBuffer<ShVertex> g_staticVertices;
#else
[[vk::binding(BINDING_VERTEX_BUFFER_STATIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<ShVertex> g_staticVertices;
#endif

#ifdef VERTEX_BUFFER_WRITEABLE
[[vk::binding(BINDING_VERTEX_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] RWStructuredBuffer<ShVertex> g_dynamicVertices;
#else
[[vk::binding(BINDING_VERTEX_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<ShVertex> g_dynamicVertices;
#endif

[[vk::binding(BINDING_INDEX_BUFFER_STATIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<uint> staticIndices;

[[vk::binding(BINDING_INDEX_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<uint> dynamicIndices;

[[vk::binding(BINDING_GEOMETRY_INSTANCES, DESC_SET_VERTEX_DATA)]] StructuredBuffer<ShGeometryInstance> geometryInstances;

[[vk::binding(BINDING_GEOMETRY_INSTANCES_MATCH_PREV, DESC_SET_VERTEX_DATA)]] StructuredBuffer<int> geomIndexPrevToCur;

#ifdef VERTEX_BUFFER_WRITEABLE
[[vk::binding(BINDING_PREV_POSITIONS_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] RWStructuredBuffer<ShVertex> g_dynamicVertices_Prev;
#else
[[vk::binding(BINDING_PREV_POSITIONS_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<ShVertex> g_dynamicVertices_Prev;
#endif

#ifdef VERTEX_BUFFER_WRITEABLE
[[vk::binding(BINDING_PREV_INDEX_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] RWStructuredBuffer<uint> prevDynamicIndices;
#else
[[vk::binding(BINDING_PREV_INDEX_BUFFER_DYNAMIC, DESC_SET_VERTEX_DATA)]] StructuredBuffer<uint> prevDynamicIndices;
#endif

float3 getStaticVerticesPositions(uint index)
{
    return g_staticVertices[index].position.xyz;
}

float3 getStaticVerticesNormals(uint index)
{
    return g_staticVertices[index].normal.xyz;
}

float3 getDynamicVerticesPositions(uint index)
{
    return g_dynamicVertices[index].position.xyz;
}

float3 getDynamicVerticesNormals(uint index)
{
    return g_dynamicVertices[index].normal.xyz;
}

#ifdef VERTEX_BUFFER_WRITEABLE
void setStaticVerticesNormals(uint index, float3 value)
{
    g_staticVertices[index].normal = float4(value, 0.0);
}

void setDynamicVerticesNormals(uint index, float3 value)
{
    g_dynamicVertices[index].normal = float4(value, 0.0);
}
#endif

uint3 getVertIndicesStatic(uint baseVertexIndex, uint baseIndexIndex, uint primitiveId)
{
    if (baseIndexIndex != UINT32_MAX)
    {
        return uint3(
            baseVertexIndex + staticIndices[baseIndexIndex + primitiveId * 3 + 0],
            baseVertexIndex + staticIndices[baseIndexIndex + primitiveId * 3 + 1],
            baseVertexIndex + staticIndices[baseIndexIndex + primitiveId * 3 + 2]);
    }
    else
    {
        return uint3(
            baseVertexIndex + primitiveId * 3 + 0,
            baseVertexIndex + primitiveId * 3 + 1,
            baseVertexIndex + primitiveId * 3 + 2);
    }
}

uint3 getVertIndicesDynamic(uint baseVertexIndex, uint baseIndexIndex, uint primitiveId)
{
    if (baseIndexIndex != UINT32_MAX)
    {
        return uint3(
            baseVertexIndex + dynamicIndices[baseIndexIndex + primitiveId * 3 + 0],
            baseVertexIndex + dynamicIndices[baseIndexIndex + primitiveId * 3 + 1],
            baseVertexIndex + dynamicIndices[baseIndexIndex + primitiveId * 3 + 2]);
    }
    else
    {
        return uint3(
            baseVertexIndex + primitiveId * 3 + 0,
            baseVertexIndex + primitiveId * 3 + 1,
            baseVertexIndex + primitiveId * 3 + 2);
    }
}

uint3 getPrevVertIndicesDynamic(uint prevBaseVertexIndex, uint prevBaseIndexIndex, uint primitiveId)
{
    if (prevBaseIndexIndex != UINT32_MAX)
    {
        return uint3(
            prevBaseVertexIndex + prevDynamicIndices[prevBaseIndexIndex + primitiveId * 3 + 0],
            prevBaseVertexIndex + prevDynamicIndices[prevBaseIndexIndex + primitiveId * 3 + 1],
            prevBaseVertexIndex + prevDynamicIndices[prevBaseIndexIndex + primitiveId * 3 + 2]);
    }
    else
    {
        return uint3(
            prevBaseVertexIndex + primitiveId * 3 + 0,
            prevBaseVertexIndex + primitiveId * 3 + 1,
            prevBaseVertexIndex + primitiveId * 3 + 2);
    }
}

float3 getPrevDynamicVerticesPositions(uint index)
{
    return g_dynamicVertices_Prev[index].position.xyz;
}

float4 getTangent(const float3x3 localPos, const float3 normal, const float2x3 texCoord)
{
    const float3 e1 = transpose(localPos)[1] - transpose(localPos)[0];
    const float3 e2 = transpose(localPos)[2] - transpose(localPos)[0];

    const float3x2 tc = transpose(texCoord);
    const float2 u1 = tc[1] - tc[0];
    const float2 u2 = tc[2] - tc[0];

    const float invDet = 1.0 / (u1.x * u2.y - u2.x * u1.y);

    const float3 tangent   = normalize((e1 * u2.y - e2 * u1.y) * invDet);
    const float3 bitangent = normalize((e2 * u1.x - e1 * u2.x) * invDet);

    float handedness = float(dot(cross(normal, tangent), bitangent) > 0.0);
    handedness = handedness * 2.0 - 1.0;

    return float4(tangent, handedness);
}

ShTriangle makeTriangle(const ShVertex a, const ShVertex b, const ShVertex c)
{
    ShTriangle tr;

    tr.positions = transpose(float3x3(a.position.xyz, b.position.xyz, c.position.xyz));

    tr.normals = transpose(float3x3(a.normal.xyz, b.normal.xyz, c.normal.xyz));

    tr.layerTexCoord[0][0] = float3(a.texCoord.x, b.texCoord.x, c.texCoord.x);
    tr.layerTexCoord[0][1] = float3(a.texCoord.y, b.texCoord.y, c.texCoord.y);

    tr.layerTexCoord[1][0] = float3(a.texCoordLayer1.x, b.texCoordLayer1.x, c.texCoordLayer1.x);
    tr.layerTexCoord[1][1] = float3(a.texCoordLayer1.y, b.texCoordLayer1.y, c.texCoordLayer1.y);

    tr.layerTexCoord[2][0] = float3(a.texCoordLayer2.x, b.texCoordLayer2.x, c.texCoordLayer2.x);
    tr.layerTexCoord[2][1] = float3(a.texCoordLayer2.y, b.texCoordLayer2.y, c.texCoordLayer2.y);

    tr.cluster = a.cluster;
    tr.lightStyleIndices = a.lightStyles;

    tr.tangent = getTangent(tr.positions, safeNormalize(transpose(tr.normals)[0] + transpose(tr.normals)[1] + transpose(tr.normals)[2]), tr.layerTexCoord[0]);

    return tr;
}

int getGeometryIndex(int instanceID, int localGeometryIndex)
{
    return globalUniform.instanceGeomInfoOffset[instanceID / 4][instanceID % 4] + localGeometryIndex;
}

bool getCurrentGeometryIndexByPrev(int prevInstanceID, int prevLocalGeometryIndex, out int curFrameGlobalGeomIndex)
{
    const int prevFrameGeomIndex = globalUniform.instanceGeomInfoOffsetPrev[prevInstanceID / 4][prevInstanceID % 4] + prevLocalGeometryIndex;

    curFrameGlobalGeomIndex = geomIndexPrevToCur[prevFrameGeomIndex];

    return curFrameGlobalGeomIndex != UINT32_MAX;
}



ShTriangle getTriangle(int instanceID, int instanceCustomIndex, int localGeometryIndex, int primitiveId)
{
    ShTriangle tr;

    const int globalGeometryIndex = getGeometryIndex(instanceID, localGeometryIndex);
    const ShGeometryInstance inst = geometryInstances[globalGeometryIndex];

    const bool isDynamic = (instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC) == INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC;

    if (isDynamic)
    {
        {
            const uint3 vertIndices = getVertIndicesDynamic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

            tr = makeTriangle(
                g_dynamicVertices[vertIndices[0]],
                g_dynamicVertices[vertIndices[1]],
                g_dynamicVertices[vertIndices[2]]);
        }

        tr.positions = transpose(float3x3(
            mul(inst.model, float4(transpose(tr.positions)[0], 1.0)).xyz,
            mul(inst.model, float4(transpose(tr.positions)[1], 1.0)).xyz,
            mul(inst.model, float4(transpose(tr.positions)[2], 1.0)).xyz));

        const bool hasPrevInfo = inst.prevBaseVertexIndex != UINT32_MAX;

        if (hasPrevInfo)
        {
            const uint3 prevVertIndices = getPrevVertIndicesDynamic(inst.prevBaseVertexIndex, inst.prevBaseIndexIndex, primitiveId);

            const float4 prevLocalPos[3] =
            {
                float4(getPrevDynamicVerticesPositions(prevVertIndices[0]), 1.0),
                float4(getPrevDynamicVerticesPositions(prevVertIndices[1]), 1.0),
                float4(getPrevDynamicVerticesPositions(prevVertIndices[2]), 1.0)
            };

            tr.prevPositions = transpose(float3x3(
                mul(inst.prevModel, prevLocalPos[0]).xyz,
                mul(inst.prevModel, prevLocalPos[1]).xyz,
                mul(inst.prevModel, prevLocalPos[2]).xyz));
        }
        else
        {
            tr.prevPositions = tr.positions;
        }
    }
    else
    {
        {
            const uint3 vertIndices = getVertIndicesStatic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

            tr = makeTriangle(
                g_staticVertices[vertIndices[0]],
                g_staticVertices[vertIndices[1]],
                g_staticVertices[vertIndices[2]]);
        }

        const float4 prevLocalPos[3] =
        {
            float4(transpose(tr.positions)[0], 1.0),
            float4(transpose(tr.positions)[1], 1.0),
            float4(transpose(tr.positions)[2], 1.0)
        };

        tr.positions = transpose(float3x3(
            mul(inst.model, prevLocalPos[0]).xyz,
            mul(inst.model, prevLocalPos[1]).xyz,
            mul(inst.model, prevLocalPos[2]).xyz));

        const bool isMovable = (inst.flags & GEOM_INST_FLAG_IS_MOVABLE) != 0;
        const bool hasPrevInfo = inst.prevBaseVertexIndex != UINT32_MAX;

        if (isMovable && hasPrevInfo)
        {
            tr.prevPositions = transpose(float3x3(
                mul(inst.prevModel, prevLocalPos[0]).xyz,
                mul(inst.prevModel, prevLocalPos[1]).xyz,
                mul(inst.prevModel, prevLocalPos[2]).xyz));
        }
        else
        {
            tr.prevPositions = tr.positions;
        }
    }


    tr.materials[0] = uint3(inst.materials0A, inst.materials0B, inst.materials0C);
    tr.materials[1] = uint3(inst.materials1A, inst.materials1B, MATERIAL_NO_TEXTURE);
    tr.materials[2] = uint3(inst.materials2A, inst.materials2B, MATERIAL_NO_TEXTURE);

    tr.materialColors[0] = inst.materialColors[0];
    tr.materialColors[1] = inst.materialColors[1];
    tr.materialColors[2] = inst.materialColors[2];


    const float3x3 model3 = (float3x3)inst.model;

    tr.normals = transpose(float3x3(
        mul(model3, transpose(tr.normals)[0]),
        mul(model3, transpose(tr.normals)[1]),
        mul(model3, transpose(tr.normals)[2])));
    tr.tangent.xyz = mul(model3, tr.tangent.xyz);


    tr.geometryInstanceFlags = inst.flags;

    tr.geomRoughness = inst.defaultRoughness;
    tr.geomMetallicity = inst.defaultMetallicity;

    tr.geomEmission = inst.defaultEmission;

    tr.portalIndex = inst.portalIndex;

    return tr;
}

float3x3 getOnlyCurPositions(int globalGeometryIndex, int instanceCustomIndex, int primitiveId)
{
    float3x3 positions;

    const ShGeometryInstance inst = geometryInstances[globalGeometryIndex];

    const bool isDynamic = (instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC) == INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC;

    if (isDynamic)
    {
        const uint3 vertIndices = getVertIndicesDynamic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

        positions = transpose(float3x3(
            mul(inst.model, float4(getDynamicVerticesPositions(vertIndices[0]), 1.0)).xyz,
            mul(inst.model, float4(getDynamicVerticesPositions(vertIndices[1]), 1.0)).xyz,
            mul(inst.model, float4(getDynamicVerticesPositions(vertIndices[2]), 1.0)).xyz));
    }
    else
    {
        const uint3 vertIndices = getVertIndicesStatic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

        positions = transpose(float3x3(
            mul(inst.model, float4(getStaticVerticesPositions(vertIndices[0]), 1.0)).xyz,
            mul(inst.model, float4(getStaticVerticesPositions(vertIndices[1]), 1.0)).xyz,
            mul(inst.model, float4(getStaticVerticesPositions(vertIndices[2]), 1.0)).xyz));
    }

    return positions;
}

float3x3 getOnlyPrevPositions(int globalGeometryIndex, int instanceCustomIndex, int primitiveId)
{
    float3x3 prevPositions;

    const ShGeometryInstance inst = geometryInstances[globalGeometryIndex];

    const bool isDynamic = (instanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC) == INSTANCE_CUSTOM_INDEX_FLAG_DYNAMIC;

    if (isDynamic)
    {
        const bool hasPrevInfo = inst.prevBaseVertexIndex != UINT32_MAX;

        if (hasPrevInfo)
        {
            const uint3 prevVertIndices = getPrevVertIndicesDynamic(inst.prevBaseVertexIndex, inst.prevBaseIndexIndex, primitiveId);

            const float4 prevLocalPos[3] =
            {
                float4(getPrevDynamicVerticesPositions(prevVertIndices[0]), 1.0),
                float4(getPrevDynamicVerticesPositions(prevVertIndices[1]), 1.0),
                float4(getPrevDynamicVerticesPositions(prevVertIndices[2]), 1.0)
            };

            prevPositions = transpose(float3x3(
                mul(inst.prevModel, prevLocalPos[0]).xyz,
                mul(inst.prevModel, prevLocalPos[1]).xyz,
                mul(inst.prevModel, prevLocalPos[2]).xyz));
        }
        else
        {
            const uint3 vertIndices = getVertIndicesDynamic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

            const float4 localPos[3] =
            {
                float4(getDynamicVerticesPositions(vertIndices[0]), 1.0),
                float4(getDynamicVerticesPositions(vertIndices[1]), 1.0),
                float4(getDynamicVerticesPositions(vertIndices[2]), 1.0)
            };

            prevPositions = transpose(float3x3(
                mul(inst.model, localPos[0]).xyz,
                mul(inst.model, localPos[1]).xyz,
                mul(inst.model, localPos[2]).xyz));
        }
    }
    else
    {
        const uint3 vertIndices = getVertIndicesStatic(inst.baseVertexIndex, inst.baseIndexIndex, primitiveId);

        const float4 localPos[3] =
        {
            float4(getStaticVerticesPositions(vertIndices[0]), 1.0),
            float4(getStaticVerticesPositions(vertIndices[1]), 1.0),
            float4(getStaticVerticesPositions(vertIndices[2]), 1.0)
        };

        const bool isMovable = (inst.flags & GEOM_INST_FLAG_IS_MOVABLE) != 0;
        const bool hasPrevInfo = inst.prevBaseVertexIndex != UINT32_MAX;

        if (isMovable && hasPrevInfo)
        {
            prevPositions = transpose(float3x3(
                mul(inst.prevModel, localPos[0]).xyz,
                mul(inst.prevModel, localPos[1]).xyz,
                mul(inst.prevModel, localPos[2]).xyz));
        }
        else
        {
            prevPositions = transpose(float3x3(
                mul(inst.model, localPos[0]).xyz,
                mul(inst.model, localPos[1]).xyz,
                mul(inst.model, localPos[2]).xyz));
        }
    }

    return prevPositions;
}

float4 packVisibilityBuffer(const ShPayload p)
{
    return float4(asfloat(p.instIdAndIndex), asfloat(p.geomAndPrimIndex), p.baryCoords);
}

int unpackInstCustomIndexFromVisibilityBuffer(const float4 v)
{
    int instanceID, instCustomIndex;
    unpackInstanceIdAndCustomIndex(asuint(v[0]), instanceID, instCustomIndex);

    return instCustomIndex;
}

void unpackVisibilityBuffer(
    const float4 v,
    out int instanceID, out int instCustomIndex,
    out int localGeomIndex, out int primIndex,
    out float2 bary)
{
    unpackInstanceIdAndCustomIndex(asuint(v[0]), instanceID, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(asuint(v[1]), localGeomIndex, primIndex);
    bary = float2(v[2], v[3]);
}

bool unpackPrevVisibilityBuffer(const float4 v, out float3 prevPos)
{
    int prevInstanceID, instCustomIndex;
    int prevLocalGeomIndex, primIndex;

    unpackInstanceIdAndCustomIndex(asuint(v[0]), prevInstanceID, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(asuint(v[1]), prevLocalGeomIndex, primIndex);

    int curFrameGlobalGeomIndex;
    const bool matched = getCurrentGeometryIndexByPrev(prevInstanceID, prevLocalGeomIndex, curFrameGlobalGeomIndex);

    if (!matched)
    {
        return false;
    }

    const float3x3 prevVerts = getOnlyCurPositions(curFrameGlobalGeomIndex, instCustomIndex, primIndex);
    const float3 baryCoords = float3(1.0 - v[2] - v[3], v[2], v[3]);

    prevPos = mul(prevVerts, baryCoords);

    return true;
}

float4x4 getModelMatrix(int instanceID, int localGeometryIndex)
{
    int globalGeometryIndex = getGeometryIndex(instanceID, localGeometryIndex);
    return geometryInstances[globalGeometryIndex].model;
}
#endif
#endif

#endif
