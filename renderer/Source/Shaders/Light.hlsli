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

#ifndef LIGHT_HLSLI_
#define LIGHT_HLSLI_
#include "Random.hlsli"

struct DirectionalLight
{
    float3 direction;
    float angularRadius;
    float3 color;
};

struct SphereLight
{
    float3 center;
    float radius;
    float3 color;
    float3 normal;
};

struct TriangleLight
{
    float3 position[3];
    float3 normal;
    float area;
    float3 color;
};

#define MAX_TEXTURED_AREA_LIGHT_VERTS 8

struct TexturedAreaLight
{
    float3 A;
    float3 B;
    float3 C;
    float3 normal;
    float area;
    float textureIndex;
    float meanEmiss;
    float coneCosInner;
    float coneCosOuter;
    float projector;
    int numVerts;
    float2 uvVerts[MAX_TEXTURED_AREA_LIGHT_VERTS];
    float3 color;
};

struct SpotLight
{
    float3 center;
    float radius;
    float3 direction;
    float cosAngleInner;
    float3 color;
    float cosAngleOuter;
};

DirectionalLight decodeAsDirectionalLight(const ShLightEncoded encoded)
{
    DirectionalLight l;
    l.direction = encoded.data_0.xyz;
    l.angularRadius = encoded.data_0.w;
    l.color = encoded.color;

    return l;
}

SphereLight decodeAsSphereLight(const ShLightEncoded encoded)
{
    SphereLight l;
    l.center = encoded.data_0.xyz;
    l.radius = encoded.data_0.w;
    l.color = encoded.color;
    l.normal = encoded.data_1.xyz;

    return l;
}

TriangleLight decodeAsTriangleLight(const ShLightEncoded encoded)
{
    TriangleLight l;
    l.position[0] = encoded.data_0.xyz;
    l.position[1] = encoded.data_1.xyz;
    l.position[2] = encoded.data_2.xyz;
    l.color = encoded.color;

    l.normal = float3(
        encoded.data_0.w,
        encoded.data_1.w,
        encoded.data_2.w
    );
    float len = length(l.normal);
    l.normal /= len;
    l.area = len * 0.5;

    return l;
}

TexturedAreaLight decodeAsTexturedAreaLight(const ShLightEncoded encoded)
{
    TexturedAreaLight l;
    l.A = encoded.data_0.xyz;
    l.textureIndex = encoded.data_0.w;
    l.B = encoded.data_1.xyz;
    l.meanEmiss = encoded.data_1.w;
    l.C = encoded.data_2.xyz;
    l.numVerts = clamp((int)encoded.data_2.w, 0, MAX_TEXTURED_AREA_LIGHT_VERTS);
    l.uvVerts[0] = encoded.data_3.xy;
    l.uvVerts[1] = encoded.data_3.zw;
    l.uvVerts[2] = encoded.data_4.xy;
    l.uvVerts[3] = encoded.data_4.zw;
    l.uvVerts[4] = encoded.data_5.xy;
    l.uvVerts[5] = encoded.data_5.zw;
    l.uvVerts[6] = encoded.data_6.xy;
    l.uvVerts[7] = encoded.data_6.zw;
    l.normal = encoded.data_7.xyz;
    l.area = encoded.data_7.w;
    l.color = encoded.color;
    l.coneCosInner = encoded.coneCosInner;
    l.coneCosOuter = encoded.coneCosOuter;
    l.projector = encoded.projector;

    return l;
}

SpotLight decodeAsSpotLight(const ShLightEncoded encoded)
{
    SpotLight l;
    l.center = encoded.data_0.xyz;
    l.radius = encoded.data_0.w;
    l.direction = encoded.data_1.xyz;
    l.color = encoded.color;
    l.cosAngleInner = encoded.data_2.x;
    l.cosAngleOuter = encoded.data_2.y;

    return l;
}

float getPolySpotFactor(const float3 lightNormal, const float3 lightToSurf)
{
    float ll = max(dot(lightNormal, lightToSurf), 0.0);
    return pow(ll, globalUniform.polyLightSpotlightFactor);
}

float getSpotFactor(float cosA, float cosAInner, float cosAOuter)
{
    return square(smoothstep(cosAOuter, cosAInner, cosA));
}

