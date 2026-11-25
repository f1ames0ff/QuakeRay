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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vid_texfilter.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition, ...)                          \
	do                                                 \
	{                                                  \
		g_checks++;                                    \
		if (!(condition))                              \
		{                                              \
			g_failures++;                              \
			printf ("FAIL: ");                         \
			printf (__VA_ARGS__);                      \
			printf ("\n");                             \
		}                                              \
	} while (0)

static void Test_DefaultsAndRoundTrips (void)
{
	QrSamplerMipmapMode mip = QR_SAMPLER_MIPMAP_MODE_NEAREST;
	uint32_t aniso = 0;

	CHECK (VID_TexFilter_PresetFromCvars (1, 16) == VID_TEXFILTER_ANISO_16X,
	       "Default cvars (1, 16) must map to Anisotropic 16x");
	CHECK (strcmp (VID_TexFilter_PresetName (VID_TEXFILTER_ANISO_16X), "Anisotropic 16x") == 0,
	       "Default label must be Anisotropic 16x");

	VID_TexFilter_ToSamplerParams (1, 16, &mip, &aniso);
	CHECK (mip == QR_SAMPLER_MIPMAP_MODE_LINEAR, "Default mip mode must be trilinear");
	CHECK (aniso == 16u, "Default anisotropy must be 16x");

	const vid_texfilter_preset_t presets[VID_TEXFILTER_COUNT] = {
		VID_TEXFILTER_BILINEAR,
		VID_TEXFILTER_TRILINEAR,
		VID_TEXFILTER_ANISO_2X,
		VID_TEXFILTER_ANISO_4X,
		VID_TEXFILTER_ANISO_8X,
		VID_TEXFILTER_ANISO_16X,
	};
	const int expected_trilinear[VID_TEXFILTER_COUNT] = { 0, 1, 1, 1, 1, 1 };
	const int expected_aniso[VID_TEXFILTER_COUNT]     = { 0, 0, 2, 4, 8, 16 };
	const QrSamplerMipmapMode expected_mip[VID_TEXFILTER_COUNT] = {
		QR_SAMPLER_MIPMAP_MODE_NEAREST,
		QR_SAMPLER_MIPMAP_MODE_LINEAR,
		QR_SAMPLER_MIPMAP_MODE_LINEAR,
		QR_SAMPLER_MIPMAP_MODE_LINEAR,
		QR_SAMPLER_MIPMAP_MODE_LINEAR,
		QR_SAMPLER_MIPMAP_MODE_LINEAR,
	};
	const uint32_t expected_sampler_aniso[VID_TEXFILTER_COUNT] = { 1u, 1u, 2u, 4u, 8u, 16u };

	for (int i = 0; i < VID_TEXFILTER_COUNT; i++)
	{
		int tri = -1;
		int raw = -1;
		VID_TexFilter_PresetToCvars (presets[i], &tri, &raw);
		CHECK (tri == expected_trilinear[i], "Preset %d trilinear cvar mismatch", i);
		CHECK (raw == expected_aniso[i], "Preset %d anisotropic cvar mismatch", i);
		CHECK (VID_TexFilter_PresetFromCvars (tri, raw) == presets[i], "Preset %d round-trip mismatch", i);

		VID_TexFilter_ToSamplerParams (tri, raw, &mip, &aniso);
		CHECK (mip == expected_mip[i], "Preset %d sampler mipmap mode mismatch", i);
		CHECK (aniso == expected_sampler_aniso[i], "Preset %d sampler anisotropy mismatch", i);
	}
}

static void Test_UpstreamCompatNormalizationAndStep (void)
{
	QrSamplerMipmapMode mip = QR_SAMPLER_MIPMAP_MODE_NEAREST;
	uint32_t aniso = 0;

	/* Upstream vkQuake stores boolean vid_anisotropic=1 for enabled (max / 16x)
	   and 0 for disabled. Ensure 1 enables 16x trilinear filtering smoothly. */
	CHECK (VID_TexFilter_PresetFromCvars (0, 1) == VID_TEXFILTER_ANISO_16X,
	       "vid_anisotropic=1 must map to Anisotropic 16x");
	VID_TexFilter_ToSamplerParams (0, 1, &mip, &aniso);
	CHECK (mip == QR_SAMPLER_MIPMAP_MODE_LINEAR && aniso == 16u,
	       "Anisotropic filtering must force linear mipmap mode even if vid_trilinear=0");

	CHECK (VID_TexFilter_NormalizeAnisotropy (-4) == 0, "Negative anisotropy must clamp to 0");
	CHECK (VID_TexFilter_NormalizeAnisotropy (3) == 2, "3 must normalize down to 2");
	CHECK (VID_TexFilter_NormalizeAnisotropy (6) == 4, "6 must normalize down to 4");
	CHECK (VID_TexFilter_NormalizeAnisotropy (12) == 8, "12 must normalize down to 8");
	CHECK (VID_TexFilter_NormalizeAnisotropy (32) == 16, "32 must normalize down to 16");

	CHECK (VID_TexFilter_StepPreset (VID_TEXFILTER_BILINEAR, -1) == VID_TEXFILTER_ANISO_16X,
	       "Step backwards from Bilinear must wrap to Anisotropic 16x");
	CHECK (VID_TexFilter_StepPreset (VID_TEXFILTER_ANISO_16X, 1) == VID_TEXFILTER_BILINEAR,
	       "Step forwards from Anisotropic 16x must wrap to Bilinear");
	CHECK (strcmp (VID_TexFilter_PresetName (VID_TEXFILTER_BILINEAR), "Bilinear") == 0,
	       "Bilinear label mismatch");
	CHECK (strcmp (VID_TexFilter_PresetName (VID_TEXFILTER_TRILINEAR), "Trilinear") == 0,
	       "Trilinear label mismatch");
}

int main (void)
{
	Test_DefaultsAndRoundTrips ();
	Test_UpstreamCompatNormalizationAndStep ();

	printf ("vid_texfilter_tests: %d check(s), %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
