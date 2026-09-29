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




#include "EfSimple.hlsli"

static const float3x3 yiq_mat = float3x3(
      0.2989, 0.5959, 0.2115,
      0.5870, -0.2744, -0.5229,
      0.1140, -0.3216, 0.3114
);

float3 rgb2yiq(float3 col)
{
   return mul(col, yiq_mat);
}

#define CHROMA_MOD_FREQ (4.0 * M_PI / 15.0)

#define SATURATION 1.0
#define BRIGHTNESS 1.0
#define ARTIFACTING 0.0
#define FRINGING 0.0

static const float3x3 mix_mat = float3x3(
	BRIGHTNESS, ARTIFACTING, ARTIFACTING,
	FRINGING, 2.0 * SATURATION, 0.0,
	FRINGING, 0.0, 2.0 * SATURATION
);

float crtDemodulateMod(float x, float y)
{
    return x - y * floor(x / y);
}

float3 demodulateAndEncode(int2 pix)
{
	float3 yiq = rgb2yiq(effect_loadFromSource(pix));

    float chroma_phase = M_PI * (crtDemodulateMod(float(pix.y), 2.0) + globalUniform.frameId);

	float mod_phase = chroma_phase + float(pix.x) * CHROMA_MOD_FREQ;

	float i_mod = cos(mod_phase);
	float q_mod = sin(mod_phase);

	yiq.yz *= float2(i_mod, q_mod);
	yiq = mul(yiq, mix_mat);
	yiq.yz *= float2(i_mod, q_mod);

    return yiq;
}

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.x, dispatchThreadID.y);

    if (!effect_isPixValid(pix))
    {
        return;
    }

    float3 yiq = demodulateAndEncode(pix);
    effect_storeToTarget(encodeYiqForStorage(yiq), pix);
}
