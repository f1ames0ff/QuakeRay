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

#if defined(VERTEX_PREPROCESS_PARTIAL_DYNAMIC)

    #define GET_POSITIONS getDynamicVerticesPositions
    #define GET_NORMALS getDynamicVerticesNormals
    #define SET_NORMALS setDynamicVerticesNormals
    #define INDICES dynamicIndices

#elif defined(VERTEX_PREPROCESS_PARTIAL_STATIC_ALL) || defined(VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE)

    #define GET_POSITIONS getStaticVerticesPositions
    #define GET_NORMALS getStaticVerticesNormals
    #define SET_NORMALS setStaticVerticesNormals
    #define INDICES staticIndices

#else
    #error
#endif




const int geomIndexOffset = globalUniform.instanceGeomInfoOffset[tlasInstanceIndex / 4][tlasInstanceIndex % 4];
const int geomCount = globalUniform.instanceGeomCount[tlasInstanceIndex / 4][tlasInstanceIndex % 4];

for (uint localGeomIndex = gl_LocalInvocationID.x; localGeomIndex < geomCount; localGeomIndex += gl_WorkGroupSize.x)
{
    const ShGeometryInstance inst = geometryInstances[geomIndexOffset + localGeomIndex];

#if defined(VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE)
    const bool isMovable = (inst.flags & GEOM_INST_FLAG_IS_MOVABLE) != 0;

    if (!isMovable)
    {
        continue;
    }
#endif

    const bool useIndices = inst.baseIndexIndex != UINT32_MAX;
    const bool genNormals = (inst.flags & GEOM_INST_FLAG_GENERATE_NORMALS) != 0;
    const float normalSign = float((inst.flags & GEOM_INST_FLAG_INVERTED_NORMALS) == 0) * 2.0 - 1.0;

    const mat4 model = inst.model;
    const mat3 model3 = mat3(model);


    if (useIndices)
    {
        for (uint tri = 0; tri < inst.indexCount / 3; tri++)
        {
            const uint i = inst.baseIndexIndex + tri * 3;

            const uvec3 vertexIndices = uvec3(
                inst.baseVertexIndex + INDICES[i + 0],
                inst.baseVertexIndex + INDICES[i + 1],
                inst.baseVertexIndex + INDICES[i + 2]);

            const vec3 localPos[] =
            {
                GET_POSITIONS(vertexIndices[0]),
                GET_POSITIONS(vertexIndices[1]),
                GET_POSITIONS(vertexIndices[2])
            };

            vec3 localNormal;

            if (genNormals)
            {
                localNormal = normalSign * normalize(cross(localPos[1] - localPos[0], localPos[2] - localPos[0]));

                SET_NORMALS(vertexIndices[0], localNormal);
                SET_NORMALS(vertexIndices[1], localNormal);
                SET_NORMALS(vertexIndices[2], localNormal);
            }
        }
    }
    else
    {
        for (uint tri = 0; tri < inst.vertexCount / 3; tri++)
        {
            const uint v = inst.baseVertexIndex + tri * 3;

            const uvec3 vertexIndices = uvec3(
                v + 0,
                v + 1,
                v + 2);

            const vec3 localPos[] =
            {
                GET_POSITIONS(vertexIndices[0]),
                GET_POSITIONS(vertexIndices[1]),
                GET_POSITIONS(vertexIndices[2])
            };

            vec3 localNormal;

            if (genNormals)
            {
                localNormal = normalSign * normalize(cross(localPos[1] - localPos[0], localPos[2] - localPos[0]));

                SET_NORMALS(vertexIndices[0], localNormal);
                SET_NORMALS(vertexIndices[1], localNormal);
                SET_NORMALS(vertexIndices[2], localNormal);
            }
        }
    }
}


#undef GET_POSITIONS
#undef GET_NORMALS
#undef SET_NORMALS
#undef INDICES

#undef VERTEX_PREPROCESS_PARTIAL_STATIC_ALL
#undef VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE
#undef VERTEX_PREPROCESS_PARTIAL_DYNAMIC
