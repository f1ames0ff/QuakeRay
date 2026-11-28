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

extern cvar_t r_lerpmodels, r_lerpmove; // johnfitz
extern cvar_t scr_fov;

extern cvar_t rt_model_rough, rt_model_metal, rt_enable_pvs, rt_alias_persistent;
extern cvar_t rt_viewm_fovscale, rt_viewm_wide, rt_viewm_scale, rt_viewm_normalize;
extern cvar_t rt_upload_trace;

static uint32_t RT_AliasClusterOverride (int cluster)
{
	return cluster > 0 ? (uint32_t)cluster : 0u;
}

atomic_uint32_t rs_alias_enhanced_draws;
atomic_uint32_t rs_alias_pose_draws, rs_alias_pose_static, rs_alias_pose_repeat;

#define RT_ALIAS_POSECACHE_SIZE 2048
COMPILE_TIME_ASSERT (rt_alias_posecache_pow2, (RT_ALIAS_POSECACHE_SIZE & (RT_ALIAS_POSECACHE_SIZE - 1)) == 0);

typedef struct
{
	const entity_t *ent;
	const void     *model;
	int             pose1;
	qboolean        was_static;
} rt_alias_posecache_t;

float rt_viewmodel_depth_near = 0.0f;
float rt_viewmodel_depth_far = 0.0f;
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

static void LerpNormal(float* dst, const float* src1, const float* src2, float blend)
{
    float nx = Lerp(src1[0], src2[0], blend);
    float ny = Lerp(src1[1], src2[1], blend);
    float nz = Lerp(src1[2], src2[2], blend);
    const float len_sq = nx * nx + ny * ny + nz * nz;

    if (len_sq > 1e-12f)
    {
        const float inv_len = 1.0f / sqrtf(len_sq);
        dst[0] = nx * inv_len;
        dst[1] = ny * inv_len;
        dst[2] = nz * inv_len;
    }
    else
    {
        dst[0] = 0.0f;
        dst[1] = 0.0f;
        dst[2] = 1.0f;
    }
}

