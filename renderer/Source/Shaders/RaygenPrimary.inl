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

#if defined(RAYGEN_PRIMARY_SHADER) && defined(RAYGEN_REFL_REFR_SHADER)
    #error Only one of RAYGEN_PRIMARY_SHADER and RAYGEN_REFL_REFR_SHADER must be defined
#endif
#if !defined(RAYGEN_PRIMARY_SHADER) && !defined(RAYGEN_REFL_REFR_SHADER) && !defined(Q2_REFL_REFR_SHADER)
    #error RAYGEN_PRIMARY_SHADER, RAYGEN_REFL_REFR_SHADER or Q2_REFL_REFR_SHADER must be defined
#endif
#ifndef MATERIAL_MAX_ALBEDO_LAYERS
    #error MATERIAL_MAX_ALBEDO_LAYERS is not defined
#endif



#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_CUBEMAPS 7
#define DESC_SET_RENDER_CUBEMAP 8
#define DESC_SET_PORTALS 9
#define DESC_SET_RAY_STATS 11
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_NONE)
#include "RaygenCommon.h"
#include "Q2Fog.h"
#include "Q2Asvgf.h"

#if defined(Q2_REFL_REFR_SHADER)

/* Defined in RayClearance.h, which only the Q2 reflect/refract raygen
   includes: its ray query must not reach the modules that do not use it. */
float traceClearance(vec3 origin, vec3 direction, float maxDistance, uint cullMask);

#define GLASS_ROUGHNESS_MAX_ANGLE 0.35

/* One GGX micro-normal of a pane for the view direction v, in world space: */
/* roughness is the microfacet distribution the ray leaves along. */
vec3 sampleRoughNormal(vec3 n, vec3 v, float alpha, vec2 u)
{
    const mat3 basis = getONB(n);
    float oneOverPdf;
    const vec3 m = sampleGGXVNDF(transpose(basis) * v, alpha, u.x, u.y, oneOverPdf);

    return normalize(basis * m);
}

#endif

vec2 getMotionVectorForUpscaler(const vec2 motionCurToPrev)
{
    return motionCurToPrev;
}

#include "SkyMotion.h"

void storeQ2GBuffer(
    const ivec2 pix,
    const vec3 baseColor, float transparency, const vec2 glassParams,
    float metallic, float roughness,
    float depth,
    float halfConeAngle, float distToLight,
    const vec3 transparentColor, float transparentAlpha,
    const vec4 fogAccum,
    const uint cluster)
{
    if (globalUniform.coreQ2RTX == 0)
    {
        return;
    }

    imageStore(framebufQ2ViewDepth,          pix, vec4(depth));
    imageStore(framebufQ2BaseColor,          pix, vec4(baseColor, transparency));
    imageStore(framebufQ2Metallic,           pix, vec4(metallic, roughness, 0.0, 0.0));
    imageStore(framebufQ2BounceThroughput,   pix, vec4(glassParams, 1.0, halfConeAngle));
    imageStore(framebufQ2Transparent,        pix, vec4(transparentColor, transparentAlpha));
    imageStore(framebufQ2GodRaysThroughputDist, pix, vec4(1.0, 1.0, 1.0, distToLight));
    imageStore(framebufQ2RngSeed,            pix, uvec4(getRandomSeed(pix, globalUniform.frameId)));
    imageStore(framebufQ2FogAccum,           pix, fogAccum);
    imageStore(framebufQ2Cluster,            pix, uvec4(cluster));
}

void storeSky(
    const ivec2 pix, const vec3 rayDir, bool calculateSkyAndStoreToAlbedo, const vec3 throughput,
#ifdef RAYGEN_PRIMARY_SHADER
    float firstHitDepthNDC,
#else
    bool wasSplit,
#endif
    const vec4 fogAccum )
{
    imageStore(framebufIsSky, pix, ivec4(1));

    {
        vec3 albedo;

        if (calculateSkyAndStoreToAlbedo)
        {
            albedo = getSkyPrimary(rayDir);
        }
        else
        {
            albedo = imageLoad(framebufAlbedo, getRegularPixFromCheckerboardPix(pix)).rgb;
        }

        imageStore(framebufAlbedo, getRegularPixFromCheckerboardPix(pix), vec4(albedo, 0.0));

        storeQ2GBuffer(pix, albedo, 1.0, vec2(0.0), 0.0, 1.0, MAX_RAY_LENGTH * 2.0, 0.0, MAX_RAY_LENGTH * 2.0, albedo, 1.0, fogAccum, ~0u);
    }

    vec2 m = getMotionForInfinitePoint(rayDir);
    if (globalUniform.skyType == SKY_TYPE_PROCEDURAL && globalUniform.cloudLayerMotion.w > 0.0)
    {
        float share = textureLod(samplerCube(renderCubemap, renderCubemap_Sampler), rayDir, 0.0).a;
        m = mix(m, getMotionForCloudLayer(rayDir, m), share);
    }

    imageStoreNormal(                       pix, vec3(0.0));
    imageStoreNormalGeometry(               pix, vec3(0.0));
    imageStore(framebufMetallicRoughness,   pix, vec4(0.0));
    imageStore(framebufDepthWorld,          pix, vec4(MAX_RAY_LENGTH * 2.0));
    imageStore(framebufMotion,              pix, vec4(m, 0.0, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(SURFACE_POSITION_INCORRECT));
    imageStore(framebufVisibilityBuffer,    pix, vec4(UINT32_MAX));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix( pix ), vec4( 0.0 ) );
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix( pix ), vec4( 0.0 ) );
#ifdef RAYGEN_PRIMARY_SHADER
    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(0, 0, PORTAL_INDEX_NONE, 0));
    imageStore(framebufDepthGrad,           pix, vec4(0.0));
    imageStore(framebufDepthNdc,            getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getMotionVectorForUpscaler(m), 0.0, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, 0.0));
