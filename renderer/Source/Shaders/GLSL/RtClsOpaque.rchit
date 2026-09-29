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

#version 460
#extension GL_EXT_ray_tracing : require

#include "ShaderCommonGLSLFunc.h"

layout(location = PAYLOAD_INDEX_DEFAULT) rayPayloadInEXT ShPayload g_payload;
hitAttributeEXT vec2 inBaryCoords;

void main()
{
    g_payload.baryCoords = inBaryCoords;
    g_payload.instIdAndIndex = packInstanceIdAndCustomIndex(gl_InstanceID, gl_InstanceCustomIndexEXT);
    g_payload.geomAndPrimIndex = packGeometryAndPrimitiveIndex(gl_GeometryIndexEXT, gl_PrimitiveID);
}