float isSphereInFront(const float3 planeNormal, const float3 planePos, const float3 sphereCenter, float sphereRadius)
{
    return (float)(dot(planeNormal, sphereCenter - planePos) > -sphereRadius);
}



float getGeometryFactor(const float3 lightNormal, const float3 lightToSurface, float surfaceToLightDistance)
{
    return abs(dot(lightNormal, lightToSurface)) / square(surfaceToLightDistance);
}
float getGeometryFactorClamped(const float3 lightNormal, const float3 lightToSurface, float surfaceToLightDistance)
{
    return max(0.0, dot(lightNormal, lightToSurface)) * safePositiveRcp(square(surfaceToLightDistance));
}

float safeSolidAngle(float a)
{
    return a > 0.0 && !isnan(a) && !isinf(a) ? clamp(a, 0.0, 4.0 * M_PI) : 0.0;
}

float calcSolidAngleForSphere(float sphereRadius, float distanceToSphereCenter)
{
    float sinTheta = sphereRadius / max(sphereRadius, distanceToSphereCenter);
    float cosTheta = sqrt(1.0 - sinTheta * sinTheta);
    return safeSolidAngle(2 * M_PI * (1.0 - cosTheta));
}

float calcSolidAngleForArea(float area, const float3 areaPosition, const float3 areaNormal, const float3 surfPosition)
{
    const DirectionAndLength areaLightToSurf = calcDirectionAndLength(areaPosition, surfPosition);
    return safeSolidAngle(area * getGeometryFactor(areaNormal, areaLightToSurf.dir, areaLightToSurf.len));
}



float getLightColorWeight(const float3 color)
{
    return clamp(getLuminance(color) * 0.1 + 0.9, 1.0, 10.0);
}

float getDirectionalLightWeight(const SphereLight l, const float3 cellCenter, float cellRadius)
{
    return
        getLightColorWeight(l.color);
}

float getSphereLightWeight(const SphereLight l, const float3 cellCenter, float cellRadius)
{
    return
        getLightColorWeight(l.color) *
        calcSolidAngleForSphere(l.radius, max(length(l.center - cellCenter), cellRadius));
}

float getTriangleLightWeight(const TriangleLight l, const float3 cellCenter, float cellRadius)
{
    const float3 triCenter =
        l.position[0] / 3.0 +
        l.position[1] / 3.0 +
        l.position[2] / 3.0;

    const float aprxTriRadius =
        length(l.position[0] - triCenter) / 3.0 +
        length(l.position[1] - triCenter) / 3.0 +
        length(l.position[2] - triCenter) / 3.0;

    return
        getLightColorWeight(l.color) *
        calcSolidAngleForSphere(aprxTriRadius, max(length(triCenter - cellCenter), cellRadius)) *
        isSphereInFront(l.normal, triCenter, cellCenter, cellRadius);
}

float3 texturedAreaLightWorldPos(const TexturedAreaLight l, const float2 uv)
{
    return l.C + l.A * uv.x + l.B * uv.y;
}

float3 getTexturedAreaLightCenter(const TexturedAreaLight l)
{
    float2 uvCenter = (float2)0.0;
    for (int i = 0; i < l.numVerts; i++)
    {
        uvCenter += l.uvVerts[i];
    }
    uvCenter /= max((float)l.numVerts, 1.0);
    return texturedAreaLightWorldPos(l, uvCenter);
}

float2 sampleConvexPolygon(const float2 verts[MAX_TEXTURED_AREA_LIGHT_VERTS], int numVerts, float u1, float u2)
{
    if (numVerts < 3)
    {
        return verts[0];
    }

    float triArea[MAX_TEXTURED_AREA_LIGHT_VERTS - 2];
    float totalArea = 0.0;
    for (int i = 0; i < numVerts - 2; i++)
    {
        const float2 e1 = verts[i + 1] - verts[0];
        const float2 e2 = verts[i + 2] - verts[0];
        triArea[i] = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);
        totalArea += triArea[i];
    }
    totalArea = max(totalArea, 1e-8);

    float r = u1 * totalArea;
    int t = numVerts - 3;
    float acc = 0.0;
    for (int i = 0; i < numVerts - 2; i++)
    {
        acc += triArea[i];
        if (r <= acc)
        {
            t = i;
            break;
        }
    }

    const float accBefore = acc - triArea[t];
    const float uTri = clamp((r - accBefore) / max(triArea[t], 1e-8), 0.0, 1.0);

    const float beta  = 1.0 - sqrt(uTri);
    const float gamma = (1.0 - beta) * u2;
    const float alpha = 1.0 - beta - gamma;

    return alpha * verts[0] + beta * verts[t + 1] + gamma * verts[t + 2];
}

