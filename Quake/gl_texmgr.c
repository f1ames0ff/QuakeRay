/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

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

// gl_texmgr.c -- fitzquake's texture manager. manages texture images

#include "quakedef.h"
#include "gl_heap.h"
#include "rt_material.h"
#include "sys.h"
#include "texmgr_emissive.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL2/SDL.h>
#else
#include "SDL.h"
#endif

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_resize.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

static cvar_t gl_picmip = {"gl_picmip", "0", CVAR_NONE};

extern cvar_t vid_filter;
extern cvar_t vid_anisotropic;

extern cvar_t rt_emis_fullbright_dflt;
extern cvar_t rt_brush_rough;
extern cvar_t rt_brush_metal;
extern cvar_t rt_model_rough;
extern cvar_t rt_model_metal;

#define MAX_MIPS 16

#define RT_COLOR_EMISSIVE_FALLOFF 2.0f

static int          numgltextures;
static gltexture_t *active_gltextures, *free_gltextures;
static TexMgrTextureResolver texmgr_external_resolver;
static unsigned int texmgr_list_revision;
gltexture_t        *notexture, *nulltexture, *whitetexture, *greytexture;

unsigned int d_8to24table[256];
unsigned int d_8to24table_fbright[256];
unsigned int d_8to24table_fbright_fence[256];
#if !RT_RENDERER
unsigned int d_8to24table_nobright[256];
unsigned int d_8to24table_nobright_fence[256];
#endif
unsigned int d_8to24table_conchars[256];
unsigned int d_8to24table_shirt[256];
unsigned int d_8to24table_pants[256];


SDL_mutex *texmgr_mutex;


static qboolean texmgr_live_reload = false;
static qboolean texmgr_live_recreate = false;
qboolean texmgr_live_material_replaced = false;

static QrMaterialCreateFlags TexMgr_GetRtFlags (gltexture_t *glt)
{
	QrMaterialCreateFlags fs = 0;

	if (!(glt->flags & TEXPREF_MIPMAP))
	{
		fs |= QR_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT;
	}

	// if controlled by cvar
	if (!(glt->flags & TEXPREF_NEAREST) && !(glt->flags & TEXPREF_LINEAR))
	{
		fs |= QR_MATERIAL_CREATE_DYNAMIC_SAMPLER_FILTER_BIT;
	}

	if (glt->source_format == SRC_LIGHTMAP)
	{
		fs |= QR_MATERIAL_CREATE_UPDATEABLE_BIT;
	}

	return fs;
}

static QrMaterialCreateFlags TexMgr_GetRtCreateFlags (gltexture_t *glt)
{
	QrMaterialCreateFlags fs = TexMgr_GetRtFlags (glt);

	if (texmgr_live_reload)
	{
		fs |= QR_MATERIAL_CREATE_UPDATEABLE_BIT;
	}

	return fs;
}

static QrSamplerFilter TexMgr_GetFilterMode (gltexture_t *glt)
{
	if (glt->flags & TEXPREF_NEAREST)
	{
		return QR_SAMPLER_FILTER_NEAREST;
	}

	if (glt->flags & TEXPREF_LINEAR)
	{
		return QR_SAMPLER_FILTER_LINEAR;
	}

	return CVAR_TO_INT32 (vid_filter) == 1 ? QR_SAMPLER_FILTER_NEAREST : QR_SAMPLER_FILTER_LINEAR;
}

static SDL_mutex *rtspecial_mutex;

static THREAD_LOCAL qboolean     rtspecial_started;
static THREAD_LOCAL qboolean     rtspecial_foundfullbright = false;
static THREAD_LOCAL gltexture_t *rtspecial_target = NULL;
static THREAD_LOCAL byte         rtspecial_default_rough;
static THREAD_LOCAL byte         rtspecial_default_metallic;

static THREAD_LOCAL QrMaterialCreateInfo rtspecial_info = {0};
static THREAD_LOCAL void                *rtspecial_info_albedoAlpha = NULL; // to point to data from rtspecial_info
static THREAD_LOCAL char                 rtspecial_info_pRelativePath[MAX_QPATH];

static qboolean TexMgr_ApplyMaterialFromMatInternal (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride);
static qboolean TexMgr_ApplyMaterialFromMat (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride);

/* Bumped after every material synthesis writes its fields -- never before them -- so a reader that
   sees the new revision sees a finished texture. What a texture emits (is_light, the mask, its
   glow extents) is read off those fields, and the DTAL piece cache of an alias model is only as
   fresh as the revision it was built and published under (see RT_AddAliasEmissiveLights). */
atomic_uint32_t rt_material_revision;


void TexMgr_RT_SpecialStart (float default_rough, float default_metallic)
{

	assert (!rtspecial_started && !rtspecial_foundfullbright && rtspecial_target == NULL);
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_started = true;
	rtspecial_default_rough = CLAMP( 0, (int)(default_rough * 255), 255);
	rtspecial_default_metallic = CLAMP (0, (int)(default_metallic * 255), 255);
}

static void TexMgr_RT_SpecialSave (gltexture_t *glt, const QrMaterialCreateInfo *info)
{
	assert (rtspecial_info_albedoAlpha == NULL);

	rtspecial_target = glt;
	rtspecial_info = *info;

	{
		size_t sz = sizeof (uint32_t) * glt->width * glt->height;

		rtspecial_info_albedoAlpha = Mem_Alloc (sz);
		memcpy (rtspecial_info_albedoAlpha, info->textures.pDataAlbedoAlpha, sz);
	}

	if (info->pRelativePath)
	{
		q_strlcpy (rtspecial_info_pRelativePath, info->pRelativePath, sizeof (rtspecial_info_pRelativePath));
	}
	else
	{
		rtspecial_info_pRelativePath[0] = '\0';
	}
}

static byte Luminance (byte r, byte g, byte b)
{
	float l = 0.2126f * (float)r / 255.0f + 0.7152f * (float)g / 255.0f + 0.0722f * (float)b / 255.0f;
	int   i = (int)(l * 255);
	    
	return q_min (i, 255);
}

// https://gist.github.com/marukrap/7c361f2c367eaf40537a8715e3fd952a
void RGBtoHSV (const vec3_t rgb, vec3_t out_hsv)
{
	float R = CLAMP (0.0f, rgb[0], 1.0f);
	float G = CLAMP (0.0f, rgb[1], 1.0f);
	float B = CLAMP (0.0f, rgb[2], 1.0f);

	float M = max (R, max (G, B));
	float m = min (R, min (G, B));
	float C = M - m; // Chroma

	float H = 0.f; // Hue
	float S = 0.f; // Saturation
	float V = 0.f; // Value

	if (C != 0.f)
	{
		if (M == R)
			H = fmodf (((G - B) / C), 6.f);
		else if (M == G)
			H = ((B - R) / C) + 2;
		else if (M == B)
			H = ((R - G) / C) + 4;

		H *= 60;
	}

	if (H < 0.f)
		H += 360;

	V = M;

	if (V != 0.f)
		S = C / V;

	out_hsv[0] = CLAMP (0.0f, H, 360.0f);
	out_hsv[1] = CLAMP (0.0f, S, 1.0f);
	out_hsv[2] = CLAMP (0.0f, V, 1.0f);
}

void HSVtoRGB (const vec3_t hsv, vec3_t out_rgb)
{
	float H = CLAMP (0.0f, hsv[0], 360.0f);
	float S = CLAMP (0.0f, hsv[1], 1.0f);
	// Note: don't clamp, as we modify it
	float V = max (0.0f, hsv[2]);

	float C = S * V;                        // Chroma
	float HPrime = fmodf (H / 60, 6.f); // H'
	float X = C * (1 - fabsf (fmodf (HPrime, 2.f) - 1));
	float M = V - C;

	float R = 0.f;
	float G = 0.f;
	float B = 0.f;

	switch ((int)HPrime)
	{
	case 0:
		R = C;
		G = X;
		break; // [0, 1)
	case 1:
		R = X;
		G = C;
		break; // [1, 2)
	case 2:
		G = C;
		B = X;
		break; // [2, 3)
	case 3:
		G = X;
		B = C;
		break; // [3, 4)
	case 4:
		R = X;
		B = C;
		break; // [4, 5)
	case 5:
		R = C;
		B = X;
		break; // [5, 6)
	default:
		break;
	}

	R += M;
	G += M;
	B += M;

	// Note: don't clamp, as we modify luminance
	out_rgb[0] = max (0.0f, R);
	out_rgb[1] = max (0.0f, G);
	out_rgb[2] = max (0.0f, B);
}

static void FullbrightToRME (unsigned width, unsigned height, byte *fullbright)
{
	size_t pixels = (size_t)width * (size_t)height;

	while (pixels-- > 0)
	{
		byte lum = Luminance (fullbright[0], fullbright[1], fullbright[2]);

		if (lum > 0)
		{
			lum = CLAMP (0, CVAR_TO_UINT32 (rt_emis_fullbright_dflt), 255);
		}
		else
		{
			lum = 0;
		}

		// rough
		fullbright[0] = rtspecial_default_rough;
		// metallic
		fullbright[1] = rtspecial_default_metallic;
		// emissive
		fullbright[2] = lum;

		fullbright += 4;
	}
}

static void TexMgr_RT_SpecialFullbright (unsigned width, unsigned height, uint32_t *fullbright)
{
	uint32_t *resized = NULL;

	assert (rtspecial_target != NULL && rtspecial_info_albedoAlpha != NULL);
	assert (rtspecial_info.size.width > 0 && rtspecial_info.size.height > 0);

	if (rtspecial_info.size.width != width || rtspecial_info.size.height != height)
	{
		/* A hand-authored mask may be of another resolution than its base. The
		   base's albedo decides the size, so the mask is resampled to it instead
		   of being dropped (which used to abort a Debug build as well). */
		Con_DWarning ("Resizing fullbright of \"%s\": %ux%u against albedo %ux%u\n",
		              rtspecial_info_pRelativePath, width, height,
		              rtspecial_info.size.width, rtspecial_info.size.height);

		resized = (uint32_t *)Mem_Alloc ((size_t)rtspecial_info.size.width * rtspecial_info.size.height * 4);
		TexMgr_ResizeRgba ((byte *)fullbright, (int)width, (int)height,
		                   (byte *)resized, (int)rtspecial_info.size.width, (int)rtspecial_info.size.height);

		fullbright = resized;
		width = rtspecial_info.size.width;
		height = rtspecial_info.size.height;
	}

	rtspecial_foundfullbright = true;

	FullbrightToRME (width, height, (byte *)fullbright);

	if (rtspecial_target->rtmaterial != QR_NULL_HANDLE)
	{
		if (TexMgr_ApplyMaterialFromMat (rtspecial_target, (unsigned *)rtspecial_info_albedoAlpha, (byte *)fullbright))
		{
			if (resized)
				Mem_Free (resized);
			return;
		}
	}

	{
		const byte *alb = (const byte *)rtspecial_info_albedoAlpha;
		const byte *fb  = (const byte *)fullbright;
		const size_t npix = (size_t)width * height;
		double emR = 0.0, emG = 0.0, emB = 0.0;
		for (size_t i = 0; i < npix; i++)
		{
			const float e = fb[i * 4 + 2] / 255.0f;
			if (e > 0.0f)
			{
				emR += alb[i * 4 + 0] * e;
				emG += alb[i * 4 + 1] * e;
				emB += alb[i * 4 + 2] * e;
			}
		}
		if (emR > 0.0 || emG > 0.0 || emB > 0.0)
		{
			rtspecial_target->rtemissive = true;
			rtspecial_target->rtemissivecolor[0] = emR / (npix * 255.0);
			rtspecial_target->rtemissivecolor[1] = emG / (npix * 255.0);
			rtspecial_target->rtemissivecolor[2] = emB / (npix * 255.0);
		}
	}

	rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
	rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

	rtspecial_info.textures.pDataRoughnessMetallicEmission = fullbright;

    SDL_LockMutex (rtspecial_mutex);
	QrResult r = qrCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
	QR_CHECK (r);
	SDL_UnlockMutex (rtspecial_mutex);

	if (resized)
		Mem_Free (resized);
}

void TexMgr_RT_SpecialEnd ()
{
	assert (rtspecial_started);

	if (rtspecial_target == NULL || rtspecial_info_albedoAlpha == NULL)
	{
		/* nothing was saved: the albedo image could not be read back */
		rtspecial_target = NULL;
		rtspecial_started = false;
		return;
	}

	if (!rtspecial_foundfullbright && rtspecial_target->rtmaterial == QR_NULL_HANDLE)
	{
		rtspecial_info.textures.pDataAlbedoAlpha = rtspecial_info_albedoAlpha;
		rtspecial_info.pRelativePath = rtspecial_info_pRelativePath;

		SDL_LockMutex (rtspecial_mutex);
		QrResult r = qrCreateMaterial (vulkan_globals.instance, &rtspecial_info, &rtspecial_target->rtmaterial);
		QR_CHECK (r);
		SDL_UnlockMutex (rtspecial_mutex);

	}

	Mem_Free (rtspecial_info_albedoAlpha);
	
	rtspecial_started=false;
	rtspecial_target = NULL;
	rtspecial_foundfullbright = false;
	memset (&rtspecial_info, 0, sizeof (rtspecial_info));
	rtspecial_info_albedoAlpha = NULL;
	rtspecial_info_pRelativePath[0] = '\0';

}

/*
================================================================================

    COMMANDS

================================================================================
*/

/*
===============
TexMgr_Imagelist_f -- report loaded textures
===============
*/
static void TexMgr_Imagelist_f (void)
{
	float        mb;
	float        texels = 0;
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		Con_SafePrintf ("   %4i x%4i %s\n", glt->width, glt->height, glt->name);
		if (glt->flags & TEXPREF_MIPMAP)
			texels += glt->width * glt->height * 4.0f / 3.0f;
		else
			texels += (glt->width * glt->height);
	}

	mb = (texels * 4) / 0x100000;
	Con_Printf ("%i textures %i pixels %1.1f megabytes\n", numgltextures, (int)texels, mb);
}

