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

// r_alias.c -- alias model rendering

#include "quakedef.h"
#include "rt_lights.h"

extern cvar_t r_drawflat, gl_fullbrights, r_lerpmodels, r_lerpmove, r_showtris; // johnfitz
extern cvar_t scr_fov;

extern cvar_t rt_model_rough, rt_model_metal, rt_enable_pvs;
extern cvar_t rt_viewm_fovscale, rt_viewm_wide, rt_viewm_scale, rt_viewm_normalize;
extern cvar_t rt_dlight_intensity, rt_dlight_radius;
extern cvar_t rt_cluster_dlights;

// up to 16 color translated skins
gltexture_t* playertextures[MAX_SCOREBOARD]; // johnfitz -- changed to an array of pointers

#define NUMVERTEXNORMALS 162

float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16

// johnfitz -- struct for passing lerp information to drawing functions
typedef struct
{
    short pose1;
    short pose2;
    float blend;
    vec3_t origin;
    vec3_t angles;
} lerpdata_t;

// johnfitz

void R_SetupAliasFrame (entity_t *e, aliashdr_t *paliashdr, int frame, lerpdata_t *lerpdata);
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata);

typedef struct
{
    float model_matrix[16];
    float shade_vector[3];
    float blend_factor;
    float light_color[3];
    float entalpha;
    unsigned int flags;
} aliasubo_t;

static const QrVertex* GetModelVerticesForPose(const qmodel_t* m, const aliashdr_t* hdr, int pose)
{
    assert(m != NULL && m->rtvertices != NULL);

    return &m->rtvertices[(size_t)pose * hdr->numverts_vbo];
}

static size_t GetNextAllocStep(size_t x)
{
    const size_t step = 4096;

    size_t i = (x + (step - 1)) / step;
    return i * step;
}

static float Lerp(float a, float b, float t)
{
    float dt = b - a;
    return a + dt * t;
}

static void LerpPosition(float* dst, const float* src1, const float* src2, float blend)
{
    for (int j = 0; j < 3; j++)
    {
        dst[j] = Lerp(src1[j], src2[j], blend);
    }
}

static const QrVertex*
GetPoseVertices(const qmodel_t* m, const aliashdr_t* hdr, int pose1, int pose2, float blend, int cluster)
{
    const QrVertex* v_pose1 = GetModelVerticesForPose(m, hdr, pose1);
    const QrVertex* v_pose2 = GetModelVerticesForPose(m, hdr, pose2);

    // we don't care about per-vertex colors with RT
    if (blend < FLT_EPSILON && cluster <= 0)
    {
        return v_pose1;
    }

    static QrVertex* tempstorage = NULL;
    static size_t tempstorage_numverts = 0;
    if ((size_t)hdr->numverts_vbo > tempstorage_numverts)
    {
        tempstorage_numverts = GetNextAllocStep(hdr->numverts_vbo);
        Mem_Free(tempstorage);
        tempstorage = Mem_Alloc(tempstorage_numverts * sizeof(QrVertex));
    }

    memcpy(tempstorage, v_pose1, hdr->numverts_vbo * sizeof(QrVertex));

    for (int i = 0; i < hdr->numverts_vbo; i++)
    {
        QrVertex* dst = &tempstorage[i];

        const QrVertex* src1 = &v_pose1[i];
        const QrVertex* src2 = &v_pose2[i];

        LerpPosition(dst->position, src1->position, src2->position, blend);

        if (cluster > 0)
            dst->cluster = (uint32_t)cluster;
    }

    return tempstorage;
}

typedef struct
{
    const char *model;
    float       factor;
} rt_viewm_norm_manual_t;

static const rt_viewm_norm_manual_t rt_viewm_norm_manual[] = {
    {"v_axe", 0.90f},
    {"v_shot", 1.15f},
    {"v_shot2", 1.20f},
    {"v_nail", 1.20f},
    {"v_nail2", 1.29f},
    {"v_rock", 1.1f},
    {"v_rock2", 1.05f},
    {"v_light", 0.95f},
};

static float RT_ViewmodelNormalizeManual (const char *modelname)
{
    float  factor = 0.0f;
    size_t match = 0;

    if (modelname == NULL)
        return 0.0f;

    for (size_t i = 0; i < sizeof (rt_viewm_norm_manual) / sizeof (rt_viewm_norm_manual[0]); i++)
    {
        const char *found = strstr (modelname, rt_viewm_norm_manual[i].model);

        if (found == NULL || rt_viewm_norm_manual[i].factor <= 0.0f)
            continue;

        const size_t length = strlen (rt_viewm_norm_manual[i].model);
        const char   boundary = found[length];

        if (boundary != '\0' && boundary != '.' && boundary != '_')
            continue;

        if (length > match)
        {
            match = length;
            factor = rt_viewm_norm_manual[i].factor;
        }
    }

    return factor;
}

