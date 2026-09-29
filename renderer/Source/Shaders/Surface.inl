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

#ifndef SURFACE_INL_
#define SURFACE_INL_

#include "BRDF.h"


struct Surface
{
    vec3    position;
    uint    instCustomIndex;
    vec3    normalGeom;
    float   roughness;
    vec3    normal;
    vec3    albedo;
    bool    isSky;
    vec3    specularColor;
    float   emission;
    vec3    toViewerDir;
    uint    cluster;
};


#if defined(DESC_SET_FRAMEBUFFERS)
#if !defined(FRAMEBUF_IGNORE_ATTACHMENTS)
#if defined(CHECKERBOARD_FULL_WIDTH) && defined(CHECKERBOARD_FULL_HEIGHT)

Surface fetchGbufferSurface(const ivec2 pix)
{
    Surface s;
    s.isSky = isSkyPix(pix);

    if (s.isSky)
    {
        return s;
    }

    s.albedo = texelFetch(framebufAlbedo_Sampled, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    s.emission = getLuminance(texelFetch(framebufScreenEmisRT_Sampled, getRegularPixFromCheckerboardPix(pix), 0).rgb);
    {
        vec4 posEnc             = texelFetch(framebufSurfacePosition_Sampled, pix, 0);
        s.position              = posEnc.xyz;
        s.instCustomIndex       = floatBitsToUint(posEnc.a);
    }
    {
        vec2 metallicRoughness  = texelFetch(framebufMetallicRoughness_Sampled, pix, 0).xy;
        s.specularColor         = getSpecularColor(s.albedo, metallicRoughness[0]);
        s.roughness             = metallicRoughness[1];
    }
    s.normalGeom                = texelFetchNormalGeometry(pix);
    s.normal                    = texelFetchNormal(pix);
    s.toViewerDir               = -imageLoad(framebufViewDirection, pix).xyz;
    s.cluster                   = texelFetch(framebufQ2Cluster_Sampled, pix, 0).r;
    return s;
}

Surface fetchGbufferSurface_NoAlbedoViewDir_Prev(const ivec2 pix)
{
    Surface s;
    s.isSky = false;
    s.albedo = vec3(1.0);
    s.emission = 0.0;
    {
        vec4 posEnc             = texelFetch(framebufSurfacePosition_Prev_Sampled, pix, 0);
        s.position              = posEnc.xyz;
        s.instCustomIndex       = floatBitsToUint(posEnc.a);
    }
    {
        vec2 metallicRoughness  = texelFetch(framebufMetallicRoughness_Prev_Sampled, pix, 0).xy;
        s.specularColor         = getSpecularColor(s.albedo, metallicRoughness[0]);
        s.roughness             = metallicRoughness[1];
    }
    s.normalGeom                = texelFetchNormalGeometry_Prev(pix);
    s.normal                    = texelFetchNormal_Prev(pix);
    s.toViewerDir               = vec3(0.0);
    return s;
}
#endif
#endif
#endif


Surface hitInfoToSurface_Indirect(const ShHitInfo h, const vec3 rayDirection)
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
