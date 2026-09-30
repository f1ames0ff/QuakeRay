/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "quakedef.h"
#include "rt_material.h"
#include "rt_lights.h"

static void      Mod_LoadSpriteModel (qmodel_t *mod, void *buffer);
static void      Mod_LoadBrushModel (qmodel_t *mod, const char *loadname, void *buffer);
static void      Mod_LoadAliasModel (qmodel_t *mod, void *buffer);
static void      Mod_LoadMD3Model (qmodel_t *mod, const void *buffer);
static qboolean  Mod_LoadMD5Model (qmodel_t *mod, const void *buffer);
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash);
static void      Mod_EnhancedModels_f (cvar_t *var);

cvar_t external_ents = {"external_ents", "1", CVAR_ARCHIVE};
cvar_t external_vis = {"external_vis", "1", CVAR_ARCHIVE};
cvar_t r_enhancedmodels = {"r_enhancedmodels", "1", CVAR_ARCHIVE};

static byte *mod_novis;
static int   mod_novis_capacity;

static byte *mod_decompressed;
static int   mod_decompressed_capacity;

#define MAX_MOD_KNOWN 2048 /*johnfitz -- was 512 */
qmodel_t mod_known[MAX_MOD_KNOWN];
int      mod_numknown;

texture_t *r_notexture_mip;  // johnfitz -- moved here from r_main.c
texture_t *r_notexture_mip2; // johnfitz -- used for non-lightmapped surfs with a missing texture

SDL_mutex *lightcache_mutex;

extern cvar_t rt_brush_metal;
extern cvar_t rt_brush_rough;
extern cvar_t rt_model_metal;
extern cvar_t rt_model_rough;
extern cvar_t rt_enable_pvs;

/*
===============
ReadShortUnaligned
===============
*/
static short ReadShortUnaligned (byte *ptr)
{
	short temp;
	memcpy (&temp, ptr, sizeof (short));
	return LittleShort (temp);
}

/*
===============
ReadLongUnaligned
===============
*/
static int ReadLongUnaligned (byte *ptr)
{
	int temp;
	memcpy (&temp, ptr, sizeof (int));
	return LittleLong (temp);
}

/*
===============
ReadFloatUnaligned
===============
*/
static float ReadFloatUnaligned (byte *ptr)
{
	float temp;
	memcpy (&temp, ptr, sizeof (float));
	return LittleFloat (temp);
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	Cvar_RegisterVariable (&external_vis);
	Cvar_RegisterVariable (&external_ents);

	Cvar_RegisterVariable (&r_enhancedmodels);
	Cvar_SetCallback (&r_enhancedmodels, Mod_EnhancedModels_f);

	// johnfitz -- create notexture miptex
	r_notexture_mip = (texture_t *)Mem_Alloc (sizeof (texture_t));
	strcpy (r_notexture_mip->name, "notexture");
	r_notexture_mip->height = r_notexture_mip->width = 32;

	r_notexture_mip2 = (texture_t *)Mem_Alloc (sizeof (texture_t));
	strcpy (r_notexture_mip2->name, "notexture2");
	r_notexture_mip2->height = r_notexture_mip2->width = 32;

	lightcache_mutex = SDL_CreateMutex ();
	// johnfitz
}

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *Mod_Extradata (qmodel_t *mod)
{
	Mod_LoadModel (mod, true);
	return mod->extradata;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (float *p, qmodel_t *model)
{
	mnode_t  *node;
	float     d;
	mplane_t *plane;

	if (!model || !model->nodes)
		Sys_Error ("Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p, plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL; // never reached
}

/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, qmodel_t *model)
{
	int   c;
	byte *out;
	byte *outend;
	int   row;

	row = (model->numleafs + 31) / 8;
	if (mod_decompressed == NULL || row > mod_decompressed_capacity)
	{
		mod_decompressed_capacity = row;
		mod_decompressed = (byte *)Mem_Realloc (mod_decompressed, mod_decompressed_capacity);
		if (!mod_decompressed)
			Sys_Error ("Mod_DecompressVis: realloc() failed on %d bytes", mod_decompressed_capacity);
	}
	out = mod_decompressed;
	outend = mod_decompressed + row;

	if (!in)
	{ // no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return mod_decompressed;
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}

		c = in[1];
		in += 2;
		if (c > row - (out - mod_decompressed))
			c = row -
			    (out -
			     mod_decompressed); // now that we're dynamically allocating pvs buffers, we have to be more careful to avoid heap overflows with buggy maps.
		while (c)
		{
			if (out == outend)
			{
				if (!model->viswarn)
				{
					model->viswarn = true;
					Con_Warning ("Mod_DecompressVis: output overrun on model \"%s\"\n", model->name);
				}
				return mod_decompressed;
			}
			*out++ = 0;
			c--;
		}
	} while (out - mod_decompressed < row);

	return mod_decompressed;
}

/*
===================
Mod_LeafPVS
===================
*/
byte *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model)
{
	if (leaf == model->leafs)
		return Mod_NoVisPVS (model);
	return Mod_DecompressVis (leaf->compressed_vis, model);
}

/*
===================
Mod_NoVisPVS
===================
*/
byte *Mod_NoVisPVS (qmodel_t *model)
{
	int pvsbytes;

	pvsbytes = (model->numleafs + 31) / 8;
	if (mod_novis == NULL || pvsbytes > mod_novis_capacity)
	{
		mod_novis_capacity = pvsbytes;
		mod_novis = (byte *)Mem_Realloc (mod_novis, mod_novis_capacity);
		if (!mod_novis)
			Sys_Error ("Mod_NoVisPVS: realloc() failed on %d bytes", mod_novis_capacity);
	}
	memset (mod_novis, 0xff, mod_novis_capacity);
	return mod_novis;
}

/*
===================
Mod_FreeSpriteMemory
===================
*/
static void Mod_FreeSpriteMemory (msprite_t *psprite)
{
	for (int i = 0; i < psprite->numframes; ++i)
	{
		if (psprite->frames[i].type == SPR_SINGLE)
		{
			SAFE_FREE (psprite->frames[i].frameptr);
		}
		else
		{
			mspritegroup_t *group = (mspritegroup_t *)psprite->frames[i].frameptr;
			for (int j = 0; j < group->numframes; ++j)
			{
				SAFE_FREE (group->frames[i]);
			}
			SAFE_FREE (psprite->frames[i].frameptr);
		}
	}
	psprite->numframes = 0;
}

/*
===================
Mod_FreeModelMemory
===================
*/
static void Mod_FreeModelMemory (qmodel_t *mod)
{
	if (mod->name[0] != '*')
	{
		if ((mod->type == mod_sprite) && (mod->extradata))
			Mod_FreeSpriteMemory ((msprite_t *)mod->extradata);
		// Last two ones are dummy textures
		for (int i = 0; i < mod->numtextures - 2; ++i)
			SAFE_FREE (mod->textures[i]);
		for (int i = 0; i < mod->numsurfaces; ++i)
			SAFE_FREE (mod->surfaces[i].polys);
		SAFE_FREE (mod->hulls[0].clipnodes);
		SAFE_FREE (mod->submodels);
		mod->numsubmodels = 0;
		SAFE_FREE (mod->planes);
		mod->numplanes = 0;
		SAFE_FREE (mod->leafs);
		mod->numleafs = 0;
		SAFE_FREE (mod->vertexes);
		mod->numvertexes = 0;
		SAFE_FREE (mod->edges);
		mod->numedges = 0;
		SAFE_FREE (mod->nodes);
		mod->numnodes = 0;
		SAFE_FREE (mod->texinfo);
		mod->numtexinfo = 0;
		SAFE_FREE (mod->surfaces);
		mod->numsurfaces = 0;
		SAFE_FREE (mod->surfedges);
		mod->numsurfedges = 0;
		SAFE_FREE (mod->clipnodes);
		mod->numclipnodes = 0;
		SAFE_FREE (mod->marksurfaces);
		mod->nummarksurfaces = 0;
		SAFE_FREE (mod->soa_leafbounds);
		SAFE_FREE (mod->surfvis);
		SAFE_FREE (mod->soa_surfplanes);
		SAFE_FREE (mod->textures);
		mod->numtextures = 0;
		SAFE_FREE (mod->visdata);
		mod->visdatasize = 0;
		mod->nowatervis = false;
		SAFE_FREE (mod->lightdata);
		SAFE_FREE (mod->entities);
		SAFE_FREE (mod->extradata);
	}
	if (!isDedicated)
		TexMgr_FreeTexturesForOwner (mod);
}

static void Mod_EnhancedModels_f (cvar_t *var)
{
	int       i;
	qmodel_t *mod;

	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (mod->type != mod_alias)
			continue;
		GLMesh_DeleteVertexBuffer (mod);
		Mod_FreeModelMemory (mod);
		mod->needload = true;
	}

	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
		if (mod->type == mod_alias)
			Mod_LoadModel (mod, false);

	InvalidateTraceLineCache ();
}

/*
===================
Mod_ClearAll
===================
*/
void Mod_ClearAll (void)
{
	int       i;
	qmodel_t *mod;

	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (mod->type != mod_alias)
		{
			mod->needload = true;
			Mod_FreeModelMemory (mod); // johnfitz
		}
	}

	InvalidateTraceLineCache ();
}

/*
===================
Mod_ResetAll
===================
*/
void Mod_ResetAll (void)
{
	int       i;
	qmodel_t *mod;

	// ericw -- free alias model VBOs
	GLMesh_DeleteVertexBuffers ();

	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (!mod->needload) // otherwise Mod_ClearAll() did it already
			Mod_FreeModelMemory (mod);

		/* The vertex-buffer walk above stops at the first hole in the client precache, and a
		   model loaded outside it (a teleport's, a beam's) would keep its DTAL cache: free it
		   here, before the memset drops the pointer. */
		RT_ModelLightsCacheFree (mod);

		memset (mod, 0, sizeof (qmodel_t));
	}
	mod_numknown = 0;

	InvalidateTraceLineCache ();
}

/*
==================
Mod_FindName

==================
*/
qmodel_t *Mod_FindName (const char *name)
{
	int       i;
	qmodel_t *mod;

	if (!name[0])
		Sys_Error ("Mod_FindName: NULL name"); // johnfitz -- was "Mod_ForName"

	//
	// search the currently loaded models
	//
	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
		if (!strcmp (mod->name, name))
			break;

	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
			Sys_Error ("mod_numknown == MAX_MOD_KNOWN");
		q_strlcpy (mod->name, name, MAX_QPATH);
		mod->needload = true;
		mod_numknown++;
		InvalidateTraceLineCache ();
	}

	return mod;
}

/*
==================
Mod_TouchModel

==================
*/
void Mod_TouchModel (const char *name)
{
	Mod_FindName (name);
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash)
{
	byte *buf;
	int   mod_type;

	if (!mod->needload)
	{
		return mod;
	}

	InvalidateTraceLineCache ();

	//
	// load the file
	//
	buf = COM_LoadFile (mod->name, &mod->path_id);
	if (!buf)
	{
		if (crash)
			Host_Error ("Mod_LoadModel: %s not found", mod->name); // johnfitz -- was "Mod_NumForName"
		return NULL;
	}

	//
	// allocate a new model
	//
	char loadname[256];
	COM_FileBase (mod->name, loadname, sizeof (loadname));

	//
	// fill it in
	//

	// call the apropriate loader
	mod->needload = false;

	mod_type = (buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));

	if (CVAR_TO_BOOL (r_enhancedmodels) && mod_type == IDPOLYHEADER)
	{
		char         md3_name[MAX_QPATH], md5_name[MAX_QPATH];
		unsigned int md3_path_id = 0, md5_path_id = 0;

		COM_StripExtension (mod->name, md3_name, sizeof (md3_name));
		COM_AddExtension (md3_name, ".md3", sizeof (md3_name));
		if (!COM_FileExists (md3_name, &md3_path_id) || md3_path_id < mod->path_id)
			md3_path_id = 0;

		COM_StripExtension (mod->name, md5_name, sizeof (md5_name));
		COM_AddExtension (md5_name, ".md5mesh", sizeof (md5_name));
		if (!COM_FileExists (md5_name, &md5_path_id) || md5_path_id < mod->path_id)
			md5_path_id = 0;

		if (md3_path_id && md5_path_id && md5_path_id > md3_path_id)
			md3_path_id = 0;
		else if (md3_path_id && md5_path_id)
			md5_path_id = 0;

		if (md3_path_id)
		{
			byte *md3_buf = COM_LoadFile (md3_name, &md3_path_id);
			if (md3_buf)
			{
				mod->path_id = md3_path_id;
				Mod_LoadMD3Model (mod, md3_buf);
				Mem_Free (md3_buf);
				Mem_Free (buf);
				return mod;
			}
		}
		else if (md5_path_id)
		{
			byte *md5_buf = COM_LoadFile (md5_name, &md5_path_id);
			if (md5_buf)
			{
				mod->path_id = md5_path_id;
				if (Mod_LoadMD5Model (mod, md5_buf))
				{
					Mem_Free (md5_buf);
					Mem_Free (buf);
					return mod;
				}
				Mem_Free (md5_buf);
			}
		}
	}

	switch (mod_type)
	{
	case IDPOLYHEADER:
		Mod_LoadAliasModel (mod, buf);
		break;

	case IDMD3HEADER:
		Mod_LoadMD3Model (mod, buf);
		break;

	case IDMD5HEADER:
		if (!Mod_LoadMD5Model (mod, buf))
			Sys_Error ("Mod_LoadModel: failed to load %s", mod->name);
		break;

	case IDSPRITEHEADER:
		Mod_LoadSpriteModel (mod, buf);
		break;

	case BSPVERSION:
	case BSP2VERSION_2PSB:
	case BSP2VERSION_BSP2:
	case 0x20343651:
		Mod_LoadBrushModel (mod, loadname, buf);
		break;

	default:
		Con_DWarning ("Mod_LoadModel: %s has unknown model format (0x%08X); skipping\n",
		              mod->name, (unsigned)mod_type);
		mod->needload = true;
		Mem_Free (buf);
		return NULL;
	}

	Mem_Free (buf);
	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
qmodel_t *Mod_ForName (const char *name, qboolean crash)
{
	qmodel_t *mod;

	mod = Mod_FindName (name);

	return Mod_LoadModel (mod, crash);
}

/*
===============================================================================

                    BRUSHMODEL LOADING

===============================================================================
*/

/*
=================
Mod_CheckAnimTextureArrayQ64

Quake64 bsp
Check if we have any missing textures in the array
=================
*/
qboolean Mod_CheckAnimTextureArrayQ64 (texture_t *anims[], int numTex)
{
	int i;

	for (i = 0; i < numTex; i++)
	{
		if (!anims[i])
			return false;
	}
	return true;
}

typedef struct load_texture_task_args_s
{
	qmodel_t *mod;
	byte     *mod_base;
	src_offset_t *fileoffsets; // per texture: the file offset its pixels are copied from, 0 when unknown
} load_texture_task_args_t;

/*
=================
Mod_LoadTextureTask
=================
*/
static void Mod_LoadTextureTask (int i, load_texture_task_args_t *args)
{
	qmodel_t  *mod = args->mod;
	texture_t *tx = mod->textures[i];
	if (!tx)
		return;

	byte        *pixels_p = (byte *)tx + sizeof (texture_t);
	char         texturename[64];
	src_offset_t offset;
	int          fwidth, fheight;
	char         filename[MAX_OSPATH], filename2[MAX_OSPATH], mapname[MAX_OSPATH];
	char         rtname[MAX_OSPATH];
	byte        *data = NULL;

	// The pixels are a copy inside tx (the BSP buffer itself is not kept), so a
	// reload reads either the file the copy came from or the copy itself. The
	// difference of the copy's address and the file buffer underflows (the copy
	// is not inside the file), which is what made the reload report an invalid
	// source for a map texture.
	offset = (args->fileoffsets && args->fileoffsets[i] > 0)
	         ? args->fileoffsets[i] : (src_offset_t)(uintptr_t)pixels_p;
	const char *src_file = (args->fileoffsets && args->fileoffsets[i] > 0) ? mod->name : "";

	if (!q_strncasecmp (tx->name, "sky", 3)) // sky texture //also note -- was strncmp, changed to match qbsp
	{
		if (mod->bspversion == BSPVERSION_QUAKE64)
			Sky_LoadTextureQ64 (mod, tx, i);
		else
			Sky_LoadTexture (mod, tx, i);
	}
	else if (tx->name[0] == '*') // warping texture
	{
		// external textures -- first look in "textures/mapname/" then look in "textures/"
		COM_StripExtension (mod->name + 5, mapname, sizeof (mapname));
		q_snprintf (filename, sizeof (filename), "textures/%s/#%s", mapname, tx->name + 1); // this also replaces the '*' with a '#'
		data = Image_LoadImage (filename, &fwidth, &fheight);
		if (!data)
		{
			q_snprintf (filename, sizeof (filename), "textures/#%s", tx->name + 1);
			data = Image_LoadImage (filename, &fwidth, &fheight);
		}

		q_snprintf (rtname, sizeof (rtname), "maps/#%s", tx->name + 1);

		// now load whatever we found
		if (data) // load external image
		{
			q_strlcpy (texturename, filename, sizeof (texturename));
			tx->gltexture = TexMgr_LoadImage (rtname, mod, texturename, fwidth, fheight, SRC_RGBA, data, filename, 0, TEXPREF_NONE);
		}
		else // use the texture from the bsp file
		{
			q_snprintf (texturename, sizeof (texturename), "%s:%s", mod->name, tx->name);
			tx->gltexture = TexMgr_LoadImage (rtname, mod, texturename, tx->width, tx->height, SRC_INDEXED, (byte *)(tx + 1), src_file, offset, TEXPREF_NONE);
		}

		// now create the warpimage, using dummy data from the hunk to create the initial image
		q_snprintf (texturename, sizeof (texturename), "%s_warp", texturename);
		q_snprintf (rtname, sizeof (rtname), "%s_warp", rtname);
		tx->warpimage = TexMgr_LoadImage (rtname, mod, texturename, WARPIMAGESIZE, WARPIMAGESIZE, SRC_RGBA, NULL, "", 0, TEXPREF_NOPICMIP | TEXPREF_WARPIMAGE);
		Atomic_StoreUInt32 (&tx->update_warp, true);
	}
	else // regular texture
	{
		// ericw -- fence textures
		int extraflags;

		extraflags = 0;
		if (tx->name[0] == '{')
			extraflags |= TEXPREF_ALPHA;
		// ericw

		// external textures -- first look in "textures/mapname/" then look in "textures/"
		COM_StripExtension (mod->name + 5, mapname, sizeof (mapname));
		q_snprintf (filename, sizeof (filename), "textures/%s/%s", mapname, tx->name);
		data = Image_LoadImage (filename, &fwidth, &fheight);
		if (!data)
		{
			q_snprintf (filename, sizeof (filename), "textures/%s", tx->name);
			data = Image_LoadImage (filename, &fwidth, &fheight);
		}

		q_snprintf (rtname, sizeof (rtname), "maps/%s", tx->name);

		TexMgr_RT_SpecialStart (CVAR_TO_FLOAT (rt_brush_rough), CVAR_TO_FLOAT (rt_brush_metal));

		// now load whatever we found
		if (data) // load external image
		{
			tx->gltexture = TexMgr_LoadImage (rtname, mod, filename, fwidth, fheight, SRC_RGBA, data, filename, 0, TEXPREF_MIPMAP | extraflags);
			Mem_Free (data);

			// now try to load glow/luma image from the same place
			q_snprintf (filename2, sizeof (filename2), "%s_glow", filename);
			data = Image_LoadImage (filename2, &fwidth, &fheight);
			if (!data)
			{
				q_snprintf (filename2, sizeof (filename2), "%s_luma", filename);
				data = Image_LoadImage (filename2, &fwidth, &fheight);
			}

			if (data)
			{
				// the glow/luma file is the source, so that vid_restart can read it back
				tx->fullbright = TexMgr_LoadImage (
					NULL,
					mod, filename2, fwidth, fheight, SRC_RGBA, data, filename2, 0, 
					TEXPREF_RT_IS_EMISSIVE | TEXPREF_MIPMAP | extraflags);
			}
		}
		else // use the texture from the bsp file
		{
			q_snprintf (texturename, sizeof (texturename), "%s:%s", mod->name, tx->name);
			tx->gltexture = TexMgr_LoadImage (
				rtname,
				mod, texturename, tx->width, tx->height, SRC_INDEXED, (byte *)(tx + 1), src_file, offset, TEXPREF_MIPMAP | extraflags);
		}

		TexMgr_RT_SpecialEnd ();
	}
	Mem_Free (data);
}

