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

#define MATERIAL_MAX_ALBEDO_LAYERS 0

#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#include "ShaderCommonHLSLFunc.hlsli"

#include "TurbWarp.hlsli"

struct HitAttributes
{
    float2 inBaryCoords;
};


#define ALPHA_THRESHOLD 0.5


[shader("anyhit")]
void main(inout ShPayloadShadow g_payloadShadow, in HitAttributes attribs)
{
	const ShTriangle tr = getTriangle((int)InstanceIndex(), (int)InstanceID(), (int)GeometryIndex(), (int)PrimitiveIndex());

	const float3 baryCoords = float3(1.0f - attribs.inBaryCoords.x - attribs.inBaryCoords.y, attribs.inBaryCoords.x, attribs.inBaryCoords.y);
    const float2 texCoord = getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords));

 	const float4 color = getTextureSampleLod(tr.materials[0][MATERIAL_ALBEDO_ALPHA_INDEX], texCoord, 0.0) * tr.materialColors[0];

	if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_ALPHA_TRANSMISSION) != 0)
	{
		uint h = (uint)InstanceID() * 73856093u ^ (uint)PrimitiveIndex() * 19349663u ^ globalUniform.frameId * 83492791u;
		h ^= h >> 13;
		h *= 1274126177u;
		h ^= h >> 16;

		if (float(h) * (1.0 / 4294967296.0) < 1.0 - color.a)
		{
			IgnoreHit();
		}
	}
	else if ((color.r + color.g + color.b) / 3 * color.a + color.a < ALPHA_THRESHOLD)
	{
		IgnoreHit();
	}

}
