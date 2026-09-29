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

#ifndef RAY_CONE_HLSLI_
#define RAY_CONE_HLSLI_
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

float4 getUVDerivativesFromRayCone(
    const RayCone rayCone,
    const float3 rayDir,
    const float3 worldNormal,
    const float3 vertWorldPositions[3],
    const float2 vertTexCoords[3])
{
    const float2 uv10 = vertTexCoords[1] - vertTexCoords[0];
    const float2 uv20 = vertTexCoords[2] - vertTexCoords[0];
    float quadUVArea = abs(uv10.x * uv20.y - uv20.x * uv10.y);

    const float3 edge10 = vertWorldPositions[1] - vertWorldPositions[0];
    const float3 edge20 = vertWorldPositions[2] - vertWorldPositions[0];
    const float3 faceNormal = cross(edge10, edge20);
    float quadArea = length(faceNormal);

    float normalTerm = abs(dot(rayDir, worldNormal));
    float projectedConeWidth = rayCone.width / normalTerm;
    float visibleAreaRatio = (projectedConeWidth * projectedConeWidth) / quadArea;

    float visibleUVArea = quadUVArea * visibleAreaRatio;
    float ULength = sqrt(visibleUVArea);

    return float4(ULength, 0.0, 0.0, ULength);
}

float getWaterDerivU(const RayCone rayCone, const float3 rayDir, const float3 worldNormal)
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
    const ShTriangle tri,
    const float3 worldNormal,
    const RayCone rayCone,
    const float3 rayDir)
{
    const float3 edge10 = getColumn(tri.positions, 1) - getColumn(tri.positions, 0);
    const float3 edge20 = getColumn(tri.positions, 2) - getColumn(tri.positions, 0);
    const float3 faceNormal = cross(edge10, edge20);
    float quadArea = length(faceNormal);

    float normalTerm = abs(dot(rayDir, worldNormal));
    float projectedConeWidth = rayCone.width / normalTerm;
    float visibleAreaRatio = (projectedConeWidth * projectedConeWidth) / quadArea;


    DerivativeSet derivSet;

    for (int i = 0; i < MATERIAL_MAX_ALBEDO_LAYERS; i++)
    {
        const float3x2 vertTexCoords = transpose(tri.layerTexCoord[i]);

        const float2 uv10 = vertTexCoords[1] - vertTexCoords[0];
        const float2 uv20 = vertTexCoords[2] - vertTexCoords[0];
        float quadUVArea = abs(uv10.x * uv20.y - uv20.x * uv10.y);

        float visibleUVArea = quadUVArea * visibleAreaRatio;
        float ULength = sqrt(visibleUVArea);

        derivSet.u[i] = ULength;
    }

    return derivSet;
}

float4 getTextureSampleDerivU(uint textureIndex, const float2 texCoord, const float uDeriv)
{
    return getTextureSampleGrad(textureIndex, texCoord, float2(uDeriv, 0.0), float2(0.0, uDeriv));
}

float4 getTextureSampleDerivSet(uint textureIndex, const float2 texCoord, const DerivativeSet derivSet, int index)
{
    return getTextureSampleDerivU(textureIndex, texCoord, derivSet.u[index]);
}

#endif