float getTexturedAreaLightWeight(const TexturedAreaLight l, const float3 cellCenter, float cellRadius)
{
    const float3 center = getTexturedAreaLightCenter(l);

    float aprxRadius = 0.0;
    for (int i = 0; i < l.numVerts; i++)
    {
        aprxRadius = max(aprxRadius, length(texturedAreaLightWorldPos(l, l.uvVerts[i]) - center));
    }

    return
        getLightColorWeight(l.color) * l.meanEmiss *
        calcSolidAngleForSphere(aprxRadius, max(length(center - cellCenter), cellRadius)) *
        isSphereInFront(l.normal, center, cellCenter, cellRadius);
}

float getSpotLightWeight(const SpotLight l, const float3 cellCenter, float cellRadius)
{
    return
        getLightColorWeight(l.color) *
        calcSolidAngleForSphere(l.radius, max(length(l.center - cellCenter), cellRadius)) *
        isSphereInFront(l.direction, l.center, cellCenter, cellRadius);
}




struct LightSample
{
    float3 position;
    float3 color;
    float dw;
};

LightSample emptyLightSample()
{
    LightSample r;
    r.position = (float3)0.0;
    r.color = (float3)0.0;
    r.dw = 0;
    return r;
}

LightSample sampleDirectionalLight(const DirectionalLight l, const float3 surfPosition, const float2 pointRnd)
{
    float3 lightNormal;
    {
        const float diskRadiusAtUnit = sin(max(0.01, l.angularRadius));
        const float2 disk = sampleDisk(diskRadiusAtUnit, pointRnd.x, pointRnd.y);
        const float3x3 basis = getONB(l.direction);

        lightNormal = normalize(l.direction + getColumn(basis, 0) * disk.x + getColumn(basis, 1) * disk.y);
    }

    LightSample r;
    r.position = surfPosition - lightNormal * MAX_RAY_LENGTH;
    r.color = l.color;
    r.dw = 1.0;

    return r;
}

LightSample sampleSphereLight(const SphereLight l, const float3 surfPosition, const float2 pointRnd)
{
    const DirectionAndLength toLightCenter = calcDirectionAndLength(surfPosition, l.center);

    float ltHsOneOverPdf;
    const float3 lightNormal = sampleOrientedHemisphere(-toLightCenter.dir, pointRnd.x, pointRnd.y, ltHsOneOverPdf);

    LightSample r;
    r.position = l.center + lightNormal * l.radius;
    r.color = l.color;
    r.dw = calcSolidAngleForSphere(l.radius, toLightCenter.len);

    return r;
}

LightSample sampleTriangleLight(const TriangleLight l, const float3 surfPosition, const float2 pointRnd)
{
    LightSample r;
    r.position = sampleTriangle(l.position[0], l.position[1], l.position[2], pointRnd.x, pointRnd.y);
    r.color = l.color * getPolySpotFactor(l.normal, normalize(surfPosition - r.position));
    r.dw = calcSolidAngleForArea(l.area, r.position, l.normal, surfPosition);

    return r;
}

bool getTalCdfUv(const uint textureIndex, const float rnd, const float2 jitter, out float2 uv)
{
    const uint entryCount = (uint)TAL_CDF_LUT_ENTRIES;
    const uint index = min((uint)(rnd * (float)entryCount), entryCount - 1u);
    const uint packed = talCdf[textureIndex * entryCount + index];

    if (packed == TAL_CDF_EMPTY_ENTRY)
    {
        return false;
    }

    const float2 texSize = (float2)getTextureSize(textureIndex, 0);
    const float2 cellSize = 1.0 / min(texSize, (float2)TAL_CDF_GRID_MAX_SIZE);

    uv = float2((float)(packed & 0xFFFFu), (float)(packed >> 16u)) * (1.0 / 65535.0) + (jitter - 0.5) * cellSize;
    return true;
}

