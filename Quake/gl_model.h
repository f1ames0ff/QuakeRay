/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

#ifndef __MODEL__
#define __MODEL__

#include "modelgen.h"
#include "spritegn.h"
#include "atomics.h"

/*

d*_t structures are on-disk representations
m*_t structures are in-memory

*/

// entity effects

#define EF_BRIGHTFIELD     1
#define EF_MUZZLEFLASH     2
#define EF_BRIGHTLIGHT     4
#define EF_DIMLIGHT        8
#define EF_QEX_QUADLIGHT   16 // 2021 rerelease
#define EF_QEX_PENTALIGHT  32 // 2021 rerelease
#define EF_QEX_CANDLELIGHT 64 // 2021 rerelease

/*
==============================================================================

BRUSH MODELS

==============================================================================
*/

//
// in memory representation
//
// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	vec3_t position;
} mvertex_t;

#define SIDE_FRONT 0
#define SIDE_BACK  1
#define SIDE_ON    2

// plane_t structure
// !!! if this is changed, it must be changed in asm_i386.h too !!!
typedef struct mplane_s
{
	vec3_t normal;
	float  dist;
	byte   type;     // for texture axis selection and fast side tests
	byte   signbits; // signx + signy<<1 + signz<<1
	byte   pad[2];
} mplane_t;

// ericw -- each texture has two chains, so we can clear the model chains
//          without affecting the world
typedef enum
{
	chain_world,
	chain_model_0,
	chain_model_1,
	chain_model_2,
	chain_model_3,
	chain_model_4,
	chain_model_5,
	chain_alpha_model,
	chain_num,
} texchain_t;

typedef struct texture_s
{
	char                name[16];
	unsigned            width, height;
	unsigned            shift;                    // Q64
	struct gltexture_s *gltexture;                // johnfitz -- pointer to gltexture
	struct gltexture_s *fullbright;               // johnfitz -- fullbright mask texture; RT -- not used
	struct gltexture_s *warpimage;                // johnfitz -- for water animation
	atomic_uint32_t     update_warp;              // johnfitz -- update warp this frame
	struct msurface_s  *texturechains[chain_num]; // for texture chains
	uint32_t            chain_size[chain_num];    // for texture chains
	int                 anim_total;               // total tenths in sequence ( 0 = no)
	int                 anim_min, anim_max;       // time for this frame min <=time< max
	struct texture_s   *anim_next;                // in the animation sequence
	struct texture_s   *alternate_anims;          // bmodels in frmae 1 use these
	unsigned            offsets[MIPLEVELS];       // four mip maps stored
} texture_t;

#define SURF_PLANEBACK      2
#define SURF_DRAWSKY        4
#define SURF_DRAWSPRITE     8
#define SURF_DRAWTURB       0x10
#define SURF_DRAWTILED      0x20
#define SURF_DRAWBACKGROUND 0x40
#define SURF_UNDERWATER     0x80
#define SURF_NOTEXTURE      0x100 // johnfitz
#define SURF_DRAWFENCE      0x200
#define SURF_DRAWLAVA       0x400
#define SURF_DRAWSLIME      0x800
#define SURF_DRAWTELE       0x1000
#define SURF_DRAWWATER      0x2000

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct
{
	unsigned int v[2];
	unsigned int cachededgeoffset;
} medge_t;

typedef struct
{
	float      vecs[2][4];
	texture_t *texture;
	int        flags;
} mtexinfo_t;

#define VERTEXSIZE 7

typedef struct glpoly_s
{
	struct glpoly_s *next;
	int              numverts;
	float            verts[4][VERTEXSIZE]; // variable sized (xyz s1t1 s2t2)
} glpoly_t;

typedef struct msurface_s
{
	int visframe; // should be drawn when node is crossed

	mplane_t *plane;
	int       flags;

	int firstedge; // look up in model->surfedges[], negative numbers
	int numedges;  // are backwards edges

	short texturemins[2];
	short extents[2];

	int light_s, light_t; // gl lightmap coordinates

	glpoly_t          *polys; // multiple if warped
	struct msurface_s *texturechains[chain_num];

	mtexinfo_t *texinfo;

	int vbo_firstvert; // index of this surface's first vert in the VBO

	// lighting info
	int          dlightframe;
	unsigned int dlightbits[(MAX_DLIGHTS + 31) >> 5];
	// int is 32 bits, need an array for MAX_DLIGHTS > 32

	int      lightmaptexturenum;
	byte     styles[MAXLIGHTMAPS];
	int      cached_light[MAXLIGHTMAPS]; // values currently used in lightmap
	qboolean cached_dlight;              // true if dynamic light in cache
	byte    *samples;                    // [numstyles*surfsize]
} msurface_t;

