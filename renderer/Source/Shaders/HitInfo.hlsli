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

#ifdef DESC_SET_VERTEX_DATA
#ifdef DESC_SET_GLOBAL_UNIFORM
#ifdef DESC_SET_TEXTURES

#if defined(HITINFO_INL_PRIM)
float3 processAlbedoGrad(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], const float2 dPdx[3], const float2 dPdy[3])
#elif defined(HITINFO_INL_RFL)
float3 processAlbedoRayConeDeriv(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], const DerivativeSet derivSet)
#elif defined(HITINFO_INL_INDIR)
float3 processAlbedo(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], float lod)
#endif
{
    const uint blendsFlags[3] =
    {
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_FIRST_LAYER)  >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 0),
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_SECOND_LAYER) >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 1),
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_THIRD_LAYER)  >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 2)
    };

    float3 dst = (float3)1.0;
    bool hasAnyAlbedoTexture = false;

    for (int i = 0; i < MATERIAL_MAX_ALBEDO_LAYERS; i++)
    {
        if (materials[i][MATERIAL_ALBEDO_ALPHA_INDEX] != MATERIAL_NO_TEXTURE)
        {
            const float4 src = materialColors[i] *
        #if defined(HITINFO_INL_PRIM)
                getTextureSampleGrad(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], dPdx[i], dPdy[i]);
        #elif defined(HITINFO_INL_RFL)
                getTextureSampleDerivSet(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], derivSet, i);
        #elif defined(HITINFO_INL_INDIR)
                getTextureSampleLod(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], lod);
        #endif

            bool opq = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_OPAQUE) != 0;
            bool alp = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_ALPHA)  != 0;
            bool add = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_ADD)    != 0;
            bool shd = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_SHADE)  != 0;

            opq = opq || (alp && i == 0);
            alp = alp && !opq;

            dst = float(opq) * (src.rgb) +
                  float(alp) * (src.rgb * src.a + dst * (1 - src.a)) +
                  float(add) * (src.rgb + dst) +
                  float(shd) * (src.rgb * dst * 2);

            hasAnyAlbedoTexture = true;
        }
    }

    dst = lerp(materialColors[0].rgb, dst, float(hasAnyAlbedoTexture));

    return clamp(dst, (float3)0.0, (float3)1.0);
}


#if defined(HITINFO_INL_INDIR)
float3 getHitInfoAlbedoOnly(ShPayload pl)
{
    int instanceId, instCustomIndex;
    int geomIndex, primIndex;

    unpackInstanceIdAndCustomIndex(pl.instIdAndIndex, instanceId, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(pl.geomAndPrimIndex, geomIndex, primIndex);

    const ShTriangle tr = getTriangle(instanceId, instCustomIndex, geomIndex, primIndex);

    const float2 inBaryCoords = pl.baryCoords;
    const float3 baryCoords = float3(1.0f - inBaryCoords.x - inBaryCoords.y, inBaryCoords.x, inBaryCoords.y);

    const float2 texCoords[3] =
    {
        getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords)),
        mul(tr.layerTexCoord[1], baryCoords),
        mul(tr.layerTexCoord[2], baryCoords)
    };

    return processAlbedo(tr.geometryInstanceFlags, texCoords, tr.materials, tr.materialColors, 0);
}
#endif


#if defined(HITINFO_INL_PRIM)
float getBounceLOD(float roughness, float viewDist, float hitDist, float screenWidth, float bounceMipBias)
{
    const float range = 300.0 * pow((1.0 - roughness) * 0.9 + 0.1, 4.0);

    float2 f = float2(viewDist, hitDist);
    f = clamp(f / range, (float2)0.0, (float2)1.0);
    f = sqrt(f);

    float mip = max(log2(3840.0 / screenWidth), 0.0);

    mip += f.x * 10.0;
    mip += f.y * 10.0;

    return mip + bounceMipBias;
}
#endif