bool isUvInsideConvexPolygon(const float2 verts[MAX_TEXTURED_AREA_LIGHT_VERTS], const int numVerts, const float2 uv)
{
    if (numVerts < 3)
    {
        return true;
    }

    float sign = 0.0;
    for (int i = 0; i < numVerts; i++)
    {
        const float2 a = verts[i];
        const float2 b = verts[(i + 1) % numVerts];

        const float cross = (b.x - a.x) * (uv.y - a.y) - (b.y - a.y) * (uv.x - a.x);

        if (cross != 0.0)
        {
            if (sign == 0.0)
            {
                sign = cross > 0.0 ? 1.0 : -1.0;
            }
            else if (cross * sign < 0.0)
            {
                return false;
            }
        }
    }

    return true;
}

void getTalUvTiles(const TexturedAreaLight l, out float2 tileMin, out float2 tileMax)
{
    float2 uvMin = l.uvVerts[0];
    float2 uvMax = l.uvVerts[0];

    const int vertCount = min((int)l.numVerts, MAX_TEXTURED_AREA_LIGHT_VERTS);

    for (int i = 1; i < MAX_TEXTURED_AREA_LIGHT_VERTS; i++)
    {
        if (i >= vertCount)
        {
            break;
        }

        uvMin = min(uvMin, l.uvVerts[i]);
        uvMax = max(uvMax, l.uvVerts[i]);
    }

    tileMin = floor(uvMin);
    tileMax = max(ceil(uvMax) - 1.0, tileMin);
}

LightSample sampleProjectedAreaLight(const TexturedAreaLight l, const float3 surfPosition, const float2 pointRnd)
{
    LightSample r;
    r.position = surfPosition;
    r.color = float3(0.0, 0.0, 0.0);
    r.dw = 0.0;

    const float3 center = getTexturedAreaLightCenter(l);
    const DirectionAndLength centerToSurf = calcDirectionAndLength(center, surfPosition);
    const float cosNL = dot(l.normal, centerToSurf.dir);
    const float spotlight = getSpotFactor(max(cosNL, 0.0), l.coneCosInner, l.coneCosOuter);

    if (!(spotlight > 0.0))
    {
        return r;
    }

    const int verts = clamp(l.numVerts, 1, MAX_TEXTURED_AREA_LIGHT_VERTS);
    float maskRadius = 0.0;

    for (int i = 0; i < verts; i++)
    {
        maskRadius = max(maskRadius, length(texturedAreaLightWorldPos(l, l.uvVerts[i]) - center));
    }

    if (!(maskRadius > 1e-4))
    {
        return r;
    }

    const float angleOuter = max(acos(clamp(l.coneCosOuter, 0.001, 1.0)), 1e-3);
    const float angleInner = max(acos(clamp(l.coneCosInner, 0.001, 1.0)), 1e-3);
    const float maskLod = clamp((angleOuter - angleInner) / angleOuter, 0.0, 1.0) * 8.0;
    const float focal = maskRadius / tan(angleOuter);
    const float cosNLClamped = max(cosNL, 1e-3);
    const float3 tangentDirection = centerToSurf.dir - l.normal * cosNL;
    const float3 pos = center + tangentDirection * (focal / cosNLClamped);
    const float3 rel = pos - l.C;

    const float a11 = dot(l.A, l.A);
    const float a12 = dot(l.A, l.B);
    const float a22 = dot(l.B, l.B);
    const float b1 = dot(l.A, rel);
    const float b2 = dot(l.B, rel);
    const float det = a11 * a22 - a12 * a12;

    if (!(abs(det) > 1e-12))
    {
        return r;
    }

    const float2 uv = float2(b1 * a22 - b2 * a12, b2 * a11 - b1 * a12) / det;

    if (!isUvInsideConvexPolygon(l.uvVerts, l.numVerts, uv))
    {
        return r;
    }

    const uint textureIndex = asuint(l.textureIndex);
    float mask = 1.0;
    float emiss = l.meanEmiss;

    if (textureIndex != 0u)
    {
        mask = getTextureSampleLod(textureIndex, uv, maskLod).b;
        emiss = 1.0;
    }

    const DirectionAndLength lightToSurf = calcDirectionAndLength(pos, surfPosition);

    r.position = pos;
    r.color = l.color * mask * spotlight;
    r.dw = safeSolidAngle(emiss * l.area * getGeometryFactorClamped(l.normal, lightToSurf.dir, lightToSurf.len));

    return r;
}

