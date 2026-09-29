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


#ifndef SHADER_COMMON_HLSL_FUNC_HLSLI_
#define SHADER_COMMON_HLSL_FUNC_HLSLI_
#include "ShaderCommonHLSL.hlsli"
#include "Structs.hlsli"




#define FAKE_ROUGH_SPECULAR_THRESHOLD 0.5
#define FAKE_ROUGH_SPECULAR_LENGTH 0.25

#define SHIPPING_HACK 1
#define ILLUMINATION_VOLUME 0



#ifdef DESC_SET_TEXTURES
[[vk::binding(BINDING_TEXTURES, DESC_SET_TEXTURES)]]         Texture2D<float4> globalTextures[];
[[vk::binding(BINDING_TEXTURES_SAMPLER, DESC_SET_TEXTURES)]] SamplerState      globalTextures_Sampler[];

#define getTexture(idx) globalTextures[NonUniformResourceIndex(idx)]
#define getTextureSampler(idx) globalTextures_Sampler[NonUniformResourceIndex(idx)]

float4 getTextureSample(uint textureIndex, const float2 texCoord)
{
    return getTexture(textureIndex).Sample(getTextureSampler(textureIndex), texCoord);
}

float4 getTextureSampleLod(uint textureIndex, const float2 texCoord, float lod)
{
    return getTexture(textureIndex).SampleLevel(getTextureSampler(textureIndex), texCoord, lod);
}

float4 getTextureSampleGrad(uint textureIndex, const float2 texCoord, const float2 dPdx, const float2 dPdy)
{
    return getTexture(textureIndex).SampleGrad(getTextureSampler(textureIndex), texCoord, dPdx, dPdy);
}

int2 getTextureSize(uint textureIndex, uint mipLevel)
{
    uint width, height, levelCount;
    getTexture(textureIndex).GetDimensions(mipLevel, width, height, levelCount);
    return int2(width, height);
}
#endif



uint packInstanceIdAndCustomIndex(int instanceID, int instanceCustomIndexEXT)
{
    return (instanceID << 24) | instanceCustomIndexEXT;
}

int2 unpackInstanceIdAndCustomIndex(uint instanceIdAndIndex)
{
    return int2(
        instanceIdAndIndex >> 24,
        instanceIdAndIndex & 0xFFFFFF
    );
}

void unpackInstanceIdAndCustomIndex(uint instanceIdAndIndex, out int instanceId, out int instanceCustomIndexEXT)
{
    instanceId = int(instanceIdAndIndex >> 24);
    instanceCustomIndexEXT = int(instanceIdAndIndex & 0xFFFFFF);
}

uint packGeometryAndPrimitiveIndex(int geometryIndex, int primitiveIndex)
{
#if MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW + MAX_GEOMETRY_PRIMITIVE_COUNT_POW != 32
    #error The sum of MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW and MAX_GEOMETRY_PRIMITIVE_COUNT_POW must be 32 for packing geometry and primitive index
#endif

    return (primitiveIndex << MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW) | geometryIndex;
}

int2 unpackGeometryAndPrimitiveIndex(uint geomAndPrimIndex)
{
#if (1 << MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW) != MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT
    #error MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT must be (1 << MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW)
#endif

    return int2(
        geomAndPrimIndex >> MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW,
        geomAndPrimIndex & (MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT - 1)
    );
}

void unpackGeometryAndPrimitiveIndex(uint geomAndPrimIndex, out int geometryIndex, out int primitiveIndex)
{
#if (1 << MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW) != MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT
    #error MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT must be (1 << MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW)
#endif

    primitiveIndex = int(geomAndPrimIndex >> MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT_POW);
    geometryIndex = int(geomAndPrimIndex & (MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT - 1));
}



#ifdef DESC_SET_GLOBAL_UNIFORM
#ifdef DESC_SET_VERTEX_DATA
#include "VertexData.hlsli"
#endif
#endif