typedef struct mnode_s
{
	// common with leaf
	int   contents;   // 0, to differentiate from leafs
	float minmaxs[6]; // for bounding box culling

	// node specific
	unsigned int    firstsurface;
	unsigned int    numsurfaces;
	mplane_t       *plane;
	struct mnode_s *children[2];
} mnode_t;

typedef struct mleaf_s
{
	// common with node
	int   contents;   // wil be a negative contents number
	float minmaxs[6]; // for bounding box culling

	// leaf specific
	int      nummarksurfaces;
	byte     ambient_sound_level[NUM_AMBIENTS];
	byte    *compressed_vis;
	int     *firstmarksurface;
	efrag_t *efrags;
} mleaf_t;

// johnfitz -- for clipnodes>32k
typedef struct mclipnode_s
{
	int planenum;
	int children[2]; // negative numbers are contents
} mclipnode_t;
// johnfitz

// !!! if this is changed, it must be changed in asm_i386.h too !!!
typedef struct
{
	mclipnode_t *clipnodes; // johnfitz -- was dclipnode_t
	mplane_t    *planes;
	int          firstclipnode;
	int          lastclipnode;
	vec3_t       clip_mins;
	vec3_t       clip_maxs;
} hull_t;

typedef float soa_aabb_t[2 * 3 * 8]; // 8 AABB's in SoA form
typedef float soa_plane_t[4 * 8];    // 8 planes in SoA form

/*
==============================================================================

SPRITE MODELS

==============================================================================
*/

// FIXME: shorten these?
typedef struct mspriteframe_s
{
	int                 width, height;
	float               up, down, left, right;
	float               smax, tmax; // johnfitz -- image might be padded
	struct gltexture_s *gltexture;
} mspriteframe_t;

typedef struct
{
	int             numframes;
	float		  *intervals;
	mspriteframe_t *frames[1];
} mspritegroup_t;

typedef struct
{
	spriteframetype_t type;
	mspriteframe_t   *frameptr;
} mspriteframedesc_t;

typedef struct
{
	int                type;
	int                maxwidth;
	int                maxheight;
	int                numframes;
	float              beamlength; // remove?
	void			  *cachespot;  // remove?
	mspriteframedesc_t frames[1];
} msprite_t;

/*
==============================================================================

ALIAS MODELS

Alias models are position independent, so the cache manager can move them.
==============================================================================
*/

//-- from RMQEngine
// split out to keep vertex sizes down
typedef struct aliasmesh_s
{
	float          st[2];
	unsigned short vertindex;
} aliasmesh_t;

#if 0
typedef struct meshxyz_s
{
	byte        xyz[4];
	signed char normal[4];
} meshxyz_t;

typedef struct meshst_s
{
	float st[2];
} meshst_t;
#endif
//--

typedef struct
{
	int        firstpose;
	int        numposes;
	float      interval;
	trivertx_t bboxmin;
	trivertx_t bboxmax;
	int        frame;
	char       name[16];
} maliasframedesc_t;

typedef struct
{
	trivertx_t bboxmin;
	trivertx_t bboxmax;
	int        frame;
} maliasgroupframedesc_t;

typedef struct
{
	int                    numframes;
	int                    intervals;
	maliasgroupframedesc_t frames[1];
} maliasgroup_t;

//=============================================================================

enum
{
	PV_QUAKE1 = 0,
	PV_QUAKE3,
	PV_MD5,
	PV_SIZE
};

#define MAX_SURFACES 32

#define MD3_VERSION  15
#define IDMD3HEADER  (('I' << 0) | ('D' << 8) | ('P' << 16) | ('3' << 24))
#define MD3_XYZ_SCALE (1.0f / 64.0f)

#define IDMD5HEADER (('M' << 0) + ('D' << 8) + ('5' << 16) + ('V' << 24))

typedef struct md3XyzNormal_s
{
	short xyz[3];
	short normal;
} md3XyzNormal_t;