#else
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));
#endif
}

uint getNewRayMedia(int i, uint prevMedia, uint geometryInstanceFlags)
{
    if (i == 0 && globalUniform.cameraMediaType != MEDIA_TYPE_VACUUM)
    {
       return MEDIA_TYPE_VACUUM;
    }

    return getMediaTypeFromFlags(geometryInstanceFlags);
}

vec3 getWaterNormal(const RayCone rayCone, const vec3 rayDir, const vec3 normalGeom, const vec3 position, bool wasPortal)
{
    const mat3 basis = getONB(normalGeom);
    const vec2 baseUV = vec2(dot(position, basis[0]), dot(position, basis[1]));


    float verticality = 1.0 - abs(dot(normalGeom, globalUniform.worldUpVector.xyz));

    vec2 flowSpeedVertical = 10 * vec2(dot(basis[0], globalUniform.worldUpVector.xyz),
                                       dot(basis[1], globalUniform.worldUpVector.xyz));

    vec2 flowSpeedHorizontal = vec2(1.0);


    const float uvScale = 0.05 / globalUniform.waterTextureAreaScale;
    vec2 speed0 = uvScale * mix(flowSpeedHorizontal, flowSpeedVertical, verticality) * globalUniform.waterWaveSpeed;
    vec2 speed1 = -0.9 * speed0 * mix(1.0, -0.1, verticality);


    float derivU = globalUniform.waterTextureDerivativesMultiplier * 0.5 * uvScale * getWaterDerivU(rayCone, rayDir, normalGeom);

    if (wasPortal)
    {
        derivU *= 0.1;
    }


    vec2 uv0 = uvScale * baseUV + globalUniform.time * speed0;
    vec3 n0 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv0, derivU).xyz;
    n0.xy = n0.xy * 2.0 - vec2(1.0);


    vec2 uv1 = 0.8 * uvScale * baseUV + globalUniform.time * speed1;
    vec3 n1 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv1, derivU).xyz;
    n1.xy = n1.xy * 2.0 - vec2(1.0);


    vec2 uv2 = 0.1 * (uvScale * baseUV + speed0 * sin(globalUniform.time * 0.5));
    vec3 n2 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv2, derivU).xyz;
    n2.xy = n2.xy * 2.0 - vec2(1.0);


    const float strength = globalUniform.waterWaveStrength;

    const vec3 n = normalize(vec3(0, 0, 1) + strength * (0.25 * n0 + 0.2 * n1 + 0.1 * n2));
    return basis * n;
}

mat3 lookAt(const vec3 forward, const vec3 worldUp)
{
    vec3 right = cross(forward, worldUp);
    vec3 up = cross(right, forward);

    return mat3(right, up, forward);
}

vec3 getPortalNormal(const vec3 baseNormal, const vec3 inWorldOffset)
{
    if (globalUniform.twirlPortalNormal == 0)
    {
        return -baseNormal;
    }

    float phaseScale = 3;
    float timeScale = 3;
    float waveScale = 0.01;
    float tm = mod(timeScale * globalUniform.time, M_PI * 2);

    const mat3 inLookAt_Plain = lookAt(-baseNormal, globalUniform.worldUpVector.xyz);
    const vec2 localOffset_Plain = vec2(dot(inWorldOffset, inLookAt_Plain[0]),
                                        dot(inWorldOffset, inLookAt_Plain[1]));

    float distance = length(localOffset_Plain);
    float angle = atan(localOffset_Plain.y, localOffset_Plain.x);

    float phase = sin(phaseScale * sqrt(distance) + angle + tm) + 1.0;
    phase *= waveScale;
    phase *= clamp(distance / 20, 0, 1);

    vec3 localN = { phase, phase, 1.0 };

    return inLookAt_Plain * normalize(localN);
}

bool isBackface(const vec3 normalGeom, const vec3 rayDir)
{
    return dot(normalGeom, -rayDir) < 0.0;
}

vec3 getNormal(const vec3 position, const vec3 normalFromMap, const vec3 normalGeom, const RayCone rayCone, const vec3 rayDir, bool isWater, bool wasPortal)
{
    if (isWater)
    {
        vec3 n = normalGeom;

        if (isBackface(normalGeom, rayDir))
        {
            n *= -1;
        }

        return getWaterNormal(rayCone, rayDir, n, position, wasPortal);
    }
    else
    {
        vec3 n = normalFromMap;

        if (isBackface(normalGeom, rayDir))
        {
           n *= -1;
        }

        return normalize(n);
    }
}

#ifdef RAYGEN_PRIMARY_SHADER
vec3 rtGlassSurfaceKey(vec3 normal, uint instance, uint geometry)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 0.001);
    vec2 oct = normal.xy;
    if (normal.z < 0.0)
    {
        oct = (1.0 - abs(oct.yx)) * vec2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
    }
    uint key = instance * 73856093u ^ (geometry & (MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT - 1u)) * 19349663u;
    key ^= key >> 13u;
    key *= 1274126177u;
    return vec3(oct, unpackHalf2x16(key & 0x7bffu).x);
}