/*
=================
Mod_LoadTextures
=================
*/
static void Mod_LoadTextures (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int        i, j, pixels, num, maxanim, altmax;
	miptex_t   mt;
	texture_t *tx, *tx2;
	texture_t *anims[10];
	texture_t *altanims[10];
	byte      *m;
	byte      *pixels_p;
	int        nummiptex;
	int        dataofs;

	// johnfitz -- don't return early if no textures; still need to create dummy texture
	if (!l->filelen)
	{
		Con_Printf ("Mod_LoadTextures: no textures in bsp file\n");
		nummiptex = 0;
		m = NULL; // avoid bogus compiler warning
	}
	else
	{
		m = mod_base + l->fileofs;
		nummiptex = ReadLongUnaligned (m + offsetof (dmiptexlump_t, nummiptex));
	}
	// johnfitz

	mod->numtextures = nummiptex + 2; // johnfitz -- need 2 dummy texture chains for missing textures
	mod->textures = (texture_t **)Mem_Alloc (mod->numtextures * sizeof (*mod->textures));

	// The file offset each texture's pixels are copied from, so that a reload can
	// read them back from the bsp file (0 for a texture whose offset is unknown).
	src_offset_t *fileoffsets = (src_offset_t *)Mem_Alloc ((nummiptex > 0 ? nummiptex : 1) * sizeof (*fileoffsets));
	memset (fileoffsets, 0, (nummiptex > 0 ? nummiptex : 1) * sizeof (*fileoffsets));

	for (i = 0; i < nummiptex; i++)
	{
		dataofs = ReadLongUnaligned (m + offsetof (dmiptexlump_t, dataofs[i]));
		if (dataofs == -1)
			continue;
		memcpy (&mt, m + dataofs, sizeof (miptex_t));
		mt.width = LittleLong (mt.width);
		mt.height = LittleLong (mt.height);
		for (j = 0; j < MIPLEVELS; j++)
			mt.offsets[j] = LittleLong (mt.offsets[j]);

		pixels = mt.width * mt.height / 64 * 85;
		tx = (texture_t *)Mem_Alloc (sizeof (texture_t) + pixels);
		mod->textures[i] = tx;

		memcpy (tx->name, mt.name, sizeof (tx->name));
		tx->width = mt.width;
		tx->height = mt.height;
		for (j = 0; j < MIPLEVELS; j++)
			tx->offsets[j] = mt.offsets[j] + sizeof (texture_t) - sizeof (miptex_t);
		// the pixels immediately follow the structures

		// ericw -- check for pixels extending past the end of the lump.
		// appears in the wild; e.g. jam2_tronyn.bsp (func_mapjam2),
		// kellbase1.bsp (quoth), and can lead to a segfault if we read past
		// the end of the .bsp file buffer
		pixels_p = m + dataofs + sizeof (miptex_t);
		if ((pixels_p + pixels) > (mod_base + l->fileofs + l->filelen))
		{
			Con_DPrintf ("Texture %s extends past end of lump\n", mt.name);
			pixels = q_max (0, (mod_base + l->fileofs + l->filelen) - pixels_p);
		}

		Atomic_StoreUInt32 (&tx->update_warp, false); // johnfitz
		tx->warpimage = NULL;                         // johnfitz
		tx->shift = 0;                                // Q64 only

		if (mod->bspversion != BSPVERSION_QUAKE64)
		{
			memcpy (tx + 1, pixels_p, pixels);
		}
		else
		{ // Q64 bsp
			tx->shift = ReadLongUnaligned (m + dataofs + offsetof (miptex64_t, shift));
			memcpy (tx + 1, m + dataofs + sizeof (miptex64_t), pixels);
		}

		// where in the file those pixels came from, for a reload to read again
		fileoffsets[i] = (src_offset_t)((mod->bspversion != BSPVERSION_QUAKE64
		                                 ? m + dataofs + sizeof (miptex_t)
		                                 : m + dataofs + sizeof (miptex64_t)) - mod_base);
	}

	if (!isDedicated)
	{
		load_texture_task_args_t args = {
			.mod = mod,
			.mod_base = mod_base,
			.fileoffsets = fileoffsets,
		};
		if (!Tasks_IsWorker () && (nummiptex > 1))
		{
			task_handle_t task = Task_AllocateAssignIndexedFuncAndSubmit ((task_indexed_func_t)Mod_LoadTextureTask, nummiptex, &args, sizeof (args));
			Task_Join (task, SDL_MUTEX_MAXWAIT);
		}
		else
		{
			for (i = 0; i < nummiptex; i++)
				Mod_LoadTextureTask (i, &args);
		}
	}

	Mem_Free (fileoffsets);

	// johnfitz -- last 2 slots in array should be filled with dummy textures
	mod->textures[mod->numtextures - 2] = r_notexture_mip;  // for lightmapped surfs
	mod->textures[mod->numtextures - 1] = r_notexture_mip2; // for SURF_DRAWTILED surfs

	//
	// sequence the animations
	//
	for (i = 0; i < nummiptex; i++)
	{
		tx = mod->textures[i];
		if (!tx || tx->name[0] != '+')
			continue;
		if (tx->anim_next)
			continue; // allready sequenced

		// find the number of frames in the animation
		memset (anims, 0, sizeof (anims));
		memset (altanims, 0, sizeof (altanims));

		maxanim = tx->name[1];
		altmax = 0;
		if (maxanim >= 'a' && maxanim <= 'z')
			maxanim -= 'a' - 'A';
		if (maxanim >= '0' && maxanim <= '9')
		{
			maxanim -= '0';
			altmax = 0;
			anims[maxanim] = tx;
			maxanim++;
		}
		else if (maxanim >= 'A' && maxanim <= 'J')
		{
			altmax = maxanim - 'A';
			maxanim = 0;
			altanims[altmax] = tx;
			altmax++;
		}
		else
			Sys_Error ("Bad animating texture %s", tx->name);

		for (j = i + 1; j < nummiptex; j++)
		{
			tx2 = mod->textures[j];
			if (!tx2 || tx2->name[0] != '+')
				continue;
			if (strcmp (tx2->name + 2, tx->name + 2))
				continue;

			num = tx2->name[1];
			if (num >= 'a' && num <= 'z')
				num -= 'a' - 'A';
			if (num >= '0' && num <= '9')
			{
				num -= '0';
				anims[num] = tx2;
				if (num + 1 > maxanim)
					maxanim = num + 1;
			}
			else if (num >= 'A' && num <= 'J')
			{
				num = num - 'A';
				altanims[num] = tx2;
				if (num + 1 > altmax)
					altmax = num + 1;
			}
			else
				Sys_Error ("Bad animating texture %s", tx->name);
		}

		if (mod->bspversion == BSPVERSION_QUAKE64 && !Mod_CheckAnimTextureArrayQ64 (anims, maxanim))
			continue; // Just pretend this is a normal texture

#define ANIM_CYCLE 2
		// link them all together
		for (j = 0; j < maxanim; j++)
		{
			tx2 = anims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s", j, tx->name);
			tx2->anim_total = maxanim * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j + 1) * ANIM_CYCLE;
			tx2->anim_next = anims[(j + 1) % maxanim];
			if (altmax)
				tx2->alternate_anims = altanims[0];
		}
		for (j = 0; j < altmax; j++)
		{
			tx2 = altanims[j];
			if (!tx2)
				Sys_Error ("Missing frame %i of %s", j, tx->name);
			tx2->anim_total = altmax * ANIM_CYCLE;
			tx2->anim_min = j * ANIM_CYCLE;
			tx2->anim_max = (j + 1) * ANIM_CYCLE;
			tx2->anim_next = altanims[(j + 1) % altmax];
			if (maxanim)
				tx2->alternate_anims = anims[0];
		}
	}
}

/*
=================
Mod_LoadLighting -- johnfitz -- replaced with lit support code via lordhavoc
=================
*/
static void Mod_LoadLighting (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int          i;
	byte        *in, *out, *data;
	byte         d, q64_b0, q64_b1;
	char         litfilename[MAX_OSPATH];
	unsigned int path_id;

	mod->lightdata = NULL;
	// LordHavoc: check for a .lit file
	q_strlcpy (litfilename, mod->name, sizeof (litfilename));
	COM_StripExtension (litfilename, litfilename, sizeof (litfilename));
	q_strlcat (litfilename, ".lit", sizeof (litfilename));
	data = (byte *)COM_LoadFile (litfilename, &path_id);
	if (data)
	{
		// use lit file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < mod->path_id)
		{
			Con_DPrintf ("ignored %s from a gamedir with lower priority\n", litfilename);
		}
		else if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
		{
			i = ReadLongUnaligned (data + sizeof (int));
			if (i == 1)
			{
				if (8 + l->filelen * 3 == com_filesize)
				{
					Con_DPrintf2 ("%s loaded\n", litfilename);
					mod->lightdata = (byte *)Mem_Alloc (l->filelen * 3);
					memcpy (mod->lightdata, data + 8, (l->filelen * 3) - 8);
					Mem_Free (data);
					return;
				}
				Con_Printf ("Outdated .lit file (%s should be %u bytes, not %u)\n", litfilename, 8 + l->filelen * 3, com_filesize);
			}
			else
			{
				Con_Printf ("Unknown .lit file version (%d)\n", i);
			}
		}
		else
		{
			Con_Printf ("Corrupt .lit file (old version?), ignoring\n");
		}

		Mem_Free (data);
	}
	// LordHavoc: no .lit found, expand the white lighting data to color
	if (!l->filelen)
		return;

	// Quake64 bsp lighmap data
	if (mod->bspversion == BSPVERSION_QUAKE64)
	{
		// RGB lightmap samples are packed in 16bits.
		// RRRRR GGGGG BBBBBB

		mod->lightdata = (byte *)Mem_Alloc ((l->filelen / 2) * 3);
		in = mod_base + l->fileofs;
		out = mod->lightdata;

		for (i = 0; i < (l->filelen / 2); i++)
		{
			q64_b0 = *in++;
			q64_b1 = *in++;

			*out++ = q64_b0 & 0xf8;                                   /* 0b11111000 */
			*out++ = ((q64_b0 & 0x07) << 5) + ((q64_b1 & 0xc0) >> 5); /* 0b00000111, 0b11000000 */
			*out++ = (q64_b1 & 0x3f) << 2;                            /* 0b00111111 */
		}
		return;
	}

	mod->lightdata = (byte *)Mem_Alloc (l->filelen * 3);
	in = mod->lightdata + l->filelen * 2; // place the file at the end, so it will not be overwritten until the very last write
	out = mod->lightdata;
	memcpy (in, mod_base + l->fileofs, l->filelen);
	for (i = 0; i < l->filelen; i++)
	{
		d = *in++;
		*out++ = d;
		*out++ = d;
		*out++ = d;
	}
}

/*
=================
Mod_LoadVisibility
=================
*/
static void Mod_LoadVisibility (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	mod->viswarn = false;
	if (!l->filelen)
	{
		mod->visdata = NULL;
		mod->visdatasize = 0;
		return;
	}
	mod->visdata = (byte *)Mem_Alloc (l->filelen);
	mod->visdatasize = l->filelen;
	memcpy (mod->visdata, mod_base + l->fileofs, l->filelen);
}

/*
=================
Mod_LoadEntities
=================
*/
static void Mod_LoadEntities (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	char         basemapname[MAX_QPATH];
	char         entfilename[MAX_QPATH];
	char        *ents = NULL;
	unsigned int path_id;
	unsigned int crc = 0;

	if (!external_ents.value)
		goto _load_embedded;

	if (l->filelen > 0)
	{
		crc = CRC_Block (mod_base + l->fileofs, l->filelen - 1);
	}

	q_strlcpy (basemapname, mod->name, sizeof (basemapname));
	COM_StripExtension (basemapname, basemapname, sizeof (basemapname));

	q_snprintf (entfilename, sizeof (entfilename), "%s@%04x.ent", basemapname, crc);
	Con_DPrintf2 ("trying to load %s\n", entfilename);
	ents = (char *)COM_LoadFile (entfilename, &path_id);

	if (!ents)
	{
		q_snprintf (entfilename, sizeof (entfilename), "%s.ent", basemapname);
		Con_DPrintf2 ("trying to load %s\n", entfilename);
		ents = (char *)COM_LoadFile (entfilename, &path_id);
	}

	if (ents)
	{
		// use ent file only from the same gamedir as the map
		// itself or from a searchpath with higher priority.
		if (path_id < mod->path_id)
		{
			Con_DPrintf ("ignored %s from a gamedir with lower priority\n", entfilename);
		}
		else
		{
			mod->entities = ents;
			Con_DPrintf ("Loaded external entity file %s\n", entfilename);
			return;
		}
	}

_load_embedded:
	if (!l->filelen)
	{
		Mem_Free (mod->entities);
		mod->entities = NULL;
		return;
	}
	mod->entities = (char *)Mem_Alloc (l->filelen);
	memcpy (mod->entities, mod_base + l->fileofs, l->filelen);
	Mem_Free (ents);
}

/*
=================
Mod_LoadVertexes
=================
*/
static void Mod_LoadVertexes (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	byte      *in;
	mvertex_t *out;
	int        i, count;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dvertex_t))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (dvertex_t);
	out = (mvertex_t *)Mem_Alloc (count * sizeof (*out));

	mod->vertexes = out;
	mod->numvertexes = count;

	for (i = 0; i < count; i++, in += sizeof (dvertex_t), out++)
	{
		out->position[0] = ReadFloatUnaligned (in + offsetof (dvertex_t, point[0]));
		out->position[1] = ReadFloatUnaligned (in + offsetof (dvertex_t, point[1]));
		out->position[2] = ReadFloatUnaligned (in + offsetof (dvertex_t, point[2]));
	}
}