#if defined(HITINFO_INL_PRIM)
float3 intersectRayTriangle(const float3x3 positions, const float3 orig, const float3 dir)
{
    const float3 edge1 = getColumn(positions, 1) - getColumn(positions, 0);
    const float3 edge2 = getColumn(positions, 2) - getColumn(positions, 0);

    const float3 pvec = cross(dir, edge2);

    const float det = dot(edge1, pvec);
    const float invDet = 1.0 / det;

    const float3 tvec = orig - getColumn(positions, 0);
    const float3 qvec = cross(tvec, edge1);

    const float u = dot(tvec, pvec) * invDet;
    const float v = dot(dir, qvec) * invDet;

    return float3(1 - u - v, u, v);
}
#endif


#if defined(HITINFO_INL_PRIM)

ShHitInfo getHitInfoPrimaryRay(
    const ShPayload pl,
    const float3 rayOrigin, const float3 rayDirAX, const float3 rayDirAY,
    out float2 motion, out float motionDepthLinear,
    out float3 gradDepth, out float depthNDC, out float depthLinear,
    out float screenEmission,
    out uint emissionBlendCode)

#elif defined(HITINFO_INL_RFL)

ShHitInfo getHitInfoWithRayCone_ReflectionRefraction(
    const ShPayload pl, const RayCone rayCone,
    const float3 rayOrigin, const float3 rayDir, const float3 viewDir,
    inout float3 virtualPosForMotion,
    out float rayLen,
    out float2 motion, out float motionDepthLinear,
    out float screenEmission,
    out uint emissionBlendCode)

#elif defined(HITINFO_INL_INDIR)

ShHitInfo getHitInfoBounce(
    const ShPayload pl, const float3 rayOrigin, float originRoughness, float bounceMipBias)

#endif
{
    ShHitInfo h;
    h.transparency = 1.0f;

    int instanceId, instCustomIndex;
    int geomIndex, primIndex;

    unpackInstanceIdAndCustomIndex(pl.instIdAndIndex, instanceId, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(pl.geomAndPrimIndex, geomIndex, primIndex);

    const ShTriangle tr = getTriangle(instanceId, instCustomIndex, geomIndex, primIndex);

    const float2 inBaryCoords = pl.baryCoords;
    const float3 baryCoords = float3(1.0f - inBaryCoords.x - inBaryCoords.y, inBaryCoords.x, inBaryCoords.y);

    const float2 texCoords[3] =
    {
        getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords)),
        mul(tr.layerTexCoord[1], baryCoords),
        mul(tr.layerTexCoord[2], baryCoords)
    };

    h.hitPosition = mul(tr.positions, baryCoords);

    if( ( tr.geometryInstanceFlags & GEOM_INST_FLAG_EXACT_NORMALS ) == 0 )
    {
        h.normalGeom = normalize( mul(tr.normals, baryCoords) );
    }
    else
    {
        h.normalGeom = safeNormalize(
            cross( getColumn(tr.positions, 1) - getColumn(tr.positions, 0),
                   getColumn(tr.positions, 2) - getColumn(tr.positions, 0) ) );

        if( dot( h.normalGeom, h.hitPosition - rayOrigin ) > 0 )
        {
            h.normalGeom *= -1;
        }
    }


#if defined(HITINFO_INL_PRIM)
    const float3 baryCoordsAX = intersectRayTriangle(tr.positions, rayOrigin, rayDirAX);
    const float3 baryCoordsAY = intersectRayTriangle(tr.positions, rayOrigin, rayDirAY);

    const float4 viewSpacePosCur   = mul(globalUniform.view, float4(h.hitPosition, 1.0));
    const float4 viewSpacePosPrev  = mul(globalUniform.viewPrev, float4(mul(tr.prevPositions, baryCoords), 1.0));
    const float4 viewSpacePosAX    = mul(globalUniform.view, float4(mul(tr.positions, baryCoordsAX), 1.0));
    const float4 viewSpacePosAY    = mul(globalUniform.view, float4(mul(tr.positions, baryCoordsAY), 1.0));

    const float4 clipSpacePosCur   = mul(globalUniform.projection, viewSpacePosCur);
    const float4 clipSpacePosPrev  = mul(globalUniform.projectionPrev, viewSpacePosPrev);

    const float clipSpaceDepth   = clipSpacePosCur[2];
    const float clipSpaceDepthAX = dot(getColumn(globalUniform.projection, 2), viewSpacePosAX);
    const float clipSpaceDepthAY = dot(getColumn(globalUniform.projection, 2), viewSpacePosAY);

    const float3 ndcCur            = clipSpacePosCur.xyz  / clipSpacePosCur.w;
    const float3 ndcPrev           = clipSpacePosPrev.xyz / clipSpacePosPrev.w;

    const float2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    const float2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;