void main()
{
    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraOrigin = globalUniform.cameraPosition.xyz;
    const vec3 cameraRayDir = getRayDir(inUV);
    const vec3 cameraRayDirAX = getRayDirAX(inUV);
    const vec3 cameraRayDirAY = getRayDirAY(inUV);

    const uint randomSeed = getRandomSeed(pix, globalUniform.frameId);


    const ShPayload primaryPayload = tracePrimaryRay(cameraOrigin, cameraRayDir);
    rayStatsAdd(RAY_STATS_CATEGORY_PRIMARY, 1);
    imageStore(framebufQ2GlassFilter, pix, vec4(0.0));
    imageStore(framebufQ2GlassReflection, regularPix, vec4(0.0));


    const uint currentRayMedia = globalUniform.cameraMediaType;


    if (!doesPayloadContainHitInfo(primaryPayload))
    {
        vec3 throughput = vec3(1.0);

        vec4 q2FogAccum = vec4(0);
        if (globalUniform.coreQ2RTX != 0)
        {
            uvec4 q2SkyFog1, q2SkyFog2;
            q2FindFogVolumes(cameraOrigin, cameraRayDir, 0.0, 1e6, q2SkyFog1, q2SkyFog2);
            q2FogAccum = q2SegmentFog(q2SkyFog1, q2SkyFog2, 1e6);
        }

        storeSky(pix, cameraRayDir, globalUniform.skyType != SKY_TYPE_RASTERIZED_GEOMETRY, throughput, MAX_RAY_LENGTH * 2.0, q2FogAccum);
        if (primaryPayload.glassFilter.z != 0.0)
        {
            imageStore(framebufQ2GlassFilter, pix, vec4(primaryPayload.glassTint, 1.0 + primaryPayload.glassFilter.x));
            imageStore(framebufQ2GlassReflection, regularPix, vec4(0.0, 0.0, 0.0, primaryPayload.glassFilter.w));
        }
        return;
    }


    vec2 motionCurToPrev;
    float motionDepthLinearCurToPrev;
    vec3 gradDepth;
    float firstHitDepthNDC;
    float firstHitDepthLinear;
    float screenEmission;
    uint emissionBlendCode;
    const ShHitInfo h = getHitInfoPrimaryRay(primaryPayload, cameraOrigin, cameraRayDirAX, cameraRayDirAY, motionCurToPrev, motionDepthLinearCurToPrev, gradDepth, firstHitDepthNDC, firstHitDepthLinear, screenEmission, emissionBlendCode);
    if (globalUniform.glassBlur == 0u && 
        globalUniform.reflectRefractMaxDepth > 0u &&
        (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0u &&
        (h.geometryInstanceFlags & GEOM_INST_FLAG_IGNORE_REFRACT_AFTER) == 0u)
    {
        const float field = isRegularPixOdd(regularPix) == 0 ? -1.0 : 1.0;
        imageStore(framebufQ2GlassFilter, pix, vec4(rtGlassSurfaceKey(h.normalGeom, primaryPayload.instIdAndIndex, primaryPayload.geomAndPrimIndex),
                                                  field * (4.0 + h.roughness)));
        imageStore(framebufQ2GlassReflection, regularPix, vec4(motionCurToPrev, firstHitDepthLinear, motionDepthLinearCurToPrev));
    }
    if (primaryPayload.glassFilter.z != 0.0 && (primaryPayload.glassDistance < length(h.hitPosition - cameraOrigin) ||
        (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0u))
    {
        const float field = globalUniform.reflectRefractMaxDepth > 0u && isRegularPixOdd(regularPix) == 0 ? -1.0 : 1.0;
        imageStore(framebufQ2GlassFilter, pix, vec4(primaryPayload.glassTint, field * (1.0 + primaryPayload.glassFilter.x)));
        imageStore(framebufQ2GlassReflection, regularPix, vec4(0.0, 0.0, 0.0, primaryPayload.glassFilter.w));
    }


    vec3 throughput = vec3(1.0);
    throughput *= getMediaTransmittance(currentRayMedia, firstHitDepthLinear);


    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_LUMA) != 0 ? vec3(screenEmission) : h.albedo * screenEmission * throughput , 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(getGlowingMediaFog(currentRayMedia, firstHitDepthLinear), 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(firstHitDepthLinear));
    float depthGrad = length(gradDepth.xy);
    if (globalUniform.q2DepthGradMode != 0u)
    {
        depthGrad = 1.0 / max(Q2_DEPTH_GRAD_MIN_STEP, gradDepth.z);
    }
    imageStore(framebufDepthGrad,           pix, vec4(depthGrad));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(primaryPayload));
    imageStore(framebufViewDirection,       pix, vec4(cameraRayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, 0.0));

    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(h.geometryInstanceFlags, primaryPayload.instIdAndIndex, h.portalIndex, emissionBlendCode));

    imageStore(framebufDepthNdc,            getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getMotionVectorForUpscaler(motionCurToPrev), 0.0, 0.0));

    uvec4 q2Fog1, q2Fog2;
    q2FindFogVolumes(cameraOrigin, cameraRayDir, 0.0, firstHitDepthLinear, q2Fog1, q2Fog2);
    const vec4 q2FogAccum = q2SegmentFog(q2Fog1, q2Fog2, firstHitDepthLinear);
    storeQ2GBuffer(pix, h.albedo, h.transparency, h.glassParams, h.metallic, h.roughness,
                   firstHitDepthLinear, 0.5 * length(cameraRayDir - cameraRayDirAX), firstHitDepthLinear,
                   vec3(0.0), 0.0, q2FogAccum, h.cluster);
}
#endif