#define RT_VIEWM_NORMALIZE_SPAN 0.64f

static float RT_ViewmodelProjectedSpan (const float mins[3], const float maxs[3], const float origin[3],
                                        const float scale[3], const float fov[2])
{
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;

    for (int corner = 0; corner < 8; corner++)
    {
        const float cx = (corner & 1) ? maxs[0] : mins[0];
        const float cy = (corner & 2) ? maxs[1] : mins[1];
        const float cz = (corner & 4) ? maxs[2] : mins[2];

        const float depth = origin[0] + scale[0] * cx;
        if (depth <= 0.5f)
            continue;

        const float u = (origin[1] + scale[1] * cy) * fov[0] / depth;
        const float v = (origin[2] + scale[2] * cz) * fov[1] / depth;

        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    if (umin > umax)
        return 0.0f;

    const float du = umax - umin;
    const float dv = vmax - vmin;

    return sqrtf (du * du + dv * dv);
}

static float RT_ViewmodelNormalizeScale (const aliashdr_t *hdr, const float mins[3], const float maxs[3],
                                         const float fovscalex, const float fovscaley)
{
    if (!(maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2]))
        return 1.0f;

    const float fov[2] = {fovscalex, fovscaley};
    const float span = RT_ViewmodelProjectedSpan (mins, maxs, hdr->scale_origin, hdr->scale, fov);

    if (!(span > 1e-4f))
        return 1.0f;

    return CLAMP (0.2f, RT_VIEWM_NORMALIZE_SPAN / span, 5.0f);
}

static QrTransform RT_GetAliasModelTransform(const aliashdr_t* paliashdr, lerpdata_t* lerpdata, qboolean isfirstperson,
                                             const char *modelname)
{
    float model_matrix[16];
    IdentityMatrix(model_matrix);
    R_RotateForEntity(model_matrix, lerpdata->origin, lerpdata->angles);

    float fovscalex = 1.0f;
    float fovscaley = 1.0f;
    if (isfirstperson && CVAR_TO_FLOAT(rt_viewm_fovscale) > 0)
    {
        fovscalex = CVAR_TO_FLOAT(rt_viewm_fovscale) * CVAR_TO_FLOAT(rt_viewm_wide);
        fovscaley = CVAR_TO_FLOAT(rt_viewm_fovscale);
    }

    // The model is scaled about its own zero here and the zero itself is moved
    // towards the eye by V_CalcRefdef, so the whole weapon is scaled about the eye.
    float viewmscale = 1.0f;
    if (isfirstperson && CVAR_TO_FLOAT(rt_viewm_scale) > 0)
    {
        viewmscale = CVAR_TO_FLOAT(rt_viewm_scale);
    }

    float sizescale = 1.0f;
    float center[3] = {0.0f, 0.0f, 0.0f};
    if (isfirstperson)
    {
        float mins[3], maxs[3];
        for (int axis = 0; axis < 3; axis++)
        {
            mins[axis] = paliashdr->frames[0].bboxmin.v[axis];
            maxs[axis] = paliashdr->frames[0].bboxmax.v[axis];
            center[axis] = 0.5f * (mins[axis] + maxs[axis]);
        }

        const float normalize = CLAMP(0.0f, CVAR_TO_FLOAT(rt_viewm_normalize), 1.0f);
        if (normalize > 0.0f)
        {
            const float manual = RT_ViewmodelNormalizeManual (modelname);
            const float factor = manual > 0.0f ? manual
                                               : RT_ViewmodelNormalizeScale(paliashdr, mins, maxs, fovscalex, fovscaley);

            sizescale = 1.0f + normalize * (factor - 1.0f);
        }
    }

    const float centerPull = 1.0f - sizescale;

    float translation_matrix[16];
    TranslationMatrix(translation_matrix,
                      viewmscale * (paliashdr->scale_origin[0] + centerPull * paliashdr->scale[0] * center[0]),
                      viewmscale * fovscalex * (paliashdr->scale_origin[1] + centerPull * paliashdr->scale[1] * center[1]),
                      viewmscale * fovscaley * (paliashdr->scale_origin[2] + centerPull * paliashdr->scale[2] * center[2]));
    MatrixMultiply(model_matrix, translation_matrix);

    float scale_matrix[16];
    ScaleMatrix(scale_matrix, paliashdr->scale[0] * viewmscale * sizescale,
                paliashdr->scale[1] * fovscalex * viewmscale * sizescale,
                paliashdr->scale[2] * fovscaley * viewmscale * sizescale);
    MatrixMultiply(model_matrix, scale_matrix);

    return RT_GetModelTransform(model_matrix);
}