#endif


#if defined(HITINFO_INL_RFL)
    rayLen = length(h.hitPosition - rayOrigin);
#endif


#if defined(HITINFO_INL_RFL)
    virtualPosForMotion += viewDir * rayLen;

    const float4 viewSpacePosCur   = mul(globalUniform.view, float4(virtualPosForMotion, 1.0));
    const float4 viewSpacePosPrev  = mul(globalUniform.viewPrev, float4(virtualPosForMotion, 1.0));
    const float4 clipSpacePosCur   = mul(globalUniform.projection, viewSpacePosCur);
    const float4 clipSpacePosPrev  = mul(globalUniform.projectionPrev, viewSpacePosPrev);
    const float3 ndcCur            = clipSpacePosCur.xyz  / clipSpacePosCur.w;
    const float3 ndcPrev           = clipSpacePosPrev.xyz / clipSpacePosPrev.w;
    const float2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    const float2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;

    const float clipSpaceDepth   = clipSpacePosCur[2];
#endif


#if defined(HITINFO_INL_PRIM)
    depthNDC = ndcCur.z;
    depthLinear = length(viewSpacePosCur.xyz);
#endif


#if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
    motion = (screenSpacePrev - screenSpaceCur);
#endif


#if defined(HITINFO_INL_PRIM)
    motionDepthLinear = length(viewSpacePosPrev.xyz) - depthLinear;
#elif defined(HITINFO_INL_RFL)
    motionDepthLinear = length(viewSpacePosPrev.xyz) - length(viewSpacePosCur.xyz);
#endif


#if defined(HITINFO_INL_PRIM)
    gradDepth = float3(
        clipSpaceDepthAX - clipSpaceDepth,
        clipSpaceDepthAY - clipSpaceDepth,
        abs(length(viewSpacePosAX.xyz) - depthLinear) + abs(length(viewSpacePosAY.xyz) - depthLinear));
#elif defined(HITINFO_INL_RFL)
#endif


#if defined(HITINFO_INL_PRIM)
    const float2 dTdx[3] =
    {
        (getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoordsAX)) - texCoords[0]),
        (mul(tr.layerTexCoord[1], baryCoordsAX) - texCoords[1]),
        (mul(tr.layerTexCoord[2], baryCoordsAX) - texCoords[2])
    };

    const float2 dTdy[3] =
    {
        (getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoordsAY)) - texCoords[0]),
        (mul(tr.layerTexCoord[1], baryCoordsAY) - texCoords[1]),
        (mul(tr.layerTexCoord[2], baryCoordsAY) - texCoords[2])
    };

    h.albedo = processAlbedoGrad(
        tr.geometryInstanceFlags, texCoords,
            tr.materials, tr.materialColors,
            dTdx, dTdy);
#endif


#if defined(HITINFO_INL_RFL)
    DerivativeSet derivSet = getTriangleUVDerivativesFromRayCone(tr, h.normalGeom, rayCone, rayDir);

    h.albedo = processAlbedoRayConeDeriv(
        tr.geometryInstanceFlags, texCoords,
        tr.materials, tr.materialColors,
        derivSet);
#endif


#if defined(HITINFO_INL_INDIR)
    const float viewDist = length(h.hitPosition - globalUniform.cameraPosition.xyz);
    const float hitDistance = length(h.hitPosition - rayOrigin);

    const float lod = getBounceLOD(originRoughness, viewDist, hitDistance, globalUniform.renderWidth, bounceMipBias);

    h.albedo = processAlbedo(tr.geometryInstanceFlags, texCoords, tr.materials, tr.materialColors, lod);