#ifdef DESC_SET_TONEMAPPING
#ifdef TONEMAPPING_BUFFER_WRITEABLE
[[vk::binding(BINDING_LUM_HISTOGRAM, DESC_SET_TONEMAPPING)]] RWStructuredBuffer<ShTonemapping> tonemapping;
#else
[[vk::binding(BINDING_LUM_HISTOGRAM, DESC_SET_TONEMAPPING)]] StructuredBuffer<ShTonemapping> tonemapping;
#endif
#endif



#ifdef DESC_SET_LIGHT_SOURCES
[[vk::binding(BINDING_LIGHT_SOURCES, DESC_SET_LIGHT_SOURCES)]]                       StructuredBuffer<ShLightEncoded> lightSources;
[[vk::binding(BINDING_LIGHT_SOURCES_PREV, DESC_SET_LIGHT_SOURCES)]]                  StructuredBuffer<ShLightEncoded> lightSources_Prev;
[[vk::binding(BINDING_LIGHT_SOURCES_INDEX_PREV_TO_CUR, DESC_SET_LIGHT_SOURCES)]]     StructuredBuffer<uint> lightSources_Index_PrevToCur;
[[vk::binding(BINDING_LIGHT_SOURCES_INDEX_CUR_TO_PREV, DESC_SET_LIGHT_SOURCES)]]     StructuredBuffer<uint> lightSources_Index_CurToPrev;
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_OFFSETS, DESC_SET_LIGHT_SOURCES)]] StructuredBuffer<uint> q2LightListOffsets;
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_LIGHT_LIST_LIGHTS, DESC_SET_LIGHT_SOURCES)]]  StructuredBuffer<uint> q2LightListLights;
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_LIGHT_STATS, DESC_SET_LIGHT_SOURCES)]]        RWStructuredBuffer<uint> q2LightStats;
[[vk::binding(BINDING_LIGHT_SOURCES_TAL_CDF, DESC_SET_LIGHT_SOURCES)]]               StructuredBuffer<uint> talCdf;
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_CLUSTER_SKY_VIS, DESC_SET_LIGHT_SOURCES)]]    StructuredBuffer<uint> q2ClusterSkyVis;
#endif



#ifdef DESC_SET_VOLUMETRIC
[[vk::binding(BINDING_VOLUMETRIC_STORAGE, DESC_SET_VOLUMETRIC), vk::image_format("rgba16f")]]
RWTexture3D<float4> g_volumetric;

[[vk::binding(BINDING_VOLUMETRIC_SAMPLED, DESC_SET_VOLUMETRIC)]]
Texture3D<float4> g_volumetric_Sampled;

[[vk::binding(BINDING_VOLUMETRIC_SAMPLER, DESC_SET_VOLUMETRIC)]]
SamplerState g_volumetric_Sampler;

[[vk::binding(BINDING_VOLUMETRIC_SAMPLED_PREV, DESC_SET_VOLUMETRIC)]]
Texture3D<float4> g_volumetric_Sampled_Prev;

[[vk::binding(BINDING_VOLUMETRIC_SAMPLER_PREV, DESC_SET_VOLUMETRIC)]]
SamplerState g_volumetric_Sampler_Prev;

[[vk::binding(BINDING_VOLUMETRIC_ILLUMINATION, DESC_SET_VOLUMETRIC), vk::image_format("r11g11b10f")]]
RWTexture3D<float4> g_illuminationVolume;

[[vk::binding(BINDING_VOLUMETRIC_ILLUMINATION_SAMPLED, DESC_SET_VOLUMETRIC)]]
Texture3D<float4> g_illuminationVolume_Sampled;

[[vk::binding(BINDING_VOLUMETRIC_ILLUMINATION_SAMPLER, DESC_SET_VOLUMETRIC)]]
SamplerState g_illuminationVolume_Sampler;
#endif



#ifdef DESC_SET_LENS_FLARES
[[vk::binding(BINDING_LENS_FLARES_CULLING_INPUT, DESC_SET_LENS_FLARES)]]
StructuredBuffer<ShIndirectDrawCommand> lensFlareCullingInput;