/*
=============
GL_DrawAliasFrame -- ericw

Optimized alias model drawing codepath. This makes 1 draw call,
no vertex data is uploaded (it's already in the r_meshvbo and r_meshindexesvbo
static VBOs), and lerping and lighting is done in the vertex shader.

Supports optional fullbright pixels.

Based on code by MH from RMQEngine
=============
*/
static void GL_DrawAliasFrame(
    cb_context_t* cbx, entity_t* e, aliashdr_t* paliashdr, lerpdata_t lerpdata, gltexture_t* tx, float entity_alpha,
    qboolean alphatest, int entuniqueid)
{
    // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
    float blend = lerpdata.pose1 != lerpdata.pose2 ? lerpdata.blend : 0;

    qboolean rasterize = entity_alpha < 1.0f;
    qboolean isfirstperson = (e == &cl.viewent);
    qboolean isviewer = (e == &cl.entities[cl.viewentity]) && !CVAR_TO_BOOL(chase_active);
    rt_light_t *light_ov = tx ? RT_LIGHT_FindEmitter (tx->name, RT_GetAliasModelUniqueId (entuniqueid)) : NULL;

    if (tx && (tx->rtforcerasterize || (light_ov && light_ov->force_rasterize)))
        rasterize = true;

    int cluster = RT_ResolvePointCluster (lerpdata.origin);

    /* The transform is the one the geometry upload below uses, so the light of the model stands
       exactly where the model is drawn. The light reads the model's own per-pose vertices, not
       the shared lerp scratch GetPoseVertices hands the geometry uploads: widening the window in
       which those uploads read it would let the parallel entity passes overwrite each other's
       pose. */
    const QrTransform transform =
        RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson, e->model->name);

    /* DTAL: the model lights the scene from its own geometry when its material says it is a
       light and carries an emissive mask. The fake dlight stays as the fallback for everything
       the geometry path does not take (see RT_AddAliasEmissiveLights). The first-person weapon
       is left out: its geometry is the one carried towards the eye by the view-model scale, so
       a light built from it would ride the camera, light the room from inside the viewer and
       churn the cluster lists every frame; a weapon that lights the room keeps its light_color
       and the dlight below. */
    const int dtal_lights =
        isfirstperson ? 0
                      : RT_AddAliasEmissiveLights (e->model, tx, RT_GetAliasModelUniqueId (entuniqueid),
                                                   GetModelVerticesForPose (e->model, paliashdr, lerpdata.pose1),
                                                   GetModelVerticesForPose (e->model, paliashdr, lerpdata.pose2), blend,
                                                   paliashdr->numverts_vbo, e->model->rtindices, paliashdr->numindexes,
                                                   &transform);

    if (dtal_lights <= 0 && tx && tx->rthaslightcolor && tx->rtislight && RT_AllowFakeLights ())
    {
        rt_emitter_light_t light;

        memset (&light, 0, sizeof (light));
        light.name = tx->name;
        light.uniqueID = RT_GetAliasModelUniqueId (entuniqueid);
        light.kind = RT_LIGHT_KIND_MATERIAL;
        VectorCopy (lerpdata.origin, light.position);
        VectorCopy (tx->rtlightcolor, light.color);
        light.intensity = CVAR_TO_FLOAT (rt_dlight_intensity);
        light.radius = CVAR_TO_FLOAT (rt_dlight_radius);
        light.offset[2] = tx->rtupoffset;
        light.style = -1;

        RT_LIGHT_Emit (&light);
    }

assert(
    (!isviewer && !isfirstperson) ||
    (isviewer && !isfirstperson) ||
    (!isviewer && isfirstperson));

if
(rasterize)
{
    if (isviewer)
    {
        return;
    }

    QrRasterizedGeometryUploadInfo info = {
        .renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
        .vertexCount = paliashdr->numverts_vbo,
        .pVertices = GetPoseVertices(e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, cluster),
        .indexCount = paliashdr->numindexes,
        .pIndices = e->model->rtindices,
        .transform = transform,
        .color = RT_COLOR_WHITE,
        .material = tx ? tx->rtmaterial : QR_NO_MATERIAL,
        .pipelineState = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE,
        .blendFuncSrc = 0,
        .blendFuncDst = 0,
    };

    if (alphatest)
    {
        info.pipelineState |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
    }

    QrResult r = qrUploadRasterizedGeometry(vulkan_globals.instance, &info, NULL, NULL);
    QR_CHECK(r);
}

