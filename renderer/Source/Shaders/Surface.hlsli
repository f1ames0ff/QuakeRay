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

#ifndef SURFACE_HLSLI_
#define SURFACE_HLSLI_
#include "BRDF.hlsli"


struct Surface
{
    float3  position;
    uint    instCustomIndex;
    float3  normalGeom;
    float   roughness;
    float3  normal;
    float3  albedo;
    bool    isSky;
    float3  specularColor;
    float   emission;
    float3  toViewerDir;
    uint    cluster;
};


#if defined(DESC_SET_FRAMEBUFFERS)
#if !defined(FRAMEBUF_IGNORE_ATTACHMENTS)
#if defined(CHECKERBOARD_FULL_WIDTH) && defined(CHECKERBOARD_FULL_HEIGHT)

Surface fetchGbufferSurface(const int2 pix)
{
    Surface s;
    s.isSky = isSkyPix(pix);

    if (s.isSky)
    {
        return s;
    }

    s.albedo = framebufAlbedo_Sampled.Load(int3(getRegularPixFromCheckerboardPix(pix), 0)).rgb;
    s.emission = getLuminance(framebufScreenEmisRT_Sampled.Load(int3(getRegularPixFromCheckerboardPix(pix), 0)).rgb);
    {
        float4 posEnc           = framebufSurfacePosition_Sampled.Load(int3(pix, 0));
        s.position              = posEnc.xyz;
        s.instCustomIndex       = asuint(posEnc.a);
    }
    {
        float2 metallicRoughness  = framebufMetallicRoughness_Sampled.Load(int3(pix, 0)).xy;
        s.specularColor           = getSpecularColor(s.albedo, metallicRoughness[0]);
        s.roughness               = metallicRoughness[1];
    }
    s.normalGeom                = texelFetchNormalGeometry(pix);
    s.normal                    = texelFetchNormal(pix);
    s.toViewerDir               = -framebufViewDirection.Load(pix).xyz;
    s.cluster                   = framebufQ2Cluster_Sampled.Load(int3(pix, 0)).r;
    return s;
}

Surface fetchGbufferSurface_NoAlbedoViewDir_Prev(const int2 pix)
{
    Surface s;
    s.isSky = false;
    s.albedo = (float3)1.0;
    s.emission = 0.0;
    {
        float4 posEnc           = framebufSurfacePosition_Prev_Sampled.Load(int3(pix, 0));
        s.position              = posEnc.xyz;
        s.instCustomIndex       = asuint(posEnc.a);
    }
    {
        float2 metallicRoughness  = framebufMetallicRoughness_Prev_Sampled.Load(int3(pix, 0)).xy;
        s.specularColor           = getSpecularColor(s.albedo, metallicRoughness[0]);
        s.roughness               = metallicRoughness[1];
    }
    s.normalGeom                = texelFetchNormalGeometry_Prev(pix);
    s.normal                    = texelFetchNormal_Prev(pix);
    s.toViewerDir               = (float3)0.0;
    return s;
}
#endif
#endif
#endif


Surface hitInfoToSurface_Indirect(const ShHitInfo h, const float3 rayDirection)
{
    Surface s;
    s.position = h.hitPosition;
    s.instCustomIndex = h.instCustomIndex;
    s.normalGeom = h.normalGeom;
    s.roughness = h.roughness;
    s.normal = h.normalGeom;
    s.albedo = h.albedo;
    s.isSky = false;
    s.specularColor = getSpecularColor(h.albedo, h.metallic);
    s.emission = h.emission;
    s.toViewerDir = -rayDirection;
    s.cluster = h.cluster;
    return s;
}

#endif