LightSample sampleTexturedAreaLight(const TexturedAreaLight l, const float3 surfPosition, const float2 pointRnd)
{
    if (l.projector > 0.5)
    {
        return sampleProjectedAreaLight(l, surfPosition, pointRnd);
    }

    LightSample r;

    const uint textureIndex = asuint(l.textureIndex);

    float2 uv = sampleConvexPolygon(l.uvVerts, l.numVerts, pointRnd.x, pointRnd.y);

    float mask = 1.0;
    float emiss = l.meanEmiss;

    if (textureIndex != 0u)
    {
        mask = getTextureSampleLod(textureIndex, uv, 0.0).b;
        emiss = 1.0;
    }

    r.position = texturedAreaLightWorldPos(l, uv);

    const DirectionAndLength lightToSurf = calcDirectionAndLength(r.position, surfPosition);

    const float cosNL = max(dot(l.normal, lightToSurf.dir), 0.0);
    const float spotlight = (l.coneCosOuter > 0.0) ? getSpotFactor(cosNL, l.coneCosInner, l.coneCosOuter) : sqrt(cosNL);

    r.color = l.color * mask * spotlight;
    r.dw = safeSolidAngle(emiss * l.area * getGeometryFactorClamped(l.normal, lightToSurf.dir, lightToSurf.len));

    return r;
}

LightSample sampleSpotLight(const SpotLight l, const float3 surfPosition, const float2 pointRnd)
{
    LightSample r;
    {
        const float2 disk = sampleDisk(l.radius, pointRnd.x, pointRnd.y);
        const float3x3 basis = getONB(l.direction);

        r.position = l.center + getColumn(basis, 0) * disk.x + getColumn(basis, 1) * disk.y;
    }

    const DirectionAndLength toLightCenter = calcDirectionAndLength(surfPosition, l.center);
    const float cosA = max(dot(l.direction, -toLightCenter.dir), 0.0);

    r.color = l.color * getSpotFactor(cosA, l.cosAngleInner, l.cosAngleOuter);
    r.dw = calcSolidAngleForSphere(l.radius, toLightCenter.len);

    return r;
}



float getLightWeight(const ShLightEncoded encoded, const float3 cellCenter, float cellRadius)
{
    switch (encoded.lightType)
    {
        case LIGHT_TYPE_DIRECTIONAL:       return getDirectionalLightWeight    (decodeAsSphereLight          (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_SPHERE:            return getSphereLightWeight         (decodeAsSphereLight          (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_TRIANGLE:          return getTriangleLightWeight       (decodeAsTriangleLight        (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_SPOT:              return getSpotLightWeight           (decodeAsSpotLight            (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_TEXTURED_AREA:     return getTexturedAreaLightWeight   (decodeAsTexturedAreaLight    (encoded), cellCenter, cellRadius);
        default:                           return 0.0;
    }
}

LightSample sampleLight(const ShLightEncoded encoded, const float3 surfPosition, const float2 pointRnd)
{
    switch (encoded.lightType)
    {
        case LIGHT_TYPE_DIRECTIONAL:       return sampleDirectionalLight       (decodeAsDirectionalLight     (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_SPHERE:            return sampleSphereLight            (decodeAsSphereLight          (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_TRIANGLE:          return sampleTriangleLight          (decodeAsTriangleLight        (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_SPOT:              return sampleSpotLight              (decodeAsSpotLight            (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_TEXTURED_AREA:     return sampleTexturedAreaLight      (decodeAsTexturedAreaLight    (encoded), surfPosition, pointRnd);
        default:                           return emptyLightSample();
    }
}

#endif
