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

#ifndef VID_TEXFILTER_H
#define VID_TEXFILTER_H

#include <stddef.h>
#include <stdint.h>

#include <qray/qray.h>

typedef enum vid_texfilter_preset_e
{
	VID_TEXFILTER_BILINEAR = 0,
	VID_TEXFILTER_TRILINEAR = 1,
	VID_TEXFILTER_ANISO_2X = 2,
	VID_TEXFILTER_ANISO_4X = 3,
	VID_TEXFILTER_ANISO_8X = 4,
	VID_TEXFILTER_ANISO_16X = 5,
	VID_TEXFILTER_COUNT = 6,
} vid_texfilter_preset_t;

static inline int VID_TexFilter_NormalizeAnisotropy (int raw_aniso)
{
	if (raw_aniso <= 0)
		return 0;
	/* Preserve upstream vkQuake boolean vid_anisotropic=1 meaning enabled (16x). */
	if (raw_aniso == 1 || raw_aniso >= 16)
		return 16;
	if (raw_aniso >= 8)
		return 8;
	if (raw_aniso >= 4)
		return 4;
	return 2;
}

static inline vid_texfilter_preset_t VID_TexFilter_PresetFromCvars (int trilinear, int raw_aniso)
{
	switch (VID_TexFilter_NormalizeAnisotropy (raw_aniso))
	{
	case 16: return VID_TEXFILTER_ANISO_16X;
	case 8:  return VID_TEXFILTER_ANISO_8X;
	case 4:  return VID_TEXFILTER_ANISO_4X;
	case 2:  return VID_TEXFILTER_ANISO_2X;
	default: return trilinear != 0 ? VID_TEXFILTER_TRILINEAR : VID_TEXFILTER_BILINEAR;
	}
}

static inline vid_texfilter_preset_t VID_TexFilter_StepPreset (int current, int dir)
{
	const int clamped = (current >= 0 && current < VID_TEXFILTER_COUNT) ? current : (int)VID_TEXFILTER_ANISO_16X;
	const int step = (dir < 0) ? -1 : 1;
	return (vid_texfilter_preset_t)((clamped + VID_TEXFILTER_COUNT + step) % VID_TEXFILTER_COUNT);
}

static inline void VID_TexFilter_PresetToCvars (vid_texfilter_preset_t preset, int *out_trilinear, int *out_aniso)
{
	static const int kTrilinear[VID_TEXFILTER_COUNT] = { 0, 1, 1, 1, 1, 1 };
	static const int kAniso[VID_TEXFILTER_COUNT]     = { 0, 0, 2, 4, 8, 16 };
	const int idx = ((int)preset >= 0 && (int)preset < VID_TEXFILTER_COUNT)
		? (int)preset
		: (int)VID_TEXFILTER_ANISO_16X;

	if (out_trilinear != NULL)
		*out_trilinear = kTrilinear[idx];
	if (out_aniso != NULL)
		*out_aniso = kAniso[idx];
}

static inline const char *VID_TexFilter_PresetName (vid_texfilter_preset_t preset)
{
	static const char *const kNames[VID_TEXFILTER_COUNT] = {
		"Bilinear",
		"Trilinear",
		"Anisotropic 2x",
		"Anisotropic 4x",
		"Anisotropic 8x",
		"Anisotropic 16x",
	};
	const int idx = ((int)preset >= 0 && (int)preset < VID_TEXFILTER_COUNT)
		? (int)preset
		: (int)VID_TEXFILTER_ANISO_16X;
	return kNames[idx];
}

static inline void VID_TexFilter_ToSamplerParams (
	int trilinear,
	int raw_aniso,
	QrSamplerMipmapMode *out_mipmap_mode,
	uint32_t *out_anisotropy)
{
	const int effective_trilinear = (VID_TexFilter_NormalizeAnisotropy (raw_aniso) > 0) ? 1 : (trilinear != 0);

	if (out_mipmap_mode != NULL)
		*out_mipmap_mode = effective_trilinear ? QR_SAMPLER_MIPMAP_MODE_LINEAR : QR_SAMPLER_MIPMAP_MODE_NEAREST;

	if (out_anisotropy != NULL)
	{
		const int aniso = VID_TexFilter_NormalizeAnisotropy (raw_aniso);
		*out_anisotropy = (aniso > 0) ? (uint32_t)aniso : 1u;
	}
}

#endif