static void TexMgr_RTMatDump_f (void)
{
	const char *filter = Cmd_Argc () > 1 ? Cmd_Argv (1) : NULL;
	gltexture_t *glt;
	int          found = 0;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (filter)
		{
			if (!strstr (glt->name, filter) && !strstr (glt->rtname, filter))
				continue;
		}
		else if (!(glt->rthasmaterial || glt->rtemissive || glt->rthaslightcolor || glt->rtislight || glt->rtemissivetex))
		{
			continue;
		}

		rt_material_t *mat = RT_MAT_Find (glt->name);
		Con_Printf ("RT dump: '%s' (rtname='%s') flags_emissive=%d\n",
		            glt->name, glt->rtname, !!(glt->flags & TEXPREF_RT_IS_EMISSIVE));
		if (mat)
		{
			Con_Printf ("RT dump:   authored: brightness=%.3f is_light=%d light_styles=%d has_light_color=%d light_color=(%.4f,%.4f,%.4f) luma_blend=%d\n",
			            mat->light_brightness, mat->is_light, mat->light_styles, mat->has_light_color,
			            mat->light_color[0], mat->light_color[1], mat->light_color[2], mat->emissive_blend);
			Con_Printf ("RT dump:   authored: luma=%d blocks=%d first=(%.4f,%.4f,%.4f) threshold=%.4f feather=%.2f blend=%s emissive_factor=%.3f\n",
			            mat->has_color_emissive, mat->color_emissive_count,
			            mat->color_emissive[0].color[0], mat->color_emissive[0].color[1], mat->color_emissive[0].color[2],
			            mat->color_emissive[0].threshold, mat->color_emissive[0].feather,
			            RT_MAT_EmissiveBlendName (mat->color_emissive[0].blend), mat->emissive_factor);
		}
		else
		{
			Con_Printf ("RT dump:   authored: <no qray.materials.yaml entry>\n");
		}
		Con_Printf ("RT dump:   applied: is_light=%d lightstyles=%d emissivetex=%d emissive=%d haslightcolor=%d\n",
		            glt->rtislight, glt->rtlightstyles, glt->rtemissivetex, glt->rtemissive, glt->rthaslightcolor);
		Con_Printf ("RT dump:   applied: rtlightcolor=(%.4f,%.4f,%.4f) rtemissivecolor=(%.4f,%.4f,%.4f) rtemissivemean=%.4f\n",
		            glt->rtlightcolor[0], glt->rtlightcolor[1], glt->rtlightcolor[2],
		            glt->rtemissivecolor[0], glt->rtemissivecolor[1], glt->rtemissivecolor[2],
		            glt->rtemissivemean);
		found++;
	}
	Con_Printf ("RT dump: %d texture(s) matched\n", found);
}

/*
================================================================================

    TEXTURE MANAGER

================================================================================
*/

/*
================
TexMgr_FindTexture
================
*/
gltexture_t *TexMgr_FindTexture (qmodel_t *owner, const char *name)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt = NULL;

	if (name)
	{
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			if (glt->owner == owner && !strcmp (glt->name, name))
				goto unlock_mutex;
		}
	}

unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

/*
================
TexMgr_FindTextureByName
================
*/
gltexture_t *TexMgr_FindTextureByName (const char *name)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt = NULL;

	if (name)
	{
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			if (!strcmp (glt->name, name))
				goto unlock_mutex;
		}
		glt = NULL;
	}

unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

/*
================
TexMgr_NewTexture
================
*/
gltexture_t *TexMgr_NewTexture (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	glt = free_gltextures;
	free_gltextures = glt->next;
	glt->next = active_gltextures;
	active_gltextures = glt;

	numgltextures++;
	texmgr_list_revision++;
	SDL_UnlockMutex (texmgr_mutex);
	return glt;
}

static void GL_DeleteTexture (gltexture_t *texture);

/*
================
TexMgr_FreeTexture
================
*/
void TexMgr_FreeTexture (gltexture_t *kill)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	if (kill == NULL)
	{
		Con_Printf ("TexMgr_FreeTexture: NULL texture\n");
		goto unlock_mutex;
	}

	if (active_gltextures == kill)
	{
		active_gltextures = kill->next;
		kill->next = free_gltextures;
		free_gltextures = kill;

		GL_DeleteTexture (kill);
		numgltextures--;
		texmgr_list_revision++;
		goto unlock_mutex;
	}

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->next == kill)
		{
			glt->next = kill->next;
			kill->next = free_gltextures;
			free_gltextures = kill;

			GL_DeleteTexture (kill);
			numgltextures--;
			texmgr_list_revision++;
			goto unlock_mutex;
		}
	}

	Con_Printf ("TexMgr_FreeTexture: not found\n");
unlock_mutex:
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTextures

compares each bit in "flags" to the one in glt->flags only if that bit is active in "mask"
================
*/
void TexMgr_FreeTextures (unsigned int flags, unsigned int mask)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if ((glt->flags & mask) == (flags & mask))
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_FreeTexturesForOwner
================
*/
void TexMgr_FreeTexturesForOwner (qmodel_t *owner)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt, *next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if (glt && glt->owner == owner)
			TexMgr_FreeTexture (glt);
	}
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================
TexMgr_DeleteTextureObjects
================
*/
void TexMgr_DeleteTextureObjects (void)
{
	SDL_LockMutex (texmgr_mutex);
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		GL_DeleteTexture (glt);
	SDL_UnlockMutex (texmgr_mutex);
}

/*
================================================================================

    INIT

================================================================================
*/

/*
=================
TexMgr_LoadPalette -- johnfitz -- was VID_SetPalette, moved here, renamed, rewritten
=================
*/
void TexMgr_LoadPalette (void)
{
	byte *src, *dst;
	int   i;
	FILE *f;

	COM_FOpenFile ("gfx/palette.lmp", &f, NULL);
	if (!f)
		Sys_Error ("Couldn't load gfx/palette.lmp");

	byte pal[768];
	if (fread (pal, 1, 768, f) != 768)
		Sys_Error ("Couldn't load gfx/palette.lmp");
	fclose (f);

	// standard palette, 255 is transparent
	dst = (byte *)d_8to24table;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	((byte *)&d_8to24table[255])[3] = 0;

	// fullbright palette, 0-223 are black (for additive blending)
	src = pal + 224 * 3;
	dst = (byte *)&d_8to24table_fbright[224];
	for (i = 224; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 0; i < 224; i++)
	{
		dst = (byte *)&d_8to24table_fbright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

#if !RT_RENDERER
	// nobright palette, 224-255 are black (for additive blending)
	dst = (byte *)d_8to24table_nobright;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 224; i < 256; i++)
	{
		dst = (byte *)&d_8to24table_nobright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}
#endif

	// fullbright palette, for fence textures
	memcpy (d_8to24table_fbright_fence, d_8to24table_fbright, 256 * 4);
	d_8to24table_fbright_fence[255] = 0; // Alpha of zero.

#if !RT_RENDERER
	// nobright palette, for fence textures
	memcpy (d_8to24table_nobright_fence, d_8to24table_nobright, 256 * 4);
	d_8to24table_nobright_fence[255] = 0; // Alpha of zero.
#endif

	// conchars palette, 0 and 255 are transparent
	memcpy (d_8to24table_conchars, d_8to24table, 256 * 4);
	((byte *)&d_8to24table_conchars[0])[3] = 0;
}

/*
================
TexMgr_NewGame
================
*/
void TexMgr_NewGame (void)
{
	TexMgr_FreeTextures (0, TEXPREF_PERSIST); // deletes all textures where TEXPREF_PERSIST is unset
	TexMgr_LoadPalette ();
}

