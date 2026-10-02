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

#version 460

// The packed unfiltered-direct image is decoded with the shared helper, exactly as every other
// shader that reads a packed framebuffer does.
#include "Utils.h"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// Set 0 is the pass's only set, and it holds all three resources at the bindings NVRHI assigns to
// slot 0: the constant buffer at 256, the ALBEDO image at 0 and its sampler at 128. The present
// reads the engine's ALBEDO render target - the image the raster passes of the frame write - and
// writes the display-ready value to the single colour attachment of the pass.
//
// Unlike RhiSkeleton.frag there is no bindless table and no set 1: the present samples exactly one
// image, so a table of its own would declare a descriptor the host would have to fill for nothing.
layout(std140, set = 0, binding = 256) uniform RhiPresentParams
{
    vec4 exposure; // x = exposure multiplier applied before the curve (unused in the
                   // display-referred mode); y = vertical mirror of the sample coordinate (0 or 1);
                   // z = non-zero enables the direct-lighting term of the traced chain (kept zero
                   // in the display-referred mode); w = the display-referred switch: non-zero
                   // passes the sample through raw - no exposure multiply, no direct-lighting term
                   // and no x / (1 + x) curve - and zero keeps the diagnostic compose
} params;

layout(set = 0, binding = 0) uniform texture2D albedoTexture;
layout(set = 0, binding = 128) uniform sampler albedoTexture_Sampler;
layout(set = 0, binding = 384, r32ui) uniform uimage2D directTexture;

void main()
{
    // The sample coordinate: params.exposure.y mirrors it vertically. The traced modes' ALBEDO is
    // written by the engine's ray-tracing passes, whose pixel-to-UV convention puts the view's
    // first row in the image's first row, while the raster passes of the frame feed this present in
    // NVRHI's raster convention, which puts the view's first row in the image's last row (the two
    // were measured in RhiDebugTrace.rgen.hlsl:107-114). The host sets the flag per frame mode, so
    // one present serves both.
    const vec2 uv = vec2(vUV.x, mix(vUV.y, 1.0 - vUV.y, params.exposure.y));

    // LOD 0 is explicit, as in RhiSkeleton.frag: the target can differ in size from the source,
    // and an implicit LOD would make the presented image depend on the screen's derivative.
    const vec3 albedo = textureLod(sampler2D(albedoTexture, albedoTexture_Sampler), uv, 0.0).rgb;

    // The direct-lighting term of the traced chain: the image is checkerboard-packed, so this
    // screen pixel's trace is addressed through the same mapping the raygens write with
    // (getCheckerboardPix, ShaderCommonGLSLFunc.h:332-341; the render width halves into the two
    // fields). params.exposure.z is set while the direct pass runs; the raster and debug modes leave
    // it zero and read nothing.
    vec3 direct = vec3(0.0);
    if (params.exposure.z != 0.0)
    {
        const ivec2 size = textureSize(sampler2D(albedoTexture, albedoTexture_Sampler), 0);

        const ivec2 p = clamp( ivec2( uv * vec2(size) ), ivec2(0), size - ivec2(1) );
        const int sep = size.x / 2;
        const int odd = ( p.x + p.y % 2 ) % 2;
        const ivec2 cb = ivec2( odd * sep + p.x / 2, p.y );

        direct = decodeE5B9G9R9( imageLoad(directTexture, cb).r );
    }

    // The diagnostic compose: the exposure scales the linear HDR value, then the monotone
    // x / (1 + x) curve folds the unbounded result into [0, 1) before it reaches the display
    // attachment. The direct term is added to the albedo (sky pixels keep their color).
    const vec3 illuminated = albedo * ( 1.0 + direct );
    const vec3 exposed = illuminated * params.exposure.x;
    const vec3 curved = exposed / (1.0 + exposed);

    // params.exposure.w is the display-referred switch: after CmPrepareFinal the sample is already
    // the display-referred linear image, so it must reach the attachment raw; the diagnostic path
    // keeps the compose above.
    const vec3 display = params.exposure.w != 0.0 ? albedo : curved;

    oColor = vec4(display, 1.0);
}