typedef struct md3Header_s
{
	int  ident;
	int  version;
	char name[64];
	int  flags;
	int  numFrames;
	int  numTags;
	int  numSurfaces;
	int  numSkins;
	int  ofsFrames;
	int  ofsTags;
	int  ofsSurfaces;
	int  ofsEnd;
} md3Header_t;

typedef struct md3Frame_s
{
	float mins[3];
	float maxs[3];
	float localOrigin[3];
	float radius;
	char  name[16];
} md3Frame_t;

typedef struct md3Tag_s
{
	char  name[64];
	float origin[3];
	float axis[3][3];
} md3Tag_t;

typedef struct md3Surface_s
{
	int  ident;
	char name[64];
	int  flags;
	int  numFrames;
	int  numShaders;
	int  numVerts;
	int  numTriangles;
	int  ofsTriangles;
	int  ofsShaders;
	int  ofsSt;
	int  ofsXyzNormals;
	int  ofsEnd;
} md3Surface_t;

typedef struct md3Triangle_s
{
	int indexes[3];
} md3Triangle_t;

typedef struct md3St_s
{
	float s;
	float t;
} md3St_t;

typedef struct md3Shader_s
{
	char name[64];
	int  shaderIndex;
} md3Shader_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct mtriangle_s
{
	int facesfront;
	int vertindex[3];
} mtriangle_t;

#define MAX_SKINS 32
typedef struct aliashdr_s
{
	int        ident;
	int        version;
	vec3_t     scale;
	vec3_t     scale_origin;
	float      boundingradius;
	vec3_t     eyeposition;
	int        numskins;
	int        skinwidth;
	int        skinheight;
	int        numverts;
	int        numtris;
	int        numframes;
	synctype_t synctype;
	int        flags;
	float      size;

	// ericw -- used to populate vbo
	int      numverts_vbo; // number of verts with unique x,y,z,s,t
	intptr_t meshdesc;     // offset into extradata: numverts_vbo aliasmesh_t
	int      numindexes;
	intptr_t indexes;  // offset into extradata: numindexes unsigned shorts
	intptr_t vertexes; // offset into extradata: numposes*vertsperframe trivertx_t
	// ericw --

	int                 numposes;
	int                 poseverts;
	int                 posedata;                 // numposes*poseverts trivert_t
	int                 commands;                 // gl command list with embedded s/t
	struct gltexture_s *gltextures[MAX_SKINS][4]; // johnfitz
	struct gltexture_s *fbtextures[MAX_SKINS][4]; // johnfitz; RT - not used
	byte			   *texels[MAX_SKINS];        // only for player skins

	struct aliashdr_s  *nextsurface; // MD3: the next surface of the same model
	int                 poseverttype; // PV_QUAKE1 or PV_QUAKE3
	int                 firstindex;   // MD3: range into the model index buffer
	int                 numindices;

	maliasframedesc_t   frames[1];                // variable sized
} aliashdr_t;

#define MAXALIASVERTS  2000 // johnfitz -- was 1024
#define MAXALIASFRAMES 256
#define MAXALIASTRIS   4096 // ericw -- was 2048
extern aliashdr_t *pheader;
extern stvert_t    stverts[MAXALIASVERTS];
extern mtriangle_t triangles[MAXALIASTRIS];
extern trivertx_t *poseverts[MAXALIASFRAMES];

//===================================================================

//
// Whole model
//

typedef enum
{
	mod_brush,
	mod_sprite,
	mod_alias
} modtype_t;

#define EF_ROCKET  1          // leave a trail
#define EF_GRENADE 2          // leave a trail
#define EF_GIB     4          // leave a trail
#define EF_ROTATE  8          // rotate (bonus items)
#define EF_TRACER  16         // green split trail
#define EF_ZOMGIB  32         // small blood trail
#define EF_TRACER2 64         // orange split trail + rotate
#define EF_TRACER3 128        // purple trail
#define MF_HOLEY   (1u << 14) // MarkV/QSS -- make index 255 transparent on mdl's
#define MF_RT_LUMA (1u << 13)