/*
================
TexMgr_Init

must be called before any texture loading
================
*/
void TexMgr_Init (void)
{
	int               i;
	static byte       notexture_data[16] = {159, 91, 83, 255, 0, 0, 0, 255, 0, 0, 0, 255, 159, 91, 83, 255};                    // black and pink checker
	static byte       nulltexture_data[16] = {127, 191, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 127, 191, 255, 255};              // black and blue checker
	static byte       whitetexture_data[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}; // white
	static byte       greytexture_data[16] = {127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255, 127, 127, 127, 255};  // 50% gray
	extern texture_t *r_notexture_mip, *r_notexture_mip2;

	texmgr_mutex = SDL_CreateMutex ();
	rtspecial_mutex = SDL_CreateMutex ();

	// init texture list
	free_gltextures = (gltexture_t *)Mem_Alloc (MAX_GLTEXTURES * sizeof (gltexture_t));
	active_gltextures = NULL;
	for (i = 0; i < MAX_GLTEXTURES - 1; i++)
		free_gltextures[i].next = &free_gltextures[i + 1];
	free_gltextures[i].next = NULL;
	numgltextures = 0;
	texmgr_list_revision++;

	// palette
	TexMgr_LoadPalette ();

	Cvar_RegisterVariable (&gl_picmip);
	Cmd_AddCommand ("imagelist", &TexMgr_Imagelist_f);
	Cmd_AddCommand ("rt_mat_dump", &TexMgr_RTMatDump_f);

	// load notexture images
	notexture = TexMgr_LoadImage (
		NULL, NULL, "notexture", 2, 2, SRC_RGBA, notexture_data, "", (src_offset_t)notexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	nulltexture = TexMgr_LoadImage (
		NULL, NULL, "nulltexture", 2, 2, SRC_RGBA, nulltexture_data, "", (src_offset_t)nulltexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	whitetexture = TexMgr_LoadImage (
		NULL, NULL, "whitetexture", 2, 2, SRC_RGBA, whitetexture_data, "", (src_offset_t)whitetexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	greytexture = TexMgr_LoadImage (
		NULL, NULL, "greytexture", 2, 2, SRC_RGBA, greytexture_data, "", (src_offset_t)greytexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);

	// have to assign these here becuase Mod_Init is called before TexMgr_Init
	r_notexture_mip->gltexture = r_notexture_mip2->gltexture = notexture;
}

/*
================================================================================

    IMAGE LOADING

================================================================================
*/

/*
================
TexMgr_Downsample
================
*/
static unsigned *TexMgr_Downsample (unsigned *data, int in_width, int in_height, int out_width, int out_height)
{
	const int out_size_bytes = out_width * out_height * 4;

	assert ((out_width >= 1) && (out_width <= in_width));
	assert ((out_height >= 1) && (out_height <= in_height));

	byte *image_resize_buffer;
	TEMP_ALLOC (byte, image_resize_buffer, out_size_bytes);
	stbir_resize_uint8 ((byte *)data, in_width, in_height, 0, image_resize_buffer, out_width, out_height, 0, 4);
	memcpy (data, image_resize_buffer, out_size_bytes);
	TEMP_FREE (image_resize_buffer);

	return data;
}

/*
===============
TexMgr_AlphaEdgeFix

eliminate pink edges on sprites, etc.
operates in place on 32bit data
===============
*/
static void TexMgr_AlphaEdgeFix (byte *data, int width, int height)
{
	int   i, j, n = 0, b, c[3] = {0, 0, 0}, lastrow, thisrow, nextrow, lastpix, thispix, nextpix;
	byte *dest = data;

	for (i = 0; i < height; i++)
	{
		lastrow = width * 4 * ((i == 0) ? height - 1 : i - 1);
		thisrow = width * 4 * i;
		nextrow = width * 4 * ((i == height - 1) ? 0 : i + 1);

		for (j = 0; j < width; j++, dest += 4)
		{
			if (dest[3]) // not transparent
				continue;

			lastpix = 4 * ((j == 0) ? width - 1 : j - 1);
			thispix = 4 * j;
			nextpix = 4 * ((j == width - 1) ? 0 : j + 1);

			b = lastrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + lastpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + thispix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = lastrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = thisrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}
			b = nextrow + nextpix;
			if (data[b + 3])
			{
				c[0] += data[b];
				c[1] += data[b + 1];
				c[2] += data[b + 2];
				n++;
			}

			// average all non-transparent neighbors
			if (n)
			{
				dest[0] = (byte)(c[0] / n);
				dest[1] = (byte)(c[1] / n);
				dest[2] = (byte)(c[2] / n);

				n = c[0] = c[1] = c[2] = 0;
			}
		}
	}
}

/*
================
TexMgr_8to32
================
*/
static void TexMgr_8to32 (byte *in, unsigned *out, int pixels, unsigned int *usepal)
{
	for (int i = 0; i < pixels; i++)
		*out++ = usepal[*in++];
}

/*
================
TexMgr_DeriveNumMips
================
*/
static int TexMgr_DeriveNumMips (int width, int height)
{
	int num_mips = 0;
	while (width >= 1 && height >= 1)
	{
		width /= 2;
		height /= 2;
		num_mips += 1;
	}
	return num_mips;
}

/*
================
TexMgr_PreMultiply32
================
*/
static void TexMgr_PreMultiply32 (byte *in, size_t width, size_t height)
{
	size_t pixels = width * height;
	while (pixels-- > 0)
	{
		in[0] = ((int)in[0] * (int)in[3]) >> 8;
		in[1] = ((int)in[1] * (int)in[3]) >> 8;
		in[2] = ((int)in[2] * (int)in[3]) >> 8;
		in += 4;
	}
}

static void TexMgr_CleanOpaqueSpriteNoise (byte *rgba, size_t pixels)
{
	for (size_t i = 0; i < pixels; i++)
	{
		byte *px = rgba + i * 4u;
		const int m = q_max ((int)px[0], q_max ((int)px[1], (int)px[2]));
		if (m <= 6)
		{
			px[0] = 0;
			px[1] = 0;
			px[2] = 0;
		}
		else if (m < 18)
		{
			const float scale = (float)(m - 6) / 12.0f;
			px[0] = (byte)((float)px[0] * scale + 0.5f);
			px[1] = (byte)((float)px[1] * scale + 0.5f);
			px[2] = (byte)((float)px[2] * scale + 0.5f);
		}
	}
}

static void TexMgr_PrepareOpaqueAlphaSprite (gltexture_t *glt, byte *rgba, int width, int height)
{
	if (glt->source_format != SRC_RGBA || !glt->owner || glt->owner->type != mod_sprite ||
	    width <= 0 || height <= 0 || !rgba)
		return;

	const size_t pixels = (size_t)width * (size_t)height;
	int          min_a = 255;
	size_t       dark_count = 0;
	qboolean     has_bright = false;

	for (size_t i = 0; i < pixels; i++)
	{
		const byte *px = rgba + i * 4u;
		const int   a = px[3];
		const int   m = q_max ((int)px[0], q_max ((int)px[1], (int)px[2]));
		if (a < min_a)
			min_a = a;
		if (m <= 18)
			dark_count++;
		if (m >= 32)
			has_bright = true;
	}

	if (min_a == 255 && has_bright && dark_count * 10u >= pixels)
	{
		glt->rtopaquealpha = true;
		TexMgr_CleanOpaqueSpriteNoise (rgba, pixels);
	}
}

/*
================
TexMgr_LoadImage32 -- handles 32bit source data
================
*/
static void TexMgr_LoadImage32 (gltexture_t *glt, unsigned *data)
{
	GL_DeleteTexture (glt);
	glt->rtopaquealpha = false;
	TexMgr_PrepareOpaqueAlphaSprite (glt, (byte *)data, (int)glt->width, (int)glt->height);

	// do this before any rescaling
	if (glt->flags & TEXPREF_PREMULTIPLY)
		TexMgr_PreMultiply32 ((byte *)data, glt->width, glt->height);

	// mipmap down
	int picmip = (glt->flags & TEXPREF_NOPICMIP) ? 0 : q_max ((int)gl_picmip.value, 0);
	int mipwidth = q_max (glt->width >> picmip, 1);
	int mipheight = q_max (glt->height >> picmip, 1);

	int maxsize = 4096;
	if ((mipwidth > maxsize) || (mipheight > maxsize))
	{
		if (mipwidth >= mipheight)
		{
			mipheight = q_max ((mipheight * maxsize) / mipwidth, 1);
			mipwidth = maxsize;
		}
		else
		{
			mipwidth = q_max ((mipwidth * maxsize) / mipheight, 1);
			mipheight = maxsize;
		}
	}

	if ((int)glt->width != mipwidth || (int)glt->height != mipheight)
	{
		TexMgr_Downsample (data, glt->width, glt->height, mipwidth, mipheight);
		glt->width = mipwidth;
		glt->height = mipheight;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix ((byte *)data, glt->width, glt->height);
	}
	int num_mips = (glt->flags & TEXPREF_MIPMAP) ? TexMgr_DeriveNumMips (glt->width, glt->height) : 1;

	SDL_LockMutex (texmgr_mutex);
	const qboolean warp_image = (glt->flags & TEXPREF_WARPIMAGE);
	if (warp_image)
		num_mips = WARPIMAGEMIPS;

	// Check for sanity. This should never be reached.
	if (num_mips > MAX_MIPS)
		Sys_Error ("Texture has over %d mips", MAX_MIPS);

	// const qboolean lightmap = glt->source_format == SRC_LIGHTMAP;
	// const qboolean surface_indices = glt->source_format == SRC_SURF_INDICES;

	// const VkFormat format = !surface_indices ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_UINT;


	QrMaterialCreateInfo info = {
		.flags = TexMgr_GetRtCreateFlags (glt),
		.size = {glt->width, glt->height},
		.textures =
			{
				.pDataAlbedoAlpha = data,
				.pDataRoughnessMetallicEmission = NULL,
				.pDataNormal = NULL,
			},
		.pRelativePath = glt->rtname,
		.filter = TexMgr_GetFilterMode (glt),
		.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT,
	};

	if (!rtspecial_started)
	{
		if (!texmgr_live_reload || glt->rtmaterial == QR_NULL_HANDLE)
		{
			SDL_LockMutex (rtspecial_mutex);
			QrResult r = qrCreateMaterial (vulkan_globals.instance, &info, &glt->rtmaterial);
			QR_CHECK (r);
			SDL_UnlockMutex (rtspecial_mutex);
		}
	}
	else
	{
		if (glt->flags & TEXPREF_RT_IS_EMISSIVE)
		{
			TexMgr_RT_SpecialFullbright (glt->width, glt->height, data);
		}
		else
		{
		    TexMgr_RT_SpecialSave (glt, &info);
		}
	}

	TexMgr_ApplyMaterialFromMat (glt, data, NULL);

	SDL_UnlockMutex (texmgr_mutex);
}

/* A texel is part of the glow extents above this emission; below it the mask is noise. The
   extents are computed once per texture, at load, so this cannot be a live cvar. */
#define RT_EMIS_GLOW_THRESHOLD 0.02f

#define QRE_DUMPED_MAX 128
static char     texmgr_dumped[QRE_DUMPED_MAX][MAX_QPATH];
static int      texmgr_dumped_count = 0;
static qboolean texmgr_dump_dir_checked = false;
// Set only while the editor re-synthesizes a material: the dump is for a live
// reload, not for the map load (which applies every material anyway).
static qboolean texmgr_dumping_reload = false;

// Verbose reload diagnostics and the TGA dump of the synthesized textures
// (registered by the qr light editor; off by default).
extern cvar_t qr_material_editor_debug;

static qboolean TexMgr_AlreadyDumped (const char *name)
{
	int i;

	for (i = 0; i < texmgr_dumped_count; i++)
	{
		if (!strcmp (texmgr_dumped[i], name))
			return true;
	}
	return false;
}

static void TexMgr_DumpReloadTGA (const char *suffix, const char *name, int w, int h, const byte *rgba)
{
	char  path[MAX_OSPATH];
	char  safe[MAX_QPATH];
	int   i;
	FILE *f;

	for (i = 0; name[i] && i < (int)sizeof (safe) - 1; i++)
	{
		const char c = name[i];
		safe[i] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
	}
	safe[i] = '\0';

	q_snprintf (path, sizeof (path), "%s/qre_dump/%s%s.tga", com_gamedir, safe, suffix);

	if (texmgr_dump_dir_checked == false)
	{
		char dir[MAX_OSPATH];

		q_snprintf (dir, sizeof (dir), "%s/qre_dump", com_gamedir);
		Sys_mkdir (dir);
		texmgr_dump_dir_checked = true;
	}

	f = fopen (path, "wb");
	if (!f)
		return;

	{
		byte header[18] = {0};

		header[2] = 2; // uncompressed true-color
		header[12] = (byte)(w & 0xff);
		header[13] = (byte)((w >> 8) & 0xff);
		header[14] = (byte)(h & 0xff);
		header[15] = (byte)((h >> 8) & 0xff);
		header[16] = 32;
		header[17] = 0x28; // top-left origin, 8 alpha bits
		fwrite (header, 1, sizeof (header), f);
	}

	for (i = 0; i < w * h; i++)
	{
		byte bgra[4] = { rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4 + 0], rgba[i * 4 + 3] };

		fwrite (bgra, 1, sizeof (bgra), f);
	}

	fclose (f);
}

/* Below this area fraction the extents are a proper part of the texture and the light is built
   as polygons over them; at or above it the whole surface glows and stays a single light. */
#define RT_EMIS_GLOW_FULL 0.999f

/*
================
TexMgr_FeatherEmissive

Softens an emission map: a box blur of the given radius over the map, so the
mask's edge fades outward from the threshold's selection while the selected
pixels keep their value, without looking at the colors themselves. Two
separable passes, each a running sum, so the cost does not depend on the radius.
================
*/
static void TexMgr_FeatherEmissive (float *emiss, int w, int h, int radius)
{
	float *tmp;
	int    x, y, i;
	const int window = radius * 2 + 1;

	if (radius <= 0 || w <= 0 || h <= 0)
		return;

	tmp = (float *)Mem_Alloc ((size_t)w * (size_t)h * sizeof (float));

	// horizontal
	for (y = 0; y < h; y++)
	{
		const float *row = emiss + (size_t)y * w;
		float       *out = tmp + (size_t)y * w;
		float        sum = 0.0f;

		for (i = -radius; i <= radius; i++)
		{
			int sx = i < 0 ? 0 : (i >= w ? w - 1 : i);

			sum += row[sx];
		}
		for (x = 0; x < w; x++)
		{
			out[x] = sum / (float)window;
			{
				int add = x + radius + 1;
				int sub = x - radius;

				sum += row[add >= w ? w - 1 : add];
				sum -= row[sub < 0 ? 0 : sub];
			}
		}
	}

	// vertical
	for (x = 0; x < w; x++)
	{
		float sum = 0.0f;

		for (i = -radius; i <= radius; i++)
		{
			int sy = i < 0 ? 0 : (i >= h ? h - 1 : i);

			sum += tmp[(size_t)sy * w + x];
		}
		for (y = 0; y < h; y++)
		{
			const float blurred = sum / (float)window;
			size_t      index = (size_t)y * w + x;

			if (blurred > emiss[index])
				emiss[index] = blurred;
			{
				int add = y + radius + 1;
				int sub = y - radius;

				sum += tmp[(size_t)(add >= h ? h - 1 : add) * w + x];
				sum -= tmp[(size_t)(sub < 0 ? 0 : sub) * w + x];
			}
		}
	}

	Mem_Free (tmp);
}

static void TexMgr_EmissiveCone (const rt_material_t *mat, float *angleInner, float *angleOuter)
{
	float outerDeg = mat->emissive_focus;

	if (!(outerDeg > 0.0f) || outerDeg > 90.0f)
	{
		if (!mat->emissive_projector)
		{
			*angleInner = 0.0f;
			*angleOuter = 0.0f;
			return;
		}

		outerDeg = 45.0f;
	}

	float softDeg = mat->emissive_focus_soft;

	if (softDeg < 0.0f)
		softDeg = mat->emissive_projector ? 45.0f : outerDeg * 0.1f;
	if (softDeg < 0.05f)
		softDeg = 0.05f;
	if (softDeg > outerDeg)
		softDeg = outerDeg;

	*angleOuter = (float)(outerDeg * M_PI_DIV_180);
	*angleInner = (float)((outerDeg - softDeg) * M_PI_DIV_180);
}

static void TexMgr_SetAuxMaterial (gltexture_t *glt, QrMaterial *target, const byte *pixels, int width, int height)
{
	const QrMaterial old = *target;
	const QrExtent2D size = {(uint32_t)q_max (1, width), (uint32_t)q_max (1, height)};
	SDL_LockMutex (rtspecial_mutex);
	if (!pixels)
	{
		if (old != QR_NO_MATERIAL)
		{
			QR_CHECK (qrDestroyMaterial (vulkan_globals.instance, old));
			*target = QR_NO_MATERIAL;
		}
	}
	else if (texmgr_live_reload && !texmgr_live_recreate && old != QR_NO_MATERIAL &&
	         qrCanUpdateMaterialContents (vulkan_globals.instance, old, size) == QR_SUCCESS)
	{
		QrMaterialUpdateInfo update = {
			.target = old,
			.textures = {.pDataAlbedoAlpha = pixels},
		};
		QR_CHECK (qrUpdateMaterialContents (vulkan_globals.instance, &update));
	}
	else
	{
		QrMaterialCreateInfo info = {
			.flags = TexMgr_GetRtCreateFlags (glt),
			.size = size,
			.textures = {.pDataAlbedoAlpha = pixels},
			.pRelativePath = NULL,
			.filter = TexMgr_GetFilterMode (glt),
			.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT,
		};
		QrMaterial material = QR_NO_MATERIAL;
		QR_CHECK (qrCreateMaterial (vulkan_globals.instance, &info, &material));
		if (old != QR_NO_MATERIAL)
			QR_CHECK (qrDestroyMaterial (vulkan_globals.instance, old));
		*target = material;
	}
	SDL_UnlockMutex (rtspecial_mutex);
	if (texmgr_live_reload && old != *target)
		texmgr_live_material_replaced = true;
}

static qboolean TexMgr_ApplyLightMaterial (gltexture_t *glt, const QrMaterialCreateInfo *info, const byte *shapeRme, qboolean splitShape)
{
	qboolean replaced = false;

	SDL_LockMutex (rtspecial_mutex);

	if (splitShape && glt->rtlightmaterial != QR_NULL_HANDLE && texmgr_live_reload && !texmgr_live_recreate)
	{
		const QrExtent2D updateSize = {info->size.width, info->size.height};

		if (qrCanUpdateMaterialContents (vulkan_globals.instance, glt->rtlightmaterial, updateSize) == QR_SUCCESS)
		{
			QrMaterialUpdateInfo update = {
				.target = glt->rtlightmaterial,
				.textures =
					{
						.pDataAlbedoAlpha = info->textures.pDataAlbedoAlpha,
						.pDataRoughnessMetallicEmission = shapeRme,
						.pDataNormal = info->textures.pDataNormal,
					},
			};

			QrResult ur = qrUpdateMaterialContents (vulkan_globals.instance, &update);
			SDL_UnlockMutex (rtspecial_mutex);

			if (ur == QR_SUCCESS)
				return true;

			SDL_LockMutex (rtspecial_mutex);
		}
	}

	if (glt->rtlightmaterial != QR_NULL_HANDLE)
	{
		QrResult dr = qrDestroyMaterial (vulkan_globals.instance, glt->rtlightmaterial);
		QR_CHECK (dr);
		glt->rtlightmaterial = QR_NULL_HANDLE;
		replaced = true;
	}

	if (splitShape)
	{
		QrMaterialCreateInfo shapeInfo = *info;
		QrMaterial            shapeMaterial = QR_NULL_HANDLE;

		shapeInfo.textures.pDataRoughnessMetallicEmission = shapeRme;

		QrResult cr = qrCreateMaterial (vulkan_globals.instance, &shapeInfo, &shapeMaterial);
		QR_CHECK (cr);

		glt->rtlightmaterial = shapeMaterial;
		replaced = true;
	}

	SDL_UnlockMutex (rtspecial_mutex);

	return replaced;
}

static byte *TexMgr_LoadMaterialTexture (const rt_material_t *mat, int which, int *width, int *height)
{
	const char   *path = NULL;
	gltexture_t  *source;
	byte         *pixels;

	pixels = RT_MAT_LoadTexture (mat, which, width, height);
	if (pixels)
		return pixels;

	switch (which)
	{
	case RT_MAT_TEX_BASE:     path = mat->filename_base; break;
	case RT_MAT_TEX_NORMALS:  path = mat->filename_normals; break;
	case RT_MAT_TEX_EMISSIVE: path = mat->filename_emissive; break;
	case RT_MAT_TEX_OVERLAY:  path = mat->filename_overlay; break;
	case RT_MAT_TEX_GLOSS:    path = mat->filename_gloss; break;
	case RT_MAT_TEX_SSS:      path = mat->filename_sss; break;
	case RT_MAT_TEX_HEIGHT:   path = mat->filename_height; break;
	default:                  break;
	}

	if (!path || !path[0])
		return NULL;

	source = TexMgr_FindTextureByNormalizedName (path);
	if (source)
		return TexMgr_LoadRgbaForPreview (source, width, height);
	if (texmgr_external_resolver)
		return texmgr_external_resolver (path, width, height);

	return NULL;
}

static void TexMgr_ResetLightShafts (gltexture_t *glt)
{
	glt->rtlightshafts = false;
	glt->rtlightshaftspower = 1.0f;
	glt->rtlightshaftsdistance = 0.0f;
	glt->rtlightshaftsfalloff = 1.0f;
}

static void TexMgr_ApplyLightShafts (gltexture_t *glt, const rt_material_t *mat)
{
	glt->rtlightshafts = mat->light_shafts;
	glt->rtlightshaftspower = mat->light_shafts_power;
	glt->rtlightshaftsdistance = mat->light_shafts_distance;
	glt->rtlightshaftsfalloff = mat->light_shafts_falloff;
}

static void TexMgr_ResetSubsurface (gltexture_t *glt)
{
	glt->rtsss = false;
	glt->rtssscolor[0] = 1.0f;
	glt->rtssscolor[1] = 0.35f;
	glt->rtssscolor[2] = 0.2f;
	glt->rtsssradius = 0.02f;
	glt->rtsssstrength = 0.0f;
	glt->rtparallaxscale = 0.0f;
}

static void TexMgr_ApplySubsurface (gltexture_t *glt, const rt_material_t *mat)
{
	const qboolean allowed = !mat->material_glass && !mat->material_water && !mat->mirror;

	VectorCopy (mat->sss_color, glt->rtssscolor);
	glt->rtsssradius = mat->sss_radius;
	glt->rtsssstrength = mat->sss_strength;
	glt->rtsss = mat->sss_enabled && allowed && mat->sss_strength > 0.0f && mat->sss_radius > 0.0f;
}

static void TexMgr_FillParallaxUpload (const gltexture_t *tx, QrGeometryUploadInfo *info)
{
	if (!tx || !info || tx->rtsss || tx->rtglass || tx->rtwater || !(tx->rtparallaxscale > 0.0f))
		return;
	if (info->visibilityType == QR_GEOMETRY_VISIBILITY_TYPE_SKY)
		return;
	if (info->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE &&
	    info->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED)
		return;
	info->layerColors[2].data[2] = CLAMP (0.0f, tx->rtparallaxscale, 0.25f);
}

void TexMgr_FillSubsurfaceUpload (const gltexture_t *tx, QrGeometryUploadInfo *info)
{
	TexMgr_FillParallaxUpload (tx, info);
	if (!tx || !tx->rtsss || !info)
		return;
	if (info->visibilityType == QR_GEOMETRY_VISIBILITY_TYPE_SKY)
		return;
	if (info->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE &&
	    info->passThroughType != QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED)
		return;
	info->flags |= QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT;
	info->subsurfaceColor.data[0] = tx->rtssscolor[0];
	info->subsurfaceColor.data[1] = tx->rtssscolor[1];
	info->subsurfaceColor.data[2] = tx->rtssscolor[2];
	info->subsurfaceRadius = tx->rtsssradius;
	info->subsurfaceStrength = tx->rtsssstrength;
}

typedef struct
{
	const byte *src;
	int         tw;
	int         th;
} texmgr_mask_src_t;

typedef struct
{
	const float *color;
	float        threshold;
	float        feather;
	float        factor;
	int          poly_count;
	const float (*poly_uv)[2];
} texmgr_mask_spec_t;

static qboolean TexMgr_PointInPolyUv (float u, float v, int poly_count, const float (*poly_uv)[2])
{
	qboolean inside = false;
	int      p, q;

	for (p = 0, q = poly_count - 1; p < poly_count; q = p++)
	{
		const float pu = poly_uv[p][0], pv = poly_uv[p][1];
		const float qu = poly_uv[q][0], qv = poly_uv[q][1];

		if (((pv > v) != (qv > v)) && (u < (qu - pu) * (v - pv) / (qv - pv) + pu))
			inside = !inside;
	}
	return inside;
}

static void TexMgr_EvalBlockMask (const texmgr_mask_src_t *srcInfo, const texmgr_mask_spec_t *spec,
                                  float *mask, float *rawmask, float *dist)
{
	const int   npix = srcInfo->tw * srcInfo->th;
	const float thr = spec->threshold;
	const int   feather = (spec->feather >= 1.0f) ? (int)(spec->feather + 0.5f) : 0;

	for (int i = 0; i < npix; i++)
	{
		const byte *src = srcInfo->src + i * 4;
		const float dr = src[0] / 255.0f - spec->color[0];
		const float dg = src[1] / 255.0f - spec->color[1];
		const float db = src[2] / 255.0f - spec->color[2];
		const float d2 = dr * dr + dg * dg + db * db;

		mask[i] = 0.0f;
		if (rawmask) rawmask[i] = 0.0f;
		if (dist) dist[i] = d2;

		if (spec->poly_count >= 3)
		{
			const int   px = i % srcInfo->tw;
			const int   py = i / srcInfo->tw;
			const float u = ((float)px + 0.5f) / (float)srcInfo->tw;
			const float v = ((float)py + 0.5f) / (float)srcInfo->th;

			if (TexMgr_PointInPolyUv (u, v, spec->poly_count, spec->poly_uv))
			{
				mask[i] = spec->factor;
				if (rawmask) rawmask[i] = 1.0f;
				if (dist) dist[i] = 0.0f;
			}
		}
		else if (d2 <= 3.0f * thr * thr)
		{
			const float dnorm = (thr > 0.0f) ? sqrtf (d2 / 3.0f) / thr : 0.0f;
			const float k = RT_COLOR_EMISSIVE_FALLOFF;
			const float tail = expf (-k);
			const float v = (expf (-k * dnorm) - tail) / (1.0f - tail);

			mask[i] = v * spec->factor;
			if (rawmask) rawmask[i] = v;
		}
	}

	if (feather > 0)
		TexMgr_FeatherEmissive (mask, srcInfo->tw, srcInfo->th, feather);
}

typedef struct
{
	float *colorEmis;
	byte  *colorBlend;
	float *colorDist;
	float *colorRaw;
} texmgr_emis_bufs_t;

static void TexMgr_BuildColorEmissiveBuffers (const rt_material_t *mat, const byte *baseSrc,
                                              int tw, int th, int emisBlendCode,
                                              texmgr_emis_bufs_t *out)
{
	const int               npix = tw * th;
	const texmgr_mask_src_t maskSrc = {baseSrc, tw, th};
	float                  *mask = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	float                  *rawmask = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	float                  *dist = (float *)Mem_Alloc ((size_t)npix * sizeof (float));

	out->colorEmis  = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	out->colorBlend = (byte *)Mem_Alloc ((size_t)npix);
	out->colorDist  = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	out->colorRaw   = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	for (int i = 0; i < npix; i++)
	{
		out->colorEmis[i] = 0.0f;
		out->colorDist[i] = 0.0f;
		out->colorRaw[i] = 0.0f;
		out->colorBlend[i] = (byte)emisBlendCode;
	}

	for (int c = 0; c < mat->color_emissive_count; c++)
	{
		const rt_emissive_t     *block = &mat->color_emissive[c];
		const byte               blockCode = (byte)((block->blend >= 0) ? (block->blend + 1) : 0);
		const texmgr_mask_spec_t spec = {block->color, block->threshold, block->feather,
		                                 block->factor, block->poly_count, block->poly_uv};

		TexMgr_EvalBlockMask (&maskSrc, &spec, mask, rawmask, dist);

		for (int i = 0; i < npix; i++)
		{
			if (!(mask[i] > 0.0f))
				continue;
			if (out->colorEmis[i] > 0.0f && !(dist[i] < out->colorDist[i]))
				continue;

			out->colorEmis[i] = mask[i];
			out->colorRaw[i] = rawmask[i];
			out->colorBlend[i] = blockCode;
			out->colorDist[i] = dist[i];
		}
	}

	Mem_Free (mask);
	Mem_Free (rawmask);
	Mem_Free (dist);
}

typedef struct texmgr_raw_slot_s
{
	byte *rgba;
	int   w;
	int   h;
} texmgr_raw_slot_t;

typedef struct texmgr_raw_slots_s
{
	texmgr_raw_slot_t base;
	texmgr_raw_slot_t norm;
	texmgr_raw_slot_t height;
	texmgr_raw_slot_t emis;
	texmgr_raw_slot_t gloss;
	texmgr_raw_slot_t overlay;
	texmgr_raw_slot_t sss;
} texmgr_raw_slots_t;

static void TexMgr_LoadRawSlot (const rt_material_t *mat, int slot, qboolean enabled,
                                texmgr_raw_slot_t *out)
{
	out->rgba = NULL;
	out->w = 0;
	out->h = 0;
	if (!enabled)
		return;
	out->rgba = TexMgr_LoadMaterialTexture (mat, slot, &out->w, &out->h);
}

static void TexMgr_FreeRawSlots (texmgr_raw_slots_t *slots)
{
	texmgr_raw_slot_t *all[] = {&slots->base, &slots->norm, &slots->height,
	                            &slots->emis, &slots->gloss, &slots->overlay, &slots->sss};
	for (size_t i = 0; i < countof (all); i++)
	{
		if (all[i]->rgba)
		{
			Mem_Free (all[i]->rgba);
			all[i]->rgba = NULL;
		}
	}
}

static void TexMgr_LoadAllRawSlots (const gltexture_t *glt, const rt_material_t *mat,
                                    qboolean allowHeight, texmgr_raw_slots_t *raw)
{
	const texmgr_raw_slots_t empty = {0};
	*raw = empty;
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_BASE, true, &raw->base);
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_NORMALS, true, &raw->norm);
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_HEIGHT, allowHeight, &raw->height);
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_EMISSIVE, mat->emissive_enabled, &raw->emis);
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_GLOSS, mat->gloss_enabled, &raw->gloss);
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_OVERLAY, true, &raw->overlay);
	const qboolean loadSss = glt->rtsss && mat->filename_sss[0] != '\0';
	TexMgr_LoadRawSlot (mat, RT_MAT_TEX_SSS, loadSss, &raw->sss);
	if (loadSss && !raw->sss.rgba)
		Con_Printf ("RT: material '%s': texture_sss '%s' could not be loaded\n",
		            mat->name, mat->filename_sss);
}

