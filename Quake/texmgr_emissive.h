#pragma once

#include <stdint.h>

typedef struct texmgr_emissive_texel_s
{
	float    emissive;
	float    raw;
	uint8_t  blend;
} texmgr_emissive_texel_t;

/*
	The visible emission and the light-shape mask can come from independent
	sources: authored colour blocks and the classic _glow/_luma sidecar. They
	overlap by texel, not by image. The stronger source owns that texel's
	emission and blend mode; on an exact tie the authored block wins.
*/
static inline texmgr_emissive_texel_t TexMgr_ComposeEmissiveTexel(
	int has_color, float color_emissive, float color_raw, uint8_t color_blend,
	int has_fullbright, float fullbright, uint8_t default_blend)
{
	texmgr_emissive_texel_t result = { 0.0f, 0.0f, default_blend };

	if (has_color)
	{
		result.emissive = color_emissive;
		result.raw = color_raw;
		result.blend = color_blend;
	}

	if (has_fullbright && fullbright > result.emissive)
	{
		result.emissive = fullbright;
		result.blend = default_blend;
	}

	if (has_fullbright && fullbright > result.raw)
		result.raw = fullbright;

	return result;
}