struct LensFlareDrawCmds_BT
{
    ShIndirectDrawCommand lensFlareDrawCmds[LENS_FLARES_MAX_DRAW_CMD_COUNT];
    uint lensFlareDrawCmdsCount;
};

[[vk::binding(BINDING_LENS_FLARES_DRAW_CMDS, DESC_SET_LENS_FLARES)]]
RWStructuredBuffer<LensFlareDrawCmds_BT> lensFlareDrawCmds;
#endif



#ifdef DESC_SET_DECALS
[[vk::binding(BINDING_DECAL_INSTANCES, DESC_SET_DECALS)]] RWStructuredBuffer<ShDecalInstance> decalInstances;
#endif



#define CHECKERBOARD_SEPARATOR_DIVISOR 2

#ifdef DESC_SET_GLOBAL_UNIFORM
#ifdef DESC_SET_FRAMEBUFFERS
float2 getPrevScreenPos(const float2 motionCurToPrev, const int2 pix)
{
    const float2 screenSize = float2(globalUniform.renderWidth / float(CHECKERBOARD_SEPARATOR_DIVISOR), globalUniform.renderHeight);
    const float2 invScreenSize = float2(1.0 / screenSize.x, 1.0 / screenSize.y);

    return ((float2(pix) + (float2)0.5) * invScreenSize + motionCurToPrev) * screenSize;
}

float2 getPrevScreenPos(Texture2D<float4> motionTexture, const int2 pix)
{
    return getPrevScreenPos(motionTexture.Load(int3(pix, 0)).rg, pix);
}
#endif
#endif


#ifdef DESC_SET_GLOBAL_UNIFORM
    #define CHECKERBOARD_FULL_WIDTH globalUniform.renderWidth
    #define CHECKERBOARD_FULL_HEIGHT globalUniform.renderHeight
#endif

#ifdef CHECKERBOARD_FULL_WIDTH
#ifdef CHECKERBOARD_FULL_HEIGHT
int getCheckerboardSeparatorX()
{
    return int(CHECKERBOARD_FULL_WIDTH) / CHECKERBOARD_SEPARATOR_DIVISOR;
}

int isRegularPixOdd(const int2 pix)
{
    return (pix.x + pix.y % 2) % 2;
}

int isCheckerboardPixOdd(const int2 checkerboardPix)
{
    return int(checkerboardPix.x >= getCheckerboardSeparatorX());
}

int2 getCheckerboardPix(const int2 pix)
{
    const int isOdd = isRegularPixOdd(pix);

    return int2(
        isOdd * getCheckerboardSeparatorX() + pix.x / 2,
        pix.y
    );
}

int2 getRegularPixFromCheckerboardPix(const int2 checkerboardPix)
{
    const int sep = getCheckerboardSeparatorX();
    const int isOdd = int(checkerboardPix.x >= sep);

    int x = checkerboardPix.x - isOdd * sep;

    return int2(
        x * 2 + (isOdd + checkerboardPix.y) % 2,
        checkerboardPix.y
    );
}

int3 getCheckerboardedRenderArea(const int2 checkerboardPix)
{
    const int sep = getCheckerboardSeparatorX();
    const int isOdd = isCheckerboardPixOdd(checkerboardPix);

    return int3(
        (isOdd + 0) * sep,
        (isOdd + 1) * sep,
        CHECKERBOARD_FULL_HEIGHT
    );
}
#endif
#endif

bool testPixInRenderArea(const int2 pix, const int3 renderArea)
{
    return
        pix.y >= 0              && pix.y < renderArea[2] &&
        pix.x >= renderArea[0]  && pix.x < renderArea[1];
}

bool testReprojectedDepth(float z, float zPrev, float zMotion)
{
    return abs(z - zPrev + zMotion) / abs(z) < 0.1;
}

bool testReprojectedNormal(const float3 n, const float3 nPrev)
{
    return dot(n, nPrev) > 0.95;
}

bool testReprojectedNormalEnc(const float3 n, const uint encodedNPrev)
{
    return testReprojectedNormal(n, decodeNormal(encodedNPrev));
}