/*
=================
Mod_LoadEdges
=================
*/
static void Mod_LoadEdges (qmodel_t *mod, byte *mod_base, lump_t *l, int bsp2)
{
	medge_t *out;
	int      i, count;

	if (bsp2)
	{
		byte *in = mod_base + l->fileofs;

		if (l->filelen % sizeof (dledge_t))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (dledge_t);
		out = (medge_t *)Mem_Alloc ((count + 1) * sizeof (*out));

		mod->edges = out;
		mod->numedges = count;

		for (i = 0; i < count; i++, in += sizeof (dledge_t), out++)
		{
			out->v[0] = ReadLongUnaligned (in + offsetof (dledge_t, v[0]));
			out->v[1] = ReadLongUnaligned (in + offsetof (dledge_t, v[1]));
		}
	}
	else
	{
		byte *in = mod_base + l->fileofs;

		if (l->filelen % sizeof (dsedge_t))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (dsedge_t);
		out = (medge_t *)Mem_Alloc ((count + 1) * sizeof (*out));

		mod->edges = out;
		mod->numedges = count;

		for (i = 0; i < count; i++, in += sizeof (dsedge_t), out++)
		{
			out->v[0] = (unsigned short)ReadShortUnaligned (in + offsetof (dsedge_t, v[0]));
			out->v[1] = (unsigned short)ReadShortUnaligned (in + offsetof (dsedge_t, v[1]));
		}
	}
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void Mod_LoadTexinfo (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	byte       *in;
	mtexinfo_t *out;
	int         i, j, count, miptex;
	int         missing = 0; // johnfitz

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (texinfo_t))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (texinfo_t);
	out = (mtexinfo_t *)Mem_Alloc (count * sizeof (*out));

	mod->texinfo = out;
	mod->numtexinfo = count;

	for (i = 0; i < count; i++, in += sizeof (texinfo_t), out++)
	{
		for (j = 0; j < 4; j++)
		{
			out->vecs[0][j] = ReadFloatUnaligned (in + offsetof (texinfo_t, vecs[0][j]));
			out->vecs[1][j] = ReadFloatUnaligned (in + offsetof (texinfo_t, vecs[1][j]));
		}

		miptex = ReadLongUnaligned (in + offsetof (texinfo_t, miptex));
		out->flags = ReadLongUnaligned (in + offsetof (texinfo_t, flags));

		// johnfitz -- rewrote this section
		if (miptex >= mod->numtextures - 1 || !mod->textures[miptex])
		{
			if (out->flags & TEX_SPECIAL)
				out->texture = mod->textures[mod->numtextures - 1];
			else
				out->texture = mod->textures[mod->numtextures - 2];
			out->flags |= TEX_MISSING;
			missing++;
		}
		else
		{
			out->texture = mod->textures[miptex];
		}
		// johnfitz
	}

	// johnfitz: report missing textures
	if (missing && mod->numtextures > 1)
		Con_Printf ("Mod_LoadTexinfo: %d texture(s) missing from BSP file\n", missing);
	// johnfitz
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void CalcSurfaceExtents (qmodel_t *mod, msurface_t *s)
{
	float       mins[2], maxs[2], val;
	int         i, j, e;
	mvertex_t  *v;
	mtexinfo_t *tex;
	int         bmins[2], bmaxs[2];

	mins[0] = mins[1] = FLT_MAX;
	maxs[0] = maxs[1] = -FLT_MAX;

	tex = s->texinfo;

	for (i = 0; i < s->numedges; i++)
	{
		e = mod->surfedges[s->firstedge + i];
		if (e >= 0)
			v = &mod->vertexes[mod->edges[e].v[0]];
		else
			v = &mod->vertexes[mod->edges[-e].v[1]];

		for (j = 0; j < 2; j++)
		{
			/* The following calculation is sensitive to floating-point
			 * precision.  It needs to produce the same result that the
			 * light compiler does, because R_BuildLightMap uses surf->
			 * extents to know the width/height of a surface's lightmap,
			 * and incorrect rounding here manifests itself as patches
			 * of "corrupted" looking lightmaps.
			 * Most light compilers are win32 executables, so they use
			 * x87 floating point.  This means the multiplies and adds
			 * are done at 80-bit precision, and the result is rounded
			 * down to 32-bits and stored in val.
			 * Adding the casts to double seems to be good enough to fix
			 * lighting glitches when Quakespasm is compiled as x86_64
			 * and using SSE2 floating-point.  A potential trouble spot
			 * is the hallway at the beginning of mfxsp17.  -- ericw
			 */
			val = ((double)v->position[0] * (double)tex->vecs[j][0]) + ((double)v->position[1] * (double)tex->vecs[j][1]) +
			      ((double)v->position[2] * (double)tex->vecs[j][2]) + (double)tex->vecs[j][3];

			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i = 0; i < 2; i++)
	{
		bmins[i] = floor (mins[i] / 16);
		bmaxs[i] = ceil (maxs[i] / 16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

		if (!(tex->flags & TEX_SPECIAL) && s->extents[i] > 2000) // johnfitz -- was 512 in glquake, 256 in winquake
			Sys_Error ("Bad surface extents");
	}
}

/*
================
Mod_PolyForUnlitSurface -- johnfitz -- creates polys for unlightmapped surfaces (sky and water)

TODO: merge this into BuildSurfaceDisplayList?
================
*/
static void Mod_PolyForUnlitSurface (qmodel_t *mod, msurface_t *fa)
{
	vec3_t    verts[64];
	int       numverts, i, lindex;
	float    *vec;
	glpoly_t *poly;
	float    *poly_vert;
	float     texscale;

	if (fa->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
		texscale = (1.0 / 128.0); // warp animation repeats every 128
	else
		texscale = (1.0 / 32.0); // to match r_notexture_mip

	// convert edges back to a normal polygon
	numverts = 0;
	for (i = 0; i < fa->numedges; i++)
	{
		lindex = mod->surfedges[fa->firstedge + i];

		if (lindex > 0)
			vec = mod->vertexes[mod->edges[lindex].v[0]].position;
		else
			vec = mod->vertexes[mod->edges[-lindex].v[1]].position;
		VectorCopy (vec, verts[numverts]);
		numverts++;
	}

	// create the poly
	poly = (glpoly_t *)Mem_Alloc (sizeof (glpoly_t) + (numverts - 4) * VERTEXSIZE * sizeof (float));
	poly->next = NULL;
	fa->polys = poly;
	poly->numverts = numverts;
	for (i = 0, vec = (float *)verts; i < numverts; i++, vec += 3)
	{
		poly_vert = &poly->verts[0][0] + (i * VERTEXSIZE);
		VectorCopy (vec, poly_vert);
		poly_vert[3] = DotProduct (vec, fa->texinfo->vecs[0]) * texscale;
		poly_vert[4] = DotProduct (vec, fa->texinfo->vecs[1]) * texscale;
	}
}

/*
=================
Mod_LoadFaces
=================
*/
static void Mod_LoadFaces (qmodel_t *mod, byte *mod_base, lump_t *l, qboolean bsp2)
{
	byte       *ins;
	byte       *inl;
	msurface_t *out;
	int         i, count, surfnum, lofs;
	int         planenum, side, texinfon;

	if (bsp2)
	{
		ins = NULL;
		inl = mod_base + l->fileofs;
		if (l->filelen % sizeof (dlface_t))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
		count = l->filelen / sizeof (dlface_t);
	}
	else
	{
		ins = mod_base + l->fileofs;
		inl = NULL;
		if (l->filelen % sizeof (dsface_t))
			Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
		count = l->filelen / sizeof (dsface_t);
	}
	out = (msurface_t *)Mem_Alloc (count * sizeof (*out));

	// johnfitz -- warn mappers about exceeding old limits
	if (count > 32767 && !bsp2)
		Con_DWarning ("%i faces exceeds standard limit of 32767.\n", count);
	// johnfitz

	mod->surfaces = out;
	mod->numsurfaces = count;

	for (surfnum = 0; surfnum < count; surfnum++, out++)
	{
		if (bsp2)
		{
			out->firstedge = ReadLongUnaligned (inl + offsetof (dlface_t, firstedge));
			out->numedges = ReadLongUnaligned (inl + offsetof (dlface_t, numedges));
			planenum = ReadLongUnaligned (inl + offsetof (dlface_t, planenum));
			side = ReadLongUnaligned (inl + offsetof (dlface_t, side));
			texinfon = ReadLongUnaligned (inl + offsetof (dlface_t, texinfo));
			for (i = 0; i < MAXLIGHTMAPS; i++)
				out->styles[i] = *(inl + offsetof (dlface_t, styles[i]));
			lofs = ReadLongUnaligned (inl + offsetof (dlface_t, lightofs));
			inl += sizeof (dlface_t);
		}
		else
		{
			out->firstedge = ReadLongUnaligned (ins + offsetof (dsface_t, firstedge));
			out->numedges = ReadShortUnaligned (ins + offsetof (dsface_t, numedges));
			planenum = ReadShortUnaligned (ins + offsetof (dsface_t, planenum));
			side = ReadShortUnaligned (ins + offsetof (dsface_t, side));
			texinfon = ReadShortUnaligned (ins + offsetof (dsface_t, texinfo));
			for (i = 0; i < MAXLIGHTMAPS; i++)
				out->styles[i] = *(ins + offsetof (dsface_t, styles[i]));
			lofs = ReadLongUnaligned (ins + offsetof (dsface_t, lightofs));
			ins += sizeof (dsface_t);
		}

		out->flags = 0;

		if (side)
			out->flags |= SURF_PLANEBACK;

		out->plane = mod->planes + planenum;

		out->texinfo = mod->texinfo + texinfon;

		CalcSurfaceExtents (mod, out);

		// lighting info
		if (mod->bspversion == BSPVERSION_QUAKE64)
			lofs /= 2; // Q64 samples are 16bits instead 8 in normal Quake

		if (lofs == -1)
			out->samples = NULL;
		else
			out->samples = mod->lightdata + (lofs * 3); // johnfitz -- lit support via lordhavoc (was "+ i")

		// johnfitz -- this section rewritten
		if (!q_strncasecmp (out->texinfo->texture->name, "sky", 3)) // sky surface //also note -- was strncmp, changed to match qbsp
		{
			out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
			Mod_PolyForUnlitSurface (mod, out); // no more subdivision
		}
		else if (out->texinfo->texture->name[0] == '*') // warp surface
		{
			out->flags |= SURF_DRAWTURB;

			if (out->texinfo->flags & TEX_SPECIAL)
				out->flags |= SURF_DRAWTILED; // unlit water
			out->lightmaptexturenum = -1;

			// detect special liquid types
			if (!strncmp (out->texinfo->texture->name, "*lava", 5))
				out->flags |= SURF_DRAWLAVA;
			else if (!strncmp (out->texinfo->texture->name, "*slime", 6))
				out->flags |= SURF_DRAWSLIME;
			else if (!strncmp (out->texinfo->texture->name, "*tele", 5))
				out->flags |= SURF_DRAWTELE;
			else
				out->flags |= SURF_DRAWWATER;

			Mod_PolyForUnlitSurface (mod, out);
		}
		else if (out->texinfo->texture->name[0] == '{') // ericw -- fence textures
		{
			out->flags |= SURF_DRAWFENCE;
		}
		else if (out->texinfo->flags & TEX_MISSING) // texture is missing from bsp
		{
			if (out->samples) // lightmapped
			{
				out->flags |= SURF_NOTEXTURE;
			}
			else // not lightmapped
			{
				out->flags |= (SURF_NOTEXTURE | SURF_DRAWTILED);
				Mod_PolyForUnlitSurface (mod, out);
			}
		}
		// johnfitz
	}
}

/*
=================
Mod_LoadNodes
=================
*/
static void Mod_LoadNodes_S (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int      i, j, count, p;
	byte    *in;
	mnode_t *out;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dsnode_t))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (dsnode_t);
	out = (mnode_t *)Mem_Alloc (count * sizeof (*out));

	// johnfitz -- warn mappers about exceeding old limits
	if (count > 32767)
		Con_DWarning ("%i nodes exceeds standard limit of 32767.\n", count);
	// johnfitz

	mod->nodes = out;
	mod->numnodes = count;

	for (i = 0; i < count; i++, in += sizeof (dsnode_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadShortUnaligned (in + offsetof (dsnode_t, mins[j]));
			out->minmaxs[3 + j] = ReadShortUnaligned (in + offsetof (dsnode_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dsnode_t, planenum));
		out->plane = mod->planes + p;

		out->firstsurface = (unsigned short)ReadShortUnaligned (in + offsetof (dsnode_t, firstface)); // johnfitz -- explicit cast as unsigned short
		out->numsurfaces = (unsigned short)ReadShortUnaligned (in + offsetof (dsnode_t, numfaces));   // johnfitz -- explicit cast as unsigned short

		for (j = 0; j < 2; j++)
		{
			// johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = (unsigned short)ReadShortUnaligned (in + offsetof (dsnode_t, children[j]));
			if (p < count)
				out->children[j] = mod->nodes + p;
			else
			{
				p = 65535 - p; // note this uses 65535 intentionally, -1 is leaf 0
				if (p < mod->numleafs)
					out->children[j] = (mnode_t *)(mod->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, mod->numleafs);
					out->children[j] = (mnode_t *)(mod->leafs); // map it to the solid leaf
				}
			}
			// johnfitz
		}
	}
}

static void Mod_LoadNodes_L1 (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int      i, j, count, p;
	byte    *in;
	mnode_t *out;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dl1node_t))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s", mod->name);

	count = l->filelen / sizeof (dl1node_t);
	out = (mnode_t *)Mem_Alloc (count * sizeof (*out));

	mod->nodes = out;
	mod->numnodes = count;

	for (i = 0; i < count; i++, in += sizeof (dl1node_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadShortUnaligned (in + offsetof (dl1node_t, mins[j]));
			out->minmaxs[3 + j] = ReadShortUnaligned (in + offsetof (dl1node_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dl1node_t, planenum));
		out->plane = mod->planes + p;

		out->firstsurface = ReadLongUnaligned (in + offsetof (dl1node_t, firstface)); // johnfitz -- explicit cast as unsigned short
		out->numsurfaces = ReadLongUnaligned (in + offsetof (dl1node_t, numfaces));   // johnfitz -- explicit cast as unsigned short

		for (j = 0; j < 2; j++)
		{
			// johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = ReadLongUnaligned (in + offsetof (dl1node_t, children[j]));
			if (p >= 0 && p < count)
				out->children[j] = mod->nodes + p;
			else
			{
				p = 0xffffffff - p; // note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < mod->numleafs)
					out->children[j] = (mnode_t *)(mod->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, mod->numleafs);
					out->children[j] = (mnode_t *)(mod->leafs); // map it to the solid leaf
				}
			}
			// johnfitz
		}
	}
}

static void Mod_LoadNodes_L2 (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int      i, j, count, p;
	byte    *in;
	mnode_t *out;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dl2node_t))
		Sys_Error ("Mod_LoadNodes: funny lump size in %s", mod->name);

	count = l->filelen / sizeof (dl2node_t);
	out = (mnode_t *)Mem_Alloc (count * sizeof (*out));

	mod->nodes = out;
	mod->numnodes = count;

	for (i = 0; i < count; i++, in += sizeof (dl2node_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadFloatUnaligned (in + offsetof (dl2node_t, mins[j]));
			out->minmaxs[3 + j] = ReadFloatUnaligned (in + offsetof (dl2node_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dl2node_t, planenum));
		out->plane = mod->planes + p;

		out->firstsurface = ReadLongUnaligned (in + offsetof (dl2node_t, firstface)); // johnfitz -- explicit cast as unsigned short
		out->numsurfaces = ReadLongUnaligned (in + offsetof (dl2node_t, numfaces));   // johnfitz -- explicit cast as unsigned short

		for (j = 0; j < 2; j++)
		{
			// johnfitz -- hack to handle nodes > 32k, adapted from darkplaces
			p = ReadLongUnaligned (in + offsetof (dl2node_t, children[j]));
			if (p > 0 && p < count)
				out->children[j] = mod->nodes + p;
			else
			{
				p = 0xffffffff - p; // note this uses 65535 intentionally, -1 is leaf 0
				if (p >= 0 && p < mod->numleafs)
					out->children[j] = (mnode_t *)(mod->leafs + p);
				else
				{
					Con_Printf ("Mod_LoadNodes: invalid leaf index %i (file has only %i leafs)\n", p, mod->numleafs);
					out->children[j] = (mnode_t *)(mod->leafs); // map it to the solid leaf
				}
			}
			// johnfitz
		}
	}
}

static void Mod_LoadNodes (qmodel_t *mod, byte *mod_base, lump_t *l, int bsp2)
{
	if (bsp2 == 2)
		Mod_LoadNodes_L2 (mod, mod_base, l);
	else if (bsp2)
		Mod_LoadNodes_L1 (mod, mod_base, l);
	else
		Mod_LoadNodes_S (mod, mod_base, l);
}