static void TexMgr_ComputeBakeDims (const texmgr_raw_slots_t *slots, int baseW, int baseH,
                                    qboolean activeFullbright, int *outW, int *outH)
{
	int w = slots->base.rgba ? slots->base.w : baseW;
	int h = slots->base.rgba ? slots->base.h : baseH;
	const texmgr_raw_slot_t *extra[] = {&slots->norm, &slots->height, &slots->gloss,
	                                    &slots->emis, &slots->overlay, &slots->sss};
	for (size_t i = 0; i < countof (extra); i++)
	{
		if (extra[i]->rgba && extra[i]->w > 0 && extra[i]->h > 0)
		{
			w = q_max (w, extra[i]->w);
			h = q_max (h, extra[i]->h);
		}
	}
	if ((slots->emis.rgba != NULL || activeFullbright) && w > 0 && h > 0)
	{
		const int minDim = q_min (w, h);
		if (minDim < 256)
		{
			const double upScale = 256.0 / (double)minDim;
			w = (int)((double)w * upScale + 0.5);
			h = (int)((double)h * upScale + 0.5);
		}
	}
	const int maxDim = q_max (w, h);
	if (maxDim > 2048)
	{
		const double scale = 2048.0 / (double)maxDim;
		w = q_max (1, (int)((double)w * scale + 0.5));
		h = q_max (1, (int)((double)h * scale + 0.5));
	}
	*outW = q_max (1, w);
	*outH = q_max (1, h);
}

static byte *TexMgr_FinishSlotBuf (texmgr_raw_slot_t *slot, int tw, int th)
{
	if (!slot->rgba)
		return NULL;
	if (slot->w == tw && slot->h == th)
	{
		byte *owned = slot->rgba;
		slot->rgba = NULL;
		return owned;
	}
	byte *resized = (byte *)Mem_Alloc ((size_t)tw * (size_t)th * 4u);
	TexMgr_ResizeRgba (slot->rgba, slot->w, slot->h, resized, tw, th);
	Mem_Free (slot->rgba);
	slot->rgba = NULL;
	return resized;
}

static byte *TexMgr_ProcessOverlaySlot (texmgr_raw_slot_t *slot, const rt_material_t *mat,
                                        int tw, int th)
{
	if (!slot->rgba)
		return NULL;
	const int ln = slot->w * slot->h;
	unsigned  covMax = 0;
	unsigned long long covSum = 0;
	for (int i = 0; i < ln; i++)
	{
		const unsigned a = slot->rgba[i * 4 + 3];
		slot->rgba[i * 4 + 0] = (byte)((slot->rgba[i * 4 + 0] * a + 127) / 255);
		slot->rgba[i * 4 + 1] = (byte)((slot->rgba[i * 4 + 1] * a + 127) / 255);
		slot->rgba[i * 4 + 2] = (byte)((slot->rgba[i * 4 + 2] * a + 127) / 255);
		covSum += a;
		if (a > covMax)
			covMax = a;
	}
	if (covMax == 0)
		Con_Printf ("RT: material '%s': texture_overlay '%s' is fully transparent; the overlay paints nothing\n",
		            mat->name, mat->filename_overlay);
	else if (ln > 0 && covSum / (unsigned long long)ln < 13)
		Con_Printf ("RT: material '%s': texture_overlay '%s' covers only %.1f%% on average; the overlay will barely show\n",
		            mat->name, mat->filename_overlay,
		            ((double)covSum / (double)ln) * 100.0 / 255.0);
	return TexMgr_FinishSlotBuf (slot, tw, th);
}

static byte *TexMgr_ResizeExternalRgba (const byte *src, int sw, int sh, int tw, int th)
{
	if (!src || (sw == tw && sh == th))
		return NULL;
	byte *out = (byte *)Mem_Alloc ((size_t)tw * (size_t)th * 4u);
	TexMgr_ResizeRgba (src, sw, sh, out, tw, th);
	return out;
}

static void TexMgr_FillSssLumaFromBuf (const byte *sssBuf, int npix, float *combined)
{
	for (int i = 0; i < npix; i++)
	{
		combined[i] = (0.2126f * sssBuf[i * 4 + 0] +
		               0.7152f * sssBuf[i * 4 + 1] +
		               0.0722f * sssBuf[i * 4 + 2]) / 255.0f;
	}
}

static byte *TexMgr_BuildSssMaskBytes (const rt_material_t *mat, const byte *baseSrc,
                                       const byte *sssBuf, int tw, int th, qboolean rtsss)
{
	const qboolean hasTex = sssBuf != NULL;
	const qboolean hasBlocks = mat->has_sss_blocks && mat->sss_block_count > 0;
	const int      npix = tw * th;
	float         *combined;
	byte          *out;

	if (!rtsss || (!hasTex && !hasBlocks))
		return NULL;

	combined = (float *)Mem_Alloc ((size_t)npix * sizeof (float));
	for (int i = 0; i < npix; i++)
		combined[i] = 0.0f;

	if (hasTex)
		TexMgr_FillSssLumaFromBuf (sssBuf, npix, combined);

	if (hasBlocks)
	{
		const texmgr_mask_src_t maskSrc = {baseSrc, tw, th};
		float                  *mask = (float *)Mem_Alloc ((size_t)npix * sizeof (float));

		for (int c = 0; c < mat->sss_block_count; c++)
		{
			const rt_sss_block_t    *block = &mat->sss_blocks[c];
			const texmgr_mask_spec_t spec = {block->color, block->threshold, block->feather,
			                                 block->factor, block->poly_count, block->poly_uv};

			TexMgr_EvalBlockMask (&maskSrc, &spec, mask, NULL, NULL);
			for (int i = 0; i < npix; i++)
			{
				if (mask[i] > combined[i])
					combined[i] = mask[i];
			}
		}
		Mem_Free (mask);
	}

	out = (byte *)Mem_Alloc ((size_t)npix);
	for (int i = 0; i < npix; i++)
	{
		const int v = (int)(combined[i] * 255.0f + 0.5f);
		out[i] = (byte)CLAMP (0, v, 255);
	}
	Mem_Free (combined);
	return out;
}

static float TexMgr_ResolvePixelRoughness (const gltexture_t *glt, const rt_material_t *mat,
                                           const byte *glossBuf, const byte *roughBaseBuf,
                                           float defaultRough, int i)
{
	if (glt->rtmirror && !glt->rtglass && !glt->rtwater)
		return 0.0f;
	if (!mat->gloss_enabled)
		return defaultRough;
	if (mat->has_roughness_override)
		return mat->roughness_override;
	if (glossBuf)
		return 1.0f - glossBuf[i * 4] / 255.0f;
	if (roughBaseBuf)
		return roughBaseBuf[i * 4 + 3] / 255.0f;
	return defaultRough;
}

static float TexMgr_ResolvePixelMetalness (const rt_material_t *mat, const byte *normBuf, int i)
{
	if (!mat->gloss_enabled || (!mat->has_metalness_factor && !mat->metalness_from_normal_alpha))
		return 0.0f;
	{
		const float factor = mat->has_metalness_factor ? mat->metalness_factor : 1.0f;
		if (mat->metalness_from_normal_alpha && normBuf)
			return (normBuf[i * 4 + 3] / 255.0f) * factor;
		return factor;
	}
}

