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

#ifndef RAY_CONE_H_
#define RAY_CONE_H_


struct RayCone
{
    float width;
    float spreadAngle;
};

void propagateRayCone(inout RayCone c, float rayLength)
{
    c.width	+= c.spreadAngle * rayLength;
    c.spreadAngle *= 2;
}

vec4 getUVDerivativesFromRayCone(
    const RayCone rayCone,
    const vec3 rayDir,
    const vec3 worldNormal,
    const vec3 vertWorldPositions[3],
    const vec2 vertTexCoords[3])
{
    const vec2 uv10 = vertTexCoords[1] - vertTexCoords[0];
    const vec2 uv20 = vertTexCoords[2] - vertTexCoords[0];
    float quadUVArea = abs(uv10.x * uv20.y - uv20.x * uv10.y);

    const vec3 edge10 = vertWorldPositions[1] - vertWorldPositions[0];
    const vec3 edge20 = vertWorldPositions[2] - vertWorldPositions[0];
    const vec3 faceNormal = cross(edge10, edge20);
    float quadArea = length(faceNormal);

    float normalTerm = abs(dot(rayDir, worldNormal));
    float projectedConeWidth = rayCone.width / normalTerm;
    float visibleAreaRatio = (projectedConeWidth * projectedConeWidth) / quadArea;

    float visibleUVArea = quadUVArea * visibleAreaRatio;
    float ULength = sqrt(visibleUVArea);

    return vec4(ULength, 0, 0, ULength);
}

float getWaterDerivU(const RayCone rayCone, const vec3 rayDir, const vec3 worldNormal)
{
    const float quadArea = 1.0;
    const float quadUVArea = 1.0;

    float normalTerm = abs(dot(rayDir, worldNormal));
    float projectedConeWidth = rayCone.width / normalTerm;
    float visibleAreaRatio = (projectedConeWidth * projectedConeWidth) / quadArea;

    float visibleUVArea = quadUVArea * visibleAreaRatio;
    float ULength = sqrt(visibleUVArea);

    return ULength;
}

struct DerivativeSet
{
    float u[3];
};

DerivativeSet getTriangleUVDerivativesFromRayCone(
    const ShTriangle triangle,
    const vec3 worldNormal,
    const RayCone rayCone,
    const vec3 rayDir)
{
    const vec3 edge10 = triangle.positions[1] - triangle.positions[0];
    const vec3 edge20 = triangle.positions[2] - triangle.positions[0];
    const vec3 faceNormal = cross(edge10, edge20);
    float quadArea = length(faceNormal);

    float normalTerm = abs(dot(rayDir, worldNormal));
    float projectedConeWidth = rayCone.width / normalTerm;
    float visibleAreaRatio = (projectedConeWidth * projectedConeWidth) / quadArea;


    DerivativeSet derivSet;

    for (int i = 0; i < MATERIAL_MAX_ALBEDO_LAYERS; i++)
    {
        const mat3x2 vertTexCoords = triangle.layerTexCoord[i];

        const vec2 uv10 = vertTexCoords[1] - vertTexCoords[0];
        const vec2 uv20 = vertTexCoords[2] - vertTexCoords[0];
        float quadUVArea = abs(uv10.x * uv20.y - uv20.x * uv10.y);

        float visibleUVArea = quadUVArea * visibleAreaRatio;
        float ULength = sqrt(visibleUVArea);

        derivSet.u[i] = ULength;
    }

    return derivSet;
}

vec4 getTextureSampleDerivU(uint textureIndex, const vec2 texCoord, const float uDeriv)
{
    return textureGrad(sampler2D(getTexture(textureIndex), getTextureSampler(textureIndex)), texCoord, vec2(uDeriv, 0), vec2(0, uDeriv));
}

vec4 getTextureSampleDerivSet(uint textureIndex, const vec2 texCoord, const DerivativeSet derivSet, int index)
{
    return getTextureSampleDerivU(textureIndex, texCoord, derivSet.u[index]);
}

#endif
