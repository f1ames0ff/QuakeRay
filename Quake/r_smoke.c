/*
Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "quakedef.h"
#include "gl_heap.h"

extern cvar_t r_smoke;

#define SMOKE_MAXPUFFS  1024

typedef struct smokePuff_s
{
	vec3_t      org;
	vec3_t      vel;

	int         cluster;

	float       birth;
	float       lifetime;

	float       size;
	float       growth;

	float       seed;
	float       alpha;
	float       density;

	byte        color[3];
} smokePuff_t;

static smokePuff_t smoke_puffs[SMOKE_MAXPUFFS];
static int         smoke_count;

static smokePuff_t *smoke_sorted[SMOKE_MAXPUFFS];

cvar_t r_smoke         = {"r_smoke", "1", CVAR_ARCHIVE};
cvar_t r_smoke_max     = {"r_smoke_max", "1024", CVAR_ARCHIVE};
cvar_t r_smoke_life    = {"r_smoke_life", "1.5", CVAR_ARCHIVE};
cvar_t r_smoke_size    = {"r_smoke_size", "16", CVAR_ARCHIVE};
cvar_t r_smoke_growth  = {"r_smoke_growth", "96", CVAR_ARCHIVE};
cvar_t r_smoke_alpha   = {"r_smoke_alpha", "1", CVAR_ARCHIVE};
cvar_t r_smoke_density = {"r_smoke_density", "2", CVAR_ARCHIVE};
cvar_t r_smoke_color   = {"r_smoke_color", "0.4 0.4 0.4", CVAR_ARCHIVE};
cvar_t r_smoke_drift   = {"r_smoke_drift", "128", CVAR_ARCHIVE};
cvar_t r_smoke_rise    = {"r_smoke_rise", "64", CVAR_ARCHIVE};
cvar_t r_smoke_damp    = {"r_smoke_damp", "20", CVAR_ARCHIVE};
cvar_t r_smoke_spacing = {"r_smoke_spacing", "4", CVAR_ARCHIVE};

cvar_t r_smoke_shape      = {"r_smoke_shape", "0", CVAR_ARCHIVE};
cvar_t r_smoke_medium     = {"r_smoke_medium", "0.1", CVAR_ARCHIVE};
cvar_t r_smoke_detail     = {"r_smoke_detail", "0.1", CVAR_ARCHIVE};
cvar_t r_smoke_wind       = {"r_smoke_wind", "8", CVAR_ARCHIVE};
cvar_t r_smoke_displace   = {"r_smoke_displace", "1", CVAR_ARCHIVE};
cvar_t r_smoke_breakup    = {"r_smoke_breakup", "0.1", CVAR_ARCHIVE};
cvar_t r_smoke_edge_power = {"r_smoke_edge_power", "2", CVAR_ARCHIVE};
cvar_t r_smoke_edge_gain  = {"r_smoke_edge_gain", "1", CVAR_ARCHIVE};

static void R_SmokePuff_f (void);

#define SMOKE_TRAIL_SCALE_ROCKET   (1.0f / 6.0f)
#define SMOKE_TRAIL_SCALE_LAVABALL 0.25f
#define SMOKE_TRAIL_SCALE_GRENADE  (0.7f / 8.0f)

static float SmokeRandom (void)
{
	return (rand () & 0x7FFF) / 32767.0f;
}

static float SmokeRandomRange (float lo, float hi)
{
	return lo + (hi - lo) * SmokeRandom ();
}

static void R_SmokeGetColor (float out[3])
{
	if (sscanf (r_smoke_color.string, "%f %f %f", &out[0], &out[1], &out[2]) != 3)
	{
		out[0] = out[1] = out[2] = 0.45f;
	}

	for (int i = 0; i < 3; i++)
		out[i] = CLAMP (0.0f, out[i], 1.0f);
}

static int R_SmokeGetMax (void)
{
	return CLAMP (0, CVAR_TO_INT32 (r_smoke_max), SMOKE_MAXPUFFS);
}

static int R_SmokeOldest (void)
{
	int oldest = 0;

	for (int i = 1; i < smoke_count; i++)
	{
		if (smoke_puffs[i].birth < smoke_puffs[oldest].birth)
			oldest = i;
	}

	return oldest;
}

static void R_SmokeSpawn (const vec3_t org, const vec3_t vel, float sizeScale, float alpha)
{
	float color[3];
	int   index;
	int   max;

	max = R_SmokeGetMax ();
	if (max <= 0)
		return;

	if (smoke_count < max)
		index = smoke_count++;
	else
		index = R_SmokeOldest ();

	R_SmokeGetColor (color);

	smokePuff_t *p = &smoke_puffs[index];

	VectorCopy (org, p->org);
	VectorCopy (vel, p->vel);
	p->cluster  = RT_ResolvePointCluster (org);

	p->birth    = (float)cl.time;
	p->lifetime = q_max (0.05f, r_smoke_life.value * SmokeRandomRange (0.75f, 1.25f));
	p->size     = q_max (0.5f, r_smoke_size.value * sizeScale * SmokeRandomRange (0.7f, 1.3f));
	p->growth   = q_max (0.0f, r_smoke_growth.value * sizeScale);
	p->seed     = SmokeRandom ();
	p->alpha    = CLAMP (0.0f, alpha, 1.0f);
	p->density  = q_max (0.0f, r_smoke_density.value);

	p->color[0] = (byte)CLAMP (0, color[0] * 255.0f, 255);
	p->color[1] = (byte)CLAMP (0, color[1] * 255.0f, 255);
	p->color[2] = (byte)CLAMP (0, color[2] * 255.0f, 255);
}

void R_SmokeInit (void)
{
	Cvar_RegisterVariable (&r_smoke);
	Cvar_RegisterVariable (&r_smoke_max);
	Cvar_RegisterVariable (&r_smoke_life);
	Cvar_RegisterVariable (&r_smoke_size);
	Cvar_RegisterVariable (&r_smoke_growth);
	Cvar_RegisterVariable (&r_smoke_alpha);
	Cvar_RegisterVariable (&r_smoke_density);
	Cvar_RegisterVariable (&r_smoke_color);
	Cvar_RegisterVariable (&r_smoke_drift);
	Cvar_RegisterVariable (&r_smoke_rise);
	Cvar_RegisterVariable (&r_smoke_damp);
	Cvar_RegisterVariable (&r_smoke_spacing);
	Cvar_RegisterVariable (&r_smoke_shape);
	Cvar_RegisterVariable (&r_smoke_medium);
	Cvar_RegisterVariable (&r_smoke_detail);
	Cvar_RegisterVariable (&r_smoke_wind);
	Cvar_RegisterVariable (&r_smoke_displace);
	Cvar_RegisterVariable (&r_smoke_breakup);
	Cvar_RegisterVariable (&r_smoke_edge_power);
	Cvar_RegisterVariable (&r_smoke_edge_gain);

	Cmd_AddCommand ("smoke_puff", R_SmokePuff_f);

	smoke_count = 0;
}

void R_SmokeClear (void)
{
	smoke_count = 0;
}

float R_SmokeTrailScale (const char *modelName, int trailType)
{
	if (trailType == 1)
		return SMOKE_TRAIL_SCALE_GRENADE;

	if (trailType != 0)
		return 0.0f;

	if (modelName != NULL && q_strcasestr (modelName, "lavaball") != NULL)
		return SMOKE_TRAIL_SCALE_LAVABALL;

	return SMOKE_TRAIL_SCALE_ROCKET;
}

void R_SmokeTrail (const vec3_t start, const vec3_t end, float sizeScale)
{
	vec3_t delta, dir, vel, org;
	float  len, spacing, alpha, jitter;
	int    count, i;

	if (!CVAR_TO_BOOL (r_smoke))
		return;

	if (sizeScale <= 0.0f)
		return;

	VectorSubtract (end, start, delta);
	len = VectorLength (delta);
	if (len < 0.01f)
		return;

	VectorScale (delta, 1.0f / len, dir);

	spacing = q_max (r_smoke_size.value * sizeScale * 0.5f, r_smoke_spacing.value * sizeScale);
	spacing = q_max (0.25f, spacing);
	count   = (int)(len / spacing);
	if (count < 1)
		count = 1;
	if (count > 64)
		count = 64;

	jitter = q_max (1.0f, r_smoke_drift.value) * 0.35f;

	for (i = 0; i < count; i++)
	{
		const float t = (i + 0.5f) / count;

		org[0] = start[0] + delta[0] * t + SmokeRandomRange (-2.0f, 2.0f);
		org[1] = start[1] + delta[1] * t + SmokeRandomRange (-2.0f, 2.0f);
		org[2] = start[2] + delta[2] * t + SmokeRandomRange (-2.0f, 2.0f);

		VectorScale (dir, -r_smoke_drift.value, vel);
		vel[2] += r_smoke_rise.value;
		vel[0] += SmokeRandomRange (-jitter, jitter);
		vel[1] += SmokeRandomRange (-jitter, jitter);
		vel[2] += SmokeRandomRange (-jitter, jitter);

		alpha = r_smoke_alpha.value * SmokeRandomRange (0.8f, 1.15f);

		R_SmokeSpawn (org, vel, sizeScale, alpha);
	}
}

void R_SmokeUpdate (void)
{
	float dt;
	int   i;

	if (!CVAR_TO_BOOL (r_smoke))
	{
		smoke_count = 0;
		return;
	}

	dt = (float)q_max (0.0, cl.time - cl.oldtime);

	const float damp = q_max (0.0f, 1.0f - r_smoke_damp.value * dt);

	for (i = 0; i < smoke_count;)
	{
		smokePuff_t *p = &smoke_puffs[i];
		const float age = (float)(cl.time - p->birth);

		if (age >= p->lifetime)
		{
			smoke_puffs[i] = smoke_puffs[--smoke_count];
			continue;
		}

		p->size = q_max (0.5f, p->size + p->growth * dt);

		VectorScale (p->vel, damp, p->vel);
		VectorMA (p->org, dt, p->vel, p->org);

		i++;
	}
}

static int R_SmokeSortCompare (const void *a, const void *b)
{
	const smokePuff_t *pa = *(const smokePuff_t **)a;
	const smokePuff_t *pb = *(const smokePuff_t **)b;

	const float da = (pa->org[0] - r_origin[0]) * (pa->org[0] - r_origin[0]) +
	                 (pa->org[1] - r_origin[1]) * (pa->org[1] - r_origin[1]) +
	                 (pa->org[2] - r_origin[2]) * (pa->org[2] - r_origin[2]);
	const float db = (pb->org[0] - r_origin[0]) * (pb->org[0] - r_origin[0]) +
	                 (pb->org[1] - r_origin[1]) * (pb->org[1] - r_origin[1]) +
	                 (pb->org[2] - r_origin[2]) * (pb->org[2] - r_origin[2]);

	if (da > db)
		return -1;
	if (da < db)
		return 1;
	return 0;
}

static const float smoke_corners[6][2] =
{
	{-1.0f, -1.0f}, { 1.0f, -1.0f}, { 1.0f, 1.0f},
	{-1.0f, -1.0f}, { 1.0f, 1.0f}, {-1.0f, 1.0f},
};

void R_DrawSmoke (cb_context_t *cbx)
{
	int i, v;

	if (!CVAR_TO_BOOL (r_smoke))
		return;

	if (smoke_count <= 0)
		return;

	R_BeginDebugUtilsLabel (cbx, "Smoke");

	for (i = 0; i < smoke_count; i++)
		smoke_sorted[i] = &smoke_puffs[i];

	qsort (smoke_sorted, smoke_count, sizeof (smoke_sorted[0]), R_SmokeSortCompare);

	QrVertex *vertices = RT_AllocScratchMemoryNulled (smoke_count * 6 * sizeof (QrVertex));

	v = 0;
	for (i = 0; i < smoke_count; i++)
	{
		const smokePuff_t *p = smoke_sorted[i];
		const float age01 = CLAMP (0.0f, (float)(cl.time - p->birth) / p->lifetime, 1.0f);
		const uint32_t packed = RT_PackColorToUint32 (p->color[0], p->color[1], p->color[2], 255);

		for (int c = 0; c < 6; c++, v++)
		{
			QrVertex *vert = &vertices[v];

			VectorCopy (p->org, vert->position);
			vert->packedColor = packed;

			vert->texCoord[0] = smoke_corners[c][0];
			vert->texCoord[1] = smoke_corners[c][1];

			vert->normal[0] = age01;
			vert->normal[1] = p->size;
			vert->normal[2] = p->seed;

			vert->texCoordLayer1[0] = p->alpha;
			vert->texCoordLayer1[1] = p->density;

			vert->cluster = (uint32_t)p->cluster;
		}
	}

	QrRasterizedGeometryUploadInfo info = {
		.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
		.vertexCount = smoke_count * 6,
		.pVertices = vertices,
		.indexCount = 0,
		.pIndices = NULL,
		.transform = RT_TRANSFORM_IDENTITY,
		.color = RT_COLOR_WHITE,
		.material = QR_NO_MATERIAL,
		.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE |
		                 QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST |
		                 QR_RASTERIZED_GEOMETRY_STATE_SMOKE,
		.blendFuncSrc = QR_BLEND_FACTOR_ONE,
		.blendFuncDst = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.smokeNoise = {{ r_smoke_shape.value, r_smoke_medium.value,
		                 r_smoke_detail.value, r_smoke_wind.value }},
		.smokeLook  = {{ r_smoke_displace.value, r_smoke_breakup.value,
		                 r_smoke_edge_power.value, r_smoke_edge_gain.value }},
	};

	QrResult r = qrUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
	QR_CHECK (r);

	R_EndDebugUtilsLabel (cbx);
}

static void R_SmokePuff_f (void)
{
	int   count = Cmd_Argc () > 1 ? atoi (Cmd_Argv (1)) : 8;
	vec3_t org;

	count = CLAMP (1, count, 64);

	for (int i = 0; i < count; i++)
	{
		VectorMA (r_origin, 96.0f + i * 24.0f, vpn, org);
		R_SmokeSpawn (org, vec3_origin, 1.5f, r_smoke_alpha.value);
	}

	Con_Printf ("smoke: %d puff(s) live\n", smoke_count);
}