static void Mod_ProcessLeafs_S (qmodel_t *mod, byte *in, int filelen)
{
	mleaf_t *out;
	int      i, j, count, p;

	if (filelen % sizeof (dsleaf_t))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", mod->name);
	count = filelen / sizeof (dsleaf_t);
	out = (mleaf_t *)Mem_Alloc (count * sizeof (*out));

	// johnfitz
	if (count > 32767)
		Host_Error ("Mod_LoadLeafs: %i leafs exceeds limit of 32767.", count);
	// johnfitz

	mod->leafs = out;
	mod->numleafs = count;

	for (i = 0; i < count; i++, in += sizeof (dsleaf_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadShortUnaligned (in + offsetof (dsleaf_t, mins[j]));
			out->minmaxs[3 + j] = ReadShortUnaligned (in + offsetof (dsleaf_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dsleaf_t, contents));
		out->contents = p;

		out->firstmarksurface =
			mod->marksurfaces + (unsigned short)ReadShortUnaligned (in + offsetof (dsleaf_t, firstmarksurface)); // johnfitz -- unsigned short
		out->nummarksurfaces = (unsigned short)ReadShortUnaligned (in + offsetof (dsleaf_t, nummarksurfaces));   // johnfitz -- unsigned short

		p = ReadLongUnaligned (in + offsetof (dsleaf_t, visofs));
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = (mod->visdata != NULL) ? (mod->visdata + p) : NULL;
		out->efrags = NULL;

		for (j = 0; j < 4; j++)
			out->ambient_sound_level[j] = *(in + offsetof (dsleaf_t, ambient_level[j]));

		// johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L1 (qmodel_t *mod, byte *in, int filelen)
{
	mleaf_t *out;
	int      i, j, count, p;

	if (filelen % sizeof (dl1leaf_t))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", mod->name);

	count = filelen / sizeof (dl1leaf_t);

	out = (mleaf_t *)Mem_Alloc (count * sizeof (*out));

	mod->leafs = out;
	mod->numleafs = count;

	for (i = 0; i < count; i++, in += sizeof (dl1leaf_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadShortUnaligned (in + offsetof (dl1leaf_t, mins[j]));
			out->minmaxs[3 + j] = ReadShortUnaligned (in + offsetof (dl1leaf_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dl1leaf_t, contents));
		out->contents = p;

		out->firstmarksurface = mod->marksurfaces + ReadLongUnaligned (in + offsetof (dl1leaf_t, firstmarksurface)); // johnfitz -- unsigned short
		out->nummarksurfaces = ReadLongUnaligned (in + offsetof (dl1leaf_t, nummarksurfaces));                       // johnfitz -- unsigned short

		p = ReadLongUnaligned (in + offsetof (dl1leaf_t, visofs));
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = mod->visdata + p;
		out->efrags = NULL;

		for (j = 0; j < 4; j++)
			out->ambient_sound_level[j] = *(in + offsetof (dl1leaf_t, ambient_level[j]));

		// johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

static void Mod_ProcessLeafs_L2 (qmodel_t *mod, byte *in, int filelen)
{
	mleaf_t *out;
	int      i, j, count, p;

	if (filelen % sizeof (dl2leaf_t))
		Sys_Error ("Mod_ProcessLeafs: funny lump size in %s", mod->name);

	count = filelen / sizeof (dl2leaf_t);

	out = (mleaf_t *)Mem_Alloc (count * sizeof (*out));

	mod->leafs = out;
	mod->numleafs = count;

	for (i = 0; i < count; i++, in += sizeof (dl2leaf_t), out++)
	{
		for (j = 0; j < 3; j++)
		{
			out->minmaxs[j] = ReadFloatUnaligned (in + offsetof (dl2leaf_t, mins[j]));
			out->minmaxs[3 + j] = ReadFloatUnaligned (in + offsetof (dl2leaf_t, maxs[j]));
		}

		p = ReadLongUnaligned (in + offsetof (dl2leaf_t, contents));
		out->contents = p;

		out->firstmarksurface = mod->marksurfaces + ReadLongUnaligned (in + offsetof (dl2leaf_t, firstmarksurface)); // johnfitz -- unsigned short
		out->nummarksurfaces = ReadLongUnaligned (in + offsetof (dl2leaf_t, nummarksurfaces));                       // johnfitz -- unsigned short

		p = ReadLongUnaligned (in + offsetof (dl2leaf_t, visofs));
		if (p == -1)
			out->compressed_vis = NULL;
		else
			out->compressed_vis = mod->visdata + p;
		out->efrags = NULL;

		for (j = 0; j < 4; j++)
			out->ambient_sound_level[j] = *(in + offsetof (dl2leaf_t, ambient_level[j]));

		// johnfitz -- removed code to mark surfaces as SURF_UNDERWATER
	}
}

/*
=================
Mod_LoadLeafs
=================
*/
static void Mod_LoadLeafs (qmodel_t *mod, byte *mod_base, lump_t *l, int bsp2)
{
	void *in = (void *)(mod_base + l->fileofs);

	if (bsp2 == 2)
		Mod_ProcessLeafs_L2 (mod, in, l->filelen);
	else if (bsp2)
		Mod_ProcessLeafs_L1 (mod, in, l->filelen);
	else
		Mod_ProcessLeafs_S (mod, in, l->filelen);
}

static qboolean Mod_IsLiquidContents (int contents)
{
	return contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA;
}

static void Mod_CheckWaterVisSealed (qmodel_t *mod)
{
	mleaf_t *leaf;
	byte    *vis;
	int      i, j, k, l, numclusters;
	qboolean found = false;

	mod->nowatervis = false;

	if (!mod->visdata || !mod->submodels || mod->numleafs < 2)
		return;

	for (i = 1; i < mod->numleafs; i++)
	{
		if (Mod_IsLiquidContents (mod->leafs[i].contents))
		{
			found = true;
			break;
		}
	}

	if (!found)
		return;

	numclusters = mod->submodels[0].visleafs;

	for (i = 1; i <= numclusters && i < mod->numleafs; i++)
	{
		leaf = mod->leafs + i;

		if (leaf->contents == CONTENTS_SOLID || leaf->compressed_vis == NULL || Mod_IsLiquidContents (leaf->contents))
			continue;

		vis = Mod_DecompressVis (leaf->compressed_vis, mod);

		for (j = 0; j < (numclusters + 7) / 8; j++)
		{
			if (!vis[j])
				continue;

			for (k = 0; k < 8; k++)
			{
				l = (j << 3) + k + 1;

				if (!(vis[j] & (1u << k)) || l > numclusters)
					continue;

				if (Mod_IsLiquidContents (mod->leafs[l].contents))
					return;
			}
		}
	}

	mod->nowatervis = true;
}

/*
=================
Mod_CheckWaterVis
=================
*/
static void Mod_CheckWaterVis (qmodel_t *mod)
{
	mleaf_t    *leaf, *other;
	msurface_t *surf;
	int         i, j, k;
	int         numclusters = mod->submodels[0].visleafs;
	int         contentfound = 0;
	int         contenttransparent = 0;
	int         contenttype;
	unsigned    hascontents = 0;

	Mod_CheckWaterVisSealed (mod);

	if (!CVAR_TO_BOOL (rt_enable_pvs))
	{ // all can be
		mod->contentstransparent = (SURF_DRAWWATER | SURF_DRAWTELE | SURF_DRAWSLIME | SURF_DRAWLAVA);
		return;
	}

	// pvs is 1-based. leaf 0 sees all (the solid leaf).
	// leaf 0 has no pvs, and does not appear in other leafs either, so watch out for the biases.
	for (i = 0, leaf = mod->leafs + 1; i < numclusters - 1; i++, leaf++)
	{
		byte *vis;
		if (leaf->contents < 0) // err... wtf?
			hascontents = 0;
		if (leaf->contents == CONTENTS_WATER)
		{
			if ((contenttransparent & (SURF_DRAWWATER | SURF_DRAWTELE)) == (SURF_DRAWWATER | SURF_DRAWTELE))
				continue;
			// this check is somewhat risky, but we should be able to get away with it.
			for (contenttype = 0, j = 0; j < leaf->nummarksurfaces; j++)
			{
				surf = &mod->surfaces[leaf->firstmarksurface[j]];
				if (surf->flags & (SURF_DRAWWATER | SURF_DRAWTELE))
				{
					contenttype = surf->flags & (SURF_DRAWWATER | SURF_DRAWTELE);
					break;
				}
			}
			// its possible that this leaf has absolutely no surfaces in it, turb or otherwise.
			if (contenttype == 0)
				continue;
		}
		else if (leaf->contents == CONTENTS_SLIME)
			contenttype = SURF_DRAWSLIME;
		else if (leaf->contents == CONTENTS_LAVA)
			contenttype = SURF_DRAWLAVA;
		// fixme: tele
		else
			continue;
		if (contenttransparent & contenttype)
		{
		nextleaf:
			continue; // found one of this type already
		}
		contentfound |= contenttype;
		vis = Mod_DecompressVis (leaf->compressed_vis, mod);
		for (j = 0; j < (numclusters + 7) / 8; j++)
		{
			if (vis[j])
			{
				for (k = 0; k < 8; k++)
				{
					if (vis[j] & (1u << k))
					{
						other = &mod->leafs[(j << 3) + k + 1];
						if (leaf->contents != other->contents)
						{
							//							Con_Printf("%p:%i sees %p:%i\n", leaf, leaf->contents, other, other->contents);
							contenttransparent |= contenttype;
							goto nextleaf;
						}
					}
				}
			}
		}
	}

	if (!contenttransparent)
	{ // no water leaf saw a non-water leaf
		// but only warn when there's actually water somewhere there...
		if (hascontents & ((1 << -CONTENTS_WATER) | (1 << -CONTENTS_SLIME) | (1 << -CONTENTS_LAVA)))
			Con_DPrintf ("%s is not watervised\n", mod->name);
	}
	else
	{
		Con_DPrintf2 ("%s is vised for transparent", mod->name);
		if (contenttransparent & SURF_DRAWWATER)
			Con_DPrintf2 (" water");
		if (contenttransparent & SURF_DRAWTELE)
			Con_DPrintf2 (" tele");
		if (contenttransparent & SURF_DRAWLAVA)
			Con_DPrintf2 (" lava");
		if (contenttransparent & SURF_DRAWSLIME)
			Con_DPrintf2 (" slime");
		Con_DPrintf2 ("\n");
	}
	// any types that we didn't find are assumed to be transparent.
	// this allows submodels to work okay (eg: ad uses func_illusionary teleporters for some reason).
	mod->contentstransparent = contenttransparent | (~contentfound & (SURF_DRAWWATER | SURF_DRAWTELE | SURF_DRAWSLIME | SURF_DRAWLAVA));
}

/*
=================
Mod_LoadClipnodes
=================
*/
static void Mod_LoadClipnodes (qmodel_t *mod, byte *mod_base, lump_t *l, qboolean bsp2)
{
	byte *ins;
	byte *inl;

	mclipnode_t *out; // johnfitz -- was dclipnode_t
	int          i, count;
	hull_t      *hull;

	if (bsp2)
	{
		ins = NULL;
		inl = mod_base + l->fileofs;
		if (l->filelen % sizeof (dlclipnode_t))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (dlclipnode_t);
	}
	else
	{
		ins = mod_base + l->fileofs;
		inl = NULL;
		if (l->filelen % sizeof (dsclipnode_t))
			Sys_Error ("Mod_LoadClipnodes: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (dsclipnode_t);
	}
	out = (mclipnode_t *)Mem_Alloc (count * sizeof (*out));

	// johnfitz -- warn about exceeding old limits
	if (count > 32767 && !bsp2)
		Con_DWarning ("%i clipnodes exceeds standard limit of 32767.\n", count);
	// johnfitz

	mod->clipnodes = out;
	mod->numclipnodes = count;

	hull = &mod->hulls[1];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count - 1;
	hull->planes = mod->planes;
	hull->clip_mins[0] = -16;
	hull->clip_mins[1] = -16;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 16;
	hull->clip_maxs[1] = 16;
	hull->clip_maxs[2] = 32;

	hull = &mod->hulls[2];
	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count - 1;
	hull->planes = mod->planes;
	hull->clip_mins[0] = -32;
	hull->clip_mins[1] = -32;
	hull->clip_mins[2] = -24;
	hull->clip_maxs[0] = 32;
	hull->clip_maxs[1] = 32;
	hull->clip_maxs[2] = 64;

	if (bsp2)
	{
		for (i = 0; i < count; i++, out++, inl += sizeof (dlclipnode_t))
		{
			out->planenum = ReadLongUnaligned (inl + offsetof (dlclipnode_t, planenum));

			// johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= mod->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			// johnfitz

			out->children[0] = ReadLongUnaligned (inl + offsetof (dlclipnode_t, children[0]));
			out->children[1] = ReadLongUnaligned (inl + offsetof (dlclipnode_t, children[1]));
			// Spike: FIXME: bounds check
		}
	}
	else
	{
		for (i = 0; i < count; i++, out++, ins += sizeof (dsclipnode_t))
		{
			out->planenum = ReadLongUnaligned (ins + offsetof (dsclipnode_t, planenum));

			// johnfitz -- bounds check
			if (out->planenum < 0 || out->planenum >= mod->numplanes)
				Host_Error ("Mod_LoadClipnodes: planenum out of bounds");
			// johnfitz

			// johnfitz -- support clipnodes > 32k
			out->children[0] = (unsigned short)ReadShortUnaligned (ins + offsetof (dsclipnode_t, children[0]));
			out->children[1] = (unsigned short)ReadShortUnaligned (ins + offsetof (dsclipnode_t, children[1]));

			if (out->children[0] >= count)
				out->children[0] -= 65536;
			if (out->children[1] >= count)
				out->children[1] -= 65536;
			// johnfitz
		}
	}
}

/*
=================
Mod_MakeHull0

Duplicate the drawing hull structure as a clipping hull
=================
*/
static void Mod_MakeHull0 (qmodel_t *mod)
{
	mnode_t     *in, *child;
	mclipnode_t *out; // johnfitz -- was dclipnode_t
	int          i, j, count;
	hull_t      *hull;

	hull = &mod->hulls[0];

	in = mod->nodes;
	count = mod->numnodes;
	out = (mclipnode_t *)Mem_Alloc (count * sizeof (*out));

	hull->clipnodes = out;
	hull->firstclipnode = 0;
	hull->lastclipnode = count - 1;
	hull->planes = mod->planes;

	for (i = 0; i < count; i++, out++, in++)
	{
		out->planenum = in->plane - mod->planes;
		for (j = 0; j < 2; j++)
		{
			child = in->children[j];
			if (child->contents < 0)
				out->children[j] = child->contents;
			else
				out->children[j] = child - mod->nodes;
		}
	}
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static void Mod_LoadMarksurfaces (qmodel_t *mod, byte *mod_base, lump_t *l, int bsp2)
{
	int  i, j, count;
	int *out;
	if (bsp2)
	{
		byte *in = mod_base + l->fileofs;

		if (l->filelen % sizeof (unsigned int))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (unsigned int);
		out = (int *)Mem_Alloc (count * sizeof (*out));

		mod->marksurfaces = out;
		mod->nummarksurfaces = count;

		for (i = 0; i < count; i++)
		{
			j = ReadLongUnaligned (in + (i * sizeof (int)));
			if (j >= mod->numsurfaces)
				Host_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
	else
	{
		byte *in = mod_base + l->fileofs;

		if (l->filelen % sizeof (short))
			Host_Error ("Mod_LoadMarksurfaces: funny lump size in %s", mod->name);

		count = l->filelen / sizeof (short);
		out = (int *)Mem_Alloc (count * sizeof (*out));

		mod->marksurfaces = out;
		mod->nummarksurfaces = count;

		// johnfitz -- warn mappers about exceeding old limits
		if (count > 32767)
			Con_DWarning ("%i marksurfaces exceeds standard limit of 32767.\n", count);
		// johnfitz

		for (i = 0; i < count; i++)
		{
			j = (unsigned short)ReadShortUnaligned (in + (i * sizeof (short))); // johnfitz -- explicit cast as unsigned short
			if (j >= mod->numsurfaces)
				Sys_Error ("Mod_LoadMarksurfaces: bad surface number");
			out[i] = j;
		}
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void Mod_LoadSurfedges (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int   i, count;
	byte *in;
	int  *out;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (int))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (int);
	out = (int *)Mem_Alloc (count * sizeof (int));

	mod->surfedges = out;
	mod->numsurfedges = count;

	for (i = 0; i < count; i++)
	{
		out[i] = ReadLongUnaligned (in + (i * sizeof (int)));
	}
}

/*
=================
Mod_LoadPlanes
=================
*/
static void Mod_LoadPlanes (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	int       i, j;
	mplane_t *out;
	byte     *in;
	int       count;
	int       bits;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dplane_t))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (dplane_t);
	out = (mplane_t *)Mem_Alloc (count * 2 * sizeof (*out));

	mod->planes = out;
	mod->numplanes = count;

	for (i = 0; i < count; i++, in += sizeof (dplane_t), out++)
	{
		bits = 0;
		for (j = 0; j < 3; j++)
		{
			out->normal[j] = ReadFloatUnaligned (in + offsetof (dplane_t, normal[j]));
			if (out->normal[j] < 0)
				bits |= 1 << j;
		}

		out->dist = ReadFloatUnaligned (in + offsetof (dplane_t, dist));
		out->type = ReadLongUnaligned (in + offsetof (dplane_t, type));
		out->signbits = bits;
	}
}

/*
=================
RadiusFromBounds
=================
*/
float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	int    i;
	vec3_t corner;

	for (i = 0; i < 3; i++)
	{
		corner[i] = fabs (mins[i]) > fabs (maxs[i]) ? fabs (mins[i]) : fabs (maxs[i]);
	}

	return VectorLength (corner);
}

/*
=================
Mod_LoadSubmodels
=================
*/
static void Mod_LoadSubmodels (qmodel_t *mod, byte *mod_base, lump_t *l)
{
	byte     *in;
	dmodel_t *out;
	int       i, j, count;

	in = mod_base + l->fileofs;
	if (l->filelen % sizeof (dmodel_t))
		Sys_Error ("MOD_LoadBmodel: funny lump size in %s", mod->name);
	count = l->filelen / sizeof (dmodel_t);
	out = (dmodel_t *)Mem_Alloc (count * sizeof (*out));

	mod->submodels = out;
	mod->numsubmodels = count;

	for (i = 0; i < count; i++, in += sizeof (dmodel_t), out++)
	{
		for (j = 0; j < 3; j++)
		{ // spread the mins / maxs by a pixel
			out->mins[j] = ReadFloatUnaligned (in + offsetof (dmodel_t, mins[j])) - 1;
			out->maxs[j] = ReadFloatUnaligned (in + offsetof (dmodel_t, maxs[j])) + 1;
			out->origin[j] = ReadFloatUnaligned (in + offsetof (dmodel_t, origin[j]));
		}
		for (j = 0; j < MAX_MAP_HULLS; j++)
		{
			out->headnode[j] = ReadLongUnaligned (in + offsetof (dmodel_t, headnode[j]));
		}
		out->visleafs = ReadLongUnaligned (in + offsetof (dmodel_t, visleafs));
		out->firstface = ReadLongUnaligned (in + offsetof (dmodel_t, firstface));
		out->numfaces = ReadLongUnaligned (in + offsetof (dmodel_t, numfaces));
	}

	// johnfitz -- check world visleafs -- adapted from bjp
	out = mod->submodels;

	if (out->visleafs > 8192)
		Con_DWarning ("%i visleafs exceeds standard limit of 8192.\n", out->visleafs);
	// johnfitz
}

/*
=================
Mod_BoundsFromClipNode -- johnfitz

update the model's clipmins and clipmaxs based on each node's plane.

This works because of the way brushes are expanded in hull generation.
Each brush will include all six axial planes, which bound that brush.
Therefore, the bounding box of the hull can be constructed entirely
from axial planes found in the clipnodes for that hull.
=================
*/
#if 0  /* disabled for now -- see in Mod_SetupSubmodels()  */
static void Mod_BoundsFromClipNode (qmodel_t *mod, int hull, int nodenum)
{
	mplane_t    *plane;
	mclipnode_t *node;

	if (nodenum < 0)
		return; // hit a leafnode

	node = &mod->clipnodes[nodenum];
	plane = mod->hulls[hull].planes + node->planenum;
	switch (plane->type)
	{

	case PLANE_X:
		if (plane->signbits == 1)
			mod->clipmins[0] = q_min (mod->clipmins[0], -plane->dist - mod->hulls[hull].clip_mins[0]);
		else
			mod->clipmaxs[0] = q_max (mod->clipmaxs[0], plane->dist - mod->hulls[hull].clip_maxs[0]);
		break;
	case PLANE_Y:
		if (plane->signbits == 2)
			mod->clipmins[1] = q_min (mod->clipmins[1], -plane->dist - mod->hulls[hull].clip_mins[1]);
		else
			mod->clipmaxs[1] = q_max (mod->clipmaxs[1], plane->dist - mod->hulls[hull].clip_maxs[1]);
		break;
	case PLANE_Z:
		if (plane->signbits == 4)
			mod->clipmins[2] = q_min (mod->clipmins[2], -plane->dist - mod->hulls[hull].clip_mins[2]);
		else
			mod->clipmaxs[2] = q_max (mod->clipmaxs[2], plane->dist - mod->hulls[hull].clip_maxs[2]);
		break;
	default:
		// skip nonaxial planes; don't need them
		break;
	}

	Mod_BoundsFromClipNode (mod, hull, node->children[0]);
	Mod_BoundsFromClipNode (mod, hull, node->children[1]);
}
#endif /* #if 0 */

/* EXTERNAL VIS FILE SUPPORT:
 */
typedef struct vispatch_s
{
	char mapname[32];
	int  filelen; // length of data after header (VIS+Leafs)
} vispatch_t;
#define VISPATCH_HEADER_LEN 36

static FILE *Mod_FindVisibilityExternal (qmodel_t *mod, const char *loadname)
{
	vispatch_t   header;
	char         visfilename[MAX_QPATH];
	const char  *shortname;
	unsigned int path_id;
	FILE        *f;
	long         pos;
	size_t       r;

	q_snprintf (visfilename, sizeof (visfilename), "maps/%s.vis", loadname);
	if (COM_FOpenFile (visfilename, &f, &path_id) < 0)
	{
		Con_DPrintf ("%s not found, trying ", visfilename);
		q_snprintf (visfilename, sizeof (visfilename), "%s.vis", COM_SkipPath (com_gamedir));
		Con_DPrintf ("%s\n", visfilename);
		if (COM_FOpenFile (visfilename, &f, &path_id) < 0)
		{
			Con_DPrintf ("external vis not found\n");
			return NULL;
		}
	}
	if (path_id < mod->path_id)
	{
		fclose (f);
		Con_DPrintf ("ignored %s from a gamedir with lower priority\n", visfilename);
		return NULL;
	}

	Con_DPrintf ("Found external VIS %s\n", visfilename);

	shortname = COM_SkipPath (mod->name);
	pos = 0;
	while ((r = fread (&header, 1, VISPATCH_HEADER_LEN, f)) == VISPATCH_HEADER_LEN)
	{
		header.filelen = LittleLong (header.filelen);
		if (header.filelen <= 0)
		{ /* bad entry -- don't trust the rest. */
			fclose (f);
			return NULL;
		}
		if (!q_strcasecmp (header.mapname, shortname))
			break;
		pos += header.filelen + VISPATCH_HEADER_LEN;
		fseek (f, pos, SEEK_SET);
	}
	if (r != VISPATCH_HEADER_LEN)
	{
		fclose (f);
		Con_DPrintf ("%s not found in %s\n", shortname, visfilename);
		return NULL;
	}

	return f;
}

static byte *Mod_LoadVisibilityExternal (FILE *f, int *p_len)
{
	int   filelen;
	byte *visdata;

	*p_len = 0;
	filelen = 0;
	if (fread (&filelen, 1, 4, f) != 4)
		return NULL;
	filelen = LittleLong (filelen);
	if (filelen <= 0)
		return NULL;
	Con_DPrintf ("...%d bytes visibility data\n", filelen);
	visdata = (byte *)Mem_Alloc (filelen);
	if (fread (visdata, filelen, 1, f) != 1)
		return NULL;
	*p_len = filelen;
	return visdata;
}

static void Mod_LoadLeafsExternal (qmodel_t *mod, FILE *f)
{
	int   filelen;
	void *in;

	filelen = 0;
	if (fread (&filelen, 1, 4, f) != 4)
		Sys_Error ("Invalid leaf");
	filelen = LittleLong (filelen);
	if (filelen <= 0)
		return;
	Con_DPrintf ("...%d bytes leaf data\n", filelen);
	in = Mem_Alloc (filelen);
	if (fread (in, filelen, 1, f) != 1)
		return;
	Mod_ProcessLeafs_S (mod, (byte *)in, filelen);
}

/*
=================
Mod_SetupSubmodels
set up the submodels (FIXME: this is confusing)
=================
*/
static void Mod_SetupSubmodels (qmodel_t *mod)
{
	int       i, j;
	float     radius;
	dmodel_t *bm;

	// johnfitz -- okay, so that i stop getting confused every time i look at this loop, here's how it works:
	// we're looping through the submodels starting at 0.  Submodel 0 is the main model, so we don't have to
	// worry about clobbering data the first time through, since it's the same data.  At the end of the loop,
	// we create a new copy of the data to use the next time through.
	for (i = 0; i < mod->numsubmodels; i++)
	{
		bm = &mod->submodels[i];

		mod->hulls[0].firstclipnode = bm->headnode[0];
		for (j = 1; j < MAX_MAP_HULLS; j++)
		{
			mod->hulls[j].firstclipnode = bm->headnode[j];
			mod->hulls[j].lastclipnode = mod->numclipnodes - 1;
		}

		mod->firstmodelsurface = bm->firstface;
		mod->nummodelsurfaces = bm->numfaces;

		VectorCopy (bm->maxs, mod->maxs);
		VectorCopy (bm->mins, mod->mins);

		// johnfitz -- calculate rotate bounds and yaw bounds
		radius = RadiusFromBounds (mod->mins, mod->maxs);
		mod->rmaxs[0] = mod->rmaxs[1] = mod->rmaxs[2] = mod->ymaxs[0] = mod->ymaxs[1] = mod->ymaxs[2] = radius;
		mod->rmins[0] = mod->rmins[1] = mod->rmins[2] = mod->ymins[0] = mod->ymins[1] = mod->ymins[2] = -radius;
		// johnfitz

		// johnfitz -- correct physics cullboxes so that outlying clip brushes on doors and stuff are handled right
		if (i > 0 || strcmp (mod->name, sv.modelname) != 0) // skip submodel 0 of sv.worldmodel, which is the actual world
		{
			// start with the hull0 bounds
			VectorCopy (mod->maxs, mod->clipmaxs);
			VectorCopy (mod->mins, mod->clipmins);

			// process hull1 (we don't need to process hull2 becuase there's
			// no such thing as a brush that appears in hull2 but not hull1)
			// Mod_BoundsFromClipNode (mod, 1, mod->hulls[1].firstclipnode); // (disabled for now becuase it fucks up on rotating models)
		}
		// johnfitz

		mod->numleafs = bm->visleafs;

		if (i < mod->numsubmodels - 1)
		{ // duplicate the basic information
			char name[12];

			sprintf (name, "*%i", i + 1);
			qmodel_t *submodel = Mod_FindName (name);
			*submodel = *mod;
			strcpy (submodel->name, name);
#ifdef PSET_SCRIPT
			// Need to NULL this otherwise we double delete in PScript_ClearSurfaceParticles
			submodel->skytrimem = NULL;
#endif
			mod = submodel;
		}
	}
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void Mod_LoadBrushModel (qmodel_t *mod, const char *loadname, void *buffer)
{
	int        i;
	int        bsp2;
	dheader_t *header;

	mod->type = mod_brush;

	header = (dheader_t *)buffer;

	mod->bspversion = LittleLong (header->version);

	switch (mod->bspversion)
	{
	case BSPVERSION:
		bsp2 = false;
		break;
	case BSP2VERSION_2PSB:
		bsp2 = 1; // first iteration
		break;
	case BSP2VERSION_BSP2:
		bsp2 = 2; // sanitised revision
		break;
	case BSPVERSION_QUAKE64:
		bsp2 = false;
		break;
	default:
		Sys_Error ("Mod_LoadBrushModel: %s has unsupported version number (%i)", mod->name, mod->bspversion);
		break;
	}

	if (sv.modelname[0] && !q_strcasecmp (loadname, sv.name))
	{
		RT_MAT_ChangeMap (loadname);
		RT_LIGHT_Reload ();
		RT_CustomLights_ChangeMap (loadname);
	}

	// swap all the lumps
	byte *mod_base = (byte *)header;

	for (i = 0; i < (int)sizeof (dheader_t) / 4; i++)
		((int *)header)[i] = LittleLong (((int *)header)[i]);

	// load into heap

	Mod_LoadVertexes (mod, mod_base, &header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges (mod, mod_base, &header->lumps[LUMP_EDGES], bsp2);
	Mod_LoadSurfedges (mod, mod_base, &header->lumps[LUMP_SURFEDGES]);
	Mod_LoadTextures (mod, mod_base, &header->lumps[LUMP_TEXTURES]);
	Mod_LoadLighting (mod, mod_base, &header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes (mod, mod_base, &header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo (mod, mod_base, &header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces (mod, mod_base, &header->lumps[LUMP_FACES], bsp2);
	Mod_LoadMarksurfaces (mod, mod_base, &header->lumps[LUMP_MARKSURFACES], bsp2);

	if (mod->bspversion == BSPVERSION && external_vis.value && sv.modelname[0] && !q_strcasecmp (loadname, sv.name))
	{
		FILE *fvis;
		Con_DPrintf ("trying to open external vis file\n");
		fvis = Mod_FindVisibilityExternal (mod, loadname);
		if (fvis)
		{
			mod->leafs = NULL;
			mod->numleafs = 0;
			Con_DPrintf ("found valid external .vis file for map\n");
			mod->visdata = Mod_LoadVisibilityExternal (fvis, &mod->visdatasize);
			if (mod->visdata)
			{
				Mod_LoadLeafsExternal (mod, fvis);
			}
			fclose (fvis);
			if (mod->visdata && mod->leafs && mod->numleafs)
			{
				goto visdone;
			}
			Con_DPrintf ("External VIS data failed, using standard vis.\n");
		}
	}

	Mod_LoadVisibility (mod, mod_base, &header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs (mod, mod_base, &header->lumps[LUMP_LEAFS], bsp2);
visdone:
	Mod_LoadNodes (mod, mod_base, &header->lumps[LUMP_NODES], bsp2);
	Mod_LoadClipnodes (mod, mod_base, &header->lumps[LUMP_CLIPNODES], bsp2);
	Mod_LoadEntities (mod, mod_base, &header->lumps[LUMP_ENTITIES]);
	Mod_LoadSubmodels (mod, mod_base, &header->lumps[LUMP_MODELS]);

	Mod_MakeHull0 (mod);

	mod->numframes = 2; // regular and alternate animation

	Mod_CheckWaterVis (mod);
	Mod_SetupSubmodels (mod);
}

/*
=================
Mod_SanitizeMapDescription

Cleans up map descriptions:
- removes colors
- replaces newlines with spaces
- replaces consecutive spaces with single one
- removes leading/trailing spaces

Returns dst string length (excluding NUL terminator)
=================
*/
size_t Mod_SanitizeMapDescription (char *dst, size_t dstsize, const char *src)
{
	int srcpos, dstpos;

	if (!dstsize)
		return 0;

	for (srcpos = dstpos = 0; src[srcpos] && (size_t)dstpos + 1 < dstsize; srcpos++)
	{
		char c = src[srcpos] & 0x7f; // remove color
		if (c == '\n' || c == '\r')	 // replace newlines with spaces
			c = ' ';
		else if (c == '\\' && src[srcpos + 1] == 'n') // replace '\\' followed by 'n' with space
		{
			c = ' ';
			srcpos++;
		}
		// remove leading spaces, replace consecutive spaces with single one
		if (c != ' ' || (dstpos > 0 && dst[dstpos - 1] != c))
			dst[dstpos++] = c;
	}
	// remove trailing space, if any
	if (dstpos > 0 && dst[dstpos - 1] == ' ')
		--dstpos;

	dst[dstpos] = '\0';
	return dstpos;
}

/*
=================
Mod_LoadMapDescription

Parses the entity lump in the given map to find its worldspawn message
Writes at most maxchars bytes to dest, including the NUL terminator
Returns true if map is playable, false otherwise
=================
*/
qboolean Mod_LoadMapDescription (char *desc, size_t maxchars, const char *map)
{
	char		buf[4 * 1024];
	char		path[MAX_QPATH];
	const char *data;
	FILE	   *f;
	lump_t	   *entlump;
	dheader_t	header;
	int			i;
	int			filesize;
	qboolean	ret = false;

	if (!maxchars)
		return false;
	*desc = '\0';

	if ((size_t)q_snprintf (path, sizeof (path), "maps/%s.bsp", map) >= sizeof (path))
		return false;

	filesize = COM_FOpenFile (path, &f, NULL);
	if (filesize <= (int)sizeof (header))
	{
		if (filesize != -1)
			fclose (f);
		return false;
	}

	if (fread (&header, sizeof (header), 1, f) != 1)
	{
		fclose (f);
		return false;
	}

	header.version = LittleLong (header.version);

	switch (header.version)
	{
	case BSPVERSION:
	case BSP2VERSION_2PSB:
	case BSP2VERSION_BSP2:
	case BSPVERSION_QUAKE64:
		break;
	default:
		fclose (f);
		return false;
	}

	for (i = 1; i < (int)(sizeof (header) / sizeof (int)); i++)
		((int *)&header)[i] = LittleLong (((int *)&header)[i]);

	entlump = &header.lumps[LUMP_ENTITIES];
	if (entlump->filelen < 0 || entlump->filelen >= filesize || entlump->fileofs < 0 || entlump->fileofs + entlump->filelen > filesize)
	{
		fclose (f);
		return false;
	}

	// if the entity lump is large enough we assume the map is playable
	// and only try to parse the first entity (worldspawn) for the map title
	if (entlump->filelen >= (int)sizeof (buf))
	{
		ret = true;
		entlump->filelen = sizeof (buf) - 1;
	}

	fseek (f, entlump->fileofs - (int)sizeof (header), SEEK_CUR);
	i = fread (buf, 1, entlump->filelen, f);
	fclose (f);

	if (i <= 0)
		return false;
	buf[i] = '\0';

	for (i = 0, data = buf; data; i++)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			return ret;

		while (1)
		{
			qboolean is_message;
			qboolean is_classname;

			// parse key
			data = COM_Parse (data);
			if (!data)
				return ret;
			if (com_token[0] == '}')
				break;

			is_message = i == 0 && !strcmp (com_token, "message");
			is_classname = i != 0 && !strcmp (com_token, "classname");

			// parse value
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return ret;

			if (is_message)
			{
				Mod_SanitizeMapDescription (desc, maxchars, com_token);
				if (ret)
					return true;
			}
			else if (is_classname)
			{
#define CLASSNAME_STARTS_WITH(str) (!strncmp (com_token, str, strlen (str)))
#define CLASSNAME_IS(str)		   (!strcmp (com_token, str))

				if (CLASSNAME_STARTS_WITH ("info_player_") || CLASSNAME_STARTS_WITH ("ammo_") || CLASSNAME_STARTS_WITH ("weapon_") ||
					CLASSNAME_STARTS_WITH ("monster_") || CLASSNAME_IS ("trigger_changelevel"))
				{
					return true;
				}

#undef CLASSNAME_IS
#undef CLASSNAME_STARTS_WITH
			}
		}
	}

	return ret;
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

aliashdr_t *pheader;

stvert_t    stverts[MAXALIASVERTS];
mtriangle_t triangles[MAXALIASTRIS];

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
trivertx_t *poseverts[MAXALIASFRAMES];
int         posenum;

byte **player_8bit_texels_tbl;
byte  *player_8bit_texels;

/*
=================
Mod_LoadAliasFrame
=================
*/
void *Mod_LoadAliasFrame (void *pin, maliasframedesc_t *frame)
{
	trivertx_t    *pinframe;
	int            i;
	daliasframe_t *pdaliasframe;

	if (posenum >= MAXALIASFRAMES)
		Sys_Error ("posenum >= MAXALIASFRAMES");

	pdaliasframe = (daliasframe_t *)pin;

	strcpy (frame->name, pdaliasframe->name);
	frame->firstpose = posenum;
	frame->numposes = 1;

	for (i = 0; i < 3; i++)
	{
		// these are byte values, so we don't have to worry about
		// endianness
		frame->bboxmin.v[i] = pdaliasframe->bboxmin.v[i];
		frame->bboxmax.v[i] = pdaliasframe->bboxmax.v[i];
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);

	poseverts[posenum] = pinframe;
	posenum++;

	pinframe += pheader->numverts;

	return (void *)pinframe;
}

/*
=================
Mod_LoadAliasGroup
=================
*/
void *Mod_LoadAliasGroup (void *pin, maliasframedesc_t *frame)
{
	daliasgroup_t    *pingroup;
	int               i, numframes;
	daliasinterval_t *pin_intervals;
	void			 *ptemp;

	pingroup = (daliasgroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	frame->firstpose = posenum;
	frame->numposes = numframes;

	for (i = 0; i < 3; i++)
	{
		// these are byte values, so we don't have to worry about endianness
		frame->bboxmin.v[i] = pingroup->bboxmin.v[i];
		frame->bboxmax.v[i] = pingroup->bboxmax.v[i];
	}

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	frame->interval = LittleFloat (pin_intervals->interval);

	pin_intervals += numframes;

	ptemp = (void *)pin_intervals;

	for (i = 0; i < numframes; i++)
	{
		if (posenum >= MAXALIASFRAMES)
			Sys_Error ("posenum >= MAXALIASFRAMES");

		poseverts[posenum] = (trivertx_t *)((daliasframe_t *)ptemp + 1);
		posenum++;

		ptemp = (trivertx_t *)((daliasframe_t *)ptemp + 1) + pheader->numverts;
	}

	return ptemp;
}

//=========================================================

/*
=================
Mod_FloodFillSkin

Fill background pixels so mipmapping doesn't have haloes - Ed
=================
*/

typedef struct
{
	short x, y;
} floodfill_t;

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP(off, dx, dy)                           \
	do                                                        \
	{                                                         \
		if (pos[off] == fillcolor)                            \
		{                                                     \
			pos[off] = 255;                                   \
			fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
			inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;          \
		}                                                     \
		else if (pos[off] != 255)                             \
			fdc = pos[off];                                   \
	} while (0)

void Mod_FloodFillSkin (byte *skin, int skinwidth, int skinheight)
{
	byte        fillcolor = *skin; // assume this is the pixel to fill
	floodfill_t fifo[FLOODFILL_FIFO_SIZE];
	int         inpt = 0, outpt = 0;
	int         filledcolor = -1;
	int         i;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
	{
		// printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
		return;
	}

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int   x = fifo[outpt].x, y = fifo[outpt].y;
		int   fdc = filledcolor;
		byte *pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)
			FLOODFILL_STEP (-1, -1, 0);
		if (x < skinwidth - 1)
			FLOODFILL_STEP (1, 1, 0);
		if (y > 0)
			FLOODFILL_STEP (-skinwidth, 0, -1);
		if (y < skinheight - 1)
			FLOODFILL_STEP (skinwidth, 0, 1);
		skin[x + skinwidth * y] = fdc;
	}
}

/*
===============
Mod_LoadSkinTask
===============
*/
typedef struct load_skin_task_args_s
{
	qmodel_t *mod;
	byte     *mod_base;
	byte    **ppskintypes;
} load_skin_task_args_t;

static qboolean Mod_SkinHasLumaMaterial (const char *skinName)
{
	rt_material_t *mat = RT_MAT_Find (skinName);
	return mat && mat->filename_emissive[0] != '\0';
}

static void Mod_LoadSkinTask (int i, load_skin_task_args_t *args)
{
	int          j, k, size, groupskins;
	char         name[MAX_QPATH];
	char         namenoext[MAX_QPATH];
	char         rtname[MAX_QPATH];
	byte        *skin, *texels;
	byte        *pskintype = args->ppskintypes[i];
	byte        *pinskingroup;
	byte        *pinskinintervals;
	src_offset_t offset;                   // johnfitz
	unsigned int texflags = TEXPREF_PAD;
	qmodel_t    *mod = args->mod;
	byte        *mod_base = args->mod_base;

	size = pheader->skinwidth * pheader->skinheight;

	if (mod->flags & MF_HOLEY)
		texflags |= TEXPREF_ALPHA;

	COM_StripExtension (mod->name, namenoext, sizeof (namenoext));

	if (ReadLongUnaligned (pskintype + offsetof (daliasskintype_t, type)) == ALIAS_SKIN_SINGLE)
	{
		skin = pskintype + sizeof (daliasskintype_t);
		Mod_FloodFillSkin (skin, pheader->skinwidth, pheader->skinheight);

		// save 8 bit texels for the player model to remap
		texels = (byte *)Mem_Alloc (size);
		pheader->texels[i] = texels;
		memcpy (texels, skin, size);

		// johnfitz -- rewritten
		q_snprintf (name, sizeof (name), "%s:frame%i", mod->name, i);
		q_snprintf (rtname, sizeof (rtname), "%s/%i", namenoext, i);

		// The skin bytes normally point into the loaded mdl buffer; when they do
		// not, the difference of their address and the buffer underflows and the
		// address itself is the source instead (see Mod_LoadTextures).
		const qboolean skin_in_file = (skin >= mod_base);

		offset = skin_in_file ? (src_offset_t)(skin - mod_base) : (src_offset_t)(uintptr_t)skin;
		if (Mod_SkinHasLumaMaterial (name))
			mod->flags |= MF_RT_LUMA;

		pheader->gltextures[i][0] = TexMgr_LoadImage (
			rtname,
			mod, name, pheader->skinwidth, pheader->skinheight, SRC_INDEXED, skin, skin_in_file ? mod->name : "", offset, texflags | TEXPREF_MIPMAP);
		pheader->fbtextures[i][0] = NULL;

		pheader->gltextures[i][3] = pheader->gltextures[i][2] = pheader->gltextures[i][1] = pheader->gltextures[i][0];
		pheader->fbtextures[i][3] = pheader->fbtextures[i][2] = pheader->fbtextures[i][1] = pheader->fbtextures[i][0];
		// johnfitz
	}
	else
	{
		// animating skin group.  yuck.
		pinskingroup = pskintype + sizeof (daliasskintype_t);
		groupskins = ReadLongUnaligned (pinskingroup + offsetof (daliasskingroup_t, numskins));
		pinskinintervals = pinskingroup + sizeof (daliasskingroup_t);
		skin = pinskinintervals + (groupskins * sizeof (daliasskininterval_t));

		for (j = 0; j < groupskins; j++)
		{
			Mod_FloodFillSkin (skin, pheader->skinwidth, pheader->skinheight);
			if (j == 0)
			{
				texels = (byte *)Mem_Alloc (size);
				pheader->texels[i] = texels;
				memcpy (texels, skin, size);
			}

			// johnfitz -- rewritten
			q_snprintf (name, sizeof (name), "%s:frame%i_%i", mod->name, i, j);
			q_snprintf (rtname, sizeof (rtname), "%s/%i_%i", namenoext, i, j);

			offset = (src_offset_t)(skin) - (src_offset_t)mod_base; // johnfitz
			if (Mod_SkinHasLumaMaterial (name))
				mod->flags |= MF_RT_LUMA;

			pheader->gltextures[i][j & 3] = TexMgr_LoadImage (
				rtname,
				mod, name, pheader->skinwidth, pheader->skinheight, SRC_INDEXED, skin, mod->name, offset, texflags | TEXPREF_MIPMAP);
			pheader->fbtextures[i][j & 3] = NULL;
			// johnfitz

			skin += size;
		}
		k = j;
		for (/**/; j < 4; j++)
			pheader->gltextures[i][j & 3] = pheader->gltextures[i][j - k];
	}
}

/*
===============
Mod_LoadAllSkins
===============
*/
void *Mod_LoadAllSkins (qmodel_t *mod, byte *mod_base, int numskins, byte *pskintype)
{
	if (numskins < 1 || numskins > MAX_SKINS)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d", numskins);

	byte **ppskintypes;
	TEMP_ALLOC (byte *, ppskintypes, numskins);
	int size = pheader->skinwidth * pheader->skinheight;
	for (int i = 0; i < numskins; i++)
	{
		ppskintypes[i] = pskintype;
		if (ReadLongUnaligned (pskintype + offsetof (daliasskintype_t, type)) == ALIAS_SKIN_SINGLE)
		{
			pskintype += sizeof (daliasskintype_t) + size;
		}
		else
		{
			// animating skin group.  yuck.
			byte *pinskingroup = pskintype + sizeof (daliasskintype_t);
			int   groupskins = ReadLongUnaligned (pinskingroup + offsetof (daliasskingroup_t, numskins));
			byte *pinskinintervals = pinskingroup + sizeof (daliasskingroup_t);
			byte *skin = pinskinintervals + (groupskins * sizeof (daliasskininterval_t));
			pskintype = skin + (groupskins * size);
		}
	}

	load_skin_task_args_t args = {
		.mod = mod,
		.mod_base = mod_base,
		.ppskintypes = ppskintypes,
	};
	if (!Tasks_IsWorker () && (numskins > 1))
	{
		task_handle_t task = Task_AllocateAssignIndexedFuncAndSubmit ((task_indexed_func_t)Mod_LoadSkinTask, numskins, &args, sizeof (args));
		Task_Join (task, SDL_MUTEX_MAXWAIT);
	}
	else
	{
		for (int i = 0; i < numskins; i++)
		{
			Mod_LoadSkinTask (i, &args);
		}
	}

	TEMP_FREE (ppskintypes);
	return (void *)pskintype;
}

//=========================================================================

/*
=================
Mod_CalcAliasBounds -- johnfitz -- calculate bounds of alias model for nonrotated, yawrotated, and fullrotated cases
=================
*/
static void Mod_CalcAliasBounds (qmodel_t *mod, aliashdr_t *a)
{
	int    i, j, k;
	float  dist, yawradius, radius;
	vec3_t v;

	// clear out all data
	for (i = 0; i < 3; i++)
	{
		mod->mins[i] = mod->ymins[i] = mod->rmins[i] = FLT_MAX;
		mod->maxs[i] = mod->ymaxs[i] = mod->rmaxs[i] = -FLT_MAX;
		radius = yawradius = 0;
	}

	// process verts
	for (i = 0; i < a->numposes; i++)
		for (j = 0; j < a->numverts; j++)
		{
			for (k = 0; k < 3; k++)
				v[k] = poseverts[i][j].v[k] * pheader->scale[k] + pheader->scale_origin[k];

			for (k = 0; k < 3; k++)
			{
				mod->mins[k] = q_min (mod->mins[k], v[k]);
				mod->maxs[k] = q_max (mod->maxs[k], v[k]);
			}
			dist = v[0] * v[0] + v[1] * v[1];
			if (yawradius < dist)
				yawradius = dist;
			dist += v[2] * v[2];
			if (radius < dist)
				radius = dist;
		}

	// rbounds will be used when entity has nonzero pitch or roll
	radius = sqrt (radius);
	mod->rmins[0] = mod->rmins[1] = mod->rmins[2] = -radius;
	mod->rmaxs[0] = mod->rmaxs[1] = mod->rmaxs[2] = radius;

	// ybounds will be used when entity has nonzero yaw
	yawradius = sqrt (yawradius);
	mod->ymins[0] = mod->ymins[1] = -yawradius;
	mod->ymaxs[0] = mod->ymaxs[1] = yawradius;
	mod->ymins[2] = mod->mins[2];
	mod->ymaxs[2] = mod->maxs[2];
}

static qboolean nameInList (const char *list, const char *name)
{
	const char *s;
	char        tmp[MAX_QPATH];
	int         i;

	s = list;

	while (*s)
	{
		// make a copy until the next comma or end of string
		i = 0;
		while (*s && *s != ',')
		{
			if (i < MAX_QPATH - 1)
				tmp[i++] = *s;
			s++;
		}
		tmp[i] = '\0';
		// compare it to the model name
		if (!strcmp (name, tmp))
		{
			return true;
		}
		// search forwards to the next comma or end of string
		while (*s && *s == ',')
			s++;
	}
	return false;
}

/*
=================
Mod_SetExtraFlags -- johnfitz -- set up extra flags that aren't in the mdl
=================
*/
void Mod_SetExtraFlags (qmodel_t *mod)
{
	extern cvar_t r_nolerp_list;

	if (!mod)
		return;

	mod->flags &= (0xFF | MF_HOLEY | MF_RT_LUMA);

	if (mod->type == mod_alias)
	{
		// nolerp flag
		if (nameInList (r_nolerp_list.string, mod->name))
			mod->flags |= MOD_NOLERP;

		// fullbright hack (TODO: make this a cvar list)
		if (!strcmp (mod->name, "progs/flame2.mdl") || !strcmp (mod->name, "progs/flame.mdl") || !strcmp (mod->name, "progs/boss.mdl"))
			mod->flags |= MOD_FBRIGHTHACK;
	}

#ifdef PSET_SCRIPT
	PScript_UpdateModelEffects (mod);
#endif
}

/*
=================
Mod_LoadAliasModel
=================
*/
static void Mod_LoadAliasModel (qmodel_t *mod, void *buffer)
{
	int   i, j;
	byte *pinstverts;
	byte *pintriangles;
	int   version, numframes;
	int   size;
	byte *pframetype;
	byte *pskintype;
	byte *mod_base = (byte *)buffer; // johnfitz

	version = ReadLongUnaligned (mod_base + offsetof (mdl_t, version));
	if (version != ALIAS_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i)", mod->name, version, ALIAS_VERSION);

	//
	// allocate space for a working header, plus all the data except the frames,
	// skin and group info
	//
	size = sizeof (aliashdr_t) + (ReadLongUnaligned (mod_base + offsetof (mdl_t, numframes)) - 1) * sizeof (pheader->frames[0]);
	pheader = (aliashdr_t *)Mem_Alloc (size);

	mod->flags = ReadLongUnaligned (mod_base + offsetof (mdl_t, flags));

	//
	// endian-adjust and copy the data, starting with the alias model header
	//
	pheader->boundingradius = ReadLongUnaligned (mod_base + offsetof (mdl_t, boundingradius));
	pheader->numskins = ReadLongUnaligned (mod_base + offsetof (mdl_t, numskins));
	pheader->skinwidth = ReadLongUnaligned (mod_base + offsetof (mdl_t, skinwidth));
	pheader->skinheight = ReadLongUnaligned (mod_base + offsetof (mdl_t, skinheight));

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		Con_DWarning ("model %s has a skin taller than %d", mod->name, MAX_LBM_HEIGHT);

	pheader->numverts = ReadLongUnaligned (mod_base + offsetof (mdl_t, numverts));

	if (pheader->numverts <= 0)
		Sys_Error ("model %s has no vertices", mod->name);

	if (pheader->numverts > MAXALIASVERTS)
		Sys_Error ("model %s has too many vertices (%d; max = %d)", mod->name, pheader->numverts, MAXALIASVERTS);

	pheader->numtris = ReadLongUnaligned (mod_base + offsetof (mdl_t, numtris));

	if (pheader->numtris <= 0)
		Sys_Error ("model %s has no triangles", mod->name);

	if (pheader->numtris > MAXALIASTRIS)
		Sys_Error ("model %s has too many triangles (%d; max = %d)", mod->name, pheader->numtris, MAXALIASTRIS);

	pheader->numframes = ReadLongUnaligned (mod_base + offsetof (mdl_t, numframes));
	numframes = pheader->numframes;
	if (numframes < 1)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of frames: %d", numframes);

	pheader->size = ReadFloatUnaligned (mod_base + offsetof (mdl_t, size)) * ALIAS_BASE_SIZE_RATIO;
	mod->synctype = (synctype_t)ReadLongUnaligned (mod_base + offsetof (mdl_t, synctype));
	mod->numframes = pheader->numframes;

	for (i = 0; i < 3; i++)
	{
		pheader->scale[i] = ReadFloatUnaligned (mod_base + offsetof (mdl_t, scale[i]));
		pheader->scale_origin[i] = ReadFloatUnaligned (mod_base + offsetof (mdl_t, scale_origin[i]));
		pheader->eyeposition[i] = ReadFloatUnaligned (mod_base + offsetof (mdl_t, eyeposition[i]));
	}

	//
	// load the skins
	//
	pskintype = mod_base + sizeof (mdl_t);
	pskintype = Mod_LoadAllSkins (mod, mod_base, pheader->numskins, pskintype);

	//
	// load base s and t vertices
	//
	pinstverts = pskintype;

	for (i = 0; i < pheader->numverts; i++)
	{
		stverts[i].onseam = ReadLongUnaligned (pinstverts + offsetof (stvert_t, onseam));
		stverts[i].s = ReadLongUnaligned (pinstverts + offsetof (stvert_t, s));
		stverts[i].t = ReadLongUnaligned (pinstverts + offsetof (stvert_t, t));
		pinstverts += sizeof (stvert_t);
	}

	//
	// load triangle lists
	//
	pintriangles = pinstverts;

	for (i = 0; i < pheader->numtris; i++)
	{
		triangles[i].facesfront = ReadLongUnaligned (pintriangles + offsetof (dtriangle_t, facesfront));

		for (j = 0; j < 3; j++)
		{
			triangles[i].vertindex[j] = ReadLongUnaligned (pintriangles + offsetof (dtriangle_t, vertindex[j]));
		}
		pintriangles += sizeof (dtriangle_t);
	}

	//
	// load the frames
	//
	posenum = 0;
	pframetype = pintriangles;

	for (i = 0; i < numframes; i++)
	{
		aliasframetype_t frametype;
		frametype = (aliasframetype_t)ReadLongUnaligned (pframetype + offsetof (daliasframetype_t, type));
		if (frametype == ALIAS_SINGLE)
			pframetype = Mod_LoadAliasFrame (pframetype + sizeof (daliasframetype_t), &pheader->frames[i]);
		else
			pframetype = Mod_LoadAliasGroup (pframetype + sizeof (daliasframetype_t), &pheader->frames[i]);
	}

	pheader->numposes = posenum;

	mod->type = mod_alias;

	Mod_SetExtraFlags (mod); // johnfitz

	Mod_CalcAliasBounds (mod, pheader); // johnfitz

	//
	// build the draw lists
	//
	GL_MakeAliasModelDisplayLists (mod, pheader);

	//
	// move the complete, relocatable alias model to the cache
	//
	mod->extradata = (byte *)pheader;
}

/*
=================
Mod_LoadLMPTexture
=================
*/
static gltexture_t *Mod_LoadLMPTexture (qmodel_t *mod, const char *name)
{
	char         path[MAX_QPATH];
	char         rtname[MAX_QPATH];
	byte        *buf;
	byte        *pixels;
	int          width, height;
	gltexture_t *tx;

	q_snprintf (path, sizeof (path), "%s.lmp", name);
	buf = COM_LoadFile (path, NULL);
	if (!buf)
		return NULL;
	if (com_filesize < 8)
	{
		Mem_Free (buf);
		return NULL;
	}

	width = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	height = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || 8 + (size_t)width * height > (size_t)com_filesize)
	{
		Mem_Free (buf);
		return NULL;
	}

	pixels = buf + 8;
	q_snprintf (rtname, sizeof (rtname), "mdx/%s", path);
	tx = TexMgr_LoadImage (rtname, mod, path, width, height, SRC_INDEXED, pixels, path, 8, TEXPREF_ALPHA | TEXPREF_NOBRIGHT | TEXPREF_MIPMAP);
	Mem_Free (buf);
	return tx;
}

/*
=================
Mod_LoadEnhancedTexture
=================
*/
static gltexture_t *Mod_LoadEnhancedTexture (qmodel_t *mod, const char *shadername)
{
	static const char *prefixes[] = {"", "progs/", "textures/"};
	char               base[MAX_QPATH];
	char               pattern[MAX_QPATH];
	char               name[MAX_QPATH];
	char               rtname[MAX_QPATH];
	byte              *data;
	int                width, height;
	gltexture_t       *tx;
	int                v;
	size_t             p;

	if (!shadername[0])
		return NULL;

	q_strlcpy (base, shadername, sizeof (base));
	COM_StripExtension (base, base, sizeof (base));
	q_snprintf (pattern, sizeof (pattern), "%s_00_00", base);

	for (v = 1; v >= 0; v--)
	{
		const char *stem = v ? pattern : base;

		for (p = 0; p < countof (prefixes); p++)
		{
			q_snprintf (name, sizeof (name), "%s%s", prefixes[p], stem);

			data = Image_LoadImage (name, &width, &height);
			if (data)
			{
				q_snprintf (rtname, sizeof (rtname), "mdx/%s", name);
				tx = TexMgr_LoadImage (rtname, mod, name, width, height, SRC_RGBA, data, name, 0, TEXPREF_ALPHA | TEXPREF_MIPMAP);
				Mem_Free (data);
				return tx;
			}

			tx = Mod_LoadLMPTexture (mod, name);
			if (tx)
				return tx;
		}
	}

	return NULL;
}

/*
=================
Mod_LoadMD3Model
=================
*/
static void Mod_LoadMD3Model (qmodel_t *mod, const void *buffer)
{
	const md3Header_t *pinheader = (const md3Header_t *)buffer;
	const md3Frame_t  *pinframes;
	md3Surface_t      *pinsurface;
	aliashdr_t        *surfaces;
	size_t             hdrsize;
	int                numsurfs, numframes, totalverts, totalindices;
	int                vertbase, indexbase;
	vec3_t             mins, maxs;
	float              yawradius, radius;
	int                m, f, v;

	if (ReadLongUnaligned ((byte *)&pinheader->ident) != IDMD3HEADER)
		Sys_Error ("MD3: %s has wrong ident", mod->name);
	if (ReadLongUnaligned ((byte *)&pinheader->version) != MD3_VERSION)
		Sys_Error ("MD3: %s has wrong version number (%d should be %d)", mod->name, ReadLongUnaligned ((byte *)&pinheader->version), MD3_VERSION);

	numsurfs = ReadLongUnaligned ((byte *)&pinheader->numSurfaces);
	numframes = ReadLongUnaligned ((byte *)&pinheader->numFrames);

	if (numframes < 1 || numframes > MAXALIASFRAMES)
		Sys_Error ("MD3: %s has bad frame count (%i)", mod->name, numframes);
	if (numsurfs < 1 || numsurfs > MAX_SURFACES)
		Sys_Error ("MD3: %s has bad surface count (%i)", mod->name, numsurfs);

	totalverts = 0;
	totalindices = 0;
	pinsurface = (md3Surface_t *)((byte *)buffer + ReadLongUnaligned ((byte *)&pinheader->ofsSurfaces));
	for (m = 0; m < numsurfs; m++)
	{
		if (ReadLongUnaligned ((byte *)&pinsurface->ident) != IDMD3HEADER)
			Sys_Error ("MD3: %s corrupt surface ident", mod->name);
		if (ReadLongUnaligned ((byte *)&pinsurface->numFrames) != numframes)
			Sys_Error ("MD3: %s mismatched framecounts", mod->name);
		totalverts += ReadLongUnaligned ((byte *)&pinsurface->numVerts);
		totalindices += ReadLongUnaligned ((byte *)&pinsurface->numTriangles) * 3;
		pinsurface = (md3Surface_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsEnd));
	}

	hdrsize = sizeof (aliashdr_t) + (size_t)(numframes - 1) * sizeof (((aliashdr_t *)0)->frames[0]);
	surfaces = (aliashdr_t *)Mem_Alloc (hdrsize * numsurfs);

	GLMesh_DeleteVertexBuffer (mod);
	mod->rtvertices = (QrVertex *)Mem_Alloc ((size_t)numframes * totalverts * sizeof (QrVertex));
	mod->rtindices = (uint32_t *)Mem_Alloc ((size_t)totalindices * sizeof (uint32_t));

	pinframes = (const md3Frame_t *)((byte *)buffer + ReadLongUnaligned ((byte *)&pinheader->ofsFrames));

	mins[0] = mins[1] = mins[2] = FLT_MAX;
	maxs[0] = maxs[1] = maxs[2] = -FLT_MAX;
	yawradius = radius = 0;

	vertbase = 0;
	indexbase = 0;
	pinsurface = (md3Surface_t *)((byte *)buffer + ReadLongUnaligned ((byte *)&pinheader->ofsSurfaces));
	for (m = 0; m < numsurfs; m++)
	{
		aliashdr_t           *surf = (aliashdr_t *)((byte *)surfaces + (size_t)m * hdrsize);
		int                   numverts = ReadLongUnaligned ((byte *)&pinsurface->numVerts);
		int                   numtris = ReadLongUnaligned ((byte *)&pinsurface->numTriangles);
		int                   numshaders = ReadLongUnaligned ((byte *)&pinsurface->numShaders);
		const md3XyzNormal_t *pinvertexes;
		const md3St_t        *pinst;
		const md3Triangle_t  *pintriangle;
		gltexture_t          *tx = NULL;

		surf->nextsurface = (m + 1 < numsurfs) ? (aliashdr_t *)((byte *)surfaces + (size_t)(m + 1) * hdrsize) : NULL;
		surf->poseverttype = PV_QUAKE3;
		surf->numverts = numverts;
		surf->numtris = numtris;
		surf->numindexes = numtris * 3;
		surf->firstindex = indexbase;
		surf->numindices = numtris * 3;
		surf->numframes = numframes;
		surf->numposes = 1;
		surf->skinwidth = 0;
		surf->skinheight = 0;
		for (int k = 0; k < 3; k++)
		{
			surf->scale[k] = 1.0f;
			surf->scale_origin[k] = 0.0f;
		}

		for (f = 0; f < numframes; f++)
		{
			surf->frames[f].firstpose = f;
			surf->frames[f].numposes = 1;
			surf->frames[f].interval = 0.1f;
			q_strlcpy (surf->frames[f].name, pinframes[f].name, sizeof (surf->frames[f].name));
		}

		if (numshaders > 0)
		{
			const md3Shader_t *pinshader = (const md3Shader_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsShaders));
			tx = Mod_LoadEnhancedTexture (mod, pinshader[0].name);
		}
		if (!tx)
			tx = notexture;

		surf->numskins = 1;
		for (int s = 0; s < MAX_SKINS; s++)
			for (int k = 0; k < 4; k++)
			{
				surf->gltextures[s][k] = tx;
				surf->fbtextures[s][k] = NULL;
			}

		pinvertexes = (const md3XyzNormal_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsXyzNormals));
		pinst = (const md3St_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsSt));

		for (f = 0; f < numframes; f++)
		{
			for (v = 0; v < numverts; v++)
			{
				const md3XyzNormal_t *src = pinvertexes + (size_t)f * numverts + v;
				QrVertex             *dst = &mod->rtvertices[((size_t)f * totalverts) + vertbase + v];
				short                 packed = (short)ReadShortUnaligned ((byte *)&src->normal);
				float                 lat = (float)(packed & 0xff) * (2.0f * (float)M_PI / 255.0f);
				float                 lng = (float)((packed >> 8) & 0xff) * (2.0f * (float)M_PI / 255.0f);

				dst->position[0] = (short)ReadShortUnaligned ((byte *)&src->xyz[0]) * MD3_XYZ_SCALE;
				dst->position[1] = (short)ReadShortUnaligned ((byte *)&src->xyz[1]) * MD3_XYZ_SCALE;
				dst->position[2] = (short)ReadShortUnaligned ((byte *)&src->xyz[2]) * MD3_XYZ_SCALE;
				dst->normal[0] = cosf (lng) * sinf (lat);
				dst->normal[1] = sinf (lng) * sinf (lat);
				dst->normal[2] = cosf (lat);
				dst->texCoord[0] = ReadFloatUnaligned ((byte *)&pinst[v].s);
				dst->texCoord[1] = ReadFloatUnaligned ((byte *)&pinst[v].t);
				dst->packedColor = RT_PACKED_COLOR_WHITE;
				dst->cluster = 0;
				dst->lightStyles = 0;

				for (int k = 0; k < 3; k++)
				{
					mins[k] = q_min (mins[k], dst->position[k]);
					maxs[k] = q_max (maxs[k], dst->position[k]);
				}
				float dist = dst->position[0] * dst->position[0] + dst->position[1] * dst->position[1];
				yawradius = q_max (yawradius, dist);
				radius = q_max (radius, dist + dst->position[2] * dst->position[2]);
			}
		}

		pintriangle = (const md3Triangle_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsTriangles));
		for (int t = 0; t < numtris; t++)
			for (int k = 0; k < 3; k++)
				mod->rtindices[indexbase + t * 3 + k] = (uint32_t)(ReadLongUnaligned ((byte *)&pintriangle[t].indexes[k]) + vertbase);

		vertbase += numverts;
		indexbase += numtris * 3;
		pinsurface = (md3Surface_t *)((byte *)pinsurface + ReadLongUnaligned ((byte *)&pinsurface->ofsEnd));
	}

	surfaces->numverts_vbo = totalverts;
	surfaces->numposes = numframes;
	surfaces->poseverts = totalverts;
	surfaces->boundingradius = sqrtf (radius);

	mod->flags = ReadLongUnaligned ((byte *)&pinheader->flags);
	mod->type = mod_alias;
	mod->numframes = numframes;
	mod->extradata = (byte *)surfaces;

	for (int k = 0; k < 3; k++)
	{
		mod->mins[k] = mod->ymins[k] = mod->rmins[k] = FLT_MAX;
		mod->maxs[k] = mod->ymaxs[k] = mod->rmaxs[k] = -FLT_MAX;
	}
	radius = sqrtf (radius);
	yawradius = sqrtf (yawradius);
	for (int k = 0; k < 3; k++)
	{
		mod->mins[k] = mins[k];
		mod->maxs[k] = maxs[k];
		mod->rmins[k] = -radius;
		mod->rmaxs[k] = radius;
	}
	mod->ymins[0] = mod->ymins[1] = -yawradius;
	mod->ymaxs[0] = mod->ymaxs[1] = yawradius;
	mod->ymins[2] = mins[2];
	mod->ymaxs[2] = maxs[2];
}

typedef struct md5joint_s
{
	ssize_t parent;
	char    name[32];
	float   loc[12];
} md5joint_t;

typedef struct md5weight_s
{
	unsigned int joint;
	float        bias;
	vec3_t       pos;
} md5weight_t;

typedef struct md5vertinfo_s
{
	float        st[2];
	unsigned int firstweight;
	unsigned int numweights;
} md5vertinfo_t;

typedef struct md5meshtmp_s
{
	char            shader[MAX_QPATH];
	size_t          numverts;
	size_t          numtris;
	size_t          numweights;
	md5vertinfo_t  *verts;
	md5weight_t    *weights;
	unsigned short *indices;
} md5meshtmp_t;

static qboolean MD5_ParseCheck (const char *s, const void **buffer)
{
	if (strcmp (com_token, s))
		return false;
	*buffer = COM_Parse (*buffer);
	return true;
}

static size_t MD5_ParseUInt (const void **buffer)
{
	size_t i = strtoull (com_token, NULL, 0);
	*buffer = COM_Parse (*buffer);
	return i;
}

static long MD5_ParseSInt (const void **buffer)
{
	long i = strtol (com_token, NULL, 0);
	*buffer = COM_Parse (*buffer);
	return i;
}

static double MD5_ParseFloat (const void **buffer)
{
	double i = strtod (com_token, NULL);
	*buffer = COM_Parse (*buffer);
	return i;
}

static size_t MD5_CountAnimatedComponents (unsigned int flags)
{
	size_t count = 0;

	for (unsigned int bit = 1; bit <= 32; bit <<= 1)
		if (flags & bit)
			count++;
	return count;
}

#define MD5ERROR(...)              \
	do                             \
	{                              \
		Con_Warning (__VA_ARGS__); \
		goto error;                \
	} while (0)
#define MD5EXPECT(s)                                                                                 \
	do                                                                                               \
	{                                                                                                \
		if (strcmp (com_token, s))                                                                   \
			MD5ERROR ("Mod_LoadMD5Model(%s): expected \"%s\", found \"%s\"\n", fname, s, com_token); \
		buffer = COM_Parse (buffer);                                                                 \
	} while (0)
#define MD5UINT()   MD5_ParseUInt (&buffer)
#define MD5SINT()   MD5_ParseSInt (&buffer)
#define MD5FLOAT()  MD5_ParseFloat (&buffer)
#define MD5CHECK(s) MD5_ParseCheck (s, &buffer)
#define MD5IGNORE() buffer = COM_Parse (buffer)

static void GenMatrixPosQuat4Scale (const vec3_t pos, const vec4_t quat, const vec3_t scale, float result[12])
{
	const float x2 = quat[0] + quat[0];
	const float y2 = quat[1] + quat[1];
	const float z2 = quat[2] + quat[2];

	const float xx = quat[0] * x2;
	const float xy = quat[0] * y2;
	const float xz = quat[0] * z2;
	const float yy = quat[1] * y2;
	const float yz = quat[1] * z2;
	const float zz = quat[2] * z2;
	const float xw = quat[3] * x2;
	const float yw = quat[3] * y2;
	const float zw = quat[3] * z2;

	result[0 * 4 + 0] = scale[0] * (1.0f - (yy + zz));
	result[1 * 4 + 0] = scale[0] * (xy + zw);
	result[2 * 4 + 0] = scale[0] * (xz - yw);

	result[0 * 4 + 1] = scale[1] * (xy - zw);
	result[1 * 4 + 1] = scale[1] * (1.0f - (xx + zz));
	result[2 * 4 + 1] = scale[1] * (yz + xw);

	result[0 * 4 + 2] = scale[2] * (xz + yw);
	result[1 * 4 + 2] = scale[2] * (yz - xw);
	result[2 * 4 + 2] = scale[2] * (1.0f - (xx + yy));

	result[0 * 4 + 3] = pos[0];
	result[1 * 4 + 3] = pos[1];
	result[2 * 4 + 3] = pos[2];
}

static void MD5_QuatFromXYZW (vec4_t quat)
{
	quat[3] = 1 - DotProduct (quat, quat);
	if (quat[3] < 0)
		quat[3] = 0;
	quat[3] = -sqrtf (quat[3]);
}

static qboolean Mod_LoadMD5Anim (const char *modelname, const md5joint_t *joints, size_t numjoints, const float *bindabs, size_t *out_numposes, float **out_poses)
{
	char        fname[MAX_QPATH];
	void       *file = NULL;
	const void *buffer;
	const char *fname_ = fname;
	size_t      numframes, numanimjoints, rawcount, m;
	char       (*animnames)[32] = NULL;
	ssize_t    *animparents = NULL;
	unsigned int *animflags = NULL;
	size_t     *animoffsets = NULL;
	ssize_t    *mesh_to_anim = NULL;
	vec3_t     *basepos = NULL;
	vec4_t     *basequat = NULL;
	float      *raw = NULL;
	float      *out = NULL;
	float       local[12];
	size_t      j;

	q_strlcpy (fname, modelname, sizeof (fname));
	COM_StripExtension (fname, fname, sizeof (fname));
	COM_AddExtension (fname, ".md5anim", sizeof (fname));

	file = COM_LoadFile (fname, NULL);
	if (!file)
		return false;

	buffer = COM_Parse (file);

	MD5EXPECT ("MD5Version");
	MD5EXPECT ("10");
	if (MD5CHECK ("commandline"))
		MD5IGNORE ();
	MD5EXPECT ("numFrames");
	numframes = MD5UINT ();
	MD5EXPECT ("numJoints");
	numanimjoints = MD5UINT ();
	MD5EXPECT ("frameRate");
	MD5IGNORE ();
	MD5EXPECT ("numAnimatedComponents");
	rawcount = MD5UINT ();

	if (numframes < 1 || numframes > 100000)
		MD5ERROR ("%s has a bad frame count\n", fname_);
	if (numanimjoints < 1 || numanimjoints > 4096)
		MD5ERROR ("%s has a bad joint count\n", fname_);
	if ((double)numframes * (double)numanimjoints > 4.0e6 || rawcount > 1.0e6)
		MD5ERROR ("%s is too large\n", fname_);

	animnames = Mem_Alloc (numanimjoints * sizeof (*animnames));
	animparents = Mem_Alloc (numanimjoints * sizeof (*animparents));
	animflags = Mem_Alloc (numanimjoints * sizeof (*animflags));
	animoffsets = Mem_Alloc (numanimjoints * sizeof (*animoffsets));
	mesh_to_anim = Mem_Alloc (numjoints * sizeof (*mesh_to_anim));
	basepos = Mem_Alloc (numanimjoints * sizeof (*basepos));
	basequat = Mem_Alloc (numanimjoints * sizeof (*basequat));
	raw = Mem_Alloc ((rawcount + 6) * sizeof (*raw));
	out = Mem_Alloc (numframes * numjoints * 12 * sizeof (*out));

	MD5EXPECT ("hierarchy");
	MD5EXPECT ("{");
	for (m = 0; m < numanimjoints; m++)
	{
		q_strlcpy (animnames[m], com_token, sizeof (animnames[m]));
		buffer = COM_Parse (buffer);
		animparents[m] = MD5SINT ();
		if (animparents[m] < -1 || animparents[m] >= (ssize_t)m)
			MD5ERROR ("%s: joint has bad parent order\n", fname_);
		animflags[m] = (unsigned int)MD5UINT ();
		if (animflags[m] & ~63u)
			MD5ERROR ("%s: joint has unsupported flags\n", fname_);
		animoffsets[m] = MD5UINT ();
		if (animoffsets[m] + MD5_CountAnimatedComponents (animflags[m]) > rawcount)
			MD5ERROR ("%s: joint has bad offset\n", fname_);
	}
	MD5EXPECT ("}");

	for (j = 0; j < numjoints; j++)
	{
		mesh_to_anim[j] = -1;
		for (m = 0; m < numanimjoints; m++)
			if (!strcmp (joints[j].name, animnames[m]))
			{
				mesh_to_anim[j] = (ssize_t)m;
				break;
			}
	}

	MD5EXPECT ("bounds");
	MD5EXPECT ("{");
	while (MD5CHECK ("("))
	{
		MD5IGNORE ();
		MD5IGNORE ();
		MD5IGNORE ();
		MD5EXPECT (")");
		MD5EXPECT ("(");
		MD5IGNORE ();
		MD5IGNORE ();
		MD5IGNORE ();
		MD5EXPECT (")");
	}
	MD5EXPECT ("}");

	MD5EXPECT ("baseframe");
	MD5EXPECT ("{");
	for (m = 0; m < numanimjoints; m++)
	{
		MD5EXPECT ("(");
		basepos[m][0] = MD5FLOAT ();
		basepos[m][1] = MD5FLOAT ();
		basepos[m][2] = MD5FLOAT ();
		MD5EXPECT (")");
		MD5EXPECT ("(");
		basequat[m][0] = MD5FLOAT ();
		basequat[m][1] = MD5FLOAT ();
		basequat[m][2] = MD5FLOAT ();
		MD5_QuatFromXYZW (basequat[m]);
		MD5EXPECT (")");
	}
	MD5EXPECT ("}");

	while (MD5CHECK ("frame"))
	{
		size_t idx = MD5UINT ();
		float *r;

		if (idx >= numframes)
			MD5ERROR ("%s: invalid frame index\n", fname_);
		MD5EXPECT ("{");
		for (m = 0; m < rawcount; m++)
			raw[m] = MD5FLOAT ();
		MD5EXPECT ("}");

		for (j = 0; j < numjoints; j++)
		{
			vec3_t pos;
			vec4_t quat;
			ssize_t aj = mesh_to_anim[j];
			float  *dst = out + (idx * numjoints + j) * 12;

			if (aj < 0)
			{
				memcpy (local, joints[j].loc, sizeof (local));
			}
			else
			{
				static vec3_t scale = {1, 1, 1};

				VectorCopy (basepos[aj], pos);
				Vector4Copy (basequat[aj], quat);
				r = raw + animoffsets[aj];
				if (animflags[aj] & 1)
					pos[0] = *r++;
				if (animflags[aj] & 2)
					pos[1] = *r++;
				if (animflags[aj] & 4)
					pos[2] = *r++;
				if (animflags[aj] & 8)
					quat[0] = *r++;
				if (animflags[aj] & 16)
					quat[1] = *r++;
				if (animflags[aj] & 32)
					quat[2] = *r++;
				MD5_QuatFromXYZW (quat);
				GenMatrixPosQuat4Scale (pos, quat, scale, local);
			}

			if (joints[j].parent < 0)
				memcpy (dst, local, sizeof (local));
			else
				R_ConcatTransforms ((float (*)[4])(out + (idx * numjoints + joints[j].parent) * 12), (float (*)[4])local, (float (*)[4])dst);
		}
	}

	Mem_Free (animnames);
	Mem_Free (animparents);
	Mem_Free (animflags);
	Mem_Free (animoffsets);
	Mem_Free (mesh_to_anim);
	Mem_Free (basepos);
	Mem_Free (basequat);
	Mem_Free (raw);
	Mem_Free (file);

	*out_numposes = numframes;
	*out_poses = out;
	return true;

error:
	Mem_Free (animnames);
	Mem_Free (animparents);
	Mem_Free (animflags);
	Mem_Free (animoffsets);
	Mem_Free (mesh_to_anim);
	Mem_Free (basepos);
	Mem_Free (basequat);
	Mem_Free (raw);
	Mem_Free (out);
	Mem_Free (file);
	return false;
}

static qboolean Mod_LoadMD5Model (qmodel_t *mod, const void *buffer)
{
	const char   *fname = mod->name;
	md5joint_t   *joints = NULL;
	float        *bindabs = NULL;
	float        *poses = NULL;
	size_t        numposes = 1;
	qboolean      poses_owned = false;
	md5meshtmp_t *meshes = NULL;
	aliashdr_t   *surfaces = NULL;
	size_t        numjoints = 0, nummeshes = 0, m, j, p;
	size_t        totalverts = 0, totalindices = 0, vertbase = 0, indexbase = 0;
	size_t        hdrsize;
	vec3_t        mins, maxs;
	float         yawradius = 0, radius = 0;

	buffer = COM_Parse (buffer);

	MD5EXPECT ("MD5Version");
	MD5EXPECT ("10");
	if (MD5CHECK ("commandline"))
		MD5IGNORE ();
	MD5EXPECT ("numJoints");
	numjoints = MD5UINT ();
	MD5EXPECT ("numMeshes");
	nummeshes = MD5UINT ();

	if (numjoints < 1 || numjoints > 4096)
		MD5ERROR ("%s has a bad joint count\n", fname);
	if (nummeshes < 1 || nummeshes > 256)
		MD5ERROR ("%s has a bad mesh count\n", fname);

	MD5EXPECT ("joints");
	MD5EXPECT ("{");
	joints = Mem_Alloc (numjoints * sizeof (*joints));
	for (j = 0; j < numjoints; j++)
	{
		vec3_t pos, scale = {1, 1, 1};
		vec4_t quat;

		q_strlcpy (joints[j].name, com_token, sizeof (joints[j].name));
		buffer = COM_Parse (buffer);
		joints[j].parent = MD5SINT ();
		if (joints[j].parent < -1 || joints[j].parent >= (ssize_t)j)
			MD5ERROR ("%s: joint has bad parent order\n", fname);
		MD5EXPECT ("(");
		pos[0] = MD5FLOAT ();
		pos[1] = MD5FLOAT ();
		pos[2] = MD5FLOAT ();
		MD5EXPECT (")");
		MD5EXPECT ("(");
		quat[0] = MD5FLOAT ();
		quat[1] = MD5FLOAT ();
		quat[2] = MD5FLOAT ();
		MD5_QuatFromXYZW (quat);
		MD5EXPECT (")");
		GenMatrixPosQuat4Scale (pos, quat, scale, joints[j].loc);
	}
	MD5EXPECT ("}");

	bindabs = Mem_Alloc (numjoints * 12 * sizeof (*bindabs));
	for (j = 0; j < numjoints; j++)
	{
		if (joints[j].parent < 0)
			memcpy (bindabs + j * 12, joints[j].loc, 12 * sizeof (*bindabs));
		else
			R_ConcatTransforms ((float (*)[4])(bindabs + joints[j].parent * 12), (float (*)[4])joints[j].loc, (float (*)[4])(bindabs + j * 12));
	}

	if (Mod_LoadMD5Anim (fname, joints, numjoints, bindabs, &numposes, &poses))
		poses_owned = true;
	else
		poses = bindabs;

	if (numposes > MAXALIASFRAMES)
	{
		Con_Warning ("%s has %i animation frames, only the first %i are used\n", fname, (int)numposes, MAXALIASFRAMES);
		numposes = MAXALIASFRAMES;
	}

	meshes = Mem_Alloc (nummeshes * sizeof (*meshes));
	for (m = 0; m < nummeshes; m++)
	{
		md5meshtmp_t *mesh = &meshes[m];

		MD5EXPECT ("mesh");
		MD5EXPECT ("{");
		MD5EXPECT ("shader");
		q_strlcpy (mesh->shader, com_token, sizeof (mesh->shader));
		buffer = COM_Parse (buffer);

		MD5EXPECT ("numverts");
		mesh->numverts = MD5UINT ();
		if (mesh->numverts < 1 || mesh->numverts > 100000)
			MD5ERROR ("%s: bad vertex count\n", fname);
		mesh->verts = Mem_Alloc (mesh->numverts * sizeof (*mesh->verts));
		while (MD5CHECK ("vert"))
		{
			size_t idx = MD5UINT ();

			if (idx >= mesh->numverts)
				MD5ERROR ("%s: vertex index out of bounds\n", fname);
			MD5EXPECT ("(");
			mesh->verts[idx].st[0] = MD5FLOAT ();
			mesh->verts[idx].st[1] = MD5FLOAT ();
			MD5EXPECT (")");
			mesh->verts[idx].firstweight = (unsigned int)MD5UINT ();
			mesh->verts[idx].numweights = (unsigned int)MD5UINT ();
		}

		MD5EXPECT ("numtris");
		mesh->numtris = MD5UINT ();
		if (mesh->numtris > 100000)
			MD5ERROR ("%s: bad triangle count\n", fname);
		mesh->indices = Mem_Alloc (mesh->numtris * 3 * sizeof (*mesh->indices));
		while (MD5CHECK ("tri"))
		{
			size_t idx = MD5UINT ();

			if (idx >= mesh->numtris)
				MD5ERROR ("%s: triangle index out of bounds\n", fname);
			for (int k = 0; k < 3; k++)
			{
				size_t v = MD5UINT ();

				if (v >= mesh->numverts)
					MD5ERROR ("%s: vertex index out of bounds\n", fname);
				mesh->indices[idx * 3 + k] = (unsigned short)v;
			}
		}

		MD5EXPECT ("numweights");
		mesh->numweights = MD5UINT ();
		if (mesh->numweights > 1000000)
			MD5ERROR ("%s: bad weight count\n", fname);
		mesh->weights = Mem_Alloc (mesh->numweights * sizeof (*mesh->weights));
		while (MD5CHECK ("weight"))
		{
			size_t idx = MD5UINT ();

			if (idx >= mesh->numweights)
				MD5ERROR ("%s: weight index out of bounds\n", fname);
			mesh->weights[idx].joint = (unsigned int)MD5UINT ();
			if (mesh->weights[idx].joint >= numjoints)
				MD5ERROR ("%s: joint index out of bounds\n", fname);
			mesh->weights[idx].bias = (float)MD5FLOAT ();
			MD5EXPECT ("(");
			mesh->weights[idx].pos[0] = (float)MD5FLOAT ();
			mesh->weights[idx].pos[1] = (float)MD5FLOAT ();
			mesh->weights[idx].pos[2] = (float)MD5FLOAT ();
			MD5EXPECT (")");
		}

		MD5EXPECT ("}");

		if (mesh->numverts > 0xFFFF)
			MD5ERROR ("%s: too many vertices in a mesh\n", fname);

		totalverts += mesh->numverts;
		totalindices += mesh->numtris * 3;
	}

	hdrsize = sizeof (aliashdr_t) + (numposes - 1) * sizeof (((aliashdr_t *)0)->frames[0]);
	surfaces = (aliashdr_t *)Mem_Alloc (hdrsize * nummeshes);

	GLMesh_DeleteVertexBuffer (mod);
	mod->rtvertices = (QrVertex *)Mem_Alloc ((size_t)numposes * totalverts * sizeof (QrVertex));
	mod->rtindices = (uint32_t *)Mem_Alloc (totalindices * sizeof (uint32_t));

	mins[0] = mins[1] = mins[2] = FLT_MAX;
	maxs[0] = maxs[1] = maxs[2] = -FLT_MAX;

	for (m = 0; m < nummeshes; m++)
	{
		md5meshtmp_t *mesh = &meshes[m];
		aliashdr_t   *surf = (aliashdr_t *)((byte *)surfaces + m * hdrsize);
		gltexture_t  *tx;

		surf->nextsurface = (m + 1 < nummeshes) ? (aliashdr_t *)((byte *)surfaces + (m + 1) * hdrsize) : NULL;
		surf->poseverttype = PV_MD5;
		surf->numverts = (int)mesh->numverts;
		surf->numtris = (int)mesh->numtris;
		surf->numindexes = (int)(mesh->numtris * 3);
		surf->firstindex = (int)indexbase;
		surf->numindices = (int)(mesh->numtris * 3);
		surf->numframes = (int)numposes;
		surf->numposes = 1;
		for (int k = 0; k < 3; k++)
		{
			surf->scale[k] = 1.0f;
			surf->scale_origin[k] = 0.0f;
		}
		for (p = 0; p < numposes; p++)
		{
			surf->frames[p].firstpose = (int)p;
			surf->frames[p].numposes = 1;
			surf->frames[p].interval = 0.1f;
			q_snprintf (surf->frames[p].name, sizeof (surf->frames[p].name), "frame%i", (int)p);
		}

		tx = Mod_LoadEnhancedTexture (mod, mesh->shader);
		if (!tx)
			tx = notexture;
		surf->numskins = 1;
		for (int s = 0; s < MAX_SKINS; s++)
			for (int k = 0; k < 4; k++)
			{
				surf->gltextures[s][k] = tx;
				surf->fbtextures[s][k] = NULL;
			}

		for (size_t i = 0; i < mesh->numtris * 3; i++)
			mod->rtindices[indexbase + i] = (uint32_t)mesh->indices[i] + (uint32_t)vertbase;

		for (p = 0; p < numposes; p++)
		{
			const float *pose = poses + (size_t)p * numjoints * 12;
			QrVertex    *out = mod->rtvertices + (size_t)p * totalverts + vertbase;
			vec3_t       v;

			for (size_t vi = 0; vi < mesh->numverts; vi++)
			{
				const md5vertinfo_t *info = &mesh->verts[vi];
				float                acc[3] = {0, 0, 0};

				if (info->firstweight + info->numweights > mesh->numweights)
					MD5ERROR ("%s: weight index out of bounds\n", fname);

				for (unsigned int w = 0; w < info->numweights; w++)
				{
					const md5weight_t *weight = &mesh->weights[info->firstweight + w];
					const float       *mat = pose + (size_t)weight->joint * 12;

					acc[0] += weight->bias * (mat[0] * weight->pos[0] + mat[4] * weight->pos[1] + mat[8] * weight->pos[2] + mat[3]);
					acc[1] += weight->bias * (mat[1] * weight->pos[0] + mat[5] * weight->pos[1] + mat[9] * weight->pos[2] + mat[7]);
					acc[2] += weight->bias * (mat[2] * weight->pos[0] + mat[6] * weight->pos[1] + mat[10] * weight->pos[2] + mat[11]);
				}

				out[vi].position[0] = acc[0];
				out[vi].position[1] = acc[1];
				out[vi].position[2] = acc[2];
				out[vi].normal[0] = 0;
				out[vi].normal[1] = 0;
				out[vi].normal[2] = 0;
				out[vi].texCoord[0] = info->st[0];
				out[vi].texCoord[1] = info->st[1];
				out[vi].packedColor = RT_PACKED_COLOR_WHITE;
				out[vi].cluster = 0;
				out[vi].lightStyles = 0;

				for (int k = 0; k < 3; k++)
				{
					mins[k] = q_min (mins[k], out[vi].position[k]);
					maxs[k] = q_max (maxs[k], out[vi].position[k]);
				}
			}

			for (size_t t = 0; t < mesh->numtris; t++)
			{
				const unsigned short *tri = &mesh->indices[t * 3];
				vec3_t                e1, e2, n;

				VectorSubtract (out[tri[1]].position, out[tri[0]].position, e1);
				VectorSubtract (out[tri[2]].position, out[tri[0]].position, e2);
				CrossProduct (e1, e2, n);
				for (int k = 0; k < 3; k++)
				{
					VectorAdd (out[tri[k]].normal, n, out[tri[k]].normal);
				}
			}

			for (size_t vi = 0; vi < mesh->numverts; vi++)
			{
				float len2 = DotProduct (out[vi].normal, out[vi].normal);

				if (len2 > 0.0f)
				{
					float inv = 1.0f / sqrtf (len2);

					out[vi].normal[0] *= inv;
					out[vi].normal[1] *= inv;
					out[vi].normal[2] *= inv;
				}
				else
				{
					out[vi].normal[0] = 0;
					out[vi].normal[1] = 0;
					out[vi].normal[2] = 1;
				}

				v[0] = out[vi].position[0];
				v[1] = out[vi].position[1];
				v[2] = out[vi].position[2];
				float dist = v[0] * v[0] + v[1] * v[1];
				yawradius = q_max (yawradius, dist);
				radius = q_max (radius, dist + v[2] * v[2]);
			}
		}

		vertbase += mesh->numverts;
		indexbase += mesh->numtris * 3;
	}

	surfaces->numverts_vbo = (int)totalverts;
	surfaces->numposes = (int)numposes;
	surfaces->poseverts = (int)totalverts;
	surfaces->boundingradius = sqrtf (radius);

	mod->flags = 0;
	mod->type = mod_alias;
	mod->numframes = (int)numposes;
	mod->extradata = (byte *)surfaces;

	for (int k = 0; k < 3; k++)
	{
		mod->mins[k] = mins[k];
		mod->maxs[k] = maxs[k];
		mod->rmins[k] = -sqrtf (radius);
		mod->rmaxs[k] = sqrtf (radius);
	}
	mod->ymins[0] = mod->ymins[1] = -sqrtf (yawradius);
	mod->ymaxs[0] = mod->ymaxs[1] = sqrtf (yawradius);
	mod->ymins[2] = mins[2];
	mod->ymaxs[2] = maxs[2];

	for (m = 0; m < nummeshes; m++)
	{
		Mem_Free (meshes[m].verts);
		Mem_Free (meshes[m].weights);
		Mem_Free (meshes[m].indices);
	}
	Mem_Free (meshes);
	Mem_Free (joints);
	Mem_Free (bindabs);
	if (poses_owned)
		Mem_Free (poses);

	return true;

error:
	if (meshes)
	{
		for (m = 0; m < nummeshes; m++)
		{
			Mem_Free (meshes[m].verts);
			Mem_Free (meshes[m].weights);
			Mem_Free (meshes[m].indices);
		}
		Mem_Free (meshes);
	}
	Mem_Free (joints);
	Mem_Free (bindabs);
	if (poses_owned)
		Mem_Free (poses);
	Mem_Free (surfaces);
	GLMesh_DeleteVertexBuffer (mod);
	return false;
}

//=============================================================================

/*
=================
Mod_LoadSpriteFrame
=================
*/
static void *Mod_LoadSpriteFrame (qmodel_t *mod, byte *mod_base, void *pin, mspriteframe_t **ppframe, int framenum)
{
	dspriteframe_t *pinframe;
	mspriteframe_t *pspriteframe;
	int             width, height, size, origin[2];
	char            name[64];
	char            rtname[64];
	src_offset_t    offset; // johnfitz

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;

	pspriteframe = (mspriteframe_t *)Mem_Alloc (sizeof (mspriteframe_t));
	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];

	pspriteframe->smax = 1;
	pspriteframe->tmax = 1;

	q_snprintf (name, sizeof (name), "%s:frame%i", mod->name, framenum);
	q_snprintf (rtname, sizeof (rtname), "%s/%i", mod->name, framenum);

	offset = (src_offset_t)(pinframe + 1) - (src_offset_t)mod_base; // johnfitz
	pspriteframe->gltexture = TexMgr_LoadImage (
		rtname,
		mod, name, width, height, SRC_INDEXED, (byte *)(pinframe + 1), mod->name, offset,
		TEXPREF_PAD | TEXPREF_ALPHA | TEXPREF_NOPICMIP); // johnfitz -- TexMgr

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}

/*
=================
Mod_LoadSpriteGroup
=================
*/
static void *Mod_LoadSpriteGroup (qmodel_t *mod, byte *mod_base, void *pin, mspriteframe_t **ppframe, int framenum)
{
	dspritegroup_t    *pingroup;
	mspritegroup_t    *pspritegroup;
	int                i, numframes;
	dspriteinterval_t *pin_intervals;
	float			 *poutintervals;
	void			  *ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	pspritegroup = (mspritegroup_t *)Mem_Alloc (sizeof (mspritegroup_t) + (numframes - 1) * sizeof (pspritegroup->frames[0]));

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = (float *)Mem_Alloc (numframes * sizeof (float));

	pspritegroup->intervals = poutintervals;

	for (i = 0; i < numframes; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Sys_Error ("Mod_LoadSpriteGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i = 0; i < numframes; i++)
	{
		ptemp = Mod_LoadSpriteFrame (mod, mod_base, ptemp, &pspritegroup->frames[i], framenum * 100 + i);
	}

	return ptemp;
}

/*
=================
Mod_LoadSpriteModel
=================
*/
static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer)
{
	int                 i;
	int                 version;
	dsprite_t          *pin;
	msprite_t          *psprite;
	int                 numframes;
	int                 size;
	dspriteframetype_t *pframetype;

	pin = (dsprite_t *)buffer;
	byte *mod_base = (byte *)buffer; // johnfitz

	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		Sys_Error (
			"%s has wrong version number "
			"(%i should be %i)",
			mod->name, version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) + (numframes - 1) * sizeof (psprite->frames);

	psprite = (msprite_t *)Mem_Alloc (size);

	mod->extradata = (byte *)psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	psprite->beamlength = LittleFloat (pin->beamlength);
	mod->synctype = (synctype_t)LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = -psprite->maxwidth / 2;
	mod->maxs[0] = mod->maxs[1] = psprite->maxwidth / 2;
	mod->mins[2] = -psprite->maxheight / 2;
	mod->maxs[2] = psprite->maxheight / 2;

	//
	// load the frames
	//
	if (numframes < 1)
		Sys_Error ("Mod_LoadSpriteModel: Invalid # of frames: %d", numframes);

	mod->numframes = numframes;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i = 0; i < numframes; i++)
	{
		spriteframetype_t frametype;

		frametype = (spriteframetype_t)LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)Mod_LoadSpriteFrame (mod, mod_base, pframetype + 1, &psprite->frames[i].frameptr, i);
		}
		else
		{
			pframetype = (dspriteframetype_t *)Mod_LoadSpriteGroup (mod, mod_base, pframetype + 1, &psprite->frames[i].frameptr, i);
		}
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
================
Mod_Print
================
*/
void Mod_Print (void)
{
	int       i;
	qmodel_t *mod;

	Con_SafePrintf ("Cached models:\n"); // johnfitz -- safeprint instead of print
	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		Con_SafePrintf ("%8p : %s\n", mod->extradata, mod->name); // johnfitz -- safeprint instead of print
	}
	Con_Printf ("%i models\n", mod_numknown); // johnfitz -- print the total too
}