static byte TexMgr_ResolveNormalAlpha (const byte *sssMask, qboolean rtsss, qboolean isGlass,
                                       byte glassAlpha, const byte *heightBuf, int i)
{
	if (sssMask)
		return sssMask[i];
	if (rtsss)
		return 255;
	if (isGlass)
		return glassAlpha;
	if (heightBuf)
	{
		const float lum = 0.299f * heightBuf[i * 4 + 0] +
		                  0.587f * heightBuf[i * 4 + 1] +
		                  0.114f * heightBuf[i * 4 + 2];
		return (byte)CLAMP (0, (int)(lum + 0.5f), 255);
	}
	return 255;
}

static qboolean TexMgr_ApplyMaterialFromMatInternal (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride)
{
	rt_material_t *mat = RT_MAT_Find (glt->name);
	if (!mat)
	{
		TexMgr_SetAuxMaterial (glt, &glt->rtoverlaymaterial, NULL, (int)glt->width, (int)glt->height);
		TexMgr_SetAuxMaterial (glt, &glt->rtrastermaterial, NULL, (int)glt->width, (int)glt->height);
		// A material the editor dropped (Cancel/Exit of a texture that had none
		// in yaml) must stop being one at once: the reload is what notices it is
		// gone, and every field below is only ever set here. The same block a
		// texture gets when it is loaded with no material.
		glt->rtlightcolor[0] = glt->rtlightcolor[1] = glt->rtlightcolor[2] = 0.0f;
		glt->rthaslightcolor = false;
		glt->rtmirror = false;
		glt->rtexactnormals = false;
		glt->rtforcerasterize = false;
		glt->rtalphatest = false;
	glt->rtopaquealpha = false;
		glt->rtglass = false;
		glt->rtglassior = 0.0f;
		glt->rtglassthickness = 2.0f;
		glt->rtglasscolor[0] = 1.0f;
		glt->rtglasscolor[1] = 1.0f;
		glt->rtglasscolor[2] = 1.0f;
		glt->rtwater = false;
		TexMgr_ResetLightShafts (glt);
		TexMgr_ResetSubsurface (glt);
		glt->rtuvscale[0] = glt->rtuvscale[1] = 1.0f;
		glt->rtuvoffset[0] = glt->rtuvoffset[1] = 0.0f;
		glt->rtemissive = false;
		glt->rtemissivecolor[0] = glt->rtemissivecolor[1] = glt->rtemissivecolor[2] = 0.0f;
		glt->rtemissivemean = 0.0f;
		glt->rtemissivemeanbase = 0.0f;
		glt->rtemisuvmin[0] = glt->rtemisuvmin[1] = 0.0f;
		glt->rtemisuvmax[0] = glt->rtemisuvmax[1] = 1.0f;
		glt->rtemissiveglow = 0.0f;
		glt->rtemisglowfrac = 1.0f;
		glt->rtemissiveglowtex = false;
		glt->rtemissivetex = false;
		glt->rtlightgain = 1.0f;
		glt->rtislight = false;
		glt->rtlightstyles = true;
		glt->rthasmaterial = false;
		return false;
	}

	glt->rthasmaterial = true;

	glt->rtislight = mat->emissive_enabled && mat->is_light;
	glt->rtlightstyles = mat->light_styles;

	if (mat->has_light_color)
	{
		VectorCopy (mat->light_color, glt->rtlightcolor);
		glt->rthaslightcolor = true;
	}
	else
	{
		// a light_color removed in the editor stops lighting now, not at the
		// next map load
		glt->rthaslightcolor = false;
		glt->rtlightcolor[0] = glt->rtlightcolor[1] = glt->rtlightcolor[2] = 0.0f;
	}
	TexMgr_EmissiveCone (mat, &glt->rtemisangleinner, &glt->rtemisangleouter);
	glt->rtemisprojector = mat->emissive_projector;
	TexMgr_ApplyLightShafts (glt, mat);
	TexMgr_ApplySubsurface (glt, mat);
	glt->rtmirror = mat->mirror;
	glt->rtexactnormals = mat->exact_normals;
	glt->rtforcerasterize = mat->force_rasterize;
	glt->rtalphatest = mat->alpha_test;
	glt->rtglass = mat->material_glass;
	glt->rtglassior = mat->glass_ior;
	glt->rtglassthickness = mat->glass_thickness;
	VectorCopy (mat->glass_color, glt->rtglasscolor);
	glt->rtwater = mat->material_water && !mat->material_glass;
	glt->rtuvscale[0] = mat->texture_scale_u > 0.0f ? mat->texture_scale_u : 1.0f;
	glt->rtuvscale[1] = mat->texture_scale_v > 0.0f ? mat->texture_scale_v : 1.0f;
	glt->rtuvoffset[0] = mat->texture_offset_u;
	glt->rtuvoffset[1] = mat->texture_offset_v;

	const qboolean allowHeight = !mat->material_glass && !glt->rtwater && !glt->rtsss && mat->filename_height[0] != '\0';
	texmgr_raw_slots_t raw;
	TexMgr_LoadAllRawSlots (glt, mat, allowHeight, &raw);

	if (!raw.base.rgba && !albedoFallback)
	{
		glt->rtparallaxscale = 0.0f;
		TexMgr_SetAuxMaterial (glt, &glt->rtoverlaymaterial, NULL, (int)glt->width, (int)glt->height);
		TexMgr_SetAuxMaterial (glt, &glt->rtrastermaterial, NULL, (int)glt->width, (int)glt->height);
		TexMgr_FreeRawSlots (&raw);
		return false;
	}

	const qboolean activeFullbrightWanted = mat->emissive_enabled && fullbrightOverride != NULL;
	int tw = (int)glt->width;
	int th = (int)glt->height;
	TexMgr_ComputeBakeDims (&raw, (int)glt->width, (int)glt->height, activeFullbrightWanted, &tw, &th);
	const int npix = tw * th;

	byte *baseBuf    = TexMgr_FinishSlotBuf (&raw.base, tw, th);
	byte *normBuf    = TexMgr_FinishSlotBuf (&raw.norm, tw, th);
	byte *heightBuf  = TexMgr_FinishSlotBuf (&raw.height, tw, th);
	byte *emisBuf    = TexMgr_FinishSlotBuf (&raw.emis, tw, th);
	glt->rtemissivetex = (emisBuf != NULL);
	byte *glossBuf   = TexMgr_FinishSlotBuf (&raw.gloss, tw, th);
	byte *overlayBuf = TexMgr_ProcessOverlaySlot (&raw.overlay, mat, tw, th);
	byte *sssTexBuf  = TexMgr_FinishSlotBuf (&raw.sss, tw, th);
	byte *baseFallbackResized = !baseBuf ? TexMgr_ResizeExternalRgba ((const byte *)albedoFallback, (int)glt->width, (int)glt->height, tw, th) : NULL;
	const byte *baseAlbedoSrc = baseBuf ? baseBuf : (baseFallbackResized ? baseFallbackResized : (const byte *)albedoFallback);
	byte *fbResized = activeFullbrightWanted ? TexMgr_ResizeExternalRgba (fullbrightOverride, (int)glt->width, (int)glt->height, tw, th) : NULL;
	glt->rtparallaxscale = (heightBuf && mat->parallax_scale > 0.0f) ? CLAMP (0.0f, mat->parallax_scale, 0.25f) : 0.0f;

	qboolean baseHasAlpha = false;
	if (baseBuf)
	{
		for (int i = 0; i < npix; i++)
		{
			if (baseBuf[i * 4 + 3] != 255)
			{
				baseHasAlpha = true;
				break;
			}
		}
	}

	qboolean normHasAlpha = false;
	if (normBuf)
	{
		for (int i = 0; i < npix; i++)
		{
			if (normBuf[i * 4 + 3] != 255)
			{
				normHasAlpha = true;
				break;
			}
		}
	}

	byte *albedo = (byte *)Mem_Alloc ((size_t)npix * 4u);
	byte *rme    = (byte *)Mem_Alloc ((size_t)npix * 4u);
	byte *normal = (byte *)Mem_Alloc ((size_t)npix * 4u);
	byte *rasterAlbedo = overlayBuf && mat->material_glass ? (byte *)Mem_Alloc ((size_t)npix * 4u) : NULL;

	const float baseFactor = (mat->base_factor >= 0.0f) ? mat->base_factor : 1.0f;

	/* The transparency of a glass material rides the alpha of the synthesized
	   normal texture, the one channel the traced path reads but nothing else
	   uses; the glass branch multiplies the refracted light by it. */
	float glassT = mat->material_glass ? mat->transparency : 1.0f;

	if (isnan (glassT))
		glassT = 1.0f;
	if (glassT < 0.0f)
		glassT = 0.0f;
	else if (glassT > 1.0f)
		glassT = 1.0f;

	const byte glassAlpha = (byte)(glassT * 255.0f + 0.5f);

	const qboolean isBrush = glt->owner && glt->owner->type == mod_brush;
	const float defaultRough = isBrush ? CVAR_TO_FLOAT (rt_brush_rough) : CVAR_TO_FLOAT (rt_model_rough);
	const qboolean engineAlpha = (glt->flags & TEXPREF_ALPHA) != 0 || mat->alpha_test;
	const byte    *roughBaseBuf = (baseHasAlpha && !engineAlpha) ? baseBuf : NULL;

	const qboolean has_luma_key = mat->emissive_enabled && (mat->filename_emissive[0] != '\0');
	const qboolean use_color_emissive = mat->emissive_enabled && mat->has_color_emissive && mat->color_emissive_count > 0 && !has_luma_key;
	const byte *activeFullbright = mat->emissive_enabled ? (fbResized ? fbResized : fullbrightOverride) : NULL;
	if (has_luma_key && !emisBuf)
		Con_Printf ("RT: material '%s': texture_emissive '%s' could not be loaded; using no emissive mask\n",
		            mat->name, mat->filename_emissive);
	if (mat->filename_overlay[0] && !overlayBuf)
		Con_Printf ("RT: material '%s': texture_overlay '%s' could not be loaded; using the base texture alone\n",
		            mat->name, mat->filename_overlay);
	const float lightBright = CLAMP (0.0f, mat->light_brightness, 1000.0f);
	/* Per-material rt_emis_blend override, packed into the alpha of the
	   roughness-metallic-emission texture: 0 = not authored, so the global
	   cvar applies; otherwise the authored mode plus one. */
	const int emisBlendCode = (mat->emissive_blend >= 0) ? (mat->emissive_blend + 1) : 0;

	glt->rtemissive = false;
	glt->rtemissivecolor[0] = 0.0f;
	glt->rtemissivecolor[1] = 0.0f;
	glt->rtemissivecolor[2] = 0.0f;
	glt->rtemissivecolorraw[0] = 0.0f;
	glt->rtemissivecolorraw[1] = 0.0f;
	glt->rtemissivecolorraw[2] = 0.0f;
	glt->rtemissivemean = 0.0f;
	glt->rtemissivemeanraw = 0.0f;
	glt->rtemissivemeanbase = 0.0f;
	glt->rtemissivemeanbaseraw = 0.0f;
	glt->rtemisuvmin[0] = 0.0f;
	glt->rtemisuvmin[1] = 0.0f;
	glt->rtemisuvmax[0] = 1.0f;
	glt->rtemisuvmax[1] = 1.0f;
	glt->rtemissiveglow = 0.0f;
	glt->rtemisglowfrac = 1.0f;
	glt->rtemissiveglowtex = false;
	glt->rtislight = false;
	float emissR = 0.0f, emissG = 0.0f, emissB = 0.0f;
	float emissRRaw = 0.0f, emissGRaw = 0.0f, emissBRaw = 0.0f;
	double emissMean = 0.0;
	double emissMeanRaw = 0.0;
	double emissMeanBase = 0.0;
	double emissMeanBaseRaw = 0.0;
	qboolean shapeDiffers = false;
	int    glowminx = tw, glowminy = th, glowmaxx = -1, glowmaxy = -1;
	byte  *emissRawBytes = NULL;

	if (emisBuf || use_color_emissive)
		emissRawBytes = (byte *)Mem_Alloc ((size_t)npix);

	/* The color blocks: each one builds its own mask (its threshold picks the
	   pixels whose color matches it) and feathers it on its own — outward from
	   the mask edge, leaving the pixels it selected untouched — then every
	   pixel goes to the block whose color is nearest among those that match
	   it, and that block's blend mode travels with it in the alpha of the rme
	   texture. */
	float *colorEmis  = NULL;
	byte  *colorBlend = NULL;
	float *colorDist  = NULL;
	float *colorRaw   = NULL;
	if (use_color_emissive && !emisBuf)
	{
		texmgr_emis_bufs_t bufs = {0};

		TexMgr_BuildColorEmissiveBuffers (mat, baseAlbedoSrc, tw, th, emisBlendCode, &bufs);
		colorEmis = bufs.colorEmis;
		colorBlend = bufs.colorBlend;
		colorDist = bufs.colorDist;
		colorRaw = bufs.colorRaw;
	}

	byte *sssMask = TexMgr_BuildSssMaskBytes (mat, baseAlbedoSrc, sssTexBuf,
	                                          tw, th, glt->rtsss);

	for (int i = 0; i < npix; i++)
	{
		const byte *src = baseAlbedoSrc + i * 4;
		int r = (int)CLAMP (0.0f, src[0] * baseFactor, 255.0f);
		int g = (int)CLAMP (0.0f, src[1] * baseFactor, 255.0f);
		int b = (int)CLAMP (0.0f, src[2] * baseFactor, 255.0f);
		if (rasterAlbedo)
		{
			rasterAlbedo[i * 4 + 0] = (byte)r;
			rasterAlbedo[i * 4 + 1] = (byte)g;
			rasterAlbedo[i * 4 + 2] = (byte)b;
		}
		if (overlayBuf && !mat->material_glass)
		{
			const int a = overlayBuf[i * 4 + 3];
			if (a != 0)
			{
				const int inv = 255 - a;
				r = (r * inv + overlayBuf[i * 4 + 0] * 255 + 127) / 255;
				g = (g * inv + overlayBuf[i * 4 + 1] * 255 + 127) / 255;
				b = (b * inv + overlayBuf[i * 4 + 2] * 255 + 127) / 255;
			}
		}
		albedo[i * 4 + 0] = CLAMP (0, r, 255);
		albedo[i * 4 + 1] = CLAMP (0, g, 255);
		albedo[i * 4 + 2] = CLAMP (0, b, 255);
		if (engineAlpha)
			albedo[i * 4 + 3] = src[3];
		else
			albedo[i * 4 + 3] = 255;
		if (rasterAlbedo)
		{
			const int a = overlayBuf[i * 4 + 3];
			for (int c = 0; c < 3; c++)
			{
				const int value = (rasterAlbedo[i * 4 + c] * (255 - a) + overlayBuf[i * 4 + c] * 255 + 127) / 255;
				rasterAlbedo[i * 4 + c] = (byte)CLAMP (0, value, 255);
			}
			rasterAlbedo[i * 4 + 3] = albedo[i * 4 + 3];
		}

		const float rough = TexMgr_ResolvePixelRoughness (glt, mat, glossBuf, roughBaseBuf,
		                                                  defaultRough, i);
		const float metal = TexMgr_ResolvePixelMetalness (mat, normBuf, i);

		float emiss = 0.0f;
		float emissRaw = 0.0f;
		byte  emissBlend = (byte)emisBlendCode;
		if (emisBuf)
		{
			emissRaw = (0.2126f * emisBuf[i * 4 + 0] + 0.7152f * emisBuf[i * 4 + 1] + 0.0722f * emisBuf[i * 4 + 2]) / 255.0f;
			emiss = emissRaw * mat->emissive_factor;
		}
		if (colorEmis || activeFullbright)
		{
			const texmgr_emissive_texel_t composed = TexMgr_ComposeEmissiveTexel(
				colorEmis != NULL, colorEmis ? colorEmis[i] : 0.0f,
				colorRaw ? colorRaw[i] : 0.0f, colorBlend ? colorBlend[i] : 0,
				activeFullbright != NULL,
				activeFullbright ? activeFullbright[i * 4 + 2] / 255.0f : 0.0f,
				(byte)emisBlendCode);

			emiss = composed.emissive;
			emissRaw = composed.raw;
			emissBlend = composed.blend;
		}

		if (emissRaw != emiss)
			shapeDiffers = true;

		if (emissRawBytes != NULL)
		{
			float rawOut = emissRaw;
			if (rawOut > 1.0f)
				rawOut = 1.0f;
			emissRawBytes[i] = (byte)(rawOut * 255.0f + 0.5f);
		}

		if (emiss > 0.0f)
		{
			emissR += albedo[i * 4 + 0] * emiss;
			emissG += albedo[i * 4 + 1] * emiss;
			emissB += albedo[i * 4 + 2] * emiss;
		}

		if (emissRaw > 0.0f)
		{
			emissRRaw += albedo[i * 4 + 0] * emissRaw;
			emissGRaw += albedo[i * 4 + 1] * emissRaw;
			emissBRaw += albedo[i * 4 + 2] * emissRaw;
		}

		emissMeanBase += emiss;
		emissMeanBaseRaw += emissRaw;

		float emissOut = emiss;
		if (emissOut > 1.0f)
			emissOut = 1.0f;
		emissMean += emissOut;

		float emissRawOut = emissRaw;
		if (emissRawOut > 1.0f)
			emissRawOut = 1.0f;
		emissMeanRaw += emissRawOut;

		/* The glow extents are the shape of the mask: brightness dims what it
		   emits, it does not move the light. */
		if (emissRaw > RT_EMIS_GLOW_THRESHOLD)
		{
			const int px = i % tw;
			const int py = i / tw;

			if (px < glowminx) glowminx = px;
			if (px > glowmaxx) glowmaxx = px;
			if (py < glowminy) glowminy = py;
			if (py > glowmaxy) glowmaxy = py;
		}

		rme[i * 4 + 0] = CLAMP (0, (int)(rough * 255), 255);
		rme[i * 4 + 1] = CLAMP (0, (int)(metal * 255), 255);
		rme[i * 4 + 2] = CLAMP (0, (int)(emissOut * 255), 255);
		rme[i * 4 + 3] = emissBlend;

		if (normBuf)
		{
			float nx = (normBuf[i * 4 + 0] - 128.0f) * mat->bump_scale + 128.0f;
			float ny = (normBuf[i * 4 + 1] - 128.0f) * mat->bump_scale + 128.0f;
			normal[i * 4 + 0] = CLAMP (0, (int)nx, 255);
			normal[i * 4 + 1] = CLAMP (0, (int)ny, 255);
			normal[i * 4 + 2] = normBuf[i * 4 + 2];
		}
		else
		{
			normal[i * 4 + 0] = 128;
			normal[i * 4 + 1] = 128;
			normal[i * 4 + 2] = 255;
		}
		normal[i * 4 + 3] = TexMgr_ResolveNormalAlpha (sssMask, glt->rtsss, mat->material_glass,
		                                               glassAlpha, heightBuf, i);
	}

	if (overlayBuf && mat->material_glass)
	{
		for (int i = 0; i < npix; i++)
		{
			const int a = overlayBuf[i * 4 + 3];
			for (int c = 0; c < 3; c++)
			{
				const int value = a > 0 ? (overlayBuf[i * 4 + c] * 255 + a / 2) / a : 0;
				overlayBuf[i * 4 + c] = (byte)CLAMP (0, value, 255);
			}
		}
		TexMgr_AlphaEdgeFix (overlayBuf, tw, th);
	}
	TexMgr_SetAuxMaterial (glt, &glt->rtoverlaymaterial, mat->material_glass ? overlayBuf : NULL, tw, th);
	TexMgr_SetAuxMaterial (glt, &glt->rtrastermaterial, rasterAlbedo, tw, th);
	if (rasterAlbedo) Mem_Free (rasterAlbedo);

	if (colorEmis)  Mem_Free (colorEmis);
	if (colorBlend) Mem_Free (colorBlend);
	if (colorDist)  Mem_Free (colorDist);
	if (colorRaw)   Mem_Free (colorRaw);

	if (baseBuf) Mem_Free (baseBuf);
	if (baseFallbackResized) Mem_Free (baseFallbackResized);
	if (fbResized) Mem_Free (fbResized);
	if (normBuf) Mem_Free (normBuf);
	if (heightBuf) Mem_Free (heightBuf);
	if (emisBuf) Mem_Free (emisBuf);
	if (glossBuf) Mem_Free (glossBuf);
	if (overlayBuf) Mem_Free (overlayBuf);
	if (sssTexBuf) Mem_Free (sssTexBuf);
	if (sssMask) Mem_Free (sssMask);

	if (emissR > 0.0f || emissG > 0.0f || emissB > 0.0f)
	{
		glt->rtemissive = true;
		glt->rtemissivecolor[0] = emissR / (npix * 255.0f);
		glt->rtemissivecolor[1] = emissG / (npix * 255.0f);
		glt->rtemissivecolor[2] = emissB / (npix * 255.0f);
		glt->rtemissivemean = (float)(emissMean / npix);
		glt->rtemissivemeanbase = (float)(emissMeanBase / npix);

		if (glowmaxx >= glowminx && glowmaxy >= glowminy && npix > 0)
		{
			const float glowarea = (float)(glowmaxx - glowminx + 1) * (float)(glowmaxy - glowminy + 1);
			const float glowfrac = glowarea / (float)npix;

			glt->rtemisuvmin[0] = (float)glowminx / (float)tw;
			glt->rtemisuvmin[1] = (float)glowminy / (float)th;
			glt->rtemisuvmax[0] = (float)(glowmaxx + 1) / (float)tw;
			glt->rtemisuvmax[1] = (float)(glowmaxy + 1) / (float)th;
			glt->rtemisglowfrac = glowfrac;
			glt->rtemissiveglowtex = (glowfrac < RT_EMIS_GLOW_FULL) ? true : false;
			glt->rtemissiveglow = (glowfrac > 1e-6f) ? glt->rtemissivemean / glowfrac : glt->rtemissivemean;
		}

	}

	glt->rtemissivecolorraw[0] = emissRRaw / (npix * 255.0f);
	glt->rtemissivecolorraw[1] = emissGRaw / (npix * 255.0f);
	glt->rtemissivecolorraw[2] = emissBRaw / (npix * 255.0f);
	glt->rtemissivemeanraw = (float)(emissMeanRaw / npix);
	glt->rtemissivemeanbaseraw = (float)(emissMeanBaseRaw / npix);

	if (use_color_emissive)
		glt->rtemissivetex = true;

	if (lightBright != 1.0f)
	{
		const float gain = lightBright;

		if (glt->rthaslightcolor)
			VectorScale (glt->rtlightcolor, gain, glt->rtlightcolor);
		VectorScale (glt->rtemissivecolor, gain, glt->rtemissivecolor);
		VectorScale (glt->rtemissivecolorraw, gain, glt->rtemissivecolorraw);
	}

	glt->rtislight = mat->emissive_enabled && mat->is_light;
	glt->rtlightgain = lightBright;

	extern cvar_t rt_mat_debug;
	if (CVAR_TO_BOOL (rt_mat_debug))
	{
		double rSum = 0.0, mSum = 0.0, eSum = 0.0;
		int    rMin = 255, rMax = 0, mMin = 255, mMax = 0, eMin = 255, eMax = 0;
		for (int i = 0; i < npix; i++)
		{
			const int rv = rme[i * 4 + 0];
			const int mv = rme[i * 4 + 1];
			const int ev = rme[i * 4 + 2];
			rSum += rv; mSum += mv; eSum += ev;
			if (rv < rMin) rMin = rv; if (rv > rMax) rMax = rv;
			if (mv < mMin) mMin = mv; if (mv > mMax) mMax = mv;
			if (ev < eMin) eMin = ev; if (ev > eMax) eMax = ev;
		}
		Con_Printf ("RT: applied material '%s' (glt='%s') base=%s norm=%s emis=%s overlay=%s gloss=%s (%ix%i) baseAlpha=%d normAlpha=%d defRough=%.2f\n",
		            mat->name, glt->name,
		            mat->filename_base[0] ? mat->filename_base : "-",
		            mat->filename_normals[0] ? mat->filename_normals : "-",
		            mat->filename_emissive[0] ? mat->filename_emissive : "-",
		            mat->filename_overlay[0] ? mat->filename_overlay : "-",
		            mat->filename_gloss[0] ? mat->filename_gloss : "-",
		            tw, th, baseHasAlpha ? 1 : 0, normHasAlpha ? 1 : 0, defaultRough);
		if (glt->rtoverlaymaterial != QR_NO_MATERIAL)
			Con_Printf ("RT: glass paint '%s': separate matte material=%u raster material=%u\n",
			            mat->name, (unsigned)glt->rtoverlaymaterial, (unsigned)glt->rtrastermaterial);
		Con_Printf ("RT:   rme rough[min=%.0f avg=%.2f max=%.0f] metal[min=%.0f avg=%.2f max=%.0f] emis[min=%.0f avg=%.2f max=%.0f]\n",
		            rMin / 255.0f * 100.0f, npix ? rSum / npix / 255.0f * 100.0f : 0.0, rMax / 255.0f * 100.0f,
		            mMin / 255.0f * 100.0f, npix ? mSum / npix / 255.0f * 100.0f : 0.0, mMax / 255.0f * 100.0f,
		            eMin / 255.0f, npix ? eSum / npix / 255.0f : 0.0, eMax / 255.0f);
		Con_Printf ("RT:   flags is_light=%d light_styles=%d light_brightness=%.3f has_light_color=%d rtlightcolor=(%.3f, %.3f, %.3f) rtemissive=%d rtemissivecolor=(%.4f, %.4f, %.4f) rtemissivemean=%.4f rtemissivemeanbase=%.4f rtemissivemeanraw=%.4f rtemissivemeanbaseraw=%.4f shapediffers=%d\n",
		            glt->rtislight ? 1 : 0, glt->rtlightstyles ? 1 : 0,
		            mat->light_brightness,
		            glt->rthaslightcolor ? 1 : 0,
		            glt->rtlightcolor[0], glt->rtlightcolor[1], glt->rtlightcolor[2],
		            glt->rtemissive ? 1 : 0,
		            glt->rtemissivecolor[0], glt->rtemissivecolor[1], glt->rtemissivecolor[2],
		            glt->rtemissivemean,
		            glt->rtemissivemeanbase,
		            glt->rtemissivemeanraw,
		            glt->rtemissivemeanbaseraw,
		            shapeDiffers ? 1 : 0);
		Con_Printf ("RT:   glow uv=(%.3f, %.3f)-(%.3f, %.3f) areaFrac=%.3f density=%.4f wholeTexture=%d\n",
		            glt->rtemisuvmin[0], glt->rtemisuvmin[1],
		            glt->rtemisuvmax[0], glt->rtemisuvmax[1],
		            glt->rtemisglowfrac, glt->rtemissiveglow,
		            glt->rtemissiveglowtex ? 1 : 0);
	}


	if (texmgr_dumping_reload && !TexMgr_AlreadyDumped (mat->name) && CVAR_TO_BOOL (qr_material_editor_debug))
	{
		Con_Printf ("qr editor dump: material '%s' tex '%s' %dx%d base='%s' emis='%s' overlay='%s' gloss='%s' norm='%s' light=%d\n",
		            mat->name, glt->name, tw, th,
		            mat->filename_base[0] ? mat->filename_base : "-",
		            mat->filename_emissive[0] ? mat->filename_emissive : "-",
		            mat->filename_overlay[0] ? mat->filename_overlay : "-",
		            mat->filename_gloss[0] ? mat->filename_gloss : "-",
		            mat->filename_normals[0] ? mat->filename_normals : "-",
		            mat->is_light ? 1 : 0);
		Con_Printf ("qr editor dump:   loaded base=%d emis=%d overlay=%d gloss=%d norm=%d mean=%.4f\n",
		            baseBuf ? 1 : 0, emisBuf ? 1 : 0, overlayBuf ? 1 : 0, glossBuf ? 1 : 0, normBuf ? 1 : 0, glt->rtemissivemean);

		TexMgr_DumpReloadTGA ("_albedo", glt->name, tw, th, albedo);
		TexMgr_DumpReloadTGA ("_rme", glt->name, tw, th, rme);
		TexMgr_DumpReloadTGA ("_normal", glt->name, tw, th, normal);

		if (texmgr_dumped_count < QRE_DUMPED_MAX)
			q_strlcpy (texmgr_dumped[texmgr_dumped_count++], mat->name, MAX_QPATH);
	}

	QrMaterial oldMaterial = glt->rtmaterial;
	QrMaterial newMaterial = QR_NULL_HANDLE;

	const qboolean splitShape = mat->is_light && glt->rtemissivetex && shapeDiffers && emissRawBytes != NULL;
	byte          *shapeRme   = NULL;

	if (splitShape)
	{
		shapeRme = (byte *)Mem_Alloc ((size_t)npix * 4);
		memcpy (shapeRme, rme, (size_t)npix * 4);

		for (int i = 0; i < npix; i++)
			shapeRme[i * 4 + 2] = emissRawBytes[i];
	}

	QrMaterialCreateInfo info = {
		.flags = TexMgr_GetRtCreateFlags (glt),
		.size = {(uint32_t)tw, (uint32_t)th},
		.textures =
			{
				.pDataAlbedoAlpha = albedo,
				.pDataRoughnessMetallicEmission = rme,
				.pDataNormal = normal,
			},
		.pRelativePath = glt->rtname,
		.filter = TexMgr_GetFilterMode (glt),
		.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT,
	};

	if (texmgr_live_reload && !texmgr_live_recreate && oldMaterial != QR_NULL_HANDLE)
	{
		const QrExtent2D updateSize = {(uint32_t)tw, (uint32_t)th};

		if (qrCanUpdateMaterialContents (vulkan_globals.instance, oldMaterial, updateSize) == QR_SUCCESS)
		{
			QrMaterialUpdateInfo update = {
				.target = oldMaterial,
				.textures =
					{
						.pDataAlbedoAlpha = albedo,
						.pDataRoughnessMetallicEmission = rme,
						.pDataNormal = normal,
					},
			};

			SDL_LockMutex (rtspecial_mutex);
			QrResult ur = qrUpdateMaterialContents (vulkan_globals.instance, &update);
			SDL_UnlockMutex (rtspecial_mutex);

			if (ur == QR_SUCCESS)
			{
				if (TexMgr_ApplyLightMaterial (glt, &info, shapeRme, splitShape) && texmgr_live_reload)
					texmgr_live_material_replaced = true;

				if (shapeRme) Mem_Free (shapeRme);
				if (emissRawBytes) Mem_Free (emissRawBytes);

				Mem_Free (albedo);
				Mem_Free (rme);
				Mem_Free (normal);

				return true;
			}
		}
	}

	SDL_LockMutex (rtspecial_mutex);
	QrResult r = qrCreateMaterial (vulkan_globals.instance, &info, &newMaterial);
	if (oldMaterial)
		qrDestroyMaterial (vulkan_globals.instance, oldMaterial);
	SDL_UnlockMutex (rtspecial_mutex);
	QR_CHECK (r);

	glt->rtmaterial = newMaterial;

	if (TexMgr_ApplyLightMaterial (glt, &info, shapeRme, splitShape) && texmgr_live_reload)
		texmgr_live_material_replaced = true;

	if (shapeRme) Mem_Free (shapeRme);
	if (emissRawBytes) Mem_Free (emissRawBytes);

	if (texmgr_live_reload && oldMaterial != QR_NULL_HANDLE && newMaterial != oldMaterial)
		texmgr_live_material_replaced = true;

	Mem_Free (albedo);
	Mem_Free (rme);
	Mem_Free (normal);

	return true;
}