// johnfitz -- extra flags for rendering
#define MOD_NOLERP       256  // don't lerp when animating
#define MOD_FBRIGHTHACK  1024 // when fullbrights are disabled, use a hack to render this model brighter
// johnfitz
// spike -- added this for particle stuff
#define MOD_EMITREPLACE  2048 // particle effect completely replaces the model (for flames or whatever).
#define MOD_EMITFORWARDS 4096 // particle effect is emitted forwards, rather than downwards. why down? good question.
// spike

struct glheap_s;
struct glheapnode_s;

typedef struct qmodel_s
{
	char         name[MAX_QPATH];
	unsigned int path_id;  // path id of the game directory
	                       // that this model came from
	qboolean     needload; // bmodels and sprites don't cache normally

	modtype_t  type;
	int        numframes;
	synctype_t synctype;

	int flags;

#ifdef PSET_SCRIPT
	int                   emiteffect;  // spike -- this effect is emitted per-frame by entities with this model
	int                   traileffect; // spike -- this effect is used when entities move
	struct skytris_s     *skytris;     // spike -- surface-based particle emission for this model
	struct skytriblock_s *skytrimem;   // spike -- surface-based particle emission for this model (for better cache performance+less allocs)
	double                skytime;     // doesn't really cope with multiples. oh well...
#endif
	//
	// volume occupied by the model graphics
	//
	vec3_t mins, maxs;
	vec3_t ymins, ymaxs; // johnfitz -- bounds for entities with nonzero yaw
	vec3_t rmins, rmaxs; // johnfitz -- bounds for entities with nonzero pitch or roll
	// johnfitz -- removed float radius;

	//
	// solid volume for clipping
	//
	qboolean clipbox;
	vec3_t   clipmins, clipmaxs;

	//
	// brush model
	//
	int firstmodelsurface, nummodelsurfaces;

	int       numsubmodels;
	dmodel_t *submodels;

	int       numplanes;
	mplane_t *planes;

	int      numleafs; // number of visible leafs, not counting 0
	mleaf_t *leafs;

	int        numvertexes;
	mvertex_t *vertexes;

	int      numedges;
	medge_t *edges;

	int      numnodes;
	mnode_t *nodes;

	int         numtexinfo;
	mtexinfo_t *texinfo;

	int         numsurfaces;
	msurface_t *surfaces;

	int  numsurfedges;
	int *surfedges;

	int          numclipnodes;
	mclipnode_t *clipnodes; // johnfitz -- was dclipnode_t

	int  nummarksurfaces;
	int *marksurfaces;

	soa_aabb_t  *soa_leafbounds;
	byte        *surfvis;
	soa_plane_t *soa_surfplanes;

	hull_t hulls[MAX_MAP_HULLS];

	int         numtextures;
	texture_t **textures;

	byte *visdata;
	int   visdatasize; // the lump holds the rows of the leafs end to end and nothing follows them
	byte *lightdata;
	char *entities;

	qboolean viswarn; // for Mod_DecompressVis()

	int bspversion;
	int contentstransparent; // spike -- added this so we can disable glitchy wateralpha where its not supported.
	qboolean nowatervis;

	//
	// alias model
	//
	uint32_t *rtindices;	// hdr->numindexes
	QrVertex *rtvertices;	// hdr->numposes * hdr->numverts_vbo
	struct rt_dtal_cache_s *rt_dtal;	// DTAL emissive pieces, see RT_AddAliasEmissiveLights
	struct rt_movable_model_s *rt_movable;

	//
	// additional model data
	//
	byte *extradata; // only access through Mod_Extradata

} qmodel_t;

//============================================================================

void      Mod_Init (void);
void      Mod_ClearAll (void);
void      Mod_ResetAll (void); // for gamedir changes (Host_Game_f)
qmodel_t *Mod_ForName (const char *name, qboolean crash);
void	 *Mod_Extradata (qmodel_t *mod); // handles caching
void      Mod_TouchModel (const char *name);

mleaf_t *Mod_PointInLeaf (float *p, qmodel_t *model);
byte    *Mod_LeafPVS (mleaf_t *leaf, qmodel_t *model);
byte    *Mod_NoVisPVS (qmodel_t *model);

void Mod_SetExtraFlags (qmodel_t *mod);

size_t   Mod_SanitizeMapDescription (char *dst, size_t dstsize, const char *src);
qboolean Mod_LoadMapDescription (char *desc, size_t maxchars, const char *map);

#endif // __MODEL__