float getAntilagAlpha(const float gradSample, const float normFactor)
{
    const float lambda = normFactor > 0.0001 ?
        clamp(abs(gradSample) / normFactor, 0.0, 1.0) :
        0.0;

    return clamp(lambda, 0.0, 1.0);
}



#ifdef DESC_SET_FRAMEBUFFERS
#include "SphericalHarmonics.hlsli"

#define SH_COMPRESSION_MULTIPLIER 1000

SH texelFetchSH(Texture2D<float4> textureIndirR, Texture2D<float4> textureIndirG, Texture2D<float4> textureIndirB, int2 pix)
{
    SH sh;
    sh.r = textureIndirR.Load(int3(pix, 0)) / SH_COMPRESSION_MULTIPLIER;
    sh.g = textureIndirG.Load(int3(pix, 0)) / SH_COMPRESSION_MULTIPLIER;
    sh.b = textureIndirB.Load(int3(pix, 0)) / SH_COMPRESSION_MULTIPLIER;

    return sh;
}

SH texelFetchUnfilteredIndirectSH(int2 pix)
{
    return texelFetchSH(
        framebufUnfilteredIndirectSH_R_Sampled,
        framebufUnfilteredIndirectSH_G_Sampled,
        framebufUnfilteredIndirectSH_B_Sampled,
        pix);
}

SH texelFetchIndirAccumSH(int2 pix)
{
    return texelFetchSH(
        framebufIndirAccumSH_R_Sampled,
        framebufIndirAccumSH_G_Sampled,
        framebufIndirAccumSH_B_Sampled,
        pix);
}

SH texelFetchIndirAccumSH_Prev(int2 pix)
{
    return texelFetchSH(
        framebufIndirAccumSH_R_Prev_Sampled,
        framebufIndirAccumSH_G_Prev_Sampled,
        framebufIndirAccumSH_B_Prev_Sampled,
        pix);
}

SH imageLoadUnfilteredIndirectSH(int2 pix)
{
    SH sh;
    sh.r = framebufUnfilteredIndirectSH_R[pix] / SH_COMPRESSION_MULTIPLIER;
    sh.g = framebufUnfilteredIndirectSH_G[pix] / SH_COMPRESSION_MULTIPLIER;
    sh.b = framebufUnfilteredIndirectSH_B[pix] / SH_COMPRESSION_MULTIPLIER;

    return sh;
}

void imageStoreUnfilteredIndirectSH(int2 pix, const SH sh)
{
    framebufUnfilteredIndirectSH_R[pix] = sh.r * SH_COMPRESSION_MULTIPLIER;
    framebufUnfilteredIndirectSH_G[pix] = sh.g * SH_COMPRESSION_MULTIPLIER;
    framebufUnfilteredIndirectSH_B[pix] = sh.b * SH_COMPRESSION_MULTIPLIER;
}

void imageStoreIndirAccumSH(int2 pix, const SH sh)
{
    framebufIndirAccumSH_R[pix] = sh.r * SH_COMPRESSION_MULTIPLIER;
    framebufIndirAccumSH_G[pix] = sh.g * SH_COMPRESSION_MULTIPLIER;
    framebufIndirAccumSH_B[pix] = sh.b * SH_COMPRESSION_MULTIPLIER;
}

void imageStoreIndirPingSH(int2 pix, const SH sh)
{
    framebufIndirPingSH_R[pix] = sh.r * SH_COMPRESSION_MULTIPLIER;
    framebufIndirPingSH_G[pix] = sh.g * SH_COMPRESSION_MULTIPLIER;
    framebufIndirPingSH_B[pix] = sh.b * SH_COMPRESSION_MULTIPLIER;
}

void imageStoreIndirPongSH(int2 pix, const SH sh)
{
    framebufIndirPongSH_R[pix] = sh.r * SH_COMPRESSION_MULTIPLIER;
    framebufIndirPongSH_G[pix] = sh.g * SH_COMPRESSION_MULTIPLIER;
    framebufIndirPongSH_B[pix] = sh.b * SH_COMPRESSION_MULTIPLIER;
}