#ifdef RAYGEN_REFL_REFR_SHADER
void main()
{
    if (globalUniform.reflectRefractMaxDepth == 0)
    {
        return;
    }


    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraRayDir = getRayDir(inUV);

    if (isSkyPix(pix))
    {
        return;
    }



    const uvec3 primaryToReflRefrBuf        = texelFetch(framebufPrimaryToReflRefr_Sampled, pix, 0).rgb;
    ShHitInfo h;
    h.albedo                                = texelFetch(framebufAlbedo_Sampled, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    h.hitPosition                           = texelFetch(framebufSurfacePosition_Sampled, pix, 0).xyz;
    h.geometryInstanceFlags                 = primaryToReflRefrBuf.r;
    h.portalIndex                           = primaryToReflRefrBuf.b;
    h.normalGeom                            = texelFetchNormalGeometry(pix);
    h.normal                                = texelFetchNormal(pix);
    h.roughness                             = texelFetch( framebufMetallicRoughness_Sampled, pix, 0 ).g;
    const vec3  motionBuf                   = texelFetch(framebufMotion_Sampled, pix, 0).rgb;
    vec2        motionCurToPrev             = motionBuf.rg;
    float       motionDepthLinearCurToPrev  = motionBuf.b;
    float       firstHitDepthLinear         = texelFetch(framebufDepthWorld_Sampled, pix, 0).r;
    vec3        screenEmission              = texelFetch(framebufScreenEmisRT_Sampled, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        acidFog                     = texelFetch(framebufAcidFogRT_Sampled, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        throughput                  = texelFetch(framebufThroughput_Sampled, pix, 0).rgb;
    ShPayload currentPayload;
    currentPayload.instIdAndIndex           = primaryToReflRefrBuf.g;

    vec4 q2FogAccum = texelFetch(framebufQ2FogAccum_Sampled, pix, 0);



    RayCone rayCone;
    rayCone.width = 0;
    rayCone.spreadAngle = globalUniform.cameraRayConeSpreadAngle;

    float fullPathLength = firstHitDepthLinear;
    float q2LastSegmentLen = 0.0;
    vec3 prevHitPosition = h.hitPosition;
    bool wasSplit = false;
    bool wasPortal = false;
    vec3 virtualPos = h.hitPosition;
    vec3 rayDir = cameraRayDir;
    uint currentRayMedia = globalUniform.cameraMediaType;
    bool hitInfoWasOverwritten = false;


    propagateRayCone(rayCone, firstHitDepthLinear);



    for (int i = 0; i < globalUniform.reflectRefractMaxDepth; i++)
    {
        const uint instIndex = unpackInstanceIdAndCustomIndex(currentPayload.instIdAndIndex).y;


        bool isPixOdd = isCheckerboardPixOdd(pix) != 0;


        uint newRayMedia = getNewRayMedia(i, currentRayMedia, h.geometryInstanceFlags);

        bool isPortal = isPortalFromFlags(h.geometryInstanceFlags) && h.portalIndex != PORTAL_INDEX_NONE;
        bool toRefract = isRefractFromFlags(h.geometryInstanceFlags);
        bool toReflect = isReflectFromFlags( h.geometryInstanceFlags ) &&
                         h.roughness < globalUniform.minRoughness;


        if (!toReflect && !toRefract && !isPortal)
        {
            break;
        }


        const float curIndexOfRefraction = getIndexOfRefraction(currentRayMedia);
        const float newIndexOfRefraction = getIndexOfRefraction(newRayMedia);

        const vec3 normal = getNormal(
            h.hitPosition,
            h.normal,
            h.normalGeom,
            rayCone,
            rayDir,
            !isPortal && ( newRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_WATER ||
                           newRayMedia == MEDIA_TYPE_ACID || currentRayMedia == MEDIA_TYPE_ACID ),
            wasPortal );


        bool delaySplitOnNextTime = false;

        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_NO_MEDIA_CHANGE) != 0)
        {
            throughput *= getMediaTransmittance(newRayMedia, 1.0);
            newRayMedia = currentRayMedia;

            delaySplitOnNextTime = (globalUniform.noBackfaceReflForNoMediaChange != 0) && isBackface(h.normalGeom, rayDir);
        }



        vec3 rayOrigin = h.hitPosition;
        bool doSplit = !wasSplit;
        bool doRefraction;
        vec3 refractionDir;
        float F;

        if (delaySplitOnNextTime)
        {
            doSplit = false;
            toRefract = true;
            isPixOdd = true;
        }

        if (toRefract && calcRefractionDirection(curIndexOfRefraction, newIndexOfRefraction, rayDir, normal, refractionDir))
        {
            doRefraction = isPixOdd;
            F = getFresnelSchlick(curIndexOfRefraction, newIndexOfRefraction, -rayDir, normal);
        }
        else
        {
            doRefraction = false;
            doSplit = false;
            F = 1.0;
        }

        if (doRefraction)
        {
            rayDir = refractionDir;
            throughput *= (1 - F);

            currentRayMedia = newRayMedia;
        }
        else if (isPortal)
        {
            const ShPortalInstance portal = g_portals[h.portalIndex];

            const vec3 inCenter = portal.inPosition.xyz;
            const vec3 inWorldOffset = h.hitPosition - inCenter;

            mat3 inLookAt = lookAt(getPortalNormal(normal, inWorldOffset), globalUniform.worldUpVector.xyz);

            const vec3 outCenter = portal.outPosition.xyz;
            const mat3 outLookAt = lookAt(portal.outDirection.xyz,
                                          portal.outUp.xyz);

            rayDir = outLookAt * (transpose(inLookAt) * rayDir);

            const vec2 localOffset = vec2(dot(inWorldOffset, inLookAt[0]),
                                          dot(inWorldOffset, inLookAt[1]));

            rayOrigin = outCenter + localOffset.x * outLookAt[0] + localOffset.y * outLookAt[1];

            wasPortal = true;
        }
        else
        {
            rayDir = reflect(rayDir, normal);
            throughput *= F;
        }

        if (doSplit)
        {
            throughput *= 2;
            wasSplit = true;
        }


        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_MULT) != 0)
        {
            throughput *= h.albedo;
        }
        else if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_ADD) != 0)
        {
            throughput += h.albedo;
        }


        currentPayload = traceReflectionRefractionRay(rayOrigin, rayDir, instIndex, h.geometryInstanceFlags, doRefraction);
        rayStatsAdd(RAY_STATS_CATEGORY_REFLECTION_REFRACTION, 1);


        if (!doesPayloadContainHitInfo(currentPayload))
        {
            throughput *= getMediaTransmittance(currentRayMedia, pow(abs(dot(rayDir, globalUniform.worldUpVector.xyz)), -3));

            uvec4 q2SegFog1, q2SegFog2;
            q2FindFogVolumes(rayOrigin, rayDir, 0.0, 1e6, q2SegFog1, q2SegFog2);
            q2FogAccum = q2AlphaBlendPremultiplied(q2FogAccum, q2SegmentFog(q2SegFog1, q2SegFog2, 1e6));

            storeSky(pix, rayDir, true, throughput, wasSplit, q2FogAccum);
            return;
        }

        float rayLen;
        float emis;
        uint emisBlendCode;

        h = getHitInfoWithRayCone_ReflectionRefraction(
            currentPayload, rayCone,
            rayOrigin, rayDir, cameraRayDir,
            virtualPos,
            rayLen,
            motionCurToPrev, motionDepthLinearCurToPrev,
            emis,
            emisBlendCode,
            0.0
        );

        uvec4 q2SegFog1, q2SegFog2;
        q2FindFogVolumes(rayOrigin, rayDir, 0.0, rayLen, q2SegFog1, q2SegFog2);
        q2FogAccum = q2AlphaBlendPremultiplied(q2FogAccum, q2SegmentFog(q2SegFog1, q2SegFog2, rayLen));

        hitInfoWasOverwritten = true;
        throughput *= getMediaTransmittance(currentRayMedia, rayLen);
        propagateRayCone(rayCone, rayLen);
        fullPathLength += rayLen;
        q2LastSegmentLen = rayLen;
        prevHitPosition = h.hitPosition;
        screenEmission += h.albedo * emis * throughput;
        acidFog += getGlowingMediaFog(currentRayMedia, rayLen) * (doSplit ? 2.0 : 1.0);
    }


    if (!hitInfoWasOverwritten)
    {
        return;
    }


    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4(screenEmission + ( globalUniform.cameraMediaType != MEDIA_TYPE_ACID ? acidFog * 0.05 : vec3( 0.0 ) ), 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(acidFog, 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(fullPathLength));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(currentPayload));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));

    const float q2HalfConeAngle = texelFetch(framebufQ2BounceThroughput_Sampled, pix, 0).w;
    storeQ2GBuffer(pix, h.albedo, 1.0, vec2(0.0), h.metallic, h.roughness,
                   -fullPathLength, q2HalfConeAngle, q2LastSegmentLen,
                   vec3(0.0), 0.0, q2FogAccum, h.cluster);
}
#endif