static const QrVertex*
GetPoseVertices(const qmodel_t* m, const aliashdr_t* hdr, int pose1, int pose2, float blend, int cluster)
{
    const double prof_pose = RT_Prof_Begin ();
    const QrVertex* v_pose1 = GetModelVerticesForPose(m, hdr, pose1);
    const QrVertex* v_pose2 = GetModelVerticesForPose(m, hdr, pose2);

    // we don't care about per-vertex colors with RT
    if (blend < FLT_EPSILON && cluster <= 0)
    {
        RT_Prof_End (RT_PROF_ALIAS_POSE, prof_pose);
        return v_pose1;
    }

    static THREAD_LOCAL QrVertex* tempstorage = NULL;
    static THREAD_LOCAL size_t tempstorage_numverts = 0;
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
        LerpNormal(dst->normal, src1->normal, src2->normal, blend);

        if (cluster > 0)
            dst->cluster = (uint32_t)cluster;
    }

    RT_Prof_End (RT_PROF_ALIAS_POSE, prof_pose);
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
                                             const qmodel_t *model)
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
        }

        if (!(maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2]))
        {
            for (int axis = 0; axis < 3; axis++)
            {
                mins[axis] = model->mins[axis];
                maxs[axis] = model->maxs[axis];
            }
        }

        for (int axis = 0; axis < 3; axis++)
            center[axis] = 0.5f * (mins[axis] + maxs[axis]);

        if (maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2])
        {
            const float normalize = CLAMP(0.0f, CVAR_TO_FLOAT(rt_viewm_normalize), 1.0f);
            if (normalize > 0.0f)
            {
                const float manual = RT_ViewmodelNormalizeManual (model->name);
                const float factor = manual > 0.0f ? manual
                                                   : RT_ViewmodelNormalizeScale(paliashdr, mins, maxs, fovscalex, fovscaley);

                sizescale = 1.0f + normalize * (factor - 1.0f);
            }
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

    if (isfirstperson)
    {
        float mins[3];
        float maxs[3];

        for (int axis = 0; axis < 3; axis++)
        {
            mins[axis] = paliashdr->frames[0].bboxmin.v[axis];
            maxs[axis] = paliashdr->frames[0].bboxmax.v[axis];
        }

        if (!(maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2]))
        {
            for (int axis = 0; axis < 3; axis++)
            {
                mins[axis] = model->mins[axis];
                maxs[axis] = model->maxs[axis];
            }
        }

        float minDepth = 1e30f;
        float maxDepth = -1e30f;

        for (int corner = 0; corner < 8; corner++)
        {
            const float cx = (corner & 1) ? maxs[0] : mins[0];
            const float cy = (corner & 2) ? maxs[1] : mins[1];
            const float cz = (corner & 4) ? maxs[2] : mins[2];

            const float world[3] = {
                model_matrix[0] * cx + model_matrix[4] * cy + model_matrix[8] * cz + model_matrix[12],
                model_matrix[1] * cx + model_matrix[5] * cy + model_matrix[9] * cz + model_matrix[13],
                model_matrix[2] * cx + model_matrix[6] * cy + model_matrix[10] * cz + model_matrix[14],
            };
            const float depth = (world[0] - r_refdef.vieworg[0]) * vpn[0] + (world[1] - r_refdef.vieworg[1]) * vpn[1] +
                                (world[2] - r_refdef.vieworg[2]) * vpn[2];

            if (depth < minDepth)
                minDepth = depth;
            if (depth > maxDepth)
                maxDepth = depth;
        }

        if (maxDepth > minDepth)
        {
            rt_viewmodel_depth_near = minDepth;
            rt_viewmodel_depth_far = maxDepth;
        }
    }

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
// The physical radius of the drawn model: half the diagonal of the pose's
// bounding box, in the metric units a light radius is measured in. An alias
// model's frames are animation groups and pose1 is a pose inside one, so the
// group that holds the pose names the box and the model's own scale is applied;
// a model whose frame boxes are degenerate falls back to its bounding box.
static float RT_AliasModelLightSize(const aliashdr_t *paliashdr, const lerpdata_t *lerpdata, const qmodel_t *model)
{
    float size[3];

    if (paliashdr->numframes > 0)
    {
        float mins[3], maxs[3];
        int   frame = CLAMP(0, lerpdata->pose1, paliashdr->numframes - 1);

        for (int i = 0; i < paliashdr->numframes; i++)
        {
            if (lerpdata->pose1 >= paliashdr->frames[i].firstpose &&
                lerpdata->pose1 < paliashdr->frames[i].firstpose + paliashdr->frames[i].numposes)
            {
                frame = i;
                break;
            }
        }

        for (int axis = 0; axis < 3; axis++)
        {
            mins[axis] = paliashdr->frames[frame].bboxmin.v[axis];
            maxs[axis] = paliashdr->frames[frame].bboxmax.v[axis];
        }

        if (maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2])
        {
            for (int axis = 0; axis < 3; axis++)
                size[axis] = (maxs[axis] - mins[axis]) * fabsf(paliashdr->scale[axis]);

            return QUAKEUNIT_TO_METRIC(0.5f * sqrtf(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]));
        }
    }

    for (int axis = 0; axis < 3; axis++)
        size[axis] = model->maxs[axis] - model->mins[axis];

    return QUAKEUNIT_TO_METRIC(0.5f * sqrtf(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]));
}

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

    if (rt_upload_trace.value != 0)
    {
        /* Sizing probe: an alias upload can reuse the previous frame's vertices only when it
           uploads the raw pose block (no lerp, no forced cluster) and that block did not move
           since the previous draw of the same entity. */

        static THREAD_LOCAL rt_alias_posecache_t stride_posecache[RT_ALIAS_POSECACHE_SIZE];
        const qboolean pose_static = (blend < FLT_EPSILON);
        const size_t   slot = ((size_t)e / sizeof (void *)) & (RT_ALIAS_POSECACHE_SIZE - 1);
        rt_alias_posecache_t *ce = &stride_posecache[slot];
        const qboolean repeat = pose_static && ce->was_static && ce->ent == e && ce->model == e->model && ce->pose1 == lerpdata.pose1;

        Atomic_AddUInt32 (&rs_alias_pose_draws, 1);
        if (pose_static)
            Atomic_AddUInt32 (&rs_alias_pose_static, 1);
        if (repeat)
            Atomic_AddUInt32 (&rs_alias_pose_repeat, 1);

        ce->ent = e;
        ce->model = e->model;
        ce->pose1 = lerpdata.pose1;
        ce->was_static = pose_static;
    }

    /* The transform is the one the geometry upload below uses, so the light of the model stands
       exactly where the model is drawn. The light reads the model's own per-pose vertices, not
       the shared lerp scratch GetPoseVertices hands the geometry uploads: widening the window in
       which those uploads read it would let the parallel entity passes overwrite each other's
       pose. */
    const QrTransform transform =
        RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson, e->model);

    /* DTAL: the model lights the scene from its own geometry when its material says it is a
       light and carries an emissive mask. The fake dlight stays as the fallback for everything
       the geometry path does not take (see RT_AddAliasEmissiveLights). The first-person weapon
       is left out: its geometry is the one carried towards the eye by the view-model scale, so
       a light built from it would ride the camera, light the room from inside the viewer and
       churn the cluster lists every frame; a weapon that lights the room keeps its light_color
       and the dlight below. */
    const double prof_lights = RT_Prof_Begin ();
    qboolean dtal_qualifies = false;

    if (!isfirstperson)
        RT_AddAliasEmissiveLights (e->model, tx, RT_GetAliasModelUniqueId (entuniqueid),
                                   GetModelVerticesForPose (e->model, paliashdr, lerpdata.pose1),
                                   GetModelVerticesForPose (e->model, paliashdr, lerpdata.pose2), blend,
                                   paliashdr->numverts_vbo, e->model->rtindices, paliashdr->numindexes,
                                   &transform, &dtal_qualifies);

    if (!dtal_qualifies && tx && tx->rthaslightcolor && tx->rtislight && RT_AllowFakeLights ())
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
        light.modelSize = RT_AliasModelLightSize (paliashdr, &lerpdata, e->model);
        light.style = -1;

        RT_LIGHT_Emit (&light);
    }
    RT_Prof_End (RT_PROF_ALIAS_LIGHTS, prof_lights);

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
        .texCoordScale = {tx ? tx->rtuvscale[0] : 1.0f, tx ? tx->rtuvscale[1] : 1.0f},
        .texCoordOffset = {tx ? tx->rtuvoffset[0] : 0.0f, tx ? tx->rtuvoffset[1] : 0.0f},
        .material = tx ? RT_RASTER_MATERIAL (tx) : QR_NO_MATERIAL,
        .pipelineState = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE |
                         (RT_UV_TRANSFORM_ACTIVE (tx) ? QR_RASTERIZED_GEOMETRY_STATE_TEXCOORD_TRANSFORM : 0),
        .blendFuncSrc = 0,
        .blendFuncDst = 0,
    };

    if (alphatest)
    {
        info.pipelineState |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
    }

    const double prof_upload = RT_Prof_Begin ();
    QrResult r = qrUploadRasterizedGeometry(vulkan_globals.instance, &info, NULL, NULL);
    RT_Prof_End (RT_PROF_ALIAS_UPLOAD, prof_upload);
    QR_CHECK(r);
}