else
	{
		qboolean is_invis = (isfirstperson || isviewer) && (cl.items & IT_INVISIBILITY);
		qboolean exact_normals = tx ? tx->rtexactnormals : 0;

		QrGeometryUploadInfo info = {
			.uniqueID = RT_GetAliasModelUniqueId (entuniqueid),
			.flags =
			    (is_invis ? QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT : 0) |
			    ((tx && tx->rtalphatest) ? QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT : 0) |
			    (exact_normals ? QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT : QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT ),
			.geomType = QR_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType =
			    is_invis ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
			    // MF_HOLEY models (index 255 = transparent) must keep their alpha in the
			    // traced path too, where the alpha test runs in the any-hit shader.
			    alphatest ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED :
		        QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType =
			    isfirstperson ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON :
		        isviewer ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER :
		        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = paliashdr->numverts_vbo,
			.pVertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, cluster),
			.indexCount = paliashdr->numindexes,
			.pIndices = e->model->rtindices,
			.layerColors = {RT_COLOR_WHITE},
			.layerBlendingTypes = {QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
			.geomMaterial = {tx ? tx->rtmaterial : QR_NO_MATERIAL},
			.defaultRoughness = CVAR_TO_FLOAT(rt_model_rough),
			.defaultMetallicity = CVAR_TO_FLOAT(rt_model_metal),
			.defaultEmission = 0,
			.transform = transform,
		};

		QrResult r = qrUploadGeometry (vulkan_globals.instance, &info);
		QR_CHECK(r);
	}

Atomic_AddUInt32(&rs_aliaspasses, paliashdr->numtris);
}