float3 texelFetchNormal(const int2 pix)
{
    return decodeNormal(framebufNormal_Sampled.Load(int3(pix, 0)).r);
}

float3 texelFetchNormal_Prev(const int2 pix)
{
    return decodeNormal(framebufNormal_Prev_Sampled.Load(int3(pix, 0)).r);
}

float3 texelFetchNormalGeometry(const int2 pix)
{
    return decodeNormal(framebufNormalGeometry_Sampled.Load(int3(pix, 0)).r);
}

float3 texelFetchNormalGeometry_Prev(const int2 pix)
{
    return decodeNormal(framebufNormalGeometry_Prev_Sampled.Load(int3(pix, 0)).r);
}

uint4 textureGatherEncNormalGeometry_Prev(const float2 uv)
{
    return framebufNormalGeometry_Prev_Sampled.GatherRed(framebufNormalGeometry_Prev_Sampler, uv, int2(0, 0));
}

uint texelFetchEncNormal(const int2 pix)
{
    return framebufNormal_Sampled.Load(int3(pix, 0)).r;
}

uint texelFetchEncNormalGeometry(const int2 pix)
{
    return framebufNormalGeometry_Sampled.Load(int3(pix, 0)).r;
}

void imageStoreNormal(const int2 pix, const float3 normal)
{
    framebufNormal[pix] = (uint4)encodeNormal(normal);
}

void imageStoreNormalGeometry(const int2 pix, const float3 normal)
{
    framebufNormalGeometry[pix] = (uint4)encodeNormal(normal);
}

bool isSkyPix(const int2 pix)
{
    return framebufIsSky_Sampled.Load(int3(pix, 0)).r != 0;
}

bool wasOnlyPrimary( float t )
{
    return abs( t ) < 0.5;
}

bool wasWithoutSplit( float t )
{
    return t < 0.5;
}

bool wasSplit( float t )
{
    return t > 0.5;
}

bool needResolveCheckerboard( const int2 checkerboardPix )
{
    float t = framebufThroughput_Sampled.Load(int3(checkerboardPix, 0)).a;
    return wasSplit( t );
}
#endif



float rmeEmissionToScreenEmission( float rmeEmis )
{
    return clamp( rmeEmis, 0.0, 1.0 );
}



float2 getColumn(const float2x3 m, const int i) { return transpose(m)[i]; }
float3 getColumn(const float3x2 m, const int i) { return transpose(m)[i]; }
float3 getColumn(const float3x3 m, const int i) { return transpose(m)[i]; }
float4 getColumn(const float4x4 m, const int i) { return transpose(m)[i]; }



#ifdef DESC_SET_GLOBAL_UNIFORM
float3 getRayDir( float2 inUV )
{
    inUV = inUV * 2.0 - 1.0;

    float4 target   = mul( globalUniform.invProjection, float4( inUV.x, inUV.y, 1, 1 ) );
    float3 localDir = abs( target.w ) < 0.001 ? target.xyz : target.xyz / target.w;

    float4 rayDir = mul( globalUniform.invView, float4( normalize( localDir ), 0 ) );

    return rayDir.xyz;
}

float2 getPixelUVWithJitter( const int2 pix )
{
    const float2 pixelCenter = float2( pix ) + (float2)0.5;
    const float2 jitter      = float2( globalUniform.jitterX, globalUniform.jitterY );

    return ( pixelCenter + jitter ) / float2( globalUniform.renderWidth, globalUniform.renderHeight );
}

float3 getRayDirAX( float2 inUV )
{
    const float AX = 1.0 / globalUniform.renderWidth;
    return getRayDir( inUV + float2( AX, 0 ) );
}

float3 getRayDirAY( float2 inUV )
{
    const float AY = 1.0 / globalUniform.renderHeight;
    return getRayDir( inUV + float2( 0, AY ) );
}
#endif

#endif