/*
================
TexMgr_ApplyMaterialFromMat

The revision moves after the synthesis, and only after it, so a reader that sees a new revision is
looking at fields that revision produced; one that catches the synthesis mid-flight reads the old
revision and its DTAL entry is refused as soon as the move lands.
================
*/
static qboolean TexMgr_ApplyMaterialFromMat (gltexture_t *glt, unsigned *albedoFallback, byte *fullbrightOverride)
{
	const qboolean applied = TexMgr_ApplyMaterialFromMatInternal (glt, albedoFallback, fullbrightOverride);

	Atomic_AddUInt32 (&rt_material_revision, 1);

	return applied;
}

/*
================
TexMgr_LoadImage8 -- handles 8bit source data, then passes it to LoadImage32
================
*/
static void TexMgr_LoadImage8 (gltexture_t *glt, byte *data)
{
	GL_DeleteTexture (glt);

	extern cvar_t gl_fullbrights;
	unsigned int *usepal;
	int           i;

	// HACK HACK HACK -- taken from tomazquake
	if (strstr (glt->name, "shot1sid") && glt->width == 32 && glt->height == 32 && CRC_Block (data, 1024) == 65393)
	{
		// This texture in b_shell1.bsp has some of the first 32 pixels painted white.
		// They are invisible in software, but look really ugly in GL. So we just copy
		// 32 pixels from the bottom to make it look nice.
		memcpy (data, data + 32 * 31, 32);
	}

	// detect false alpha cases
	if (glt->flags & TEXPREF_ALPHA && !(glt->flags & TEXPREF_CONCHARS))
	{
		for (i = 0; i < (int)(glt->width * glt->height); i++)
			if (data[i] == 255) // transparent index
				break;
		if (i == (int)(glt->width * glt->height))
			glt->flags -= TEXPREF_ALPHA;
	}

	// choose palette and padbyte
	if (glt->flags & TEXPREF_CONCHARS)
	{
		usepal = d_8to24table_conchars;
	}
#if !RT_RENDERER
	else if (glt->flags & TEXPREF_NOBRIGHT && gl_fullbrights.value)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_nobright_fence;
		else
			usepal = d_8to24table_nobright;
	}