else
	{
		qboolean is_invis = (isfirstperson || isviewer) && (cl.items & IT_INVISIBILITY);
		qboolean is_glass = tx && tx->rtglass;
		qboolean is_watermat = tx && tx->rtwater;
		qboolean exact_normals = tx ? tx->rtexactnormals : 0;

		QrGeometryUploadInfo info = {
			.uniqueID = RT_GetAliasModelUniqueId (entuniqueid),
			.flags =
			    (is_invis ? QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT : 0) |
			    (tx && tx->rtalphatest ? QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT : 0) |
			    (is_glass && alphatest ? QR_GEOMETRY_UPLOAD_GLASS_CUTOUT_BIT : 0) |
			    (RT_UV_TRANSFORM_ACTIVE (tx) ? QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT : 0) |
			    (exact_normals ? QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT : 0),
			.geomType = QR_GEOMETRY_TYPE_DYNAMIC,
			.passThroughType =
			    is_invis ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
			    is_glass ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
			    is_watermat ? QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT :
			    // MF_HOLEY models (index 255 = transparent) must keep their alpha in the
			    // traced path too, where the alpha test runs in the any-hit shader.
			    alphatest ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED :
		        QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
			.visibilityType =
			    isfirstperson ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON :
		        isviewer ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER :
		        QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
			.vertexCount = paliashdr->numverts_vbo,
			.pVertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, 0),
			.indexCount = paliashdr->numindexes,
			.pIndices = e->model->rtindices,
			.layerColors = {RT_COLOR_WHITE, {is_glass && tx ? tx->rtglasscolor[0] : 0.0f, is_glass && tx ? tx->rtglasscolor[1] : 0.0f, is_glass && tx ? tx->rtglasscolor[2] : 0.0f, 0.0f},
			                {is_glass && tx ? tx->rtglassior : 0.0f,
			                 is_glass && tx ? tx->rtglassthickness : 0.0f, 0.0f, 0.0f}},
			.layerBlendingTypes = {QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
			.texCoordScale = {tx ? tx->rtuvscale[0] : 1.0f, tx ? tx->rtuvscale[1] : 1.0f},
			.texCoordOffset = {tx ? tx->rtuvoffset[0] : 0.0f, tx ? tx->rtuvoffset[1] : 0.0f},
			.geomMaterial = {tx ? tx->rtmaterial : QR_NO_MATERIAL, is_glass && tx ? tx->rtoverlaymaterial : QR_NO_MATERIAL},
			.defaultRoughness = CVAR_TO_FLOAT(rt_model_rough),
			.defaultMetallicity = CVAR_TO_FLOAT(rt_model_metal),
			.defaultEmission = 0,
			.transform = transform,
			.clusterOverride = RT_AliasClusterOverride (cluster),
		};

		TexMgr_FillSubsurfaceUpload (tx, &info);

		const double prof_upload = RT_Prof_Begin ();
		qrUploadLanePush (vulkan_globals.instance, QR_UPLOAD_LANE_ALIAS);
		QrResult r = qrUploadGeometry (vulkan_globals.instance, &info);
		qrUploadLanePop (vulkan_globals.instance);
		RT_Prof_End (RT_PROF_ALIAS_UPLOAD, prof_upload);
		QR_CHECK(r);
	}

Atomic_AddUInt32(&rs_aliaspasses, paliashdr->numtris);
}