static void R_DrawEnhancedModel (entity_t *e, aliashdr_t *paliashdr, int entuniqueid)
{
	lerpdata_t      lerpdata;
	float           blend, entalpha;
	uint64_t        baseid;
	uint32_t        surface_index = 0;
	qboolean        isfirstperson = (e == &cl.viewent);
	qboolean        isviewer = (e == &cl.entities[cl.viewentity]) && !CVAR_TO_BOOL (chase_active);
	const QrVertex *vertices;
	QrTransform     transform;

	R_SetupAliasFrame (e, paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (CVAR_TO_BOOL (rt_enable_pvs) && R_CullModelForEntity (e))
		return;

	if (r_lightmap_cheatsafe)
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE (e->alpha);
	if (entalpha == 0)
		return;

	Atomic_AddUInt32 (&rs_aliaspolys, paliashdr->numtris);

	blend = lerpdata.pose1 != lerpdata.pose2 ? lerpdata.blend : 0;
	int cluster = RT_ResolvePointCluster (lerpdata.origin);
	vertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, cluster);
	transform = RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson, e->model->name);
	baseid = RT_GetAliasModelUniqueId (entuniqueid);

	for (aliashdr_t *surf = paliashdr; surf; surf = surf->nextsurface, ++surface_index)
	{
		int          skinnum = e->skinnum;
		gltexture_t *tx;
		qboolean     rasterize = entalpha < 1.0f;
		qboolean     alphatest = !!(e->model->flags & MF_HOLEY);

		if (skinnum < 0 || skinnum >= surf->numskins)
			skinnum = 0;
		tx = surf->gltextures[skinnum][0];
		if (!tx)
			tx = notexture;
		if (r_lightmap_cheatsafe)
			tx = whitetexture;
		if (tx && tx->rtalphatest)
			alphatest = true;
		if (tx && tx->rtforcerasterize)
			rasterize = true;

		if (rasterize)
		{
			if (isviewer)
				continue;

			QrRasterizedGeometryUploadInfo info = {
				.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT,
				.vertexCount = paliashdr->numverts_vbo,
				.pVertices = vertices,
				.indexCount = surf->numindices,
				.pIndices = e->model->rtindices + surf->firstindex,
				.transform = transform,
				.color = RT_COLOR_WHITE,
				.material = tx ? tx->rtmaterial : QR_NO_MATERIAL,
				.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE,
				.blendFuncSrc = 0,
				.blendFuncDst = 0,
			};

			if (alphatest)
				info.pipelineState |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;

			QrResult r = qrUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
			QR_CHECK (r);
		}
		else
		{
			qboolean is_invis = (isfirstperson || isviewer) && (cl.items & IT_INVISIBILITY);
			qboolean exact_normals = tx ? tx->rtexactnormals : 0;

			QrGeometryUploadInfo info = {
				.uniqueID = baseid | ((uint64_t)surface_index << 32),
				.flags =
				    (is_invis ? QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT : 0) |
				    ((tx && tx->rtalphatest) ? QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT : 0) |
				    (exact_normals ? QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT : QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT),
				.geomType = QR_GEOMETRY_TYPE_DYNAMIC,
				.passThroughType =
				    is_invis ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
				    alphatest ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED :
				                QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
				.visibilityType =
				    isfirstperson ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON :
				    isviewer ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER :
				               QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
				.vertexCount = paliashdr->numverts_vbo,
				.pVertices = vertices,
				.indexCount = surf->numindices,
				.pIndices = e->model->rtindices + surf->firstindex,
				.layerColors = {RT_COLOR_WHITE},
				.layerBlendingTypes = {QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
				.geomMaterial = {tx ? tx->rtmaterial : QR_NO_MATERIAL},
				.defaultRoughness = CVAR_TO_FLOAT (rt_model_rough),
				.defaultMetallicity = CVAR_TO_FLOAT (rt_model_metal),
				.defaultEmission = 0,
				.transform = transform,
			};

			QrResult r = qrUploadGeometry (vulkan_globals.instance, &info);
			QR_CHECK (r);
		}
	}

	Atomic_AddUInt32 (&rs_aliaspasses, paliashdr->numtris);
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame(entity_t* e, aliashdr_t* paliashdr, int frame, lerpdata_t* lerpdata)
{
    int posenum, numposes;

    if ((frame >= paliashdr->numframes) || (frame < 0))
    {
        Con_DPrintf("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
        frame = 0;
    }

    posenum = paliashdr->frames[frame].firstpose;
    numposes = paliashdr->frames[frame].numposes;

    if (numposes > 1)
    {
        e->lerptime = paliashdr->frames[frame].interval;
        posenum += (int)(cl.time / e->lerptime) % numposes;
    }
    else
        e->lerptime = 0.1;

    if (e->lerpflags & LERP_RESETANIM) // kill any lerp in progress
    {
        e->lerpstart = 0;
        e->previouspose = posenum;
        e->currentpose = posenum;
        e->lerpflags -= LERP_RESETANIM;
    }
    else if (e->currentpose != posenum) // pose changed, start new lerp
    {
        if (e->lerpflags & LERP_RESETANIM2) // defer lerping one more time
        {
            e->lerpstart = 0;
            e->previouspose = posenum;
            e->currentpose = posenum;
            e->lerpflags -= LERP_RESETANIM2;
        }
        else
        {
            e->lerpstart = cl.time;
            e->previouspose = e->currentpose;
            e->currentpose = posenum;
        }
    }

    // set up values
    if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
    {
        if (e->lerpflags & LERP_FINISH && numposes == 1)
            lerpdata->blend = CLAMP(0, (cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1);
        else
            lerpdata->blend = CLAMP(0, (cl.time - e->lerpstart) / e->lerptime, 1);

        if (e->currentpose >= paliashdr->numposes || e->currentpose < 0)
        {
            Con_DPrintf("R_AliasSetupFrame: invalid current pose %d (%d total) for '%s'\n", e->currentpose,
                        paliashdr->numposes, e->model->name);
            e->currentpose = 0;
        }

        if (e->previouspose >= paliashdr->numposes || e->previouspose < 0)
        {
            Con_DPrintf("R_AliasSetupFrame: invalid prev pose %d (%d total) for '%s'\n", e->previouspose,
                        paliashdr->numposes, e->model->name);
            e->previouspose = e->currentpose;
        }

        lerpdata->pose1 = e->previouspose;
        lerpdata->pose2 = e->currentpose;
    }
    else // don't lerp
    {
        lerpdata->blend = 1;
        lerpdata->pose1 = posenum;
        lerpdata->pose2 = posenum;
    }
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform(entity_t* e, lerpdata_t* lerpdata)
{
    float blend;
    vec3_t d;
    int i;

    // if LERP_RESETMOVE, kill any lerps in progress
    if (e->lerpflags & LERP_RESETMOVE)
    {
        e->movelerpstart = 0;
        VectorCopy(e->origin, e->previousorigin);
        VectorCopy(e->origin, e->currentorigin);
        VectorCopy(e->angles, e->previousangles);
        VectorCopy(e->angles, e->currentangles);
        e->lerpflags -= LERP_RESETMOVE;
    }
    else if (!VectorCompare(e->origin, e->currentorigin) || !VectorCompare(e->angles, e->currentangles))
    // origin/angles changed, start new lerp
    {
        e->movelerpstart = cl.time;
        VectorCopy(e->currentorigin, e->previousorigin);
        VectorCopy(e->origin, e->currentorigin);
        VectorCopy(e->currentangles, e->previousangles);
        VectorCopy(e->angles, e->currentangles);
    }

    // set up values
    if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
    {
        if (e->lerpflags & LERP_FINISH)
            blend = CLAMP(0, (cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1);
        else
            blend = CLAMP(0, (cl.time - e->movelerpstart) / 0.1, 1);

        // translation
        VectorSubtract(e->currentorigin, e->previousorigin, d);
        lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
        lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
        lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

        // rotation
        VectorSubtract(e->currentangles, e->previousangles, d);
        for (i = 0; i < 3; i++)
        {
            if (d[i] > 180)
                d[i] -= 360;
            if (d[i] < -180)
                d[i] += 360;
        }
        lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
        lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
        lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
    }
    else // don't lerp
    {
        VectorCopy(e->origin, lerpdata->origin);
        VectorCopy(e->angles, lerpdata->angles);
    }
}

/*
=================
R_DrawAliasModel -- johnfitz -- almost completely rewritten
=================
*/
void R_DrawAliasModel(cb_context_t* cbx, entity_t* e, int entuniqueid)
{
    aliashdr_t* paliashdr;
    int anim, skinnum;
    gltexture_t* tx;
    lerpdata_t lerpdata;
    qboolean alphatest = !!(e->model->flags & MF_HOLEY);

    //
    // setup pose/lerp data -- do it first so we don't miss updates due to culling
    //
    paliashdr = (aliashdr_t*)Mod_Extradata(e->model);

    if (paliashdr->poseverttype != PV_QUAKE1)
    {
        R_DrawEnhancedModel (e, paliashdr, entuniqueid);
        return;
    }

    R_SetupAliasFrame(e, paliashdr, e->frame, &lerpdata);
    R_SetupEntityTransform(e, &lerpdata);

    //
    // cull it
    //
    if (CVAR_TO_BOOL(rt_enable_pvs))
    {
        if (R_CullModelForEntity(e))
            return;
    }

    //
    // set up for alpha blending
    //
    float entalpha;
    if (r_lightmap_cheatsafe)
        entalpha = 1;
    else
        entalpha = ENTALPHA_DECODE(e->alpha);
    if (entalpha == 0)
        return;

    Atomic_AddUInt32(&rs_aliaspolys, paliashdr->numtris);

    // The per-entity light trace is gone: nothing in the RT renderer reads the shade vector or the
    // light color it produced, and the cheatsafe modes only overrode that light color.

    //
    // set up textures
    //
    anim = (int)(cl.time * 10) & 3;
    skinnum = e->skinnum;
    if ((skinnum >= paliashdr->numskins) || (skinnum < 0))
    {
        Con_DPrintf("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e->model->name);
        // ericw -- display skin 0 for winquake compatibility
        skinnum = 0;
    }
    tx = paliashdr->gltextures[skinnum][anim];
    if (e->colormap != vid.colormap && !gl_nocolors.value)
        if ((uintptr_t)e >= (uintptr_t)&cl.entities[1] && (uintptr_t)e <= (uintptr_t)&cl.entities[cl.maxclients])
            tx = playertextures[e - cl.entities - 1];

    if (r_lightmap_cheatsafe)
    {
        tx = whitetexture;
    }

    if (tx && tx->rtalphatest)
        alphatest = true;

    //
    // draw it
    //
    GL_DrawAliasFrame(cbx, e, paliashdr, lerpdata, tx, entalpha, alphatest, entuniqueid);
}

// johnfitz -- values for shadow matrix
#define SHADOW_SKEW_X -0.7 // skew along x axis. -0.7 to mimic glquake shadows
#define SHADOW_SKEW_Y 0    // skew along y axis. 0 to mimic glquake shadows
#define SHADOW_VSCALE 0    // 0=completely flat
#define SHADOW_HEIGHT 0.1  // how far above the floor to render the shadow
// johnfitz

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris(cb_context_t* cbx, entity_t* e)
{
}