#endif
	else
	{
		usepal = d_8to24table;
	}

	// convert to 32bit
	unsigned *converted;
	TEMP_ALLOC (unsigned, converted, glt->width * glt->height);
	TexMgr_8to32 (data, converted, glt->width * glt->height, usepal);

	// fix edges
	if (glt->flags & TEXPREF_ALPHA)
		TexMgr_AlphaEdgeFix ((byte *)converted, glt->width, glt->height);

	// upload it
	TexMgr_LoadImage32 (glt, (unsigned *)converted);

	TEMP_FREE (converted);
}

/*
================
TexMgr_LoadLightmap -- handles lightmap data
================
*/
static void TexMgr_LoadLightmap (gltexture_t *glt, byte *data)
{
	TexMgr_LoadImage32 (glt, (unsigned *)data);
}

/*
================
TexMgr_LoadImage -- the one entry point for loading all textures
================
*/
gltexture_t *TexMgr_LoadImage (
	const char *rtname,
	qmodel_t *owner, const char *name, int width, int height, enum srcformat format, byte *data, const char *source_file, src_offset_t source_offset,
	unsigned flags)
{
	unsigned short crc = 0;
	gltexture_t   *glt;

	if (isDedicated)
		return NULL;

	// cache check
	if (flags & TEXPREF_OVERWRITE)
		switch (format)
		{
		case SRC_INDEXED:
			crc = CRC_Block (data, width * height);
			break;
		case SRC_LIGHTMAP:
			crc = CRC_Block (data, width * height * LIGHTMAP_BYTES);
			break;
		case SRC_RGBA:
			crc = CRC_Block (data, width * height * 4);
			break;
		default: /* not reachable but avoids compiler warnings */
			crc = 0;
		}
	if ((flags & TEXPREF_OVERWRITE) && (glt = TexMgr_FindTexture (owner, name)))
	{
		if (glt->source_crc == crc)
			return glt;
	}
	else
		glt = TexMgr_NewTexture ();

	// copy data
	glt->owner = owner;
	q_strlcpy (glt->name, name, sizeof (glt->name));
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->shirt = -1;
	glt->pants = -1;
	q_strlcpy (glt->source_file, source_file, sizeof (glt->source_file));
	glt->source_offset = source_offset;
	glt->source_format = format;
	glt->source_width = width;
	glt->source_height = height;
	glt->source_crc = crc;

	if (rtname)
	{
	    q_strlcpy (glt->rtname, rtname, sizeof (glt->rtname));
	}
	else
	{
		glt->rtname[0] = '\0';
	}

	glt->rtlightcolor[0] = glt->rtlightcolor[1] = glt->rtlightcolor[2] = 0.0f;
	glt->rthaslightcolor = false;
	glt->rtmirror = false;
	glt->rtexactnormals = false;
	glt->rtforcerasterize = false;
	glt->rtalphatest = false;
	glt->rtopaquealpha = false;
	glt->rtglass = false;
	glt->rtglassior = 0.0f;
	glt->rtglassthickness = 2.0f;
	glt->rtglasscolor[0] = 1.0f;
	glt->rtglasscolor[1] = 1.0f;
	glt->rtglasscolor[2] = 1.0f;
	glt->rtwater = false;
	glt->rtemissive = false;
	glt->rtemissivecolor[0] = glt->rtemissivecolor[1] = glt->rtemissivecolor[2] = 0.0f;
	glt->rtemissivecolorraw[0] = glt->rtemissivecolorraw[1] = glt->rtemissivecolorraw[2] = 0.0f;
	glt->rtemissivemean = 0.0f;
	glt->rtemissivemeanbase = 0.0f;
	glt->rtemissivemeanbaseraw = 0.0f;
	glt->rtemisangleinner = 0.0f;
	glt->rtemisangleouter = 0.0f;
	glt->rtemisprojector = false;
	TexMgr_ResetLightShafts (glt);
	TexMgr_ResetSubsurface (glt);
	glt->rtemisuvmin[0] = glt->rtemisuvmin[1] = 0.0f;
	glt->rtemisuvmax[0] = glt->rtemisuvmax[1] = 1.0f;
	glt->rtemissiveglow = 0.0f;
	glt->rtemisglowfrac = 1.0f;
	glt->rtemissiveglowtex = false;
	glt->rtemissivetex = false;
	glt->rtlightgain = 1.0f;
	glt->rtlightmaterial = QR_NULL_HANDLE;
	glt->rtemissivemeanraw = 0.0f;
	glt->rtislight = false;
	glt->rtlightstyles = true;
	glt->rthasmaterial = false;

	// upload it
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	return glt;
}

/*
================================================================================

    COLORMAPPING AND TEXTURE RELOADING

================================================================================
*/

/*
================
TexMgr_ReloadImage -- reloads a texture, and colormaps it if needed
================
*/
void TexMgr_ReloadImage (gltexture_t *glt, int shirt, int pants)
{
	byte  translation[256];
	byte *src, *dst, *data = NULL, *allocated = NULL, *translated = NULL;
	int   size, i;
	//
	// get source data
	//

	if (glt->source_file[0] && glt->source_offset)
	{
		// lump inside file
		FILE *f = NULL;
		COM_FOpenFile (glt->source_file, &f, NULL);
		if (!f || glt->source_offset > (src_offset_t)0x7fffffff)
		{
			if (f)
				fclose (f);
			goto invalid;
		}
		if (fseek (f, (long)glt->source_offset, SEEK_CUR) != 0)
		{
			Con_DWarning ("TexMgr_ReloadImage: seek to %llu failed in %s\n",
			              (unsigned long long)glt->source_offset, glt->source_file);
			fclose (f);
			goto invalid;
		}
		size = glt->source_width * glt->source_height;
		/* should be SRC_INDEXED, but no harm being paranoid:  */
		if (glt->source_format == SRC_RGBA)
		{
			size *= 4;
		}
		else if (glt->source_format == SRC_LIGHTMAP)
		{
			size *= LIGHTMAP_BYTES;
		}
		/* A texture whose recorded offset lies outside the file cannot be read
		   back; say so instead of turning unrelated file bytes into a texture. */
		if (glt->source_offset + (src_offset_t)size > (src_offset_t)com_filesize)
		{
			Con_DWarning ("TexMgr_ReloadImage: %s + %llu (%d bytes) is outside %s (%d bytes)\n",
			              glt->name, (unsigned long long)glt->source_offset, size, glt->source_file, com_filesize);
			fclose (f);
			goto invalid;
		}
		allocated = data = (byte *)Mem_Alloc (size);
		if (fread (data, 1, size, f) != size)
		{
			fclose (f);
			Mem_Free (allocated);
			allocated = NULL;
			goto invalid;
		}
		fclose (f);
	}
	else if (glt->source_file[0] && !glt->source_offset)
	{
		allocated = data = Image_LoadImage (glt->source_file, (int *)&glt->source_width, (int *)&glt->source_height); // simple file
	}
	else if (!glt->source_file[0] && glt->source_offset)
	{
		data = (byte *)glt->source_offset; // image in memory
	}
	if (!data)
	{
	invalid:
		Con_Printf ("TexMgr_ReloadImage: invalid source for %s\n", glt->name);
		return;
	}

	glt->width = glt->source_width;
	glt->height = glt->source_height;
	//
	// apply shirt and pants colors
	//
	// if shirt and pants are -1,-1, use existing shirt and pants colors
	// if existing shirt and pants colors are -1,-1, don't bother colormapping
	if (shirt > -1 && pants > -1)
	{
		if (glt->source_format == SRC_INDEXED)
		{
			glt->shirt = shirt;
			glt->pants = pants;
		}
		else
			Con_Printf ("TexMgr_ReloadImage: can't colormap a non SRC_INDEXED texture: %s\n", glt->name);
	}
	if (glt->shirt > -1 && glt->pants > -1)
	{
		// create new translation table
		for (i = 0; i < 256; i++)
			translation[i] = i;

		shirt = glt->shirt * 16;
		if (shirt < 128)
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[TOP_RANGE + i] = shirt + 15 - i;
		}

		pants = glt->pants * 16;
		if (pants < 128)
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + i;
		}
		else
		{
			for (i = 0; i < 16; i++)
				translation[BOTTOM_RANGE + i] = pants + 15 - i;
		}

		// translate texture
		size = glt->width * glt->height;
		dst = translated = (byte *)Mem_Alloc (size);
		src = data;

		for (i = 0; i < size; i++)
			*dst++ = translation[*src++];

		data = translated;
	}
	//
	// upload it
	//
	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8 (glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap (glt, data);
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		TexMgr_LoadImage32 (glt, (unsigned *)data);
		break;
	}

	Mem_Free (translated);
	Mem_Free (allocated);
}

/*
================
TexMgr_CollectGroupNames

The animation-frame ring of a texture name as the engine actually has it: every
active texture whose normalized name shares the ring base ("textures/+Nname",
"textures/name", "progs/model.mdl:frameN") is listed once. The editor turns the
names no material exists for into editable defaults, so a face never opens a
block named after the base only -- a name no texture resolves to.
================
*/
static void TexMgr_GroupName (const char *name, char *out, size_t outsize)
{
	char *dot;

	RT_MAT_NormalizeName (name, out, outsize);
	dot = strrchr (out, '.');
	if (dot && !strchr (dot, ':'))
		*dot = '\0';
}

int TexMgr_CollectGroupNames (const char *texname, char (*names)[MAX_QPATH], int max)
{
	char         group[MAX_QPATH];
	char         name[MAX_QPATH];
	gltexture_t *glt;
	int          count = 0;

	if (!texname || !texname[0] || max <= 0)
		return 0;

	TexMgr_GroupName (texname, name, sizeof (name));
	RT_MAT_GroupBaseOf (name, group, sizeof (group));

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		char n[MAX_QPATH];
		char g[MAX_QPATH];
		int  i, dup = 0;

		if (!glt->name[0])
			continue;

		TexMgr_GroupName (glt->name, n, sizeof (n));
		RT_MAT_GroupBaseOf (n, g, sizeof (g));
		if (strcmp (g, group))
			continue;

		for (i = 0; i < count; i++)
		{
			if (!strcmp (names[i], n))
				dup = 1;
		}
		if (dup)
			continue;

		if (count >= max)
			break;
		q_strlcpy (names[count++], n, MAX_QPATH);
	}

	return count;
}

/*
================
TexMgr_CollectTextureNames

The names of the textures the engine currently has, for the material editor's picker: one name
per texture, in the editor's own spelling (what TexMgr_GroupName builds), the special fallbacks
and the interface art left out and only the two namespaces the materials live in kept
("textures/..." for a world texture, "progs/...:frameN" for a model's or sprite's skin).
================
*/
int TexMgr_CollectTextureNames (char (*names)[MAX_QPATH], int max)
{
	gltexture_t *glt;
	int          count = 0;

	if (!names || max <= 0)
		return 0;

	for (glt = active_gltextures; glt && count < max; glt = glt->next)
	{
		char n[MAX_QPATH];
		int  i, dup = 0;

		if (!glt->name[0])
			continue;

		TexMgr_GroupName (glt->name, n, sizeof (n));

		if (strncmp (n, "textures/", 9) && strncmp (n, "progs/", 6))
			continue;

		for (i = 0; i < count; i++)
		{
			if (!strcmp (names[i], n))
			{
				dup = 1;
				break;
			}
		}

		if (dup)
			continue;

		q_strlcpy (names[count++], n, MAX_QPATH);
	}

	return count;
}