static void R_DrawEnhancedModel (entity_t *e, aliashdr_t *paliashdr, int entuniqueid)
{
	Atomic_AddUInt32 (&rs_alias_enhanced_draws, 1);

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
	vertices = GetPoseVertices (e->model, paliashdr, lerpdata.pose1, lerpdata.pose2, blend, 0);
	transform = RT_GetAliasModelTransform (paliashdr, &lerpdata, isfirstperson, e->model);

	if (isfirstperson)
	{
		float minDepth = 1e30f;
		float maxDepth = -1e30f;

		for (int v = 0; v < paliashdr->numverts_vbo; v++)
		{
			const float *position = vertices[v].position;
			const float  world[3] = {
			    transform.matrix[0][0] * position[0] + transform.matrix[0][1] * position[1] +
			        transform.matrix[0][2] * position[2] + transform.matrix[0][3],
			    transform.matrix[1][0] * position[0] + transform.matrix[1][1] * position[1] +
			        transform.matrix[1][2] * position[2] + transform.matrix[1][3],
			    transform.matrix[2][0] * position[0] + transform.matrix[2][1] * position[1] +
			        transform.matrix[2][2] * position[2] + transform.matrix[2][3],
			};
			const float depth = (world[0] - r_refdef.vieworg[0]) * vpn[0] + (world[1] - r_refdef.vieworg[1]) * vpn[1] +
			                    (world[2] - r_refdef.vieworg[2]) * vpn[2];

			if (depth < minDepth)
				minDepth = depth;
			if (depth > maxDepth)
				maxDepth = depth;
		}

		if (maxDepth > minDepth)
		{
			rt_viewmodel_depth_near = minDepth;
			rt_viewmodel_depth_far = maxDepth;
		}
	}

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
				.vertexCount = (uint32_t)surf->numverts,
				.pVertices = vertices + surf->firstvertex_vbo,
				.indexCount = surf->numindices,
				.pIndices = e->model->rtindices + surf->firstindex,
				.transform = transform,
				.color = RT_COLOR_WHITE,
				.texCoordScale = {tx ? tx->rtuvscale[0] : 1.0f, tx ? tx->rtuvscale[1] : 1.0f},
				.texCoordOffset = {tx ? tx->rtuvoffset[0] : 0.0f, tx ? tx->rtuvoffset[1] : 0.0f},
				.material = tx ? RT_RASTER_MATERIAL (tx) : QR_NO_MATERIAL,
				.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE |
				                 (RT_UV_TRANSFORM_ACTIVE (tx) ? QR_RASTERIZED_GEOMETRY_STATE_TEXCOORD_TRANSFORM : 0),
				.blendFuncSrc = 0,
				.blendFuncDst = 0,
			};

			if (alphatest)
				info.pipelineState |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;

			const double prof_upload = RT_Prof_Begin ();
			QrResult r = qrUploadRasterizedGeometry (vulkan_globals.instance, &info, NULL, NULL);
			RT_Prof_End (RT_PROF_ALIAS_UPLOAD, prof_upload);
			QR_CHECK (r);
		}
		else
		{
			qboolean is_invis = (isfirstperson || isviewer) && (cl.items & IT_INVISIBILITY);
			qboolean is_glass = tx && tx->rtglass;
			qboolean is_watermat = tx && tx->rtwater;
			qboolean exact_normals = tx ? tx->rtexactnormals : 0;

			QrGeometryUploadInfo info = {
				.uniqueID = baseid | ((uint64_t)surface_index << 32),
				.flags =
				    (is_invis ? QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT : 0) |
				    ((tx && tx->rtalphatest) ? QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT : 0) |
				    (is_glass && alphatest ? QR_GEOMETRY_UPLOAD_GLASS_CUTOUT_BIT : 0) |
				    (RT_UV_TRANSFORM_ACTIVE (tx) ? QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT : 0) |
				    (exact_normals ? QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT : 0),
				.geomType = QR_GEOMETRY_TYPE_DYNAMIC,
				.passThroughType =
				    is_invis ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
				    is_glass ? QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT :
				    is_watermat ? QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT :
				    alphatest ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED :
				                QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
				.visibilityType =
				    isfirstperson ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON :
				    isviewer ? QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER :
				               QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
				.vertexCount = (uint32_t)surf->numverts,
				.pVertices = vertices + surf->firstvertex_vbo,
				.indexCount = surf->numindices,
				.pIndices = e->model->rtindices + surf->firstindex,
				.layerColors = {RT_COLOR_WHITE, {is_glass && tx ? tx->rtglasscolor[0] : 0.0f, is_glass && tx ? tx->rtglasscolor[1] : 0.0f, is_glass && tx ? tx->rtglasscolor[2] : 0.0f, 0.0f},
				                {is_glass && tx ? tx->rtglassior : 0.0f,
				                 is_glass && tx ? tx->rtglassthickness : 0.0f, 0.0f, 0.0f}},
				.layerBlendingTypes = {QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
				.texCoordScale = {tx ? tx->rtuvscale[0] : 1.0f, tx ? tx->rtuvscale[1] : 1.0f},
				.texCoordOffset = {tx ? tx->rtuvoffset[0] : 0.0f, tx ? tx->rtuvoffset[1] : 0.0f},
				.geomMaterial = {tx ? tx->rtmaterial : QR_NO_MATERIAL, is_glass && tx ? tx->rtoverlaymaterial : QR_NO_MATERIAL},
				.defaultRoughness = CVAR_TO_FLOAT (rt_model_rough),
				.defaultMetallicity = CVAR_TO_FLOAT (rt_model_metal),
				.defaultEmission = 0,
				.transform = transform,
				.clusterOverride = RT_AliasClusterOverride (cluster),
			};

			TexMgr_FillSubsurfaceUpload (tx, &info);

			const double prof_upload = RT_Prof_Begin ();
			qrUploadLanePush (vulkan_globals.instance, QR_UPLOAD_LANE_ALIAS_SURF);
			QrResult r = qrUploadGeometry (vulkan_globals.instance, &info);
			qrUploadLanePop (vulkan_globals.instance);
			RT_Prof_End (RT_PROF_ALIAS_UPLOAD, prof_upload);
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

#define RT_STATIC_ALIAS_MAX_GEOMS 4096
#define RT_STATIC_ALIAS_SETTLE_FRAMES 30
#define RT_STATIC_ALIAS_SETTLE_SECONDS 1.5
#define RT_STATIC_ALIAS_EDICT_MIN_PROMOTE 64

typedef struct
{
	const qmodel_t *model;
	int             frame;
	int             skinnum;
	int             pose1;
	byte            alpha;
	int             effects;
	vec3_t          origin;
	vec3_t          angles;
	double          settled_since;
	uint32_t        vis_gen;
	uint16_t        settled_frames;
	qboolean        never_promote;
	qboolean        promoted;
	qboolean        eligible;
} rt_alias_track_t;

static uint32_t          rt_static_alias_generation = 1u;
static int               rt_static_alias_num_statics = -1;
static qboolean          rt_static_alias_drawentities = false;
static qboolean          rt_static_alias_lightmap = false;
static double            rt_static_alias_last_cl_time = 0.0;
static uint32_t          rt_alias_vis_gen = 1u;
static rt_alias_track_t *rt_alias_track = NULL;
static int               rt_alias_track_cap = 0;
static int               rt_alias_track_count = 0;

static void RT_StaticAliasZeroTrackRange (rt_alias_track_t *tracks, int count)
{
	static const rt_alias_track_t empty_track = {0};

	for (int i = 0; i < count; i++)
		tracks[i] = empty_track;
}

static void RT_StaticAliasResetTracking (void)
{
	if (rt_alias_track && rt_alias_track_count > 0)
	{
		for (int i = 1; i < rt_alias_track_count && i < cl.num_entities; i++)
			cl.entities[i].rt_static_alias_gen = 0u;
		RT_StaticAliasZeroTrackRange (rt_alias_track, rt_alias_track_cap);
	}
	rt_alias_track_count = 0;
	rt_static_alias_last_cl_time = 0.0;
}

void RT_StaticAliasClear (void)
{
	if (++rt_static_alias_generation == 0u)
		rt_static_alias_generation = 1u;
	rt_static_alias_num_statics = -1;
	RT_StaticAliasResetTracking ();
}

static qboolean RT_StaticAliasIsEntityValid (const entity_t *e)
{
	if (!e || !e->model || e->model->type != mod_alias || e->model->needload)
		return false;
	if (!e->model->rtvertices || !e->model->rtindices)
		return false;
	if (e->eflags & EFLAGS_EXTERIORMODEL)
		return false;
	if (ENTALPHA_DECODE (e->alpha) < 1.0f)
		return false;
	if ((e->effects & (EF_BRIGHTFIELD | EF_MUZZLEFLASH | EF_DIMLIGHT | EF_BRIGHTLIGHT)) != 0)
		return false;
	return (((e->effects >> 24) | e->model->flags) & EF_ROTATE) == 0;
}

static qboolean RT_StaticAliasIsLerpSettled (const entity_t *e, int posenum)
{
	if (e->lerpflags & (LERP_RESETANIM | LERP_RESETANIM2 | LERP_RESETMOVE))
		return false;
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		double dur = (e->lerpflags & LERP_FINISH) ? (e->lerpfinish - e->lerpstart) : 0.1;
		if (e->currentpose != posenum || e->previouspose != posenum || cl.time < e->lerpstart + dur)
			return false;
	}
	if (r_lerpmove.value && (e->lerpflags & LERP_MOVESTEP))
	{
		double dur = (e->lerpflags & LERP_FINISH) ? (e->lerpfinish - e->lerpstart) : 0.1;
		if (cl.time < e->movelerpstart + dur)
			return false;
	}
	return true;
}

static gltexture_t *RT_StaticAliasFindStaticSkin (int unique_index, const aliashdr_t *hdr, int skinnum)
{
	int resolved_skin = (skinnum < 0 || skinnum >= hdr->numskins) ? 0 : skinnum;
	gltexture_t *tx = hdr->gltextures[resolved_skin][0];
	if (!tx)
		return NULL;
	for (int k = 1; k < 4; k++)
	{
		if (hdr->gltextures[resolved_skin][k] != tx)
			return NULL;
	}

	const uint64_t unique_id = RT_GetAliasModelUniqueId (unique_index);
	rt_light_t    *light_ov = RT_LIGHT_FindEmitter (tx->name, unique_id);
	if (tx->rtforcerasterize || (light_ov && light_ov->force_rasterize))
		return NULL;
	if (tx->rtislight || tx->rthaslightcolor || light_ov != NULL || RT_AliasTextureCanEmitLight (tx))
		return NULL;

	return tx;
}

/* Extracts static-promotion properties for an opaque, non-animated, non-emissive alias entity. */
static qboolean RT_StaticAliasExtractProps (int unique_index, const entity_t *e, aliashdr_t **out_hdr,
                                            int *out_pose1, gltexture_t **out_tx, qboolean check_lerp_settled)
{
	if (!RT_StaticAliasIsEntityValid (e))
		return false;

	aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (e->model);
	if (!hdr || hdr->poseverttype != PV_QUAKE1 || hdr->numverts_vbo <= 0 || hdr->numindexes < 3)
		return false;

	int frame = e->frame;
	if (frame < 0 || frame >= hdr->numframes || hdr->frames[frame].numposes != 1)
		return false;

	int posenum = hdr->frames[frame].firstpose;
	if (check_lerp_settled && !RT_StaticAliasIsLerpSettled (e, posenum))
		return false;

	gltexture_t *tx = RT_StaticAliasFindStaticSkin (unique_index, hdr, e->skinnum);
	if (!tx)
		return false;

	*out_hdr = hdr;
	*out_pose1 = posenum;
	*out_tx = tx;
	return true;
}

static void RT_StaticAliasEnsureTrackCapacity (int count)
{
	if (count <= rt_alias_track_cap)
		return;

	int new_cap = rt_alias_track_cap > 0 ? rt_alias_track_cap : 256;
	while (new_cap < count)
		new_cap *= 2;

	rt_alias_track_t *grown = (rt_alias_track_t *)Mem_Realloc (rt_alias_track, sizeof (*rt_alias_track) * (size_t)new_cap);
	if (!grown)
		return;

	RT_StaticAliasZeroTrackRange (grown + rt_alias_track_cap, new_cap - rt_alias_track_cap);
	rt_alias_track = grown;
	rt_alias_track_cap = new_cap;
}

static qboolean RT_StaticAliasTrackReady (const rt_alias_track_t *tr)
{
	if (tr->never_promote || !tr->eligible)
		return false;
	if (tr->settled_frames < RT_STATIC_ALIAS_SETTLE_FRAMES)
		return false;
	return (cl.time - tr->settled_since) >= RT_STATIC_ALIAS_SETTLE_SECONDS;
}

static qboolean RT_StaticAliasVec3Equal (const vec3_t a, const vec3_t b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static qboolean RT_StaticAliasEntryUnchanged (const entity_t *e, const rt_alias_track_t *tr, int pose1)
{
	if (e->model != tr->model || e->frame != tr->frame || e->skinnum != tr->skinnum || pose1 != tr->pose1)
		return false;
	if (e->alpha != tr->alpha || e->effects != tr->effects)
		return false;
	return RT_StaticAliasVec3Equal (e->origin, tr->origin) && RT_StaticAliasVec3Equal (e->angles, tr->angles);
}

static qboolean RT_StaticAliasSyncTrackVisibility (void)
{
	if (cl.time < rt_static_alias_last_cl_time)
		RT_StaticAliasResetTracking ();
	rt_static_alias_last_cl_time = cl.time;

	RT_StaticAliasEnsureTrackCapacity (cl.num_entities);
	if (!rt_alias_track)
		return false;

	if (cl.num_entities > rt_alias_track_count)
	{
		const int clear_count = q_min (cl.num_entities, rt_alias_track_cap) - rt_alias_track_count;
		if (clear_count > 0)
			RT_StaticAliasZeroTrackRange (rt_alias_track + rt_alias_track_count, clear_count);
		rt_alias_track_count = q_min (cl.num_entities, rt_alias_track_cap);
	}

	if (++rt_alias_vis_gen == 0u)
	{
		rt_alias_vis_gen = 1u;
		for (int i = 0; i < rt_alias_track_count; i++)
			rt_alias_track[i].vis_gen = 0u;
	}

	for (int k = 0; k < cl_numvisedicts; k++)
	{
		entity_t *ve = cl_visedicts[k];
		if (ve >= cl.entities + 1 && ve < cl.entities + rt_alias_track_count)
			rt_alias_track[ve - cl.entities].vis_gen = rt_alias_vis_gen;
	}

	return true;
}

static void RT_StaticAliasRecordTrackPose (entity_t *e, rt_alias_track_t *tr, int pose1)
{
	tr->model = e->model;
	tr->frame = e->frame;
	tr->skinnum = e->skinnum;
	tr->pose1 = pose1;
	tr->alpha = e->alpha;
	tr->effects = e->effects;
	VectorCopy (e->origin, tr->origin);
	VectorCopy (e->angles, tr->angles);
	tr->settled_since = cl.time;
	tr->settled_frames = 1u;
}

static void RT_StaticAliasUpdateTrackEntry (int idx, qboolean *out_demoted, int *out_ready)
{
	entity_t         *e = &cl.entities[idx];
	rt_alias_track_t *tr = &rt_alias_track[idx];

	const qboolean skip_edict = (idx == cl.viewentity || idx <= cl.maxclients);
	const qboolean in_vis = (!skip_edict && tr->vis_gen == rt_alias_vis_gen);

	aliashdr_t    *hdr = NULL;
	gltexture_t   *tx = NULL;
	int            pose1 = 0;
	const qboolean eligible = in_vis && RT_StaticAliasExtractProps (cl.num_statics + idx, e, &hdr, &pose1, &tx, true);

	if (tr->promoted)
	{
		if (!eligible || !RT_StaticAliasEntryUnchanged (e, tr, pose1))
		{
			tr->promoted = false;
			tr->eligible = false;
			tr->never_promote = true;
			tr->settled_frames = 0u;
			tr->model = NULL;
			e->rt_static_alias_gen = 0u;
			*out_demoted = true;
		}
		return;
	}

	tr->eligible = eligible;
	if (tr->never_promote || !eligible)
	{
		tr->settled_frames = 0u;
		tr->model = NULL;
		return;
	}

	if (RT_StaticAliasEntryUnchanged (e, tr, pose1))
	{
		if (tr->settled_frames < 255u)
			tr->settled_frames++;
		if (RT_StaticAliasTrackReady (tr))
			(*out_ready)++;
	}
	else
	{
		RT_StaticAliasRecordTrackPose (e, tr, pose1);
	}
}

qboolean RT_StaticAliasNeedsSubmit (void)
{
	if (!CVAR_TO_BOOL (rt_alias_persistent))
		return false;

	const qboolean drawentities = CVAR_TO_BOOL (r_drawentities);
	if (cl.num_statics != rt_static_alias_num_statics ||
	    drawentities != rt_static_alias_drawentities ||
	    r_lightmap_cheatsafe != rt_static_alias_lightmap)
		return true;

	if (!drawentities || r_lightmap_cheatsafe || !cl.entities || cl.num_entities <= 1)
		return false;

	if (!RT_StaticAliasSyncTrackVisibility ())
		return false;

	qboolean demoted = false;
	int      ready_to_promote = 0;
	for (int i = 1; i < rt_alias_track_count; i++)
		RT_StaticAliasUpdateTrackEntry (i, &demoted, &ready_to_promote);

	if (demoted)
		return true;

	/* Only pay a static world rebuild for edict promotion when a substantial batch of
	   stationary props has settled (e.g. AD misc_model maps); avoid promoting handfuls of
	   monsters or triggers on small maps. */
	return ready_to_promote >= RT_STATIC_ALIAS_EDICT_MIN_PROMOTE;
}

qboolean RT_StaticAliasCovers (const entity_t *e)
{
	return e != NULL &&
	       e->rt_static_alias_gen != 0u &&
	       e->rt_static_alias_gen == rt_static_alias_generation &&
	       CVAR_TO_BOOL (rt_alias_persistent);
}

static void RT_StaticAliasPrepareStatics (int *io_covered)
{
	if (!cl.static_entities || cl.num_statics <= 0)
		return;

	for (int i = 0; i < cl.num_statics && *io_covered < RT_STATIC_ALIAS_MAX_GEOMS; i++)
	{
		entity_t    *e = cl.static_entities[i];
		aliashdr_t  *hdr = NULL;
		gltexture_t *tx = NULL;
		int          pose1 = 0;
		if (!RT_StaticAliasExtractProps (i, e, &hdr, &pose1, &tx, false))
			continue;

		e->rt_static_alias_gen = rt_static_alias_generation;
		(*io_covered)++;
	}
}

static void RT_StaticAliasPromoteEdicts (int *io_covered)
{
	if (!rt_alias_track || !cl.entities)
		return;

	const int limit = q_min (rt_alias_track_count, cl.num_entities);
	int       edict_ready = 0;

	for (int i = 1; i < limit; i++)
	{
		if (RT_StaticAliasTrackReady (&rt_alias_track[i]))
			edict_ready++;
	}

	if (edict_ready < RT_STATIC_ALIAS_EDICT_MIN_PROMOTE)
		return;

	for (int i = 1; i < limit && *io_covered < RT_STATIC_ALIAS_MAX_GEOMS; i++)
	{
		rt_alias_track_t *tr = &rt_alias_track[i];
		if (!RT_StaticAliasTrackReady (tr))
			continue;

		cl.entities[i].rt_static_alias_gen = rt_static_alias_generation;
		tr->promoted = true;
		(*io_covered)++;
	}
}

void RT_StaticAliasPrepare (void)
{
	if (++rt_static_alias_generation == 0u)
		rt_static_alias_generation = 1u;

	rt_static_alias_num_statics = cl.num_statics;
	rt_static_alias_drawentities = CVAR_TO_BOOL (r_drawentities);
	rt_static_alias_lightmap = r_lightmap_cheatsafe;

	for (int i = 0; i < rt_alias_track_count; i++)
		rt_alias_track[i].promoted = false;

	if (!CVAR_TO_BOOL (rt_alias_persistent) || !rt_static_alias_drawentities || rt_static_alias_lightmap)
		return;

	int covered = 0;
	RT_StaticAliasPrepareStatics (&covered);
	RT_StaticAliasPromoteEdicts (&covered);
}

static QrGeometryPassThroughType RT_StaticAliasPassThroughType (qboolean is_glass, qboolean is_watermat, qboolean alphatest)
{
	if (is_glass)
		return QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT;
	if (is_watermat)
		return QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT;
	if (alphatest)
		return QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED;
	return QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
}

static uint32_t RT_StaticAliasUploadFlags (const gltexture_t *tx, qboolean is_glass, qboolean alphatest)
{
	uint32_t flags = 0;
	if (!tx)
		return flags;
	if (tx->rtalphatest)
		flags |= QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT;
	if (is_glass && alphatest)
		flags |= QR_GEOMETRY_UPLOAD_GLASS_CUTOUT_BIT;
	if (tx->rtuvscale[0] != 1.0f || tx->rtuvscale[1] != 1.0f ||
	    tx->rtuvoffset[0] != 0.0f || tx->rtuvoffset[1] != 0.0f)
		flags |= QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT;
	if (tx->rtexactnormals)
		flags |= QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT;
	return flags;
}

static void RT_StaticAliasUploadOne (entity_t *e, int unique_index)
{
	aliashdr_t  *hdr = NULL;
	gltexture_t *tx = NULL;
	int          pose1 = 0;
	if (!RT_StaticAliasExtractProps (unique_index, e, &hdr, &pose1, &tx, false))
		return;

	const uint64_t unique_id = RT_GetAliasModelUniqueId (unique_index);
	const qboolean alphatest = !!(e->model->flags & MF_HOLEY) || tx->rtalphatest;
	const qboolean is_glass = tx->rtglass;

	lerpdata_t lerpdata = {0};
	lerpdata.pose1 = (short)pose1;
	lerpdata.pose2 = (short)pose1;
	VectorCopy (e->origin, lerpdata.origin);
	VectorCopy (e->angles, lerpdata.angles);

	const int         cluster = RT_ResolvePointCluster (e->origin);
	const QrTransform transform = RT_GetAliasModelTransform (hdr, &lerpdata, false, e->model);
	const QrVertex   *vertices = GetModelVerticesForPose (e->model, hdr, pose1);

	QrGeometryUploadInfo info = {
		.uniqueID = unique_id,
		.flags = RT_StaticAliasUploadFlags (tx, is_glass, alphatest),
		.geomType = QR_GEOMETRY_TYPE_STATIC,
		.passThroughType = RT_StaticAliasPassThroughType (is_glass, tx->rtwater, alphatest),
		.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0,
		.vertexCount = hdr->numverts_vbo,
		.pVertices = vertices,
		.indexCount = hdr->numindexes,
		.pIndices = e->model->rtindices,
		.layerColors = {RT_COLOR_WHITE},
		.layerBlendingTypes = {QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE},
		.texCoordScale = {tx->rtuvscale[0], tx->rtuvscale[1]},
		.texCoordOffset = {tx->rtuvoffset[0], tx->rtuvoffset[1]},
		.geomMaterial = {tx->rtmaterial, is_glass ? tx->rtoverlaymaterial : QR_NO_MATERIAL},
		.defaultRoughness = CVAR_TO_FLOAT (rt_model_rough),
		.defaultMetallicity = CVAR_TO_FLOAT (rt_model_metal),
		.defaultEmission = 0,
		.transform = transform,
		.clusterOverride = RT_AliasClusterOverride (cluster),
	};

	if (is_glass)
	{
		VectorCopy (tx->rtglasscolor, info.layerColors[1].data);
		info.layerColors[2].data[0] = tx->rtglassior;
		info.layerColors[2].data[1] = tx->rtglassthickness;
	}

	TexMgr_FillSubsurfaceUpload (tx, &info);

	QrResult r = qrUploadGeometry (vulkan_globals.instance, &info);
	QR_CHECK (r);
}

void RT_StaticAliasUpload (void)
{
	if (!CVAR_TO_BOOL (rt_alias_persistent) || !rt_static_alias_drawentities || rt_static_alias_lightmap)
		return;

	if (cl.static_entities && cl.num_statics > 0)
	{
		for (int i = 0; i < cl.num_statics; i++)
		{
			entity_t *e = cl.static_entities[i];
			if (e && e->rt_static_alias_gen == rt_static_alias_generation)
				RT_StaticAliasUploadOne (e, i);
		}
	}

	if (cl.entities && cl.num_entities > 1)
	{
		const int limit = q_min (rt_alias_track_count, cl.num_entities);
		for (int i = 1; i < limit; i++)
		{
			entity_t *e = &cl.entities[i];
			if (e->rt_static_alias_gen == rt_static_alias_generation)
				RT_StaticAliasUploadOne (e, cl.num_statics + i);
		}
	}
}