#ifdef Q2_REFL_REFR_SHADER
void main()
{
    if (globalUniform.reflectRefractMaxDepth == 0)
    {
        return;
    }

    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);
    const vec3 cameraRayDir = getRayDir(inUV);

    if (imageLoad(framebufIsSky, pix).r != 0)
    {
        return;
    }

    const uvec3 primaryToReflRefrBuf = texelFetch(framebufPrimaryToReflRefr_Sampled, pix, 0).rgb;


    if (globalUniform.reflRefrEarlyOut != 0u)
    {
        const uint primaryFlags = primaryToReflRefrBuf.r;
        bool primaryNeedsReflRefr =
            (primaryFlags & (GEOM_INST_FLAG_MEDIA_TYPE_WATER | GEOM_INST_FLAG_MEDIA_TYPE_ACID |
                             GEOM_INST_FLAG_MEDIA_TYPE_GLASS)) != 0 ||
            (isPortalFromFlags(primaryFlags) && primaryToReflRefrBuf.b != PORTAL_INDEX_NONE);
        if (!primaryNeedsReflRefr && (primaryFlags & GEOM_INST_FLAG_REFLECT) != 0)
        {
            primaryNeedsReflRefr = imageLoad(framebufMetallicRoughness, pix).g < globalUniform.minRoughness;
        }
        if (!primaryNeedsReflRefr)
        {
            return;
        }
    }

    ShHitInfo h;
    h.albedo                            = imageLoad(framebufAlbedo, getRegularPixFromCheckerboardPix(pix)).rgb;
    h.hitPosition                       = imageLoad(framebufSurfacePosition, pix).xyz;
    h.geometryInstanceFlags             = primaryToReflRefrBuf.r;
    h.portalIndex                       = primaryToReflRefrBuf.b;
    h.normalGeom                        = decodeNormal(imageLoad(framebufNormalGeometry, pix).r);
    h.normal                            = decodeNormal(imageLoad(framebufNormal, pix).r);
    h.metallic                          = imageLoad(framebufMetallicRoughness, pix).r;
    h.roughness                         = imageLoad(framebufMetallicRoughness, pix).g;
    const vec3  motionBuf               = imageLoad(framebufMotion, pix).rgb;
    vec2        motionCurToPrev         = motionBuf.rg;
    float       motionDepthLinearCurToPrev = motionBuf.b;
    const float firstHitDepthLinear     = imageLoad(framebufDepthWorld, pix).r;
    vec3        screenEmission          = imageLoad(framebufScreenEmisRT, getRegularPixFromCheckerboardPix(pix)).rgb;
    vec3        acidFog                 = imageLoad(framebufAcidFogRT, getRegularPixFromCheckerboardPix(pix)).rgb;
    vec3        throughput              = imageLoad(framebufThroughput, pix).rgb;
    ShPayload currentPayload;
    currentPayload.instIdAndIndex       = primaryToReflRefrBuf.g;

    const vec4 q2BaseColor              = imageLoad(framebufQ2BaseColor, pix);
    /* The channel carries the primary surface's glass transparency (written by
       the primary pass), which the glass branch below absorbs the rays with. */
    h.transparency = clamp(q2BaseColor.a, 0.0, 1.0);
    h.glassParams = imageLoad(framebufQ2BounceThroughput, pix).xy;
    const float q2HalfConeAngle         = imageLoad(framebufQ2BounceThroughput, pix).w;
    vec4 q2Transparent                  = imageLoad(framebufQ2Transparent, pix);
    vec4 q2FogAccum                     = imageLoad(framebufQ2FogAccum, pix);

    RayCone rayCone;
    rayCone.width = 0;
    rayCone.spreadAngle = globalUniform.cameraRayConeSpreadAngle;

    float fullPathLength = firstHitDepthLinear;
    float q2LastSegmentLen = 0.0;
    vec3 prevHitPosition = h.hitPosition;
    bool wasSplit = false;
    bool wasPortal = false;
    vec3 virtualPos = h.hitPosition;
    vec3 rayDir = cameraRayDir;
    uint currentRayMedia = globalUniform.cameraMediaType;
    bool hitInfoWasOverwritten = false;
    bool pathReachedRefraction = false;
    const bool shaderGlassReflection = globalUniform.glassBlur != 0u &&
        (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0u;
    vec3 refrHitPosition = h.hitPosition;

    propagateRayCone(rayCone, firstHitDepthLinear);

    for (int i = 0; i < globalUniform.reflectRefractMaxDepth; i++)
    {
        const uint instIndex = unpackInstanceIdAndCustomIndex(currentPayload.instIdAndIndex).y;
        bool isPixOdd = isCheckerboardPixOdd(pix) != 0;

        uint newRayMedia = getNewRayMedia(i, currentRayMedia, h.geometryInstanceFlags);
        bool isPortal = isPortalFromFlags(h.geometryInstanceFlags) && h.portalIndex != PORTAL_INDEX_NONE;

        const bool primaryIsWater  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) != 0;
        const bool primaryIsSlime  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_ACID) != 0;
        const bool primaryIsGlass  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0;
    /* the roughness of the surface this iteration leaves, for the next cone */
    float traversalBlur = 0.0f;
        const bool primaryIsChrome = (h.geometryInstanceFlags & GEOM_INST_FLAG_REFLECT) != 0 &&
                                     h.roughness < globalUniform.minRoughness;

        if (!primaryIsWater && !primaryIsSlime && !primaryIsGlass && !primaryIsChrome && !isPortal)
        {
            break;
        }

        const vec3 normal = getNormal(h.hitPosition, h.normal, h.normalGeom, rayCone, rayDir,
                                      !isPortal && (newRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_WATER ||
                                                    newRayMedia == MEDIA_TYPE_ACID || currentRayMedia == MEDIA_TYPE_ACID),
                                      wasPortal);

        vec3 rayOrigin = h.hitPosition;
        bool doSplit = !wasSplit && !shaderGlassReflection;
        bool doRefraction = false;
        int correctMotionVector = 0;

        if (isPortal)
        {
            const ShPortalInstance portal = g_portals[h.portalIndex];

            const vec3 inCenter = portal.inPosition.xyz;
            const vec3 inWorldOffset = h.hitPosition - inCenter;

            mat3 inLookAt = lookAt(getPortalNormal(normal, inWorldOffset), globalUniform.worldUpVector.xyz);

            const vec3 outCenter = portal.outPosition.xyz;
            const mat3 outLookAt = lookAt(portal.outDirection.xyz,
                                          portal.outUp.xyz);

            rayDir = outLookAt * (transpose(inLookAt) * rayDir);

            const vec2 localOffset = vec2(dot(inWorldOffset, inLookAt[0]),
                                          dot(inWorldOffset, inLookAt[1]));

            rayOrigin = outCenter + localOffset.x * outLookAt[0] + localOffset.y * outLookAt[1];

            wasPortal = true;
            doSplit = false;
        }
        else if (primaryIsWater || primaryIsSlime)
        {
            const float ior = getIndexOfRefraction(primaryIsWater ? MEDIA_TYPE_WATER : MEDIA_TYPE_ACID);
            const vec3 reflected = reflect(rayDir, normal);
            const float nDotV = abs(dot(rayDir, normal));

            if (currentRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_ACID)
            {
                const vec3 refracted = refract(rayDir, normal, ior);
                float ndv = 1.0 - (1.0 - nDotV) * 3.0;
                if (ndv <= 0.0 || dot(refracted, refracted) == 0.0)
                {
                    rayDir = reflected;
                    correctMotionVector = 1;
                }
                else
                {
                    const float f = pow(1.0 - ndv, 5.0);
                    doRefraction = (i == 0) ? isPixOdd : isPixOdd;
                    if (doRefraction)
                    {
                        rayDir = refracted;
                        throughput *= (1.0 - f);
                        currentRayMedia = MEDIA_TYPE_VACUUM;
                        correctMotionVector = 2;
                    }
                    else
                    {
                        rayDir = reflected;
                        throughput *= f;
                        correctMotionVector = 1;
                    }
                    if (doSplit)
                    {
                        throughput *= 2.0;
                    }
                }
            }
            else
            {
                const vec3 refracted = refract(rayDir, normal, 1.0 / ior);
                const float F = 0.1 + 0.9 * pow(1.0 - nDotV, 5.0);
                doSplit = (i == 0);
                doRefraction = isPixOdd;
                if (doRefraction)
                {
                    rayDir = refracted;
                    throughput *= (1.0 - F);
                    currentRayMedia = newRayMedia;
                    correctMotionVector = 2;
                }
                else
                {
                    rayDir = reflected;
                    throughput *= F;
                    correctMotionVector = 1;
                }
                if (doSplit)
                {
                    throughput *= 2.0;
                }
            }
        }
        else if (primaryIsGlass)
        {
            // per-material refraction: a material may carry its own index, and a
            // slab thickness that moves where the ray leaves the pane
            const float ior = (h.glassParams.x > 0.0) ? clamp(h.glassParams.x, 1.0, 5.0)
                                                      : getIndexOfRefraction(MEDIA_TYPE_GLASS);
            const float thickness = max(h.glassParams.y, 0.0);
            vec3 glassGeomN = h.normalGeom;
            vec3 glassN = normal;

            float gnDotV = dot(rayDir, glassGeomN);
            if (gnDotV > 0)
            {
                glassGeomN = -glassGeomN;
                glassN = -glassN;
                gnDotV = -gnDotV;
            }

            vec3 entryN = globalUniform.glassBlur != 0u ? glassGeomN : glassN;

            vec3 reflected = reflect(rayDir, entryN);
            if (globalUniform.glassBlur == 0u && !isPixOdd && !wasSplit && dot(reflected, glassGeomN) < 0.01)
            {
                entryN = glassGeomN;
                reflected = reflect(rayDir, entryN);
            }
            const float nDotV = dot(rayDir, -entryN);
            const float F0 = pow((1.0 - ior) / (1.0 + ior), 2.0);
            float F = F0 + (1.0 - F0) * pow(1.0 - abs(nDotV), 5.0);
            if (dot(reflected, glassGeomN) < 0.01)
            {
                F = 0.0;
            }

            doSplit = globalUniform.glassBlur == 0u && !wasSplit;
            doRefraction = globalUniform.glassBlur == 0u && h.transparency > 0.0 && (doSplit ? isPixOdd : true);
            if (doRefraction)
            {
                const vec3 refr1 = refract(rayDir, entryN, 1.0 / ior);

                if (dot(refr1, refr1) == 0.0)
                {
                    /* the sampled facet passes nothing: the ray reflects off it */
                    rayDir = reflected;
                    throughput *= F;
                    correctMotionVector = 1;
                }
                else
                {
                    const vec3 refr2 = refract(refr1, glassGeomN, ior);

                    if (dot(refr2, refr2) > 0.0)
                    {
                        rayDir = refr2;
                        if (thickness > 0.0)
                        {
                            /* the pane has depth: the ray leaves it at the virtual far
                               face, pulled back in front of whatever the pane covers --
                               a wall at the junction closer than the thickness would
                               leave the next ray starting inside it, and the culled
                               back face would read as a hole */
                            const float cosT = max(-dot(refr1, entryN), 0.1);
                            const uint clearanceMask = getReflectionRefractionCullMask(instIndex, h.geometryInstanceFlags, true);

                            rayOrigin += refr1 * traceClearance(rayOrigin, refr1, thickness / cosT, clearanceMask);
                        }
                        /* transparency bleaches the diffuse filter: 1 lets the texture
                           block nothing, 0 applies its colour whole */
                        throughput *= (1.0 - F);
                        throughput *= mix(h.albedo, vec3(1.0), clamp(h.transparency, 0.0, 1.0)) * clamp(h.transparency, 0.0, 1.0);
                        correctMotionVector = 2;
                    }
                    else
                    {
                        /* total internal reflection at the far face */
                        rayDir = reflect(rayDir, entryN);
                        throughput *= F;
                        correctMotionVector = 1;
                    }
                }
            }
            else
            {
                rayDir = reflected;
                throughput *= F;
                correctMotionVector = 1;
            }

            traversalBlur = 0.0;

            if (doSplit)
            {
                throughput *= 2.0;
            }
        }
        else
        {
            throughput *= h.albedo;
            rayDir = reflect(rayDir, normal);
            correctMotionVector = 1;
        }

        if (doSplit)
        {
            wasSplit = true;
        }

        currentPayload = traceReflectionRefractionRay(rayOrigin, rayDir, instIndex, h.geometryInstanceFlags, doRefraction);
        rayStatsAdd(RAY_STATS_CATEGORY_REFLECTION_REFRACTION, 1);

        if (!doesPayloadContainHitInfo(currentPayload))
        {
            const float skyLod = primaryIsGlass && globalUniform.glassBlur == 0u ? 0.0 : h.roughness * (SKY_MIP_COUNT - 1.0);
            const vec3 env = getSkyVisibleFiltered(rayDir, skyLod);
            q2Transparent = q2AlphaBlendPremultiplied(vec4(env * throughput, 1.0), q2Transparent);

            uvec4 q2SegFog1, q2SegFog2;
            q2FindFogVolumes(rayOrigin, rayDir, 0.0, 1e6, q2SegFog1, q2SegFog2);
            q2FogAccum = q2AlphaBlendPremultiplied(q2SegmentFog(q2SegFog1, q2SegFog2, 1e6), q2FogAccum);

            if (correctMotionVector == 2 && isPixOdd)
            {
                const ivec2 refrPix = getRegularPixFromCheckerboardPix(pix);
                const ivec2 pairPix = ivec2(refrPix.x + (refrPix.x % 2 == 0 ? 1 : -1), refrPix.y);
                imageStore(framebufDepthNdc, refrPix, vec4(1.0));
                imageStore(framebufDepthNdc, pairPix, vec4(1.0));
            }

            storeSky(pix, rayDir, true, throughput, wasSplit, q2FogAccum);
            storeQ2GBuffer(pix, vec3(0.0), 1.0, vec2(0.0), 0.0, 1.0, -MAX_RAY_LENGTH * 2.0, q2HalfConeAngle, MAX_RAY_LENGTH * 2.0,
                           q2Transparent.rgb, q2Transparent.a, q2FogAccum, ~0u);

            /* The transport's spare RGB carries the segment's own origin to the
               god-rays reflection pass, whose sky continuation would otherwise
               march from the camera and count the primary segment twice. */
            imageStore(framebufQ2GodRaysThroughputDist, pix, vec4(rayOrigin, MAX_RAY_LENGTH * 2.0));
            return;
        }

        float rayLen;
        float emis;
        uint emisBlendCode;

        h = getHitInfoWithRayCone_ReflectionRefraction(
            currentPayload, rayCone,
            rayOrigin, rayDir, cameraRayDir,
            virtualPos,
            rayLen,
            motionCurToPrev, motionDepthLinearCurToPrev,
            emis,
            emisBlendCode,
            traversalBlur
        );

        uvec4 q2SegFog1, q2SegFog2;
        q2FindFogVolumes(rayOrigin, rayDir, 0.0, rayLen, q2SegFog1, q2SegFog2);
        q2FogAccum = q2AlphaBlendPremultiplied(q2SegmentFog(q2SegFog1, q2SegFog2, rayLen), q2FogAccum);

        hitInfoWasOverwritten = true;
        if (correctMotionVector == 2 && isPixOdd)
        {
            pathReachedRefraction = true;
            refrHitPosition = h.hitPosition;
        }
        throughput *= getMediaTransmittance(currentRayMedia, rayLen);
        propagateRayCone(rayCone, rayLen);
        fullPathLength += rayLen;
        q2LastSegmentLen = rayLen;
        prevHitPosition = h.hitPosition;
        screenEmission += h.albedo * emis * throughput;
        acidFog += getGlowingMediaFog(currentRayMedia, rayLen) * (doSplit ? 2.0 : 1.0);
    }


    if (!hitInfoWasOverwritten)
    {
        return;
    }

    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4(screenEmission + ( globalUniform.cameraMediaType != MEDIA_TYPE_ACID ? acidFog * 0.05 : vec3( 0.0 ) ), 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(acidFog, 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(fullPathLength));
    if (pathReachedRefraction)
    {
        const vec4 refrClipPos = globalUniform.projection * globalUniform.view * vec4(refrHitPosition, 1.0);
        const float refrDepthNdc = clamp(refrClipPos.z / refrClipPos.w + 1e-5, 0.0, 1.0);
        const ivec2 refrPix = getRegularPixFromCheckerboardPix(pix);
        const ivec2 pairPix = ivec2(refrPix.x + (refrPix.x % 2 == 0 ? 1 : -1), refrPix.y);
        imageStore(framebufDepthNdc, refrPix, vec4(refrDepthNdc));
        imageStore(framebufDepthNdc, pairPix, vec4(refrDepthNdc));
    }
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(currentPayload));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));

    storeQ2GBuffer(pix, h.albedo, h.transparency, h.glassParams, h.metallic, h.roughness,
                   -fullPathLength, q2HalfConeAngle, q2LastSegmentLen,
                   q2Transparent.rgb, q2Transparent.a, q2FogAccum, h.cluster);
}
#endif