gltexture_t *TexMgr_FindTextureByNormalizedName (const char *name)
{
	char         target[MAX_QPATH];
	gltexture_t *glt;

	if (!name || !name[0])
		return NULL;

	TexMgr_GroupName (name, target, sizeof (target));

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		char candidate[MAX_QPATH];

		if (!glt->name[0])
			continue;

		TexMgr_GroupName (glt->name, candidate, sizeof (candidate));
		if (!q_strcasecmp (target, candidate))
			return glt;
	}

	return NULL;
}

qboolean TexMgr_TextureNameExists (const char *name)
{
	return TexMgr_FindTextureByNormalizedName (name) != NULL;
}

static void TexMgr_ResizeRgbaBilinear (const byte *src, int src_width, int src_height,
                                       byte *dst, int dst_width, int dst_height)
{
	int x, y;

	for (y = 0; y < dst_height; y++)
	{
		const float sy = ((float)y + 0.5f) * (float)src_height / (float)dst_height - 0.5f;
		int         y0 = (int)floorf (sy);
		float       fy = sy - (float)y0;

		if (y0 < 0)
		{
			y0 = 0;
			fy = 0.0f;
		}
		if (y0 >= src_height - 1)
		{
			y0 = src_height - 1;
			fy = 0.0f;
		}

		for (x = 0; x < dst_width; x++)
		{
			const float sx = ((float)x + 0.5f) * (float)src_width / (float)dst_width - 0.5f;
			int         x0 = (int)floorf (sx);
			float       fx = sx - (float)x0;
			const int   x1 = x0 + 1 < src_width ? x0 + 1 : src_width - 1;
			const int   y1 = y0 + 1 < src_height ? y0 + 1 : src_height - 1;
			const byte *p00;
			const byte *p10;
			const byte *p01;
			const byte *p11;
			byte       *out;
			int         c;

			if (x0 < 0)
			{
				x0 = 0;
				fx = 0.0f;
			}
			if (x0 >= src_width - 1)
			{
				x0 = src_width - 1;
				fx = 0.0f;
			}

			p00 = src + ((size_t)y0 * src_width + x0) * 4;
			p10 = src + ((size_t)y0 * src_width + x1) * 4;
			p01 = src + ((size_t)y1 * src_width + x0) * 4;
			p11 = src + ((size_t)y1 * src_width + x1) * 4;
			out = dst + ((size_t)y * dst_width + x) * 4;

			for (c = 0; c < 4; c++)
			{
				const float top = (float)p00[c] + ((float)p10[c] - (float)p00[c]) * fx;
				const float bottom = (float)p01[c] + ((float)p11[c] - (float)p01[c]) * fx;

				out[c] = (byte)(top + (bottom - top) * fy + 0.5f);
			}
		}
	}
}

qboolean TexMgr_ResizeRgba (const byte *src, int src_width, int src_height,
                            byte *dst, int dst_width, int dst_height)
{
	if (!src || !dst || src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0)
		return false;

	if (src_width == dst_width && src_height == dst_height)
	{
		memcpy (dst, src, (size_t)src_width * src_height * 4);
		return true;
	}

	if (dst_width > src_width || dst_height > src_height)
	{
		TexMgr_ResizeRgbaBilinear (src, src_width, src_height, dst, dst_width, dst_height);
		return true;
	}

	return stbir_resize_uint8 (src, src_width, src_height, 0, dst, dst_width, dst_height, 0, 4) != 0;
}

void TexMgr_SetExternalTextureResolver (TexMgrTextureResolver resolver)
{
	texmgr_external_resolver = resolver;
}

unsigned int TexMgr_TextureListRevision (void)
{
	unsigned int revision;

	SDL_LockMutex (texmgr_mutex);
	revision = texmgr_list_revision;
	SDL_UnlockMutex (texmgr_mutex);

	return revision;
}

/*
================
TexMgr_LoadRgbaForPreview

The RGBA8 pixels of a texture's own source, for the editor's texture preview
and its color picker. The source is resolved the way TexMgr_ReloadImage
resolves it (a lump in a file, an image file, or a buffer the loader kept).
The caller frees the returned buffer.
================
*/
byte *TexMgr_LoadRgbaForPreview (gltexture_t *glt, int *outWidth, int *outHeight)
{
	byte *src = NULL, *allocated = NULL, *out;
	int   npix, i;
	int   width, height;

	if (!glt)
		return NULL;

	width = (int)glt->source_width;
	height = (int)glt->source_height;

	if (glt->source_file[0] && glt->source_offset)
	{
		FILE *f = NULL;
		int   size;

		COM_FOpenFile (glt->source_file, &f, NULL);
		if (!f || glt->source_offset > (src_offset_t)0x7fffffff)
		{
			if (f)
				fclose (f);
			return NULL;
		}
		if (fseek (f, (long)glt->source_offset, SEEK_CUR) != 0)
		{
			fclose (f);
			return NULL;
		}
		size = width * height;
		if (glt->source_format == SRC_RGBA)
			size *= 4;
		else if (glt->source_format == SRC_LIGHTMAP)
			size *= LIGHTMAP_BYTES;
		if (size <= 0 || glt->source_offset + (src_offset_t)size > (src_offset_t)com_filesize)
		{
			fclose (f);
			return NULL;
		}
		allocated = src = (byte *)Mem_Alloc (size);
		if (fread (src, 1, size, f) != (size_t)size)
		{
			fclose (f);
			Mem_Free (allocated);
			return NULL;
		}
		fclose (f);
	}
	else if (glt->source_file[0] && !glt->source_offset)
	{
		allocated = src = Image_LoadImage (glt->source_file, &width, &height);
	}
	else if (!glt->source_file[0] && glt->source_offset)
	{
		src = (byte *)glt->source_offset;
	}

	if (!src || width <= 0 || height <= 0)
	{
		if (allocated)
			Mem_Free (allocated);
		return NULL;
	}

	npix = width * height;
	out = (byte *)Mem_Alloc (npix * 4);

	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_8to32 (src, (unsigned *)out, npix, d_8to24table);
		break;
	case SRC_LIGHTMAP:
		for (i = 0; i < npix; i++)
		{
			out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = src[i * LIGHTMAP_BYTES];
			out[i * 4 + 3] = 255;
		}
		break;
	case SRC_RGBA:
	case SRC_SURF_INDICES:
		memcpy (out, src, (size_t)npix * 4);
		break;
	default:
		Mem_Free (out);
		out = NULL;
		break;
	}

	if (allocated)
		Mem_Free (allocated);

	if (out)
	{
		if (outWidth)
			*outWidth = width;
		if (outHeight)
			*outHeight = height;
	}
	return out;
}

/*
================
TexMgr_ReloadNobrightImages -- reloads all texture that were loaded with the nobright palette.  called when gl_fullbrights changes
================
*/
void TexMgr_ReloadNobrightImages (void)
{
#if !RT_RENDERER
	gltexture_t *glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->flags & TEXPREF_NOBRIGHT)
			TexMgr_ReloadImage (glt, -1, -1);
#endif
}

/*
================
TexMgr_FindFullbrightTexture -- the glow/luma sidecar of a two-pass base texture

The sidecar is loaded from "<base file>_glow" or "<base file>_luma", which is what
its own source file is set to, so comparing both names pairs them up.
================
*/
static gltexture_t *TexMgr_FindFullbrightTexture (const gltexture_t *base)
{
	gltexture_t *glt;
	const size_t baselen = strlen (base->source_file);

	if (!baselen || base->source_offset)
		return NULL;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		const char *suffix;

		if (!(glt->flags & TEXPREF_RT_IS_EMISSIVE))
			continue;
		if (strlen (glt->source_file) <= baselen)
			continue;
		if (q_strncasecmp (glt->source_file, base->source_file, baselen))
			continue;

		suffix = glt->source_file + baselen;
		if (!q_strcasecmp (suffix, "_glow") || !q_strcasecmp (suffix, "_luma"))
			return glt;
	}

	return NULL;
}

/*
================
TexMgr_ReloadAllImages

Reloads every reloadable image texture so that material properties baked in
at load time (emissive color, light brightness, ...) are re-applied from a
fresh qray.materials.yaml. Called by vid_restart.

Skips lightmaps / surface-indices (they never carry a material) and reloads the
auxiliary fullbright texture of the two-pass load together with its base
texture, because the emission mask of the base material is derived from it.
================
*/
void TexMgr_ReloadAllImages (void)
{
	gltexture_t *glt, *fullbright;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->flags & TEXPREF_RT_IS_EMISSIVE)
			continue;
		if (glt->source_format != SRC_INDEXED && glt->source_format != SRC_RGBA)
			continue;
		if (!glt->source_file[0] && !glt->source_offset)
			continue;

		fullbright = TexMgr_FindFullbrightTexture (glt);
		if (!fullbright)
		{
			TexMgr_ReloadImage (glt, -1, -1);
			continue;
		}

		/* Glow/luma textures are built in two passes: the base pass stores the
		   albedo, the fullbright pass turns the sidecar into the emission mask of
		   that albedo (see TexMgr_RT_SpecialFullbright). Both are needed, or the
		   reload would drop the mask and the whole map would lose its emission. */
		TexMgr_RT_SpecialStart (CVAR_TO_FLOAT (rt_brush_rough), CVAR_TO_FLOAT (rt_brush_metal));
		TexMgr_ReloadImage (glt, -1, -1);
		if (rtspecial_target != NULL)
			TexMgr_ReloadImage (fullbright, -1, -1);
		TexMgr_RT_SpecialEnd ();
	}
}

/*
================
TexMgr_ReloadOne

Reloads one texture together with its glow/luma sidecar, in the same two-pass
sequence a full reload uses: the base pass stores the albedo, the fullbright
pass turns the sidecar into the emission mask of that albedo (see
TexMgr_RT_SpecialFullbright). Both are needed, or the reload would drop the mask
and the surfaces using it would lose their emission.
================
*/
static void TexMgr_ReloadOne (gltexture_t *glt)
{
	gltexture_t *fullbright = TexMgr_FindFullbrightTexture (glt);

	if (!fullbright)
	{
		TexMgr_ReloadImage (glt, -1, -1);
		return;
	}

	TexMgr_RT_SpecialStart (CVAR_TO_FLOAT (rt_brush_rough), CVAR_TO_FLOAT (rt_brush_metal));
	TexMgr_ReloadImage (glt, -1, -1);
	if (rtspecial_target != NULL)
		TexMgr_ReloadImage (fullbright, -1, -1);
	TexMgr_RT_SpecialEnd ();
}

// A texture whose pixels can be read back: lightmaps, surface indices and the
// sidecars are skipped, as are textures that have no durable source.
static qboolean TexMgr_ReloadableSource (const gltexture_t *glt)
{
	if (glt->flags & TEXPREF_RT_IS_EMISSIVE)
		return false;
	if (glt->source_format != SRC_INDEXED && glt->source_format != SRC_RGBA)
		return false;
	if (!glt->source_file[0] && !glt->source_offset)
		return false;
	return true;
}

static void TexMgr_LogReloaded (const gltexture_t *glt)
{
	if (!CVAR_TO_BOOL (qr_material_editor_debug))
		return;

	Con_Printf ("qr editor:   tex '%s' %ux%u fmt=%d off=%llu src='%s' flags=0x%x\n",
	            glt->name, glt->width, glt->height, (int)glt->source_format,
	            (unsigned long long)glt->source_offset, glt->source_file, glt->flags);
}

/*
================
TexMgr_ReloadImagesForMaterial

Like TexMgr_ReloadAllImages, but only for the textures whose material is the named
one. Used by the live material editor to re-synthesize the affected textures after
a parameter change; the two-pass glow/luma sidecar of each base texture is reloaded
together with it, exactly as a full reload would.
================
*/
int TexMgr_ReloadImagesForMaterial (const char *materialName, qboolean recreate)
{
	gltexture_t *glt;
	int          count = 0;

	if (!materialName || !materialName[0])
		return 0;

	if (CVAR_TO_BOOL (qr_material_editor_debug))
		Con_Printf ("qr editor: reload material '%s'\n", materialName);
	texmgr_dumping_reload = true;

	const qboolean prev_live = texmgr_live_reload;
	const qboolean prev_recreate = texmgr_live_recreate;
	texmgr_live_reload = true;
	texmgr_live_recreate = recreate;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		rt_material_t *mat;

		if (!TexMgr_ReloadableSource (glt))
			continue;

		mat = RT_MAT_Find (glt->name);
		if (!mat || strcmp (mat->name, materialName))
			continue;

		TexMgr_LogReloaded (glt);
		TexMgr_ReloadOne (glt);
		count++;
	}

	texmgr_live_reload = prev_live;
	texmgr_live_recreate = prev_recreate;
	texmgr_dumping_reload = false;

	return count;
}

/*
================
TexMgr_ReloadImagesForTextureName

Reloads the textures that carry the given normalized texture name even when no
material resolves to them any more: Cancel/Exit drop a material the editor had
created for a texture, and that texture still has to be re-synthesized (without
the material this time).
================
*/
int TexMgr_ReloadImagesForTextureName (const char *texname)
{
	gltexture_t *glt;
	int          count = 0;

	if (!texname || !texname[0])
		return 0;

	if (CVAR_TO_BOOL (qr_material_editor_debug))
		Con_Printf ("qr editor: reload texture '%s'\n", texname);
	texmgr_dumping_reload = true;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		char  n[MAX_QPATH];
		char *dot;

		if (!TexMgr_ReloadableSource (glt))
			continue;

		RT_MAT_NormalizeName (glt->name, n, sizeof (n));
		dot = strrchr (n, '.');
		if (dot && !strchr (dot, ':'))
			*dot = '\0';
		if (q_strcasecmp (n, texname))
			continue;

		TexMgr_LogReloaded (glt);
		TexMgr_ReloadOne (glt);
		count++;
	}

	texmgr_dumping_reload = false;

	return count;
}

/*
================================================================================

    TEXTURE BINDING / TEXTURE UNIT SWITCHING

================================================================================
*/

/*
================
GL_DeleteTexture
================
*/
static void GL_DeleteTexture (gltexture_t *texture)
{
	SDL_LockMutex (texmgr_mutex);

	if (texture->rtmaterial != QR_NO_MATERIAL && !texmgr_live_reload)
	{
		SDL_LockMutex (rtspecial_mutex);
		QrResult r = qrDestroyMaterial (vulkan_globals.instance, texture->rtmaterial);
		SDL_UnlockMutex (rtspecial_mutex);
		QR_CHECK (r);

		texture->rtmaterial = QR_NO_MATERIAL;
	}
	if (!texmgr_live_reload)
	{
		TexMgr_SetAuxMaterial (texture, &texture->rtoverlaymaterial, NULL, (int)texture->width, (int)texture->height);
		TexMgr_SetAuxMaterial (texture, &texture->rtrastermaterial, NULL, (int)texture->width, (int)texture->height);
	}

	if (texture->rtlightmaterial != QR_NO_MATERIAL && !texmgr_live_reload)
	{
		SDL_LockMutex (rtspecial_mutex);
		QrResult r = qrDestroyMaterial (vulkan_globals.instance, texture->rtlightmaterial);
		SDL_UnlockMutex (rtspecial_mutex);
		QR_CHECK (r);

		texture->rtlightmaterial = QR_NO_MATERIAL;
	}

	SDL_UnlockMutex (texmgr_mutex);
}