#endif


    if (tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX] != MATERIAL_NO_TEXTURE)
    {
        float4 rme =
    #if defined(HITINFO_INL_PRIM)
            getTextureSampleGrad(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], dTdx[0], dTdy[0]);
    #elif defined(HITINFO_INL_RFL)
            getTextureSampleDerivSet(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], derivSet, 0);
    #elif defined(HITINFO_INL_INDIR)
            getTextureSampleLod(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], lod);
    #endif

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        if( globalUniform.emissionSharpMask != 0.0 )
        {
            const uint  rmeTexture = tr.materials[ 0 ][ MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX ];
        #if defined(HITINFO_INL_PRIM)
            const float2 rmeFootX  = dTdx[ 0 ];
            const float2 rmeFootY  = dTdy[ 0 ];
        #else
            const float2 rmeFootX  = float2( derivSet.u[ 0 ], 0.0 );
            const float2 rmeFootY  = float2( 0.0, derivSet.u[ 0 ] );
        #endif
            const int2   rmeTexSize = getTextureSize( rmeTexture, 0 );
            const float2 rmeFpx     = rmeFootX * (float2)rmeTexSize;
            const float2 rmeFpy     = rmeFootY * (float2)rmeTexSize;
            if( max( dot( rmeFpx, rmeFpx ), dot( rmeFpy, rmeFpy ) ) < 1.0 )
            {
                const float2 snappedUv = ( floor( texCoords[ 0 ] * (float2)rmeTexSize ) + (float2)0.5 ) / (float2)rmeTexSize;
                rme.b = getTextureSampleLod( rmeTexture, snappedUv, 0.0 ).b;
            }
        }
    #endif

        h.roughness = rme[ 0 ];
        h.metallic  = rme[ 1 ];
        h.emission  = rme[ 2 ];

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        emissionBlendCode = uint( rme.a * 255.0 + 0.5 );
    #endif

    }
    else
    {
        h.roughness = tr.geomRoughness;
        h.metallic  = tr.geomMetallicity;
        h.emission  = tr.geomEmission;

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        emissionBlendCode = 0u;
    #endif
    }

    h.roughness = globalUniform.squareInputRoughness == 0 ? h.roughness : square( h.roughness );
    h.roughness = max( h.roughness, MIN_GGX_ROUGHNESS );

    float lightStyleEmissionScale = 1.0;
    for( int i = 0; i < 4; i++ )
    {
        const uint styleByte = ( tr.lightStyleIndices >> ( i * 8 ) ) & 0xFFu;
        if( styleByte == 0u )
            break;
        const uint  style      = styleByte - 1u;
        const float styleValue = globalUniform.lightStyleScales[ style / 4u ][ style % 4u ];
        if( styleValue < ( 255.5 / 256.0 ) )
            lightStyleEmissionScale = min( lightStyleEmissionScale, styleValue );
    }
    h.emission *= lightStyleEmissionScale;

#if defined( HITINFO_INL_PRIM ) || defined( HITINFO_INL_RFL )
    screenEmission = rmeEmissionToScreenEmission( h.emission );
#endif
    h.emission *= globalUniform.emissionMapBoost;


#if !defined(HITINFO_INL_INDIR)
    if (tr.materials[0][MATERIAL_NORMAL_INDEX] != MATERIAL_NO_TEXTURE)
    {
        const float suppressDetails = 5.0;

        const float4 nrmSample =
    #if defined(HITINFO_INL_PRIM)
            getTextureSampleGrad(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoords[0], dTdx[0] * suppressDetails, dTdy[0] * suppressDetails)
    #elif defined(HITINFO_INL_RFL)
            getTextureSampleDerivSet(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoords[0], derivSet, 0)
    #endif
            ;

        float2 nrm = nrmSample.xy;
        // the normal map's alpha carries the material's glass transparency
        h.transparency = nrmSample.a;
        nrm.xy = nrm.xy * 2.0 - (float2)1.0;

        const float3 bitangent = cross(h.normalGeom, tr.tangent.xyz) * tr.tangent.w;
        h.normal = safeNormalize(tr.tangent.xyz * nrm.x + bitangent * nrm.y + h.normalGeom);

        h.normal = safeNormalize(lerp(h.normalGeom, h.normal, globalUniform.normalMapStrength));
    }
    else
#endif
    {
        h.normal = h.normalGeom;
    }

    h.instCustomIndex = instCustomIndex;
    h.geometryInstanceFlags = tr.geometryInstanceFlags;
    h.portalIndex = tr.portalIndex;

    h.cluster = tr.cluster;

    return h;
}

#endif
#endif
#endif
