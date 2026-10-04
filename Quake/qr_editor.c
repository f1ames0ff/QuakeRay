// qr_editor.c -- qr light editor: realtime material editor for the qray renderer.
//
// Console commands: qr_editor (which opens the mode chooser) / qr_editor_stop.
//
// While the editor runs the view belongs to a free camera (the player stands
// still): aim with the crosshair, fire selects the face under it and opens the
// material panel on the right edge of the screen. The panel is Dear ImGui
// (Quake/qr_gui.cpp), and it edits the qray.materials.yaml parameters of every
// animation frame of the picked texture (medkits, blinking buttons, ...).
// The world is frozen while the editor runs (the server is paused and cl.time
// stands still, so nothing animates). Apply writes the session to
// qray.materials.editor.yaml; Exit asks whether to save, and only Save copies
// the session file over <gamedir>/qray.materials.yaml (backing the previous
// file up as qray.backup_materials.yaml) — a mod's qray.materials.yaml overrides
// the id1 one, both because it is loaded after it and because Save writes to the
// mod's file.
//
// Editing model: the editor mutates the live rt_material_t structs and
// re-synthesizes the affected textures (TexMgr_ReloadImagesForMaterial). That
// replaces the material's QrMaterial, and the traced world bakes a material's
// texture indices when it is uploaded, so after a batch of edits the world is
// asked to re-upload itself — the rt_require_static_submit mechanism the light
// style cvar already uses — which also re-collects its emissive lights. A full
// snapshot of both material lists is taken on start (and re-taken after Apply)
// so Cancel/Exit can restore the yaml state.
//
// The light editor shares the camera and the session
// flow and keeps its emitter overrides and the level's custom dlights and fog
// in one file, <gamedir>/qray.lights.yaml, with its own session and backup.

#include "quakedef.h"
#include "glquake.h"
#include "gl_model.h"
#include "gl_texmgr.h"
#include "gl_heap.h"
#include "rt_material.h"
#include "rt_lights.h"
#include "rt_dtal_debug.h"
#include "keys.h"
#include "client.h"
#include "server.h"
#include "world.h"
#include "console.h"
#include "mathlib.h"
#include "input.h"
#include "vid.h"
#include "atomics.h"

#include "SDL.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "qr_editor.h"
#include "qr_gui.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>

extern vec3_t     vpn, vright, vup, r_origin; // gl_rmain.c
extern qboolean   keydown[MAX_KEYS];          // keys.c
extern kbutton_t  in_forward, in_back, in_moveleft, in_moveright, in_up, in_down; // cl_input.c
extern atomic_uint32_t rt_require_static_submit; // gl_rmain.c
extern atomic_uint32_t rt_require_world_light_recollect; // gl_rmain.c
extern qboolean        texmgr_live_material_replaced; // gl_texmgr.c
// The editor edits what the new light system builds (TAL and the fake dlights
// of materials); the old system has neither, so it refuses to start on it.
extern cvar_t rt_truelight; // gl_vidsdl.c
extern cvar_t rt_dtal_debug; // gl_vidsdl.c: draw the DTAL of models and sprites
extern cvar_t rt_dtal_clearance, rt_dtal_maxpolys, rt_dtal_minarea;
extern cvar_t rt_dtal_model_budget, rt_dtal_model_maxpolys, rt_dtal_model_minarea;
extern cvar_t rt_water_speed, rt_water_normstren, rt_water_normsharp, rt_water_scale;

// The level's own fog (gl_fog.c): read through the getters and written through
// the `fog` command, the same path a map's key and the console use. Whether the
// fog is drawn at all is the rt_level_fog switch (gl_vidsdl.c), which the level's
// section may carry too.
float Fog_GetDensity (void);
void  Fog_GetColor (float *c);
extern cvar_t rt_dlight_radius, rt_dlight_intensity; // gl_vidsdl.c
extern cvar_t rt_level_fog;                          // gl_vidsdl.c

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

enum
{
	QRE_T_FLOAT,
	QRE_T_INT,
	QRE_T_BOOL,
	QRE_T_TEXT,
	QRE_T_COLOR,
};

enum
{
	PARAM_BASE,     // texture_base
	PARAM_NORMALS,  // texture_normals
	PARAM_EMISSIVE, // texture_emissive
	PARAM_GLOSS,    // texture_gloss
	PARAM_BUMP,
	PARAM_ROUGH,
	PARAM_METAL,
	PARAM_BASEF,
	PARAM_METALALPHA,
	PARAM_MIRROR,
	PARAM_EXACTN,
	PARAM_FRAST,
	PARAM_ISLIGHT,
	PARAM_LSTYLES,
	PARAM_LCOLOR,
	PARAM_LBRIGHT,
	PARAM_LUPOFF,
	PARAM_EFOCUS,
	PARAM_ESOFT,
	PARAM_EPROJ,
	PARAM_CEMIS,
	PARAM_COUNT,
};

static const struct qre_param_s
{
	const char *section;
	const char *label;
	int         type;
	float       min, max, step;
	const char *tip;
} qre_params[PARAM_COUNT] = {
	[PARAM_BASE]     = { "Textures", "texture_base",     QRE_T_TEXT,  0, 0, 0,
	                     "The diffuse texture. NONE keeps the map's own; an authored file replaces it." },
	[PARAM_NORMALS]  = { NULL, "texture_normals",  QRE_T_TEXT,  0, 0, 0,
	                     "Per-pixel bump direction. Its alpha channel can drive metalness_from_normal_alpha." },
	[PARAM_EMISSIVE] = { NULL, "texture_emissive", QRE_T_TEXT,  0, 0, 0,
	                     "A luma mask image: what is bright in it is what the surface emits. Replaces color_emissive." },
	[PARAM_GLOSS]    = { NULL, "texture_gloss",    QRE_T_TEXT,  0, 0, 0,
	                     "White means mirror-smooth, black means rough (roughness = 1 - gloss). Ignored while roughness_override is set." },
	[PARAM_BUMP]     = { "Surface", "bump_scale",       QRE_T_FLOAT, 0, 4, 0.01f,
	                     "How pronounced the normal map's bumps are. Needs texture_normals." },
	[PARAM_ROUGH]    = { NULL, "roughness_override", QRE_T_FLOAT, 0, 1, 0.01f,
	                     "Ignore the gloss map and pin the roughness. 0 leaves it to the gloss; mirror forces 0." },
	[PARAM_METAL]    = { NULL, "metalness_factor", QRE_T_FLOAT, 0, 1, 0.01f,
	                     "How metal-like the surface is. With metalness_from_normal_alpha it scales that mask." },
	[PARAM_BASEF]    = { NULL, "base_factor",      QRE_T_FLOAT, 0, 4, 0.01f,
	                     "Multiplies the albedo: dims or lifts the whole texture." },
	[PARAM_METALALPHA] = { NULL, "metalness_from_normal_alpha", QRE_T_BOOL, 0, 0, 0,
	                     "Read metalness from the normal map's alpha channel instead of a flat factor." },
	[PARAM_MIRROR]   = { NULL, "mirror",           QRE_T_BOOL,  0, 0, 0,
	                     "Mirror-smooth reflection; roughness is forced to 0." },
	[PARAM_EXACTN]   = { "Model", "exact_normals",    QRE_T_BOOL,  0, 0, 0,
	                     "Use the model's own vertex normals instead of generated ones (models only)." },
	[PARAM_FRAST]    = { NULL, "force_rasterize",  QRE_T_BOOL,  0, 0, 0,
	                     "Draw the model or sprite with the rasterizer instead of tracing it (models only)." },
	[PARAM_ISLIGHT]  = { "", "is_light",         QRE_T_BOOL,  0, 0, 0,
	                     "The surface casts light into the scene, not only glows." },
	[PARAM_LSTYLES]  = { "Light", "light_styles",     QRE_T_BOOL,  0, 0, 0,
	                     "Tick to let the map's light styles dim this light; off by default, so a light stays at full brightness unless it asks otherwise." },
	[PARAM_LCOLOR]   = { NULL, "light_color",      QRE_T_COLOR, 0, 0, 0,
	                     "The color of the light the surface casts; needs is_light." },
	[PARAM_LBRIGHT]  = { NULL, "light_brightness", QRE_T_FLOAT, 0.001f, 1000, 0.0001f,
	                     "How bright the light the surface casts is; the visible glow is set by emissive_factor and does not change with this." },
	[PARAM_LUPOFF]   = { NULL, "light_upoffset",   QRE_T_FLOAT, -64, 64, 0.5f,
	                     "Lifts the cast light above the model's origin (alias models)." },
	[PARAM_EFOCUS]   = { "Emissive", "emissive_focus",   QRE_T_FLOAT, 1, 90, 0.01f,
	                     "Makes the light the surface casts a beam: its half-angle around the normal, in degrees. Full brightness up to it, nothing beyond. The edge is softened inward from this angle by emissive_focus_soft." },
	[PARAM_ESOFT]    = { NULL, "emissive_focus_soft", QRE_T_FLOAT, 1, 90, 0.01f,
	                     "How wide the beam's soft edge is, in absolute degrees: brightness holds to (emissive_focus - emissive_focus_soft) and then falls smoothly (smoothstep squared) to zero at the focus angle, so the edge always grows inward from it. In a projector it blurs the projected pattern too." },
	[PARAM_EPROJ]    = { NULL, "emissive_projector", QRE_T_BOOL, 0, 0, 0,
	                     "Gobo: the light reads its emissive mask along the direction of each point it lights instead of at a point on the surface, so the pattern is projected across the beam instead of washing out. The mask is read over the same cone (emissive_focus; no key = 45 degrees) and emissive_focus_soft blurs the projected pattern too, reading it from a blurrier mip as the edge grows." },
	[PARAM_CEMIS]    = { NULL, "color_emissive",   QRE_T_BOOL,  0, 0, 0,
	                     "Glow by color: every block below matches its own color and carries its own threshold, feather, emissive_factor and blend." },
};

// The emissive blend modes as the combos offer them: the index is the mode's
// value plus one, so index 0 is "cvar" (-1) and the rest line up with
// RT_MAT_EmissiveBlendName.
static const char *const qre_emissive_blends[] = {
	"cvar", "off", "normal", "screen", "overlay", "hard light", "color dodge"
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

#define QRE_GROUP_MAX    12
#define QRE_DIRTY_MAX    64
#define QRE_TOUCHED_MAX  512
#define QRE_PREVIEW_SLOTS QRE_GROUP_MAX

// One texture preview: the QrMaterial the panel draws and the pixels the color
// picker samples, cached under the key of what they were built from, one slot
// per group entry so rebuilding one section never frees a material another
// section's draw command already names.
typedef struct qre_preview_s
{
	char        key[MAX_QPATH * 2 + 32];
	QrMaterial  mat;
	byte       *pixels;
	int         w, h;
	unsigned    last_frame;
} qre_preview_t;

// What the editor edits: the surfaces (qray.materials.yaml) or the dynamic
// lights the emitters cast (qray.lights.yaml). The camera, the picking and the flow
// (Apply / Cancel / the exit dialog) are shared; the panel and the files differ.
enum
{
	QRE_MODE_MATERIAL = 0,
	QRE_MODE_LIGHT    = 1,
};

static struct
{
	qboolean active;
	qboolean panel_open;
	qboolean torch;

	vec3_t cam_origin;
	vec3_t player_viewangles;

	// the picked / hovered face (hover is updated while flying); ent is NULL
	// for a world face and the brush entity for a model face. A model/sprite
	// pick has no surface: pick_glt is its skin texture and pick_surf is NULL.
	qmodel_t    *pick_model;
	msurface_t  *pick_surf;
	entity_t    *pick_ent;
	gltexture_t *pick_glt;
	qmodel_t    *hover_model;
	msurface_t  *hover_surf;
	entity_t    *hover_ent;
	gltexture_t *hover_glt;

	// the picked texture's normalized material name (what the panel shows and
	// the group was built from; the engine name carries a maps/<map>.bsp: prefix)
	char         pick_name[MAX_QPATH];

	// the material group (animation frames) shown by the panel
	rt_material_t *group[QRE_GROUP_MAX];
	int            group_count;
	// detached defaults for the animation frames a model pick shows before any
	// of them has a yaml entry (each becomes a material on its first change)
	rt_material_t  extra[QRE_GROUP_MAX];
	int            extra_count;

	// snapshot of both material lists for Cancel/Exit
	rt_material_t *snap_global;
	int            snap_global_count;
	rt_material_t *snap_map;
	int            snap_map_count;

	// snapshot of the light overrides, and the light names the session touched
	rt_light_t *snap_lights;
	int         snap_light_count;
	char        light_touched[QRE_TOUCHED_MAX][MAX_QPATH];
	int         light_touched_count;

	// snapshot of the custom lights and the fog that was in effect (its color,
	// its density and the rt_level_fog switch): the Custom tab and the Global
	// tab edit the live state without marking anything, so this is what
	// Cancel/Exit restore and what QRE_CustomTouched compares to
	rt_custom_light_t snap_custom[RT_CUSTOM_LIGHTS_MAX];
	int               snap_custom_count;
	float             snap_fog_color[3];
	float             snap_fog_density;
	qboolean          snap_fog_enabled;

	// the light editor: the light the crosshair is over, and the one selected
	// (the panel edits the entry of the selected light's emitter)
	rt_tracked_light_t sel_light;
	qboolean           sel_light_valid;
	int                hover_light;

	// the light editor's tabs: 0 = the selected emitter, 1 = the sky, clouds
	// and sun (the global settings)
	int light_tab;

	int mat_tab;

	// the Custom tab's placement mode: "Add light" waits for the fire button and
	// drops the new light where the crosshair hits
	qboolean custom_placing;

	qboolean          custom_cloning;
	rt_custom_light_t clone_source;

	qboolean panel_drawing;
	qboolean stop_pending;
	qboolean stop_pending_restore;
	vec3_t   pick_impact;       // the last pick's hit point (QRE_TracePick)
	unsigned pick_impact_frame; // the frame it was taken in

	// the axis gizmo drag of the selected custom light
	qboolean custom_dragging;
	qboolean custom_drag_dir;
	int      custom_drag_axis;
	int      custom_drag_index;
	vec3_t   custom_drag_origin;
	vec3_t   custom_drag_dir_start;
	vec3_t   custom_drag_vector;
	float    custom_drag_mouse[2];

	// the flying-mode drag of the light under the crosshair (Alt+LMB grabs,
	// LMB drops, Esc returns it)
	qboolean    light_dragging;
	int         light_drag_kind;
	int         light_drag_custom;
	char        light_drag_name[MAX_QPATH];
	char        light_drag_key[MAX_QPATH];
	rt_light_t *light_drag_entry;
	qboolean    light_drag_created;
	qboolean    light_drag_backup_valid;
	rt_light_t  light_drag_backup;
	vec3_t      light_drag_emitter;
	vec3_t      light_drag_custom_origin;

	qboolean gizmo_fly_drag;
	float    gizmo_fly_mouse[2];
	vec3_t   gizmo_fly_anchor_local;

	// which of the two editors this is
	int mode;
	qboolean choosing;

	// the session files, resolved on start: <gamedir>/qray.materials.yaml (or
	// qray.lights.yaml) is the file the editor saves to (a mod's file overrides
	// the id1 one), the .editor.yaml file carries the session until the exit
	// dialog decides and the backup keeps the target as it was before a save
	char target_file[MAX_OSPATH];
	char editor_file[MAX_OSPATH];
	char backup_file[MAX_OSPATH];

	// a material created by the editor for a texture that has none in yaml
	rt_material_t tmp_mat;
	qboolean      tmp_appended;

	// the exit dialog is up: Save/Discard, the editor keeps running until
	// answered; prompt_from_flying is the mode to go back to when dismissed
	qboolean exit_prompt;
	qboolean reset_prompt;
	qboolean reset_pending;
	qboolean prompt_from_flying;

	// the pause state the editor found, restored when it closes
	qboolean sv_paused_prev;

	// the base-texture previews of the selected material (the emissive color
	// picker): one slot per group entry in flight, so a section rebuild never
	// destroys a material an earlier section's draw command still names
	qre_preview_t preview[QRE_PREVIEW_SLOTS];

	// materials waiting for live re-synthesis / all touched this session
	char    dirty[QRE_DIRTY_MAX][MAX_QPATH];
	qboolean dirty_full[QRE_DIRTY_MAX];
	qboolean dirty_light[QRE_DIRTY_MAX];
	int     dirty_count;
	char    touched[QRE_TOUCHED_MAX][MAX_QPATH];
	int     touched_count;
} qre;

// forward declarations (the panel code sits above the apply/save code)
static void     QRE_Apply (void);
static void     QRE_Cancel (void);
static void     QRE_StopEditor (qboolean restore);
static void     QRE_ClosePanel (void);
static void     QRE_RequestExit (void);
static void     QRE_StartMode (int mode);
static void     QRE_GlobalsRestore (void);
static qboolean QRE_GlobalsTouched (void);
static qboolean QRE_WriteSession (void);
static qboolean QRE_FileExists (const char *path);
static void     QRE_SessionSave (void);
static void     QRE_SessionDiscard (void);
static void     QRE_ClearSessionState (void);
static void     QRE_ResetAll (void);
static void     QRE_CancelLightDrag (void);
static void     QRE_UpdateLightDrag (void);
static void     QRE_CustomGizmoCancel (void);
static qboolean QRE_BrowseTexture (char *out, size_t outsize);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// "textures/+3_med25" -> 3, "progs/flame.mdl:frame2" -> 2 (a model or sprite
// skin frame); the ring base and the digit live in rt_material.c, shared with
// the renderer-side group enumeration.
static int QRE_FrameDigit (const char *name)
{
	return RT_MAT_FrameDigit (name);
}

static void QRE_GroupBaseOf (const char *name, char *out, size_t outsize)
{
	RT_MAT_GroupBaseOf (name, out, outsize);
}

static qboolean QRE_NameInGroup (const char *matname, const char *groupbase)
{
	char base[MAX_QPATH];

	QRE_GroupBaseOf (matname, base, sizeof (base));
	return !strcmp (base, groupbase);
}

// Editor feedback goes to the ImGui notification line and to the console log.
static void QRE_Notify (const char *fmt, ...)
{
	char    buf[256];
	va_list ap;

	va_start (ap, fmt);
	vsnprintf (buf, sizeof (buf), fmt, ap);
	va_end (ap);

	QR_GUI_Notify (buf);
	Con_Printf ("qr editor: %s\n", buf);
}

// The defaults of a material with no yaml entry: what the detached default is
// built from, and what a parameter of a material the editor created resets to.
static void QRE_InitDefault (rt_material_t *m, const char *name)
{
	memset (m, 0, sizeof (*m));
	m->valid = true;
	m->bump_scale = 1.0f;
	m->emissive_factor = 1.0f;
	m->emissive_blend = -1;
	m->emissive_focus = -1.0f;
	m->emissive_focus_soft = -1.0f;
	m->base_factor = 1.0f;
	m->light_brightness = 1.0f;
	m->light_styles = false;
	m->color_emissive_threshold = 0.02f;
	q_strlcpy (m->name, name, sizeof (m->name));
}

// ---------------------------------------------------------------------------
// Material snapshot (for Cancel/Exit)
// ---------------------------------------------------------------------------

static void QRE_TakeSnapshot (void)
{
	int count;

	RT_MAT_GetList (RT_MAT_LIST_GLOBAL, &count);
	qre.snap_global_count = count;
	if (!qre.snap_global)
		qre.snap_global = (rt_material_t *)Mem_Alloc (RT_MAT_CAP_GLOBAL * sizeof (rt_material_t));
	memcpy (qre.snap_global, RT_MAT_GetList (RT_MAT_LIST_GLOBAL, NULL), (size_t)count * sizeof (rt_material_t));

	RT_MAT_GetList (RT_MAT_LIST_MAP, &count);
	qre.snap_map_count = count;
	if (!qre.snap_map)
		qre.snap_map = (rt_material_t *)Mem_Alloc (RT_MAT_CAP_MAP * sizeof (rt_material_t));
	memcpy (qre.snap_map, RT_MAT_GetList (RT_MAT_LIST_MAP, NULL), (size_t)count * sizeof (rt_material_t));
}

static void QRE_RestoreSnapshot (void)
{
	memcpy (RT_MAT_GetList (RT_MAT_LIST_GLOBAL, NULL), qre.snap_global, (size_t)qre.snap_global_count * sizeof (rt_material_t));
	memcpy (RT_MAT_GetList (RT_MAT_LIST_MAP, NULL), qre.snap_map, (size_t)qre.snap_map_count * sizeof (rt_material_t));

	// The lists may have grown since the snapshot (a material the editor
	// created); the lengths are part of what a snapshot restores.
	RT_MAT_SetListCounts (qre.snap_global_count, qre.snap_map_count);
}

static void QRE_FreeSnapshot (void)
{
	if (qre.snap_global)
		Mem_Free (qre.snap_global);
	if (qre.snap_map)
		Mem_Free (qre.snap_map);
	qre.snap_global = NULL;
	qre.snap_map = NULL;
}

// ---------------------------------------------------------------------------
// Light snapshot (for Cancel/Exit of the light editor)
// ---------------------------------------------------------------------------

// The custom lights and the fog the level came with: the Custom tab and the
// Global tab edit the live state in place (they do not mark anything), so the
// session's "touched" test compares against this snapshot and Cancel/Exit put
// it back.
static void QRE_TakeCustomSnapshot (void)
{
	int                count = 0;
	rt_custom_light_t *lights = RT_CustomLights (&count);
	float              color[4];

	memcpy (qre.snap_custom, lights, (size_t)count * sizeof (rt_custom_light_t));
	qre.snap_custom_count = count;

	Fog_GetColor (color);
	qre.snap_fog_color[0] = color[0];
	qre.snap_fog_color[1] = color[1];
	qre.snap_fog_color[2] = color[2];
	qre.snap_fog_density = Fog_GetDensity ();
	qre.snap_fog_enabled = CVAR_TO_BOOL (rt_level_fog);
}

static void QRE_RestoreCustomSnapshot (void)
{
	rt_custom_light_t *lights = RT_CustomLights (NULL);
	float              color[4];

	if (qre.snap_custom_count > 0)
		memcpy (lights, qre.snap_custom, (size_t)qre.snap_custom_count * sizeof (rt_custom_light_t));
	RT_CustomLights_SetCount (qre.snap_custom_count);

	// the fog is engine state, not a cvar: it comes back through the same
	// `fog` command the Global tab edits it with, and only when it differs
	Fog_GetColor (color);
	if (color[0] != qre.snap_fog_color[0] || color[1] != qre.snap_fog_color[1] ||
	    color[2] != qre.snap_fog_color[2] || Fog_GetDensity () != qre.snap_fog_density)
	{
		Cbuf_AddText (va ("fog %f %f %f %f\n", qre.snap_fog_density,
		                  CLAMP (0.0f, qre.snap_fog_color[0], 1.0f),
		                  CLAMP (0.0f, qre.snap_fog_color[1], 1.0f),
		                  CLAMP (0.0f, qre.snap_fog_color[2], 1.0f)));
	}

	// the fog switch is a plain cvar: it goes back straight away
	if (CVAR_TO_BOOL (rt_level_fog) != qre.snap_fog_enabled)
		Cvar_Set ("rt_level_fog", qre.snap_fog_enabled ? "1" : "0");
}

// Whether the live custom list or the fog differs from the snapshot: what the
// Custom tab's lights and the Global tab's fog contribute to the session.
static qboolean QRE_CustomTouched (void)
{
	int                count = 0;
	rt_custom_light_t *lights = RT_CustomLights (&count);
	float              color[4];

	if (count != qre.snap_custom_count)
		return true;
	if (count > 0 && memcmp (lights, qre.snap_custom, (size_t)count * sizeof (rt_custom_light_t)))
		return true;

	Fog_GetColor (color);
	if (color[0] != qre.snap_fog_color[0] || color[1] != qre.snap_fog_color[1] ||
	    color[2] != qre.snap_fog_color[2] || Fog_GetDensity () != qre.snap_fog_density)
		return true;

	// a change of only the rt_level_fog checkbox counts too: the section
	// carries the switch as "enabled"
	if (CVAR_TO_BOOL (rt_level_fog) != qre.snap_fog_enabled)
		return true;

	return false;
}

// The light editor's session: the emitter overrides and the custom lights and
// fog, both kept in the gamedir's qray.lights.yaml.
static qboolean QRE_LightSessionTouched (void)
{
	return (qre.light_touched_count > 0 || QRE_CustomTouched () || QRE_GlobalsTouched ()) ? true : false;
}

static void QRE_TakeLightSnapshot (void)
{
	rt_light_t *list = RT_LIGHT_List (&qre.snap_light_count);

	if (!qre.snap_lights)
		qre.snap_lights = (rt_light_t *)Mem_Alloc (RT_LIGHT_NAMES_MAX * sizeof (rt_light_t));
	memcpy (qre.snap_lights, list, (size_t)qre.snap_light_count * sizeof (rt_light_t));

	QRE_TakeCustomSnapshot ();
}

static void QRE_RestoreLightSnapshot (void)
{
	rt_light_t *list = RT_LIGHT_List (NULL);

	memcpy (list, qre.snap_lights, (size_t)qre.snap_light_count * sizeof (rt_light_t));
	RT_LIGHT_SetCount (qre.snap_light_count);

	QRE_RestoreCustomSnapshot ();
	QRE_GlobalsRestore ();
}

static void QRE_FreeLightSnapshot (void)
{
	if (qre.snap_lights)
		Mem_Free (qre.snap_lights);
	qre.snap_lights = NULL;
	qre.snap_light_count = 0;
	qre.snap_custom_count = 0;
	qre.light_touched_count = 0;
}

// ---------------------------------------------------------------------------
// Dirty tracking and live re-synthesis
// ---------------------------------------------------------------------------

static qboolean QRE_NameInList (const char (*list)[MAX_QPATH], int count, const char *name)
{
	int i;

	for (i = 0; i < count; i++)
	{
		if (!strcmp (list[i], name))
			return true;
	}
	return false;
}

// The light names the session touched: what the session file writes besides the
// entries the target already carried.
static void QRE_TouchLight (const char *name)
{
	if (!name || !name[0])
		return;
	if (!QRE_NameInList (qre.light_touched, qre.light_touched_count, name) &&
	    qre.light_touched_count < QRE_TOUCHED_MAX)
		q_strlcpy (qre.light_touched[qre.light_touched_count++], name, MAX_QPATH);
}

static void QRE_MarkDirtyInternal (rt_material_t *m, qboolean full, qboolean light)
{
	static qboolean warned = false;
	int             i;

	if (!m || !m->name[0])
		return;

	if (!QRE_NameInList (qre.touched, qre.touched_count, m->name))
	{
		if (qre.touched_count < QRE_TOUCHED_MAX)
			q_strlcpy (qre.touched[qre.touched_count++], m->name, MAX_QPATH);
		else if (!warned)
		{
			warned = true;
			QRE_Notify ("too many materials edited at once; some will not be re-applied");
		}
	}

	for (i = 0; i < qre.dirty_count; i++)
	{
		if (!strcmp (qre.dirty[i], m->name))
		{
			if (full)
				qre.dirty_full[i] = true;
			if (light)
				qre.dirty_light[i] = true;
			return;
		}
	}

	if (qre.dirty_count < QRE_DIRTY_MAX)
	{
		q_strlcpy (qre.dirty[qre.dirty_count], m->name, MAX_QPATH);
		qre.dirty_full[qre.dirty_count] = full;
		qre.dirty_light[qre.dirty_count] = light;
		qre.dirty_count++;
	}
	else
	{
		warned = true;
		QRE_Notify ("too many materials edited at once; some will not be previewed");
	}
}

static void QRE_MarkDirty (rt_material_t *m)
{
	QRE_MarkDirtyInternal (m, false, false);
}

static void QRE_MarkDirtyLight (rt_material_t *m)
{
	QRE_MarkDirtyInternal (m, false, true);
}

static void QRE_MarkDirtyFull (rt_material_t *m)
{
	QRE_MarkDirtyInternal (m, true, false);
}

static void QRE_FlushDirty (void)
{
	static double last_flush = 0.0;
	double        now = Sys_DoubleTime ();
	qboolean      full = false;
	qboolean      lights = false;
	int           i;

	if (qre.dirty_count == 0)
		return;

	if (QR_GUI_Ready () && QR_GUI_AnyItemActive () && (now - last_flush) < 0.25)
		return;

	last_flush = now;

	texmgr_live_material_replaced = false;

	for (i = 0; i < qre.dirty_count; i++)
	{
		rt_material_t *mat;

		TexMgr_ReloadImagesForMaterial (qre.dirty[i], qre.dirty_full[i]);

		if (qre.dirty_full[i])
			full = true;

		if (qre.dirty_light[i])
			lights = true;

		mat = RT_MAT_Find (qre.dirty[i]);
		if (mat && mat->is_light)
			lights = true;
	}

	if (texmgr_live_material_replaced)
		full = true;

	qre.dirty_count = 0;
	memset (qre.dirty_full, 0, sizeof (qre.dirty_full));
	memset (qre.dirty_light, 0, sizeof (qre.dirty_light));

	if (full)
		Atomic_StoreUInt32 (&rt_require_static_submit, true);
	else if (lights)
		Atomic_StoreUInt32 (&rt_require_world_light_recollect, true);
}

static void QRE_ReapplyTouched (void)
{
	int i;

	qre.dirty_count = 0;
	memset (qre.dirty_full, 0, sizeof (qre.dirty_full));
	memset (qre.dirty_light, 0, sizeof (qre.dirty_light));
	for (i = 0; i < qre.touched_count; i++)
	{
		// by texture name: Cancel/Exit drop a material the editor had created,
		// and the texture still has to be re-synthesized without it
		TexMgr_ReloadImagesForTextureName (qre.touched[i]);
	}
	// the touched names are kept: they are the session's own list, and the
	// session file is rewritten from them when Cancel reverts the values

	Atomic_StoreUInt32 (&rt_require_static_submit, true);
}

// A material created by the editor becomes part of the live global list on the
// first change, so the synthesis (RT_MAT_Find) can see it. The detached
// defaults are qre.tmp_mat and the qre.extra frames of a model pick.
static qboolean QRE_IsDetached (const rt_material_t *m)
{
	return m == &qre.tmp_mat || (m >= &qre.extra[0] && m < &qre.extra[QRE_GROUP_MAX]);
}

static void QRE_EnsureLive (int g)
{
	rt_material_t *m = qre.group[g];
	int            idx;

	if (!QRE_IsDetached (m))
		return;
	if (m == &qre.tmp_mat && qre.tmp_appended)
		return;

	idx = RT_MAT_AppendGlobal (m);
	if (idx >= 0)
	{
		if (m == &qre.tmp_mat)
			qre.tmp_appended = true;
		qre.group[g] = RT_MAT_GetList (RT_MAT_LIST_GLOBAL, NULL) + idx;
	}
	else
	{
		QRE_Notify ("cannot create a material: the list is full");
	}
}

// ---------------------------------------------------------------------------
// Parameter access
// ---------------------------------------------------------------------------

static void QRE_GetColor (const rt_material_t *m, int param, qboolean *enabled, float *rgb)
{
	(void)param; // only light_color is a single color now; color_emissive is a list
	*enabled = m->has_light_color;
	VectorCopy (m->light_color, rgb);
}

static void QRE_SetColorEnabled (int g, int param, qboolean enabled)
{
	QRE_EnsureLive (g);
	rt_material_t *m = qre.group[g];

	(void)param;
	m->has_light_color = enabled;
	QRE_MarkDirtyLight (m);
}

static void QRE_SetColorChannel (int g, int param, int channel, float value)
{
	QRE_EnsureLive (g);
	rt_material_t *m = qre.group[g];

	(void)param;
	m->has_light_color = true;
	m->light_color[channel] = value;
	QRE_MarkDirtyLight (m);
}

static float QRE_GetFloat (const rt_material_t *m, int param)
{
	switch (param)
	{
	case PARAM_BUMP:     return m->bump_scale;
	case PARAM_ROUGH:    return m->roughness_override;
	case PARAM_METAL:    return m->metalness_factor;
	case PARAM_BASEF:    return m->base_factor;
	case PARAM_LBRIGHT:  return m->light_brightness;
	case PARAM_LUPOFF:   return m->light_upoffset;
	case PARAM_EFOCUS:   return m->emissive_focus > 0.0f ? m->emissive_focus : 45.0f;
	case PARAM_ESOFT:    return m->emissive_focus_soft >= 0.0f ? m->emissive_focus_soft : 45.0f;
	default:             return 0.0f;
	}
}

static int QRE_GetInt (const rt_material_t *m, int param)
{
	(void)m;
	(void)param;
	return 0; // the per-block controls are edited inside QRE_EmissiveEditor
}

static qboolean QRE_GetBool (const rt_material_t *m, int param)
{
	switch (param)
	{
	case PARAM_ISLIGHT:    return m->is_light;
	case PARAM_EPROJ:      return m->emissive_projector;
	case PARAM_CEMIS:      return m->has_color_emissive;
	case PARAM_LSTYLES:    return m->light_styles;
	case PARAM_METALALPHA: return m->metalness_from_normal_alpha;
	case PARAM_MIRROR:     return m->mirror;
	case PARAM_EXACTN:     return m->exact_normals;
	case PARAM_FRAST:      return m->force_rasterize;
	default:               return false;
	}
}

static const char *QRE_GetText (const rt_material_t *m, int param)
{
	switch (param)
	{
	case PARAM_BASE:     return m->filename_base;
	case PARAM_NORMALS:  return m->filename_normals;
	case PARAM_EMISSIVE: return m->filename_emissive;
	case PARAM_GLOSS:    return m->filename_gloss;
	default:             return "";
	}
}

static void QRE_SetFloat (int g, int param, float value)
{
	QRE_EnsureLive (g);
	rt_material_t *m = qre.group[g];

	switch (param)
	{
	case PARAM_BUMP:     m->bump_scale = value; break;
	case PARAM_ROUGH:    m->roughness_override = value; break;
	case PARAM_METAL:    m->metalness_factor = value; m->has_metalness_factor = true; break;
	case PARAM_LBRIGHT:  m->light_brightness = value; break;
	case PARAM_LUPOFF:   m->light_upoffset = value; break;
	case PARAM_EFOCUS:   m->emissive_focus = value > 0.0f ? value : -1.0f; break;
	case PARAM_ESOFT:    m->emissive_focus_soft = value; break;
	default:             break;
	}

	if (param == PARAM_LBRIGHT || param == PARAM_EFOCUS || param == PARAM_ESOFT)
		QRE_MarkDirtyLight (m);
	else
		QRE_MarkDirty (m);
}

static void QRE_SetInt (int g, int param, int value)
{
	(void)g;
	(void)param;
	(void)value; // the per-block controls are edited inside QRE_EmissiveEditor
}

static void QRE_SetBool (int g, int param, qboolean value)
{
	QRE_EnsureLive (g);
	rt_material_t *m = qre.group[g];

	switch (param)
	{
	case PARAM_ISLIGHT:    m->is_light = value; break;
	case PARAM_EPROJ:      m->emissive_projector = value; break;
	case PARAM_CEMIS:      m->has_color_emissive = value; break;
	case PARAM_LSTYLES:    m->light_styles = value; break;
	case PARAM_METALALPHA: m->metalness_from_normal_alpha = value; break;
	case PARAM_MIRROR:
		m->mirror = value;
		if (value)
			m->roughness_override = 0.0f; // the panel locks the override while mirror is on
		break;
	case PARAM_EXACTN:     m->exact_normals = value; break;
	case PARAM_FRAST:      m->force_rasterize = value; break;
	default:               break;
	}

	if (param == PARAM_ISLIGHT || param == PARAM_EPROJ)
		QRE_MarkDirtyLight (m);
	else
		QRE_MarkDirty (m);
}

static void QRE_SetText (int g, int param, const char *value)
{
	QRE_EnsureLive (g);
	rt_material_t *m = qre.group[g];

	switch (param)
	{
	case PARAM_BASE:     q_strlcpy (m->filename_base, value, sizeof (m->filename_base)); break;
	case PARAM_NORMALS:  q_strlcpy (m->filename_normals, value, sizeof (m->filename_normals)); break;
	case PARAM_EMISSIVE: q_strlcpy (m->filename_emissive, value, sizeof (m->filename_emissive)); break;
	case PARAM_GLOSS:    q_strlcpy (m->filename_gloss, value, sizeof (m->filename_gloss)); break;
	default:             break;
	}
	QRE_MarkDirtyFull (m);
}

// ---------------------------------------------------------------------------
// Material group resolution (animation frames)
// ---------------------------------------------------------------------------

// Compares frames: the plain base first, then +N in ascending digit order.
static int QRE_CompareMats (const void *a, const void *b)
{
	const rt_material_t *ma = *(rt_material_t *const *)a;
	const rt_material_t *mb = *(rt_material_t *const *)b;
	int da = QRE_FrameDigit (ma->name);
	int db = QRE_FrameDigit (mb->name);

	if (da != db)
		return (da == -1) ? -1 : (db == -1) ? 1 : da - db;
	return strcmp (ma->name, mb->name);
}

// A detached default for one frame name of the picked texture: named exactly as
// the engine names that texture, so the first change to it resolves to it (the
// synthesis looks the texture's own name up).
static void QRE_AddDefaultName (const char *groupbase, const char *name)
{
	char base[MAX_QPATH];
	int  i;

	if (!name[0])
		return;

	RT_MAT_GroupBaseOf (name, base, sizeof (base));
	if (strcmp (base, groupbase))
		return; // not a frame of this ring

	for (i = 0; i < qre.group_count; i++)
	{
		if (!strcmp (qre.group[i]->name, name))
			return; // already shown (authored or added)
	}

	if (qre.extra_count >= QRE_GROUP_MAX)
		return;

	QRE_InitDefault (&qre.extra[qre.extra_count], name);
	qre.group[qre.group_count++] = &qre.extra[qre.extra_count++];
}

// Every frame the engine actually has for the picked texture: the "textures/+N"
// ring and the "progs/x.mdl:frameN" skins of a model or sprite. Without it a
// frame with no yaml entry opened one block named after the ring base -- a name
// no texture resolves to -- so editing it changed nothing on screen.
static void QRE_AddEngineFrames (const char *texname, const char *groupbase)
{
	char names[QRE_GROUP_MAX][MAX_QPATH];
	int  count = TexMgr_CollectGroupNames (texname, names, QRE_GROUP_MAX);
	int  i;

	for (i = 0; i < count && qre.group_count < QRE_GROUP_MAX; i++)
		QRE_AddDefaultName (groupbase, names[i]);
}

// Builds the group of materials for a texture name (all animation frames).
static void QRE_ResolveGroup (const char *texname)
{
	char groupbase[MAX_QPATH];
	int  pass, i, k;

	QRE_GroupBaseOf (texname, groupbase, sizeof (groupbase));

	qre.group_count = 0;
	qre.extra_count = 0;
	qre.tmp_appended = false;

	// the map list wins over the global list for duplicate names
	for (pass = 0; pass < 2 && qre.group_count < QRE_GROUP_MAX; pass++)
	{
		int            count;
		rt_material_t *list = RT_MAT_GetList (pass == 0 ? RT_MAT_LIST_MAP : RT_MAT_LIST_GLOBAL, &count);

		for (i = 0; i < count && qre.group_count < QRE_GROUP_MAX; i++)
		{
			qboolean dup = false;

			if (!list[i].valid)
				continue;
			// A model's own base name is not a texture the engine has: only
			// its :frameN names resolve, so a stale base entry stays hidden
			// (for a texture ring the base is a real material and is shown).
			if (qre.pick_model && qre.pick_model->type != mod_brush &&
			    !q_strcasecmp (list[i].name, groupbase))
				continue;
			if (!QRE_NameInGroup (list[i].name, groupbase))
				continue;
			for (k = 0; k < qre.group_count; k++)
			{
				if (!strcmp (qre.group[k]->name, list[i].name))
				{
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			qre.group[qre.group_count++] = &list[i];
		}
	}

	// the frames the engine has but no material names yet get their own blocks
	QRE_AddEngineFrames (texname, groupbase);

	if (qre.group_count == 0)
	{
		// no material authored for this texture: edit a detached default one
		// that joins the global list on the first change
		QRE_InitDefault (&qre.tmp_mat, groupbase);
		qre.group[0] = &qre.tmp_mat;
		qre.group_count = 1;
	}
	else if (qre.group_count > 1)
	{
		qsort (qre.group, (size_t)qre.group_count, sizeof (qre.group[0]), QRE_CompareMats);
	}

	// mirror forces roughness_override to 0, and the synthesis gives it the last
	// word: a material loaded with both is shown (and saved) with the override
	// the renderer ignores taken out of the way
	for (i = 0; i < qre.group_count; i++)
	{
		if (qre.group[i]->mirror && qre.group[i]->roughness_override != 0.0f)
			qre.group[i]->roughness_override = 0.0f;
	}
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

static qboolean QRE_PointInPolygon (const glpoly_t *poly, const vec3_t p, const vec3_t normal, float eps)
{
	int ax, ay, i, j, n;
	int inside = 0;

	if (fabsf (normal[0]) >= fabsf (normal[1]) && fabsf (normal[0]) >= fabsf (normal[2]))
	{
		ax = 1; ay = 2;
	}
	else if (fabsf (normal[1]) >= fabsf (normal[2]))
	{
		ax = 0; ay = 2;
	}
	else
	{
		ax = 0; ay = 1;
	}

	n = poly->numverts;
	for (i = 0, j = n - 1; i < n; j = i++)
	{
		float xi = poly->verts[i][ax], yi = poly->verts[i][ay];
		float xj = poly->verts[j][ax], yj = poly->verts[j][ay];

		if (((yi > p[ay]) != (yj > p[ay])) &&
		    (p[ax] < (xj - xi) * (p[ay] - yi) / (yj - yi) + xi))
			inside = !inside;
	}

	if (inside || eps <= 0.0f)
		return inside != 0;

	for (i = 0, j = n - 1; i < n; j = i++)
	{
		const float x0 = poly->verts[j][ax], y0 = poly->verts[j][ay];
		const float x1 = poly->verts[i][ax], y1 = poly->verts[i][ay];
		const float dx = x1 - x0, dy = y1 - y0;
		const float len2 = dx * dx + dy * dy;
		float       t, ex, ey;

		if (len2 <= 0.0f)
			continue;

		t = ((p[ax] - x0) * dx + (p[ay] - y0) * dy) / len2;
		if (t < 0.0f)
			t = 0.0f;
		else if (t > 1.0f)
			t = 1.0f;

		ex = p[ax] - (x0 + t * dx);
		ey = p[ay] - (y0 + t * dy);
		if (ex * ex + ey * ey <= eps * eps)
			return true;
	}

	return false;
}

// World -> model space for a rigid brush transform (rotation R, translation t):
// local = R^T * (world - t).
static void QRE_WorldToModel (const QrTransform *transform, const vec3_t world, vec3_t out)
{
	vec3_t d;
	int    i;

	for (i = 0; i < 3; i++)
		d[i] = world[i] - transform->matrix[i][3];

	for (i = 0; i < 3; i++)
		out[i] = transform->matrix[0][i] * d[0] + transform->matrix[1][i] * d[1] + transform->matrix[2][i] * d[2];
}

static void QRE_SurfacePlane (const msurface_t *s, vec3_t normal, float *dist)
{
	if (s->flags & SURF_PLANEBACK)
	{
		VectorSubtract (vec3_origin, s->plane->normal, normal);
		*dist = -s->plane->dist;
	}
	else
	{
		VectorCopy (s->plane->normal, normal);
		*dist = s->plane->dist;
	}
}

static void QRE_DirToModel (const QrTransform *transform, const vec3_t dir, vec3_t out)
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = transform->matrix[0][i] * dir[0]
		       + transform->matrix[1][i] * dir[1]
		       + transform->matrix[2][i] * dir[2];
}

#define QRE_PICK_PLANE_EPS 1.0f
#define QRE_PICK_POLY_EPS  0.05f

static msurface_t *QRE_RayModelSurface (qmodel_t *model, const QrTransform *transform, vec3_t start, vec3_t dir,
                                        float maxt, float *out_t, vec3_t out_impact)
{
	vec3_t      local, localdir;
	msurface_t *best = NULL;
	float       best_t = maxt;
	int         i;

	QRE_WorldToModel (transform, start, local);
	QRE_DirToModel (transform, dir, localdir);

	for (i = 0; i < model->nummodelsurfaces; i++)
	{
		msurface_t *s = &model->surfaces[model->firstmodelsurface + i];
		vec3_t      snormal, hit;
		float       sdist, den, t;
		glpoly_t   *p;

		if (!s->texinfo || !s->texinfo->texture)
			continue;
		if (s->flags & (SURF_DRAWSKY | SURF_NOTEXTURE))
			continue;

		QRE_SurfacePlane (s, snormal, &sdist);

		den = DotProduct (snormal, localdir);
		if (den >= 0.0f)
			continue;

		t = (sdist - DotProduct (snormal, local)) / den;
		if (t < 0.0f || t >= best_t)
			continue;

		VectorMA (local, t, localdir, hit);

		for (p = s->polys; p; p = p->next)
		{
			if (!QRE_PointInPolygon (p, hit, snormal, QRE_PICK_POLY_EPS))
				continue;

			best = s;
			best_t = t;
			break;
		}
	}

	if (!best)
		return NULL;

	if (out_t)
		*out_t = best_t;
	VectorMA (start, best_t, dir, out_impact);
	return best;
}

// Finds the surface of a model whose face contains the impact point. Coplanar
// faces are told apart by a polygon test; the nearest plane is the fallback.
// The impact is world space, while a brush entity's planes and polygons are in
// its model space, so the point is transformed first (identity for the world).
static msurface_t *QRE_FindSurface (qmodel_t *model, const vec3_t impact, const vec3_t raydir, const QrTransform *transform)
{
	vec3_t      local, localdir;
	int         i;
	msurface_t *exactface = NULL, *exactany = NULL;
	msurface_t *nearface = NULL, *nearany = NULL;
	msurface_t *bestplane = NULL;
	float       exactface_t = 0.0f, exactface_fd = 0.0f, exactany_d = 0.0f;
	float       nearface_t = 0.0f, nearface_fd = 0.0f, nearany_d = 0.0f;
	float       bestplane_d = QRE_PICK_PLANE_EPS;

	QRE_WorldToModel (transform, impact, local);
	QRE_DirToModel (transform, raydir, localdir);

	for (i = 0; i < model->nummodelsurfaces; i++)
	{
		msurface_t *s = &model->surfaces[model->firstmodelsurface + i];
		vec3_t      snormal;
		float       sdist, fd, den, t;
		qboolean    exact = false, nearpoly = false;
		glpoly_t   *p;

		if (!s->texinfo || !s->texinfo->texture)
			continue;
		if (s->flags & (SURF_DRAWSKY | SURF_NOTEXTURE))
			continue;

		QRE_SurfacePlane (s, snormal, &sdist);
		fd = DotProduct (snormal, local) - sdist;
		if (fabsf (fd) > QRE_PICK_PLANE_EPS)
			continue;

		for (p = s->polys; p; p = p->next)
		{
			if (QRE_PointInPolygon (p, local, snormal, 0.0f))
			{
				exact = true;
				break;
			}
		}

		if (exact)
			nearpoly = true;
		else
		{
			for (p = s->polys; p; p = p->next)
			{
				if (QRE_PointInPolygon (p, local, snormal, QRE_PICK_POLY_EPS))
				{
					nearpoly = true;
					break;
				}
			}
		}

		if (!nearpoly)
		{
			if (fabsf (fd) < bestplane_d)
			{
				bestplane_d = fabsf (fd);
				bestplane = s;
			}
			continue;
		}

		if (exact)
		{
			if (!exactany || fabsf (fd) < exactany_d)
			{
				exactany_d = fabsf (fd);
				exactany = s;
			}
		}
		else if (!nearany || fabsf (fd) < nearany_d)
		{
			nearany_d = fabsf (fd);
			nearany = s;
		}

		den = DotProduct (snormal, localdir);
		if (den >= 0.0f)
			continue;

		t = (sdist - DotProduct (snormal, local)) / den;

		if (exact)
		{
			if (!exactface || t < exactface_t || (t == exactface_t && fabsf (fd) < fabsf (exactface_fd)))
			{
				exactface = s;
				exactface_t = t;
				exactface_fd = fd;
			}
		}
		else if (!nearface || t < nearface_t || (t == nearface_t && fabsf (fd) < fabsf (nearface_fd)))
		{
			nearface = s;
			nearface_t = t;
			nearface_fd = fd;
		}
	}

	if (exactface)
		return exactface;
	if (exactany)
		return exactany;
	if (nearface)
		return nearface;
	if (nearany)
		return nearany;
	return bestplane;
}

// The skin texture a model entity draws: the alias frame at the animation the
// renderer picked (it follows cl.time, which the editor freezes) or the sprite
// frame.
static gltexture_t *QRE_EntityTexture (entity_t *e)
{
	if (!e->model)
		return NULL;

	if (e->model->type == mod_alias)
	{
		aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (e->model);
		int         anim, skinnum;

		if (!hdr)
			return NULL;

		anim = (int)(cl.time * 10) & 3;
		skinnum = e->skinnum;
		if (skinnum < 0 || skinnum >= hdr->numskins)
			skinnum = 0;
		return hdr->gltextures[skinnum][anim];
	}

	if (e->model->type == mod_sprite)
	{
		mspriteframe_t *frame = R_GetSpriteFrame (e);

		return frame ? frame->gltexture : NULL;
	}

	return NULL;
}

// Moller-Trumbore: the fraction along the ray where it crosses the triangle,
// or -1. The direction is start->end scaled as the ray parameter.
static float QRE_RayTriangle (const vec3_t start, const vec3_t dir,
                              const vec3_t a, const vec3_t b, const vec3_t c)
{
	vec3_t e1, e2, p, t, q;
	float  det, inv, u, v, frac;

	VectorSubtract (b, a, e1);
	VectorSubtract (c, a, e2);
	CrossProduct (dir, e2, p);
	det = DotProduct (e1, p);
	if (fabsf (det) < 1e-8f)
		return -1.0f;
	inv = 1.0f / det;

	VectorSubtract (start, a, t);
	u = DotProduct (t, p) * inv;
	if (u < 0.0f || u > 1.0f)
		return -1.0f;

	CrossProduct (t, e1, q);
	v = DotProduct (dir, q) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return -1.0f;

	frac = DotProduct (e2, q) * inv;
	return (frac >= 0.0f && frac <= 1.0f) ? frac : -1.0f;
}

// A ray against an entity's model bounds (a rigid transform about the origin;
// an alias model's bounds already carry its own scale and scale_origin).
// Returns the fraction along the ray, or -1 when it misses.
static float QRE_TraceEntityBox (entity_t *e, const vec3_t start, const vec3_t end)
{
	float       m[16];
	QrTransform transform;
	vec3_t      mins, maxs, local_start, local_dir;
	float       tmin = 0.0f, tmax = 1.0f;
	int         i;

	if (!e->model || (e->model->type != mod_alias && e->model->type != mod_sprite))
		return -1.0f;

	if (e->model->type == mod_sprite)
	{
		mspriteframe_t *frame = R_GetSpriteFrame (e);
		vec3_t          corners[4];
		vec3_t          dir;
		float           t1, t2;

		if (!frame)
			return -1.0f;

		// the quad the renderer draws, axes and all (R_CreateSpriteVertices)
		R_GetSpriteQuadCorners (e, frame, corners);

		VectorSubtract (end, start, dir);
		t1 = QRE_RayTriangle (start, dir, corners[0], corners[1], corners[2]);
		t2 = QRE_RayTriangle (start, dir, corners[0], corners[2], corners[3]);
		if (t1 < 0.0f || (t2 >= 0.0f && t2 < t1))
			t1 = t2;
		return t1;
	}

	VectorCopy (e->model->mins, mins);
	VectorCopy (e->model->maxs, maxs);

	// an alias model with empty bounds cannot be tested
	if (mins[0] == maxs[0] && mins[1] == maxs[1] && mins[2] == maxs[2])
		return -1.0f;

	IdentityMatrix (m);
	R_RotateForEntity (m, e->origin, e->angles);
	transform = RT_GetModelTransform (m);

	for (i = 0; i < 3; i++)
	{
		vec3_t ds, de;

		VectorSubtract (start, e->origin, ds);
		VectorSubtract (end, e->origin, de);

		// local is the transpose of the rigid rotation applied to the delta
		local_start[i] = transform.matrix[0][i] * ds[0]
		               + transform.matrix[1][i] * ds[1]
		               + transform.matrix[2][i] * ds[2];
		local_dir[i]   = transform.matrix[0][i] * (de[0] - ds[0])
		               + transform.matrix[1][i] * (de[1] - ds[1])
		               + transform.matrix[2][i] * (de[2] - ds[2]);
	}

	for (i = 0; i < 3; i++)
	{
		const float d = local_dir[i];

		if (fabsf (d) < 1e-6f)
		{
			if (local_start[i] < mins[i] || local_start[i] > maxs[i])
				return -1.0f;
		}
		else
		{
			float t1 = (mins[i] - local_start[i]) / d;
			float t2 = (maxs[i] - local_start[i]) / d;

			if (t1 > t2)
			{
				const float t = t1;

				t1 = t2;
				t2 = t;
			}
			if (t1 > tmin)
				tmin = t1;
			if (t2 < tmax)
				tmax = t2;
			if (tmin > tmax)
				return -1.0f;
		}
	}

	return tmin;
}

static qboolean QRE_TracePick (qmodel_t **out_model, msurface_t **out_surf, entity_t **out_ent, gltexture_t **out_glt)
{
	vec3_t    start, end, dir;
	float     best = 1.0f;
	qmodel_t *bestmodel = NULL;
	entity_t *bestent = NULL;
	qboolean  bestisentity = false;
	vec3_t    bestimpact = { 0, 0, 0 };

	if (!cl.worldmodel)
		return false;

	VectorCopy (r_origin, start);
	VectorMA (start, 8192.0f, vpn, end);
	VectorSubtract (end, start, dir);

	{
		QrTransform transform = RT_GetBrushModelMatrix (NULL);
		float       t;
		vec3_t      impact;
		msurface_t *surf = QRE_RayModelSurface (cl.worldmodel, &transform, start, dir, best, &t, impact);

		if (surf)
		{
			best = t;
			bestmodel = cl.worldmodel;
			bestent = NULL;
			VectorCopy (impact, bestimpact);
			Con_DPrintf ("qr editor pick: candidate world surf %d t %.4f tex '%s'\n",
			             (int)(surf - cl.worldmodel->surfaces), t, surf->texinfo->texture->name);
		}
	}

	{
		int i;

		for (i = 1; i < cl.num_entities; i++)
		{
			entity_t   *e = &cl.entities[i];
			QrTransform transform;
			float       t;
			vec3_t      impact;
			msurface_t *surf;

			if (!e->model || e->model == cl.worldmodel || e->model->needload)
				continue;
			if (e->model->type != mod_brush)
				continue;
			if (e->alpha == ENTALPHA_ZERO)
				continue;

			transform = RT_GetBrushModelMatrix (e);
			surf = QRE_RayModelSurface (e->model, &transform, start, dir, best, &t, impact);
			if (!surf)
				continue;

			best = t;
			bestmodel = e->model;
			bestent = e;
			VectorCopy (impact, bestimpact);
			Con_DPrintf ("qr editor pick: candidate ent %d model '%s' surf %d t %.4f tex '%s'\n",
			             i, e->model->name, (int)(surf - e->model->surfaces), t,
			             surf->texinfo->texture->name);
		}
	}

	{
		int i;

		for (i = 1; i < cl.num_entities; i++)
		{
			entity_t *e = &cl.entities[i];
			float     f;

			if (!e->model || e->model == cl.worldmodel)
				continue;
			if (e->model->type != mod_alias && e->model->type != mod_sprite)
				continue;
			if (e == &cl.viewent || i == cl.viewentity)
				continue;
			if (e->alpha == ENTALPHA_ZERO || !QRE_EntityTexture (e))
				continue;

			f = QRE_TraceEntityBox (e, start, end);
			if (f >= 0.0f && f < best)
			{
				best = f;
				bestent = e;
				bestmodel = e->model;
				bestisentity = true;
				Con_DPrintf ("qr editor pick: candidate ent %d model '%s' alias/sprite t %.4f\n",
				             i, e->model->name, f);
			}
		}
	}

	if (!bestmodel)
		return false;

	if (bestisentity)
	{
		gltexture_t *glt = QRE_EntityTexture (bestent);

		if (!glt)
			return false;

		*out_surf = NULL;
		*out_model = bestmodel;
		if (out_ent)
			*out_ent = bestent;
		if (out_glt)
			*out_glt = glt;
		Con_DPrintf ("qr editor pick: winner ent %d model '%s' alias/sprite t %.4f tex '%s'\n",
		             (int)(bestent - cl.entities), bestmodel->name, best, glt->name);
		return true;
	}

	{
		QrTransform transform = RT_GetBrushModelMatrix (bestent);

		*out_surf = QRE_FindSurface (bestmodel, bestimpact, vpn, &transform);
	}
	if (!*out_surf)
		return false;

	VectorCopy (bestimpact, qre.pick_impact);
	qre.pick_impact_frame = (unsigned)host_framecount;

	*out_model = bestmodel;
	if (out_ent)
		*out_ent = bestent;
	if (out_glt)
		*out_glt = (*out_surf)->texinfo->texture->gltexture;
	Con_DPrintf ("qr editor pick: winner ent %d model '%s' surf %d t %.4f tex '%s'\n",
	             bestent ? (int)(bestent - cl.entities) : 0, bestmodel->name,
	             (int)(*out_surf - bestmodel->surfaces), best,
	             (*out_surf)->texinfo->texture->name);
	return true;
}

// ---------------------------------------------------------------------------
// The light editor's picking and wireframes
// ---------------------------------------------------------------------------

// The nearest light along the view ray, or -1.
static int QRE_LightUnderCrosshair (void)
{
	const rt_tracked_light_t *lights;
	int   count = 0, i, best = -1;
	float best_t = 1e30f;

	lights = RT_TRACK_Lights (&count);
	for (i = 0; i < count; i++)
	{
		vec3_t oc;
		float  b, c, disc, t;

		if (!lights[i].ready)
			continue;
		VectorSubtract (lights[i].position, r_origin, oc);
		b = DotProduct (oc, vpn);
		c = DotProduct (oc, oc) - lights[i].radius * lights[i].radius;
		disc = b * b - c;
		if (disc < 0.0f)
			continue;
		t = b - sqrtf (disc);
		if (t < 0.0f)
			t = b + sqrtf (disc);
		if (t < 0.0f)
			continue;
		if (t < best_t)
		{
			best_t = t;
			best = i;
		}
	}
	return best;
}

// The cursor mode: the panel is on screen and owns the cursor and the keyboard,
// and the camera stays where it stopped. Tab turns it on and off (from the
// flying side it arrives as a plain key, from the panel side through the SDL
// event hook), and picking a surface or a light turns it on too.
static void QRE_CursorMode (qboolean on)
{
	if (on == qre.panel_open)
		return;

	if (on)
	{
		qre.panel_open = true;

		// free the cursor, keeping its motion events for ImGui
		IN_FreeCursorForGui ();
		SDL_ShowCursor (SDL_DISABLE);
		QR_GUI_SetMouseCursor (1);
	}
	else
	{
		QRE_ClosePanel ();
	}
}

// Hover (flying) or select (fire button) a light by its wireframe.
static void QRE_DoLightPick (qboolean select)
{
	const rt_tracked_light_t *lights;
	int count = 0, index = QRE_LightUnderCrosshair ();

	if (!select)
	{
		qre.hover_light = index;
		return;
	}
	if (index < 0)
		return;

	lights = RT_TRACK_Lights (&count);
	if (index >= count)
		return;

	qre.sel_light = lights[index];
	qre.sel_light_valid = true;
	qre.hover_light = -1;
	q_strlcpy (qre.pick_name, qre.sel_light.name, sizeof (qre.pick_name));
	qre.pick_glt = NULL;
	qre.pick_surf = NULL;
	qre.pick_model = NULL;
	qre.pick_ent = NULL;

	Con_Printf ("qr light editor: picked light '%s' (%s)\n",
	            qre.sel_light.name[0] ? qre.sel_light.name : "(no emitter name)",
	            qre.sel_light.kind == RT_LIGHT_KIND_MATERIAL ? "material" :
	            qre.sel_light.kind == RT_LIGHT_KIND_DLIGHT ? "legacy dlight" :
	            qre.sel_light.kind == RT_LIGHT_KIND_CUSTOM ? "custom light" : "map light");

	// The picked light opens its own tab: an emitter light is edited in Entity,
	// an authored one in Custom, so the click lands on its rows.
	if (qre.sel_light.kind == RT_LIGHT_KIND_CUSTOM)
		qre.light_tab = 1;
	else
		qre.light_tab = 0;

	QRE_CursorMode (true);
}

// Keeps the selected light's live values in step with the frame's uploads.
static void QRE_RefreshSelectedLight (void)
{
	const rt_tracked_light_t *lights;
	int count = 0, i;

	if (!qre.sel_light_valid)
		return;

	lights = RT_TRACK_Lights (&count);
	for (i = 0; i < count; i++)
	{
		if (!lights[i].ready)
			continue;
		if (lights[i].uniqueID == qre.sel_light.uniqueID && lights[i].kind == qre.sel_light.kind)
		{
			qre.sel_light = lights[i];
			q_strlcpy (qre.pick_name, qre.sel_light.name, sizeof (qre.pick_name));
			return;
		}
	}
}

static qboolean QRE_WorldToScreen (const vec3_t p, float *out_x, float *out_y);
static void     QRE_DrawGizmoArrows (void);

static float QRE_DepthToCamera (const vec3_t p)
{
	vec3_t forward, right, up, d;

	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorSubtract (p, r_refdef.vieworg, d);
	return DotProduct (d, forward);
}

static float QRE_PixelsPerUnit (float depth)
{
	const float deg2rad = 3.14159265f / 180.0f;
	const float tan_x = tanf (r_refdef.fov_x * deg2rad * 0.5f);

	if (depth <= 0.0f || tan_x <= 0.0f)
		return 0.0f;

	return (float)glwidth * 0.5f / (depth * tan_x);
}

#define QRE_CUSTOM_GLYPH_SEGS 10

#define QRE_SPOT_CONE_LEN 96.0f
#define QRE_SPOT_CONE_SEGS 24
#define QRE_SPOT_CONE_RAYS 6

static void QRE_DrawSpotCone (const vec3_t pos, const vec3_t dir_in, float angle_deg, uint32_t color)
{
	const float deg2rad = 3.14159265f / 180.0f;
	vec3_t      dir, up, side, rim[QRE_SPOT_CONE_SEGS];
	float       angle, radius, xy[4];
	int         i, r;

	angle = CLAMP (0.0f, angle_deg * deg2rad, 89.5f * deg2rad);
	radius = QRE_SPOT_CONE_LEN * tanf (angle);

	VectorCopy (dir_in, dir);
	VectorNormalize (dir);

	if (fabsf (dir[2]) < 0.9f)
	{
		up[0] = up[1] = 0.0f;
		up[2] = 1.0f;
	}
	else
	{
		up[0] = 1.0f;
		up[1] = up[2] = 0.0f;
	}

	CrossProduct (dir, up, side);
	VectorNormalize (side);
	CrossProduct (side, dir, up);

	for (i = 0; i < QRE_SPOT_CONE_SEGS; i++)
	{
		const float a = 2.0f * 3.14159265f * (float)i / (float)QRE_SPOT_CONE_SEGS;
		const float cs = cosf (a), sn = sinf (a);
		int         j;

		for (j = 0; j < 3; j++)
		{
			rim[i][j] = pos[j] + dir[j] * QRE_SPOT_CONE_LEN
			          + (side[j] * cs + up[j] * sn) * radius;
		}
	}

	for (r = 0; r < QRE_SPOT_CONE_RAYS; r++)
	{
		const int i0 = r * QRE_SPOT_CONE_SEGS / QRE_SPOT_CONE_RAYS;
		float     x0, y0, x1, y1;

		if (!QRE_WorldToScreen (pos, &x0, &y0))
			break;
		if (!QRE_WorldToScreen (rim[i0], &x1, &y1))
			continue;

		xy[0] = x0;
		xy[1] = y0;
		xy[2] = x1;
		xy[3] = y1;
		QR_GUI_DrawPolyline (xy, 2, color, 2.0f);
	}

	for (i = 0; i < QRE_SPOT_CONE_SEGS; i++)
	{
		const int j = (i + 1) % QRE_SPOT_CONE_SEGS;
		float     x0, y0, x1, y1;

		if (!QRE_WorldToScreen (rim[i], &x0, &y0))
			continue;
		if (!QRE_WorldToScreen (rim[j], &x1, &y1))
			continue;

		xy[0] = x0;
		xy[1] = y0;
		xy[2] = x1;
		xy[3] = y1;
		QR_GUI_DrawPolyline (xy, 2, color, 2.0f);
	}
}

static void QRE_DrawLightWireframes (void)
{
	const rt_tracked_light_t *lights;
	const rt_custom_light_t  *custom;
	const float               deg2rad = 3.14159265f / 180.0f;
	int                       count = 0, custom_count = 0, i;

	lights = RT_TRACK_Lights (&count);
	custom = RT_CustomLights (&custom_count);

	for (i = 0; i < count; i++)
	{
		const rt_tracked_light_t *l = &lights[i];
		uint32_t                  color;
		float                     cx, cy, depth, scale;

		if (!l->ready)
			continue;
		if (!QRE_WorldToScreen (l->position, &cx, &cy))
			continue;

		depth = QRE_DepthToCamera (l->position);
		scale = QRE_PixelsPerUnit (depth);

		if (qre.sel_light_valid && l->uniqueID == qre.sel_light.uniqueID && l->kind == qre.sel_light.kind)
			color = RT_PackColorToUint32 (255, 255, 255, 255);
		else if (i == qre.hover_light)
			color = RT_PackColorToUint32 (255, 214, 64, 255);
		else
			color = RT_PackColorToUint32 (0, 255, 255, 200);

		QR_GUI_DrawCircle (cx, cy, l->radius * scale, color, 2.0f);

		if (l->kind == RT_LIGHT_KIND_CUSTOM)
		{
			float xy[(QRE_CUSTOM_GLYPH_SEGS + 1) * 2];
			vec3_t dvec;
			float  dist, radius;
			int    g;

			VectorSubtract (l->position, r_refdef.vieworg, dvec);
			dist = sqrtf (DotProduct (dvec, dvec));
			radius = CLAMP (1.0f, dist * 0.005f, 24.0f) * scale;

			for (g = 0; g <= QRE_CUSTOM_GLYPH_SEGS; g++)
			{
				const float a = (45.0f + 270.0f * (float)g / (float)QRE_CUSTOM_GLYPH_SEGS) * deg2rad;

				xy[g * 2 + 0] = cx + cosf (a) * radius;
				xy[g * 2 + 1] = cy - sinf (a) * radius;
			}

			QR_GUI_DrawPolyline (xy, QRE_CUSTOM_GLYPH_SEGS + 1, RT_PackColorToUint32 (200, 235, 255, 255), 2.0f);

			{
				const int ci = (int)(l->uniqueID - ((uint64_t)UINT32_MAX + 1));

				if (ci >= 0 && ci < custom_count && custom[ci].spot &&
				    (custom[ci].dir[0] != 0.0f || custom[ci].dir[1] != 0.0f || custom[ci].dir[2] != 0.0f))
				{
					QRE_DrawSpotCone (l->position, custom[ci].dir, custom[ci].angle_outer,
					                  RT_PackColorToUint32 (255, 214, 64, 255));
					QRE_DrawSpotCone (l->position, custom[ci].dir, custom[ci].angle_inner,
					                  RT_PackColorToUint32 (255, 255, 255, 255));
				}
			}
		}
	}

	QRE_DrawGizmoArrows ();
}

// Hover pick (crosshair, flying) or select pick (fire button).
static void QRE_DoPick (qboolean select)
{
	qmodel_t    *model;
	msurface_t  *surf;
	entity_t    *ent;
	gltexture_t *glt;

	// the light editor aims at the lights themselves, not at the surfaces
	if (qre.mode == QRE_MODE_LIGHT)
	{
		QRE_DoLightPick (select);
		return;
	}

	if (!QRE_TracePick (&model, &surf, &ent, &glt))
	{
		qre.hover_model = NULL;
		qre.hover_surf = NULL;
		qre.hover_ent = NULL;
		qre.hover_glt = NULL;
		return;
	}

	if (!select)
	{
		qre.hover_model = model;
		qre.hover_surf = surf;
		qre.hover_ent = ent;
		qre.hover_glt = glt;
		return;
	}

	if (!glt)
		return;
	if (!QR_GUI_Ready ())
	{
		QRE_Notify ("the ImGui panel is not available");
		return;
	}

	qre.pick_model = model;
	qre.pick_surf = surf;
	qre.pick_ent = ent;
	qre.pick_glt = glt;
	qre.hover_model = NULL;
	qre.hover_surf = NULL;
	qre.hover_ent = NULL;
	qre.hover_glt = NULL;

	{
		char texname[MAX_QPATH];
		char *dot;
		int   i;

		RT_MAT_NormalizeName (glt->name, texname, sizeof (texname));
		dot = strrchr (texname, '.');
		if (dot && !strchr (dot, ':'))
			*dot = '\0';

		q_strlcpy (qre.pick_name, texname, sizeof (qre.pick_name));

		if (qre.mode == QRE_MODE_LIGHT)
		{
			Con_Printf ("qr light editor: picked emitter '%s'\n", texname);
		}
		else
		{
			QRE_ResolveGroup (texname);

			Con_Printf ("qr editor: picked '%s' (%d material(s) in the group)\n", texname, qre.group_count);
			for (i = 0; i < qre.group_count; i++)
				Con_Printf ("qr editor:   group material '%s'\n", qre.group[i]->name);
		}
	}

	// the panel owns the mouse: free the cursor (keeping its motion events
	// for ImGui), freeze the camera
	QRE_CursorMode (true);
}

// ---------------------------------------------------------------------------
// Selection outline
// ---------------------------------------------------------------------------

#define QRE_OUTLINE_MAX 256

static void QRE_EmitOutline (qmodel_t *model, msurface_t *surf, entity_t *ent, uint32_t color)
{
	QrTransform transform = RT_GetBrushModelMatrix (ent);
	vec3_t      verts[QRE_OUTLINE_MAX];
	float       xy[(QRE_OUTLINE_MAX + 1) * 2];
	vec3_t      n_world, to_view, first;
	float       nudge = 0.35f;
	int         n = 0;
	int         i, j;

	if (surf->numedges > 0)
		n = surf->numedges;
	else if (surf->polys && surf->polys->numverts > 0)
		n = surf->polys->numverts;

	if (n < 3)
		return;
	if (n > QRE_OUTLINE_MAX)
		n = QRE_OUTLINE_MAX;

	if (surf->numedges > 0)
	{
		for (i = 0; i < n; i++)
		{
			const int e = model->surfedges[surf->firstedge + i];

			if (e >= 0)
				VectorCopy (model->vertexes[model->edges[e].v[0]].position, verts[i]);
			else
				VectorCopy (model->vertexes[model->edges[-e].v[1]].position, verts[i]);
		}
	}
	else
	{
		glpoly_t *p = surf->polys;

		for (i = 0; i < n; i++)
			VectorCopy (p->verts[i], verts[i]);
	}

	for (j = 0; j < 3; j++)
		n_world[j] = transform.matrix[j][0] * surf->plane->normal[0]
		           + transform.matrix[j][1] * surf->plane->normal[1]
		           + transform.matrix[j][2] * surf->plane->normal[2];

	for (j = 0; j < 3; j++)
		first[j] = transform.matrix[j][0] * verts[0][0]
		         + transform.matrix[j][1] * verts[0][1]
		         + transform.matrix[j][2] * verts[0][2]
		         + transform.matrix[j][3];
	VectorSubtract (r_origin, first, to_view);
	if (DotProduct (to_view, n_world) < 0.0f)
		nudge = -nudge;

	for (i = 0; i < n; i++)
	{
		vec3_t world;

		for (j = 0; j < 3; j++)
			world[j] = transform.matrix[j][0] * verts[i][0]
			         + transform.matrix[j][1] * verts[i][1]
			         + transform.matrix[j][2] * verts[i][2]
			         + transform.matrix[j][3]
			         + nudge * n_world[j];

		if (!QRE_WorldToScreen (world, &xy[i * 2], &xy[i * 2 + 1]))
			return;
	}

	xy[n * 2 + 0] = xy[0];
	xy[n * 2 + 1] = xy[1];

	QR_GUI_DrawPolyline (xy, n + 1, color, 2.0f);
}

static void QRE_EmitBoxOutline (entity_t *ent, uint32_t color)
{
	static const int edges[12][2] = {
		{ 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
		{ 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
	};
	float       m[16];
	QrTransform transform;
	vec3_t      mins, maxs;
	int         i, j;

	if (!ent || !ent->model)
		return;

	if (ent->model->type == mod_sprite)
	{
		mspriteframe_t *frame = R_GetSpriteFrame (ent);
		vec3_t          quad[4];
		float           xy[10];
		int             k;

		if (!frame)
			return;

		R_GetSpriteQuadCorners (ent, frame, quad);

		for (k = 0; k < 4; k++)
		{
			if (!QRE_WorldToScreen (quad[k], &xy[k * 2], &xy[k * 2 + 1]))
				return;
		}

		xy[8] = xy[0];
		xy[9] = xy[1];

		QR_GUI_DrawPolyline (xy, 5, color, 2.0f);
		return;
	}

	VectorCopy (ent->model->mins, mins);
	VectorCopy (ent->model->maxs, maxs);
	if (mins[0] == maxs[0] && mins[1] == maxs[1] && mins[2] == maxs[2])
	{
		mins[0] = mins[1] = mins[2] = -16.0f;
		maxs[0] = maxs[1] = maxs[2] = 16.0f;
	}

	IdentityMatrix (m);
	R_RotateForEntity (m, ent->origin, ent->angles);
	transform = RT_GetModelTransform (m);

	{
		vec3_t box[8];

		for (i = 0; i < 8; i++)
		{
			vec3_t local;

			local[0] = (i & 1) ? maxs[0] : mins[0];
			local[1] = (i & 2) ? maxs[1] : mins[1];
			local[2] = (i & 4) ? maxs[2] : mins[2];

			for (j = 0; j < 3; j++)
			{
				box[i][j] = transform.matrix[j][0] * local[0]
				          + transform.matrix[j][1] * local[1]
				          + transform.matrix[j][2] * local[2]
				          + transform.matrix[j][3];
			}
		}

		for (i = 0; i < 12; i++)
		{
			float seg[4];

			if (!QRE_WorldToScreen (box[edges[i][0]], &seg[0], &seg[1]))
				continue;
			if (!QRE_WorldToScreen (box[edges[i][1]], &seg[2], &seg[3]))
				continue;

			QR_GUI_DrawPolyline (seg, 2, color, 2.0f);
		}
	}
}

static void QRE_DrawOverlay (void)
{
	if (qre.mode == QRE_MODE_LIGHT)
	{
		QRE_DrawLightWireframes ();
		return;
	}

	if (qre.panel_open && (qre.pick_surf || (qre.pick_ent && qre.pick_glt)))
	{
		const uint32_t color = RT_PackColorToUint32 (255, 255, 255, 255);

		if (qre.pick_surf)
			QRE_EmitOutline (qre.pick_model, qre.pick_surf, qre.pick_ent, color);
		else
			QRE_EmitBoxOutline (qre.pick_ent, color);
	}
	else if (!qre.panel_open && qre.hover_surf)
	{
		QRE_EmitOutline (qre.hover_model, qre.hover_surf, qre.hover_ent, RT_PackColorToUint32 (255, 214, 64, 255));
	}
	else if (!qre.panel_open && qre.hover_surf == NULL && qre.hover_ent && qre.hover_glt)
	{
		QRE_EmitBoxOutline (qre.hover_ent, RT_PackColorToUint32 (255, 214, 64, 255));
	}
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

static void QRE_MoveCamera (void)
{
	vec3_t fwd, right, up;
	float  fm, sm, um, speed;
	int    i;

	AngleVectors (cl.viewangles, fwd, right, up);

	fm = (float)((in_forward.state & 1) - (in_back.state & 1));
	sm = (float)((in_moveright.state & 1) - (in_moveleft.state & 1));
	um = (float)((in_up.state & 1) - (in_down.state & 1));

	speed = 450.0f * (float)host_frametime;
	if (keydown[K_SHIFT])
		speed *= 3.0f;

	for (i = 0; i < 3; i++)
		qre.cam_origin[i] += (fwd[i] * fm + right[i] * sm + up[i] * um) * speed;
}

void QR_Editor_UpdateView (void)
{
	if (!qre.active)
		return;

	if (QR_Editor_Flying ())
		QRE_MoveCamera ();

	VectorCopy (qre.cam_origin, r_refdef.vieworg);
	VectorCopy (cl.viewangles, r_refdef.viewangles);

	// Before the frame renders: a re-synthesis replaces the material handles,
	// and the world's static upload (R_DrawWorldTask) runs later in this frame,
	// so the re-upload the flush asks for happens in the same frame.
	QRE_FlushDirty ();
}

// ---------------------------------------------------------------------------
// Panel (Dear ImGui)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Texture preview (the emissive color picker)
// ---------------------------------------------------------------------------

static void QRE_FreePreview (void)
{
	int i;

	for (i = 0; i < QRE_PREVIEW_SLOTS; i++)
	{
		qre_preview_t *slot = &qre.preview[i];

		if (slot->mat != QR_NO_MATERIAL)
			qrDestroyMaterial (vulkan_globals.instance, slot->mat);
		if (slot->pixels)
			Mem_Free (slot->pixels);
		memset (slot, 0, sizeof (*slot));
	}
}

// The pixels the emissive mask is synthesized from: the author's texture_base
// when the material has one, the engine texture of the picked face otherwise.
// The eyedropper samples this copy, so a picked color is the color the
// synthesis will match, and the same pixels become the preview's QrMaterial.
// A slot stays alive while the frame still draws it; a failed load is cached
// too, so a texture that cannot be read is not decoded once per frame.
static qre_preview_t *QRE_PreviewFor (rt_material_t *m)
{
	char          key[MAX_QPATH * 2 + 32];
	qre_preview_t *slot;
	byte         *pixels = NULL;
	int           i, victim = -1;
	int           w = 0, h = 0;

	if (!qre.pick_glt)
		return NULL;

	q_snprintf (key, sizeof (key), "%s|%s|%p", m->name, m->filename_base, (void *)qre.pick_glt);

	for (i = 0; i < QRE_PREVIEW_SLOTS; i++)
	{
		if (!strcmp (key, qre.preview[i].key))
		{
			qre.preview[i].last_frame = (unsigned)host_framecount;
			return (qre.preview[i].mat != QR_NO_MATERIAL) ? &qre.preview[i] : NULL;
		}
	}

	// an empty slot, or the least recently used one this frame has not drawn
	for (i = 0; i < QRE_PREVIEW_SLOTS; i++)
	{
		if (qre.preview[i].last_frame == (unsigned)host_framecount)
			continue;
		if (victim < 0 || qre.preview[i].last_frame < qre.preview[victim].last_frame)
			victim = i;
	}
	if (victim < 0)
		return NULL;

	slot = &qre.preview[victim];

	if (slot->mat != QR_NO_MATERIAL)
	{
		qrDestroyMaterial (vulkan_globals.instance, slot->mat);
		slot->mat = QR_NO_MATERIAL;
	}
	if (slot->pixels)
	{
		Mem_Free (slot->pixels);
		slot->pixels = NULL;
	}
	slot->w = slot->h = 0;
	q_strlcpy (slot->key, key, sizeof (slot->key));
	slot->last_frame = (unsigned)host_framecount;

	if (m->filename_base[0])
		pixels = RT_MAT_LoadTexture (m, RT_MAT_TEX_BASE, &w, &h);
	if (!pixels)
		pixels = TexMgr_LoadRgbaForPreview (qre.pick_glt, &w, &h);

	if (!pixels || w <= 0 || h <= 0)
	{
		if (pixels)
			Mem_Free (pixels);
		return NULL;
	}

	{
		QrMaterialCreateInfo info;

		memset (&info, 0, sizeof (info));
		info.size.width = (uint32_t)w;
		info.size.height = (uint32_t)h;
		info.textures.pDataAlbedoAlpha = pixels;
		info.filter = QR_SAMPLER_FILTER_LINEAR;
		info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

		if (qrCreateMaterial (vulkan_globals.instance, &info, &slot->mat) != QR_SUCCESS)
		{
			slot->mat = QR_NO_MATERIAL;
			Mem_Free (pixels);
			return NULL;
		}
	}

	slot->pixels = pixels;
	slot->w = w;
	slot->h = h;
	return slot;
}

// ---------------------------------------------------------------------------
// Per-parameter reset
// ---------------------------------------------------------------------------

// The state a parameter is reset to: the snapshot taken on start (retaken by
// Apply), or the defaults for a material the editor created in this session.
static const rt_material_t *QRE_OriginalOf (const rt_material_t *m, rt_material_t *defbuf)
{
	int i;

	for (i = 0; i < qre.snap_map_count; i++)
	{
		if (!strcmp (qre.snap_map[i].name, m->name))
			return &qre.snap_map[i];
	}
	for (i = 0; i < qre.snap_global_count; i++)
	{
		if (!strcmp (qre.snap_global[i].name, m->name))
			return &qre.snap_global[i];
	}

	QRE_InitDefault (defbuf, m->name);
	return defbuf;
}

static qboolean QRE_ParamChanged (const rt_material_t *m, const rt_material_t *orig, int p)
{
	// the metalness checkbox is the factor's "authored" flag: it is a change
	// by itself, even when the value happens to match
	if (p == PARAM_METAL)
		return m->has_metalness_factor != orig->has_metalness_factor ||
		       m->metalness_factor != orig->metalness_factor;

	if (p == PARAM_CEMIS)
	{
		// every block's color and tone controls count as one parameter
		return m->has_color_emissive != orig->has_color_emissive ||
		       memcmp (m->color_emissive, orig->color_emissive, sizeof (m->color_emissive)) != 0;
	}

	switch (qre_params[p].type)
	{
	case QRE_T_FLOAT:  return QRE_GetFloat (m, p) != QRE_GetFloat (orig, p);
	case QRE_T_INT:    return QRE_GetInt (m, p) != QRE_GetInt (orig, p);
	case QRE_T_BOOL:   return QRE_GetBool (m, p) != QRE_GetBool (orig, p);
	case QRE_T_TEXT:   return strcmp (QRE_GetText (m, p), QRE_GetText (orig, p)) != 0;
	case QRE_T_COLOR:
	{
		qboolean e1, e2;
		float    c1[3], c2[3];

		QRE_GetColor (m, p, &e1, c1);
		QRE_GetColor (orig, p, &e2, c2);
		return e1 != e2 || c1[0] != c2[0] || c1[1] != c2[1] || c1[2] != c2[2];
	}
	default:
		return false;
	}
}

static void QRE_ResetParam (int g, int p, const rt_material_t *orig)
{
	rt_material_t *m;

	if (p == PARAM_METAL && !orig->has_metalness_factor)
	{
		// QRE_SetFloat would author the factor; the original never had one
		QRE_EnsureLive (g);
		m = qre.group[g];
		m->has_metalness_factor = false;
		m->metalness_factor = orig->metalness_factor;
		QRE_MarkDirty (m);
		return;
	}

	if (p == PARAM_MIRROR)
	{
		// mirror forces roughness_override to 0 while it is on; un-mirroring
		// brings the original override back with it
		QRE_SetBool (g, p, QRE_GetBool (orig, p));
		if (!QRE_GetBool (orig, p))
			QRE_SetFloat (g, PARAM_ROUGH, QRE_GetFloat (orig, PARAM_ROUGH));
		return;
	}

	if (p == PARAM_CEMIS)
	{
		QRE_EnsureLive (g);
		m = qre.group[g];
		m->has_color_emissive = orig->has_color_emissive;
		m->color_emissive_count = orig->color_emissive_count;
		m->emissive_blend = orig->emissive_blend;
		memcpy (m->color_emissive, orig->color_emissive, sizeof (m->color_emissive));
		QRE_MarkDirty (m);
		return;
	}

	switch (qre_params[p].type)
	{
	case QRE_T_FLOAT:
		QRE_SetFloat (g, p, QRE_GetFloat (orig, p));
		break;
	case QRE_T_INT:
		QRE_SetInt (g, p, QRE_GetInt (orig, p));
		break;
	case QRE_T_BOOL:
		QRE_SetBool (g, p, QRE_GetBool (orig, p));
		break;
	case QRE_T_TEXT:
		QRE_SetText (g, p, QRE_GetText (orig, p));
		break;
	case QRE_T_COLOR:
	{
		qboolean enabled;
		float    rgb[3];
		int      c;

		// the channels are restored even behind a disabled color: enabling it
		// again has to show the original tint, not the last one edited
		QRE_GetColor (orig, p, &enabled, rgb);
		for (c = 0; c < 3; c++)
			QRE_SetColorChannel (g, p, c, rgb[c]);
		QRE_SetColorEnabled (g, p, enabled);
		break;
	}
	default:
		break;
	}
}

// The Emissive section of a material: the color blocks with their previews (the
// eyedropper for a color, the polygon editor for a mask) and the buttons that
// add one. The preview comes first so adding blocks never pushes it off the
// panel, and a click picks into the last color — adding a block is the button's
// job.
static void QRE_EmissiveEditor (int g)
{
	rt_material_t *m = qre.group[g];
	int            ci;

	for (ci = 0; ci < m->color_emissive_count; ci++)
	{
		char     id[32];
		char     label[32];
		qboolean open;
		qboolean poly;

		q_snprintf (id, sizeof (id), "cemis%d", ci);
		poly = (m->color_emissive[ci].poly_count >= 3) ? true : false;
		q_snprintf (label, sizeof (label), poly ? "Mask %d" : "Color %d", ci + 1);

		QR_GUI_PushID (id);
		open = QR_GUI_Section (label, 1) ? true : false;

		if (open)
		{
			if (poly)
			{
				if (QR_GUI_Button ("Remove mask"))
				{
					int k;

					for (k = ci; k + 1 < m->color_emissive_count; k++)
						m->color_emissive[k] = m->color_emissive[k + 1];
					m->color_emissive_count--;
					if (m->color_emissive_count == 0)
						m->has_color_emissive = false;
					QRE_MarkDirty (m);
					QR_GUI_PopID ();
					break;
				}
				QR_GUI_Tooltip ("Remove this mask block");

				// this block's own preview: the polygon editor draws and edits
				// the mask over the texture
				{
					qre_preview_t *slot = QRE_PreviewFor (m);

					if (slot)
					{
						float poly_uv[RT_MAT_EMIS_POLY_MAX][2];
						int   poly_count = m->color_emissive[ci].poly_count;

						memcpy (poly_uv, m->color_emissive[ci].poly_uv, sizeof (poly_uv));

						if (QR_GUI_PolygonEdit ("##mask_pick", (int64_t)slot->mat, slot->w, slot->h,
						                        poly_uv, &poly_count, RT_MAT_EMIS_POLY_MAX))
						{
							QRE_EnsureLive (g);
							m = qre.group[g];
							memcpy (m->color_emissive[ci].poly_uv, poly_uv, sizeof (poly_uv));
							m->color_emissive[ci].poly_count = poly_count;
							QRE_MarkDirty (m);
						}
					}
				}
				QR_GUI_Tooltip ("Drag a point to move it, an edge to slide it, the mask to move the whole polygon, Ctrl+click to add a point, right-click a point to remove it");
			}
			else
			{
				const int res = QR_GUI_ColorRow ("##color", m->color_emissive[ci].color,
				                                 "The color this block matches, up to the threshold's distance.");

				if (res & 1)
					QRE_MarkDirty (m);
				if (res & 2)
				{
					int k;

					for (k = ci; k + 1 < m->color_emissive_count; k++)
						m->color_emissive[k] = m->color_emissive[k + 1];
					m->color_emissive_count--;
					if (m->color_emissive_count == 0)
						m->has_color_emissive = false;
					QRE_MarkDirty (m);
					QR_GUI_PopID ();
					break;
				}

				// this block's own preview: the eyedropper picks a color into it
				{
					qre_preview_t *slot = QRE_PreviewFor (m);

					if (slot)
					{
						float u = 0.5f, v = 0.5f;

						if (QR_GUI_ImagePick ("##color_pick", (int64_t)slot->mat, slot->w, slot->h, &u, &v))
						{
							const int   px = CLAMP (0, (int)(u * (float)slot->w), slot->w - 1);
							const int   py = CLAMP (0, (int)(v * (float)slot->h), slot->h - 1);
							const byte *pix = slot->pixels + ((size_t)py * (size_t)slot->w + (size_t)px) * 4;

							QRE_EnsureLive (g);
							m = qre.group[g];
							m->color_emissive[ci].color[0] = pix[0] / 255.0f;
							m->color_emissive[ci].color[1] = pix[1] / 255.0f;
							m->color_emissive[ci].color[2] = pix[2] / 255.0f;
							QRE_MarkDirty (m);
						}
					}
				}

				if (QR_GUI_SliderFloat ("color_emissive_threshold", &m->color_emissive[ci].threshold, 0.0f, 1.0f,
				                        "How far a pixel's color may differ from this block's color and still glow."))
					QRE_MarkDirty (m);
			}
			{
				int feather = (int)(m->color_emissive[ci].feather + 0.5f);

				if (QR_GUI_SliderInt ("color_emissive_feather", &feather, 0, 16,
				                      "Softens this block's mask edge over this many pixels, extending it outward without dimming the pixels it selected."))
				{
					m->color_emissive[ci].feather = (float)feather;
					QRE_MarkDirty (m);
				}
			}
			if (QR_GUI_SliderFloat ("emissive_factor", &m->color_emissive[ci].factor, 0.0f, 5.0f,
			                        "Scales this block's emission: below 1 it dims, above 1 it brightens."))
				QRE_MarkDirty (m);
			{
				int idx = m->color_emissive[ci].blend + 1;

				if (QR_GUI_Combo ("emissive_blend", &idx, qre_emissive_blends, (int)countof (qre_emissive_blends),
				                  "How this block's glow is composited into the frame (normal, screen, overlay...)."))
				{
					m->color_emissive[ci].blend = idx - 1;
					QRE_MarkDirty (m);
				}
			}
		}

		QR_GUI_PopID ();
	}

	if (m->color_emissive_count < RT_MAT_MAX_EMISSIVE_COLORS)
	{
		if (QR_GUI_Button ("Add color"))
		{
			rt_emissive_t *block;

			QRE_EnsureLive (g);
			m = qre.group[g];
			block = &m->color_emissive[m->color_emissive_count];
			memset (block, 0, sizeof (*block));
			block->color[0] = 1.0f;
			block->threshold = (m->color_emissive_threshold > 0.0f) ? m->color_emissive_threshold : 0.02f;
			block->feather = m->color_emissive_feather;
			block->factor = m->emissive_factor;
			block->blend = m->emissive_blend;
			block->has_threshold = block->has_feather = true;
			block->has_factor = block->has_blend = true;
			m->color_emissive_count++;
			m->has_color_emissive = true;
			QRE_MarkDirty (m);
		}
		QR_GUI_Tooltip ("Add a color block: it glows where the texture matches its color (up to ten)");
		QR_GUI_SameLine ();
		if (QR_GUI_Button ("Add mask"))
		{
			rt_emissive_t *block;

			QRE_EnsureLive (g);
			m = qre.group[g];
			block = &m->color_emissive[m->color_emissive_count];
			memset (block, 0, sizeof (*block));
			block->color[0] = 1.0f;
			block->threshold = (m->color_emissive_threshold > 0.0f) ? m->color_emissive_threshold : 0.02f;
			block->feather = m->color_emissive_feather;
			block->factor = m->emissive_factor;
			block->blend = m->emissive_blend;
			block->has_threshold = block->has_feather = true;
			block->has_factor = block->has_blend = true;
			block->poly_count = 4;
			block->poly_uv[0][0] = 0.25f; block->poly_uv[0][1] = 0.25f;
			block->poly_uv[1][0] = 0.75f; block->poly_uv[1][1] = 0.25f;
			block->poly_uv[2][0] = 0.75f; block->poly_uv[2][1] = 0.75f;
			block->poly_uv[3][0] = 0.25f; block->poly_uv[3][1] = 0.75f;
			m->color_emissive_count++;
			m->has_color_emissive = true;
			QRE_MarkDirty (m);
		}
		QR_GUI_Tooltip ("Add a mask block: a white square over the texture; drag its points to shape the glow (up to ten)");
	}
}

static void QRE_ParamRow (int g, int p, const rt_material_t *orig)
{
	const char   *label = qre_params[p].label;
	const char   *tip   = qre_params[p].tip;
	// re-read every iteration: the first change of a material that has no
	// yaml entry moves qre.group[g] into the live list (QRE_EnsureLive)
	rt_material_t *m = qre.group[g];
	// mirror drives roughness on its own (the synthesis gives it the last
	// word): the override is meaningless there, and is locked at 0
	const qboolean mirror_locks_rough = (p == PARAM_ROUGH && m->mirror);
	const qboolean is_light_locks_color = (p == PARAM_LCOLOR && !m->is_light);

	if (mirror_locks_rough || is_light_locks_color)
		QR_GUI_PushDisabled (1);

	switch (qre_params[p].type)
	{
	case QRE_T_TEXT:
	{
		char buf[MAX_QPATH];
		int  res;
		char file[MAX_QPATH];

		q_strlcpy (buf, QRE_GetText (m, p), sizeof (buf));
		res = QR_GUI_TexturePath (label, buf, sizeof (buf), tip);
		if (res & 1)
		{
			// NONE typed by hand means "no texture", as an empty field does.
			// Normalize before the compare: typing NONE into an empty field
			// is not an edit, and it must not create the material.
			if (!q_strcasecmp (buf, "NONE"))
				buf[0] = '\0';
			if (strcmp (buf, QRE_GetText (m, p)))
				QRE_SetText (g, p, buf);
		}
		if (res & 2)
		{
			if (QRE_BrowseTexture (file, sizeof (file)))
				QRE_SetText (g, p, file);
		}
		break;
	}
	case QRE_T_FLOAT:
	{
		float       value = QRE_GetFloat (m, p);
		const float step = qre_params[p].step;
		const float grid = (step > 0.0f && step < 0.01f) ? step : 0.01f;

		if (QR_GUI_SliderFloatFmt (label, &value, qre_params[p].min, qre_params[p].max,
		                           grid < 0.01f ? "%.4f" : "%.2f", tip))
		{
			// the panel shows the parameter's precision; a value out of the
			// slider (or typed) is snapped to that grid. A reset does not pass
			// through here, so it restores the snapshot exactly.
			QRE_SetFloat (g, p, roundf (value / grid) * grid);
		}
		break;
	}
	case QRE_T_INT:
	{
		int value = QRE_GetInt (m, p);

		if (QR_GUI_SliderInt (label, &value, (int)qre_params[p].min, (int)qre_params[p].max, tip))
			QRE_SetInt (g, p, value);
		break;
	}
	case QRE_T_BOOL:
	{
		int value = QRE_GetBool (m, p) ? 1 : 0;
		if (QR_GUI_Checkbox (label, &value, tip))
			QRE_SetBool (g, p, value != 0);
		break;
	}
	case QRE_T_COLOR:
	{
		qboolean enabled;
		float    rgb[3];
		float    old_rgb[3];
		int      en;
		int      c;

		QRE_GetColor (m, p, &enabled, rgb);
		VectorCopy (rgb, old_rgb);
		en = enabled ? 1 : 0;

		if (QR_GUI_ColorHex (label, rgb, &en, tip))
		{
			if (!en)
			{
				QRE_SetColorEnabled (g, p, false);
			}
			else
			{
				QRE_SetColorEnabled (g, p, true);
				for (c = 0; c < 3; c++)
					if (rgb[c] != old_rgb[c])
						QRE_SetColorChannel (g, p, c, rgb[c]);
			}
		}
		break;
	}
	default:
		break;
	}

	// Reset one parameter to the state it had when the editor started
	// (or to the defaults, for a material the editor created itself).
	m = qre.group[g];
	if (QR_GUI_ResetButton (label, !mirror_locks_rough && !is_light_locks_color && QRE_ParamChanged (m, orig, p)))
		QRE_ResetParam (g, p, orig);

	if (mirror_locks_rough || is_light_locks_color)
		QR_GUI_PopDisabled ();
}

static void QRE_ParamWidgets (int g)
{
	rt_material_t        defbuf;
	const rt_material_t *orig = QRE_OriginalOf (qre.group[g], &defbuf);
	const char          *section = NULL;
	int                  p;

	for (p = 0; p < PARAM_COUNT; p++)
	{
		if (p >= PARAM_LSTYLES && p <= PARAM_LUPOFF && !qre.group[g]->is_light)
			continue;

		if (qre_params[p].section && (!section || strcmp (section, qre_params[p].section)))
		{
			section = qre_params[p].section;
			if (p == PARAM_LSTYLES)
				QR_GUI_SectionTitle (section);
			else if (section[0])
				QR_GUI_SectionHeader (section);
			else
				QR_GUI_Separator ();
		}

		QRE_ParamRow (g, p, orig);

		// The Emissive blocks sit right under the color_emissive checkbox row
		// (its own row is the one just drawn); without the checkbox there are
		// none.
		if (p == PARAM_CEMIS && qre.group[g]->has_color_emissive)
		{
			QRE_EmissiveEditor (g);
		}
	}
}

static void QRE_PanelActionRow (void (*on_exit)(void))
{
	int torch = qre.torch ? 1 : 0;

	if (QR_GUI_Checkbox ("torch mode", &torch,
	                     "A small light on the editor camera to see by until the map has light."))
		qre.torch = torch ? true : false;

	if (QR_GUI_Button ("Save"))
		QRE_Apply ();
	QR_GUI_SameLine ();
	if (QR_GUI_Button ("Cancel"))
		QRE_Cancel ();
	QR_GUI_SameLine ();
	if (QR_GUI_Button ("Exit"))
		on_exit ();
	QR_GUI_Separator ();
	QR_GUI_BeginScroll ();
}

#define QRE_SNAPSHOT_MAX 256

static const struct
{
	const char *name;
	float       min, max;
	const char *tip;
} qre_water[] = {
	{ "rt_water_speed",       0.0f,   2.0f, "How fast the water waves travel." },
	{ "rt_water_normstren",   0.0f,   4.0f, "How strongly the water's normal map bends the surface." },
	{ "rt_water_normsharp",   0.0f,  16.0f, "Sharpness of the water's normal map: higher tightens the ripple pattern." },
	{ "rt_water_scale",       0.0f,   4.0f, "Scale of the wave pattern over the water: larger stretches the waves." },
};

static char     qre_water_snapshot[countof (qre_water)][QRE_SNAPSHOT_MAX];
static qboolean qre_water_snapshot_set[countof (qre_water)];
static float    qre_water_color_snapshot[3];
static float    qre_acid_color_snapshot[3];

static const char *const qre_mat_cvars[] = {
	"rt_dtal_clearance",
	"rt_dtal_maxpolys",
	"rt_dtal_minarea",
	"rt_dtal_model_minarea",
	"rt_dtal_model_maxpolys",
	"rt_dtal_model_budget",
};

static char     qre_mat_cvar_snapshot[countof (qre_mat_cvars)][QRE_SNAPSHOT_MAX];
static qboolean qre_mat_cvar_snapshot_set[countof (qre_mat_cvars)];

static void QRE_MatCvarsRestore (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_mat_cvars); i++)
	{
		if (qre_mat_cvar_snapshot_set[i])
			Cvar_Set (qre_mat_cvars[i], qre_mat_cvar_snapshot[i]);
	}
}

static void QRE_WaterColorSet (const char *name, const float rgb[3])
{
	Cvar_Set (name, va ("%d %d %d",
	                    (int)(CLAMP (0.0f, rgb[0], 1.0f) * 255.0f + 0.5f),
	                    (int)(CLAMP (0.0f, rgb[1], 1.0f) * 255.0f + 0.5f),
	                    (int)(CLAMP (0.0f, rgb[2], 1.0f) * 255.0f + 0.5f)));
}

static void QRE_TakeWaterSnapshot (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_water); i++)
	{
		cvar_t *var = Cvar_FindVar (qre_water[i].name);

		if (!var)
		{
			qre_water_snapshot_set[i] = false;
			qre_water_snapshot[i][0] = '\0';
			continue;
		}

		qre_water_snapshot_set[i] = true;
		q_strlcpy (qre_water_snapshot[i], var->string ? var->string : "", sizeof (qre_water_snapshot[i]));
	}

	RT_GetWaterColor (qre_water_color_snapshot);
	RT_GetAcidColor (qre_acid_color_snapshot);

	for (i = 0; i < (int)countof (qre_mat_cvars); i++)
	{
		cvar_t *var = Cvar_FindVar (qre_mat_cvars[i]);

		if (!var)
		{
			qre_mat_cvar_snapshot_set[i] = false;
			qre_mat_cvar_snapshot[i][0] = '\0';
			continue;
		}

		qre_mat_cvar_snapshot_set[i] = true;
		q_strlcpy (qre_mat_cvar_snapshot[i], var->string ? var->string : "", sizeof (qre_mat_cvar_snapshot[i]));
	}
}

static qboolean QRE_WaterTouched (void)
{
	float color[3];
	int   i;

	for (i = 0; i < (int)countof (qre_water); i++)
	{
		cvar_t *var;

		if (!qre_water_snapshot_set[i])
			continue;

		var = Cvar_FindVar (qre_water[i].name);
		if (var && strcmp (var->string ? var->string : "", qre_water_snapshot[i]))
			return true;
	}

	RT_GetWaterColor (color);
	if (memcmp (color, qre_water_color_snapshot, sizeof (color)))
		return true;
	RT_GetAcidColor (color);
	if (memcmp (color, qre_acid_color_snapshot, sizeof (color)) != 0)
		return true;

	for (i = 0; i < (int)countof (qre_mat_cvars); i++)
	{
		cvar_t *var;

		if (!qre_mat_cvar_snapshot_set[i])
			continue;

		var = Cvar_FindVar (qre_mat_cvars[i]);
		if (var && strcmp (var->string ? var->string : "", qre_mat_cvar_snapshot[i]))
			return true;
	}
	return false;
}

static void QRE_WaterRestore (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_water); i++)
	{
		if (qre_water_snapshot_set[i])
			Cvar_Set (qre_water[i].name, qre_water_snapshot[i]);
	}

	QRE_WaterColorSet ("rt_water_color", qre_water_color_snapshot);
	QRE_WaterColorSet ("rt_water_acidcolor", qre_acid_color_snapshot);

	QRE_MatCvarsRestore ();
}

static void QRE_MatWaterSection (void)
{
	int i;

	QR_GUI_Spacing ();
	QR_GUI_Separator ();
	QR_GUI_Label ("Water / Acid");
	QR_GUI_Spacing ();

	for (i = 0; i < (int)countof (qre_water); i++)
	{
		cvar_t *var = Cvar_FindVar (qre_water[i].name);
		float   value;
		qboolean changed;

		if (!var)
		{
			QR_GUI_LabelDim (va ("%s: no such cvar", qre_water[i].name));
			continue;
		}

		value = var->value;
		if (QR_GUI_SliderFloat (qre_water[i].name, &value, qre_water[i].min, qre_water[i].max, qre_water[i].tip))
			Cvar_Set (qre_water[i].name, va ("%.4g", value));

		changed = qre_water_snapshot_set[i] &&
		          strcmp (var->string ? var->string : "", qre_water_snapshot[i]) != 0;
		if (QR_GUI_ResetButton (qre_water[i].name, changed))
			Cvar_Set (qre_water[i].name, qre_water_snapshot[i]);
	}

	{
		float rgb[3];
		int   en = 1;

		RT_GetWaterColor (rgb);
		if (QR_GUI_ColorHex ("rt_water_color", rgb, &en, "The color of the water surface."))
			QRE_WaterColorSet ("rt_water_color", rgb);
		if (QR_GUI_ResetButton ("rt_water_color", memcmp (rgb, qre_water_color_snapshot, sizeof (rgb)) != 0))
		{
			VectorCopy (qre_water_color_snapshot, rgb);
			QRE_WaterColorSet ("rt_water_color", rgb);
		}

		RT_GetAcidColor (rgb);
		if (QR_GUI_ColorHex ("rt_water_acidcolor", rgb, &en, "The color of the acid."))
			QRE_WaterColorSet ("rt_water_acidcolor", rgb);
		if (QR_GUI_ResetButton ("rt_water_acidcolor", memcmp (rgb, qre_acid_color_snapshot, sizeof (rgb)) != 0))
		{
			VectorCopy (qre_acid_color_snapshot, rgb);
			QRE_WaterColorSet ("rt_water_acidcolor", rgb);
		}
	}

	QR_GUI_Spacing ();
	QR_GUI_LabelDim ("the water, acid and DTAL values are saved to the config by Save; Cancel puts them back");
}

static void QRE_MatSystemTab (void)
{
	static const char *const dbg_modes[] = { "off", "wireframe", "normals" };
	float                     value;
	int                       dbg = CVAR_TO_INT32 (rt_dtal_debug);
	int                       maxpolys;

	if (dbg < 0 || dbg > 2)
		dbg = 0;
	if (QR_GUI_Combo ("debug DTAL", &dbg, dbg_modes, (int)countof (dbg_modes),
	                  "Draw the triangle area lights every model and sprite pose generates: the polygons (wireframe) or their centers and normals (rt_dtal_debug)."))
		Cvar_Set ("rt_dtal_debug", va ("%d", dbg));

	QR_GUI_Spacing ();

	QR_GUI_SectionHeader ("DTAL (BSP)");

	value = CVAR_TO_FLOAT (rt_dtal_clearance);
	if (QR_GUI_SliderFloat ("rt_dtal_clearance", &value, 0.0f, 16.0f,
	                        "A DTAL polygon facing solid geometry within this many units is not created (0 off)."))
		Cvar_Set ("rt_dtal_clearance", va ("%.4g", value));

	maxpolys = CVAR_TO_INT32 (rt_dtal_maxpolys);
	if (QR_GUI_SliderInt ("rt_dtal_maxpolys", &maxpolys, 0, 64,
	                      "Caps one surface's DTAL pieces, the largest kept (0 = no cuts)."))
		Cvar_Set ("rt_dtal_maxpolys", va ("%d", maxpolys));

	value = CVAR_TO_FLOAT (rt_dtal_minarea);
	if (QR_GUI_SliderFloat ("rt_dtal_minarea", &value, 0.0f, 1024.0f,
	                        "Drops a DTAL polygon under this area, in world units squared (0 off)."))
		Cvar_Set ("rt_dtal_minarea", va ("%.4g", value));

	QR_GUI_SectionHeader ("DTAL (models)");

	value = CVAR_TO_FLOAT (rt_dtal_model_minarea);
	if (QR_GUI_SliderFloat ("rt_dtal_model_minarea", &value, 0.0f, 1024.0f,
	                        "Drops a model's DTAL polygon under this area, in world units squared (0 off)."))
		Cvar_Set ("rt_dtal_model_minarea", va ("%.4g", value));

	maxpolys = CVAR_TO_INT32 (rt_dtal_model_maxpolys);
	if (QR_GUI_SliderInt ("rt_dtal_model_maxpolys", &maxpolys, 0, 64,
	                      "Caps one model's DTAL pieces, the largest kept (0 = no cuts)."))
		Cvar_Set ("rt_dtal_model_maxpolys", va ("%d", maxpolys));

	maxpolys = CVAR_TO_INT32 (rt_dtal_model_budget);
	if (QR_GUI_SliderInt ("rt_dtal_model_budget", &maxpolys, 0, 4096,
	                      "Caps the DTAL pieces every model of a frame may upload, the largest ranked first (0 = none)."))
		Cvar_Set ("rt_dtal_model_budget", va ("%d", maxpolys));

	QRE_MatWaterSection ();
}

static void QRE_BuildPanelGUI (void)
{
	int      panel_w = glwidth / 4; // a quarter of the screen wide, as asked
	int      g;

	// Below this the fixed label column and the browse and reset buttons stop
	// fitting: a quarter of a small window is not worth an unusable panel.
	if (panel_w < 352)
		panel_w = 352;

	QR_GUI_BeginPanel ("qr_material_editor", glwidth - panel_w, 0, panel_w, glheight);

	QR_GUI_Label ("MATERIAL EDITOR");

	if (qre.pick_glt)
	{
		char buf[MAX_QPATH + 16];

		q_snprintf (buf, sizeof (buf), qre.pick_surf ? "face: %s" : "model: %s",
		            qre.pick_name[0] ? qre.pick_name : qre.pick_glt->name);
		QR_GUI_LabelDim (buf);
	}
	QR_GUI_Spacing ();

	{
		static const char *const tabs[] = { "Materials", "System" };
		int reset = 0;

		QR_GUI_Tabs ("material_tabs", tabs, (int)countof (tabs), &qre.mat_tab, &reset);
		if (reset)
			qre.reset_prompt = true;
		QR_GUI_Spacing ();
	}

	QRE_PanelActionRow (QRE_RequestExit);

	if (qre.mat_tab == 1)
	{
		QRE_MatSystemTab ();
		QR_GUI_EndScroll ();
		QR_GUI_EndPanel ();
		return;
	}

	for (g = 0; g < qre.group_count; g++)
	{
		// the animation frames share their parameter names, so every section
		// needs its own ID scope, or their widgets collide
		QR_GUI_PushID (qre.group[g]->name);
		if (QR_GUI_Section (qre.group[g]->name, 1))
			QRE_ParamWidgets (g);
		QR_GUI_PopID ();
	}

	QR_GUI_EndScroll ();
	QR_GUI_EndPanel ();
}

// ---------------------------------------------------------------------------
// The light editor's entries: fields, their originals, and the group
// ---------------------------------------------------------------------------

enum
{
	QRE_LIGHT_F_RADIUS = 0,
	QRE_LIGHT_F_INTENSITY,
	QRE_LIGHT_F_OFFSET,
	QRE_LIGHT_F_OFFSET_X,
	QRE_LIGHT_F_OFFSET_Y,
	QRE_LIGHT_F_OFFSET_Z,
	QRE_LIGHT_F_COLOR,
	QRE_LIGHT_F_STYLE,
	QRE_LIGHT_F_FRAST,
};

// The state the entry had when the editor started (NULL when it had none).
static const rt_light_t *QRE_LightOriginal (const char *name)
{
	int i;

	for (i = 0; i < qre.snap_light_count; i++)
	{
		if (qre.snap_lights[i].valid && !strcmp (qre.snap_lights[i].name, name))
			return &qre.snap_lights[i];
	}
	return NULL;
}

// One field of one entry.
static void QRE_LightApply (rt_light_t *l, int field, float v0, float v1, float v2, qboolean b)
{
	switch (field)
	{
	case QRE_LIGHT_F_RADIUS:    l->radius = v0; l->has_radius = true; break;
	case QRE_LIGHT_F_INTENSITY: l->intensity = v0; l->has_intensity = true; break;
	case QRE_LIGHT_F_OFFSET:
		l->offset[0] = v0; l->offset[1] = v1; l->offset[2] = v2;
		l->has_offset = true;
		break;
	case QRE_LIGHT_F_OFFSET_X:  l->offset[0] = v0; l->has_offset = true; break;
	case QRE_LIGHT_F_OFFSET_Y:  l->offset[1] = v0; l->has_offset = true; break;
	case QRE_LIGHT_F_OFFSET_Z:  l->offset[2] = v0; l->has_offset = true; break;
	case QRE_LIGHT_F_COLOR:
		if (v0 < 0.0f)
		{
			l->has_color = false; // the color is switched off
		}
		else
		{
			l->color[0] = v0; l->color[1] = v1; l->color[2] = v2;
			l->has_color = true;
		}
		break;
	case QRE_LIGHT_F_FRAST:     l->force_rasterize = b; break;
	case QRE_LIGHT_F_STYLE:
		l->style = (int)v0;
		l->has_style = (v0 >= 0.0f);
		break;
	default: break;
	}
	QRE_TouchLight (l->name);
}

// The field of the entry as the editor started with it (the has_ flag too).
static void QRE_LightApplyOriginal (rt_light_t *l, const rt_light_t *orig, int field)
{
	if (!orig)
	{
		// the light had no entry: the field goes back to "not authored"
		switch (field)
		{
		case QRE_LIGHT_F_RADIUS:    l->has_radius = false; break;
		case QRE_LIGHT_F_INTENSITY: l->has_intensity = false; break;
		case QRE_LIGHT_F_OFFSET:    l->has_offset = false; break;
		case QRE_LIGHT_F_COLOR:     l->has_color = false; break;
		case QRE_LIGHT_F_STYLE:     l->has_style = false; break;
		case QRE_LIGHT_F_FRAST:     l->force_rasterize = false; break;
		default: break;
		}
	}
	else
	{
		switch (field)
		{
		case QRE_LIGHT_F_RADIUS:    l->has_radius = orig->has_radius; l->radius = orig->radius; break;
		case QRE_LIGHT_F_INTENSITY: l->has_intensity = orig->has_intensity; l->intensity = orig->intensity; break;
		case QRE_LIGHT_F_OFFSET:    l->has_offset = orig->has_offset; VectorCopy (orig->offset, l->offset); break;
		case QRE_LIGHT_F_COLOR:     l->has_color = orig->has_color; VectorCopy (orig->color, l->color); break;
		case QRE_LIGHT_F_STYLE:     l->has_style = orig->has_style; l->style = orig->style; break;
		case QRE_LIGHT_F_FRAST:     l->force_rasterize = orig->force_rasterize; break;
		default: break;
		}
	}
	QRE_TouchLight (l->name);
}

// A light that follows its group writes an edit to every light of the same
// group (the emitter's model, e.g. progs/flame.mdl) that also follows it --
// in the file's list and among the lights of the frame, whose entries a group
// edit creates when they are missing. A light with the flag off is its own.
static void QRE_LightApplyToGroup (const char *source_name, int field, float v0, float v1, float v2, qboolean b)
{
	char group[MAX_QPATH];
	char g2[MAX_QPATH];
	int  count = 0, i;

	RT_MAT_GroupBaseOf (source_name, group, sizeof (group));

	{
		rt_light_t *list = RT_LIGHT_List (&count);

		for (i = 0; i < count; i++)
		{
			if (!list[i].valid || !list[i].group_edit)
				continue;
			RT_MAT_GroupBaseOf (list[i].name, g2, sizeof (g2));
			if (!strcmp (g2, group))
				QRE_LightApply (&list[i], field, v0, v1, v2, b);
		}
	}

	{
		const rt_tracked_light_t *lights = RT_TRACK_Lights (&count);

		for (i = 0; i < count; i++)
		{
			rt_light_t *l;

			if (!lights[i].ready || !lights[i].name[0])
				continue;
			RT_MAT_GroupBaseOf (lights[i].name, g2, sizeof (g2));
			if (strcmp (g2, group))
				continue;

			l = RT_LIGHT_Ensure (lights[i].name);
			if (l && l->group_edit)
				QRE_LightApply (l, field, v0, v1, v2, b);
		}
	}
}

// A light that follows its group resets the field for every follower: each one
// returns to the state the editor started with (its own snapshot entry, or "not
// authored" when it had none), so the group does not keep mixed values.
static void QRE_LightResetField (rt_light_t *self, int field)
{
	char group[MAX_QPATH];
	char g2[MAX_QPATH];
	int  count = 0, i;
	rt_light_t *list;

	if (!self->group_edit)
	{
		QRE_LightApplyOriginal (self, QRE_LightOriginal (self->name), field);
		return;
	}

	list = RT_LIGHT_List (&count);
	RT_MAT_GroupBaseOf (self->name, group, sizeof (group));
	for (i = 0; i < count; i++)
	{
		if (!list[i].valid || !list[i].group_edit)
			continue;
		RT_MAT_GroupBaseOf (list[i].name, g2, sizeof (g2));
		if (!strcmp (g2, group))
			QRE_LightApplyOriginal (&list[i], QRE_LightOriginal (list[i].name), field);
	}
}

// The radius and intensity a light of this kind uses when its field is not
// authored: a material light and a legacy dlight take the dlight settings, a
// map light entity its own.
static float QRE_LightDefaultRadius (int kind)
{
	extern cvar_t rt_elight_radius;

	return CVAR_TO_FLOAT (kind == RT_LIGHT_KIND_MAP ? rt_elight_radius : rt_dlight_radius);
}

static float QRE_LightDefaultIntensity (int kind)
{
	return (kind == RT_LIGHT_KIND_MAP) ? 1.0f : CVAR_TO_FLOAT (rt_dlight_intensity);
}

// The panel's one place to write a field: the group when the light follows it,
// the light itself otherwise.
static void QRE_LightWrite (rt_light_t *l, int field, float v0, float v1, float v2, qboolean b)
{
	if (l->group_edit)
		QRE_LightApplyToGroup (l->name, field, v0, v1, v2, b);
	else
		QRE_LightApply (l, field, v0, v1, v2, b);
}

static qboolean QRE_LightFieldChanged (const rt_light_t *l, const rt_light_t *orig, int field)
{
	switch (field)
	{
	case QRE_LIGHT_F_RADIUS:
		return l->has_radius != (orig && orig->has_radius ? true : false) ||
		       (l->has_radius && orig && orig->radius != l->radius);
	case QRE_LIGHT_F_INTENSITY:
		return l->has_intensity != (orig && orig->has_intensity ? true : false) ||
		       (l->has_intensity && orig && orig->intensity != l->intensity);
	case QRE_LIGHT_F_OFFSET:
		return l->has_offset != (orig && orig->has_offset ? true : false) ||
		       (l->has_offset && orig && memcmp (orig->offset, l->offset, sizeof (l->offset)) != 0);
	case QRE_LIGHT_F_COLOR:
		return l->has_color != (orig && orig->has_color ? true : false) ||
		       (l->has_color && orig && memcmp (orig->color, l->color, sizeof (l->color)) != 0);
	case QRE_LIGHT_F_FRAST:
		return l->force_rasterize != (orig ? orig->force_rasterize : false);
	case QRE_LIGHT_F_STYLE:
		return l->has_style != (orig && orig->has_style ? true : false) ||
		       (l->has_style && orig && orig->style != l->style);
	default:
		return false;
	}
}

// A member of the same group that follows it: the source of the values a light
// joining the group takes.
static rt_light_t *QRE_LightGroupShared (const char *name, const rt_light_t *self)
{
	char group[MAX_QPATH];
	char g2[MAX_QPATH];
	int  count = 0, i;
	rt_light_t *list = RT_LIGHT_List (&count);

	RT_MAT_GroupBaseOf (name, group, sizeof (group));
	for (i = 0; i < count; i++)
	{
		if (!list[i].valid || !list[i].group_edit || &list[i] == self)
			continue;
		if (!RT_LIGHT_HasFields (&list[i]))
			continue; // a member with nothing authored has no values to share
		RT_MAT_GroupBaseOf (list[i].name, g2, sizeof (g2));
		if (!strcmp (g2, group))
			return &list[i];
	}
	return NULL;
}

static void QRE_LightJoinGroup (rt_light_t *l)
{
	rt_light_t *shared = QRE_LightGroupShared (l->name, l);

	if (shared)
	{
		l->has_radius = shared->has_radius;
		l->radius = shared->radius;
		l->has_intensity = shared->has_intensity;
		l->intensity = shared->intensity;
		l->has_offset = shared->has_offset;
		VectorCopy (shared->offset, l->offset);
		l->has_color = shared->has_color;
		VectorCopy (shared->color, l->color);
		l->has_style = shared->has_style;
		l->style = shared->style;
		l->force_rasterize = shared->force_rasterize;
	}
	l->group_edit = true;
	QRE_TouchLight (l->name);
}

static int QRE_LightFieldDiffers (const rt_light_t *l, const rt_light_t *self, int kind, int field)
{
	switch (field)
	{
	case QRE_LIGHT_F_RADIUS:
		return (l && l->has_radius ? l->radius : QRE_LightDefaultRadius (kind)) !=
		       (self->has_radius ? self->radius : QRE_LightDefaultRadius (kind));
	case QRE_LIGHT_F_INTENSITY:
		return (l && l->has_intensity ? l->intensity : QRE_LightDefaultIntensity (kind)) !=
		       (self->has_intensity ? self->intensity : QRE_LightDefaultIntensity (kind));
	case QRE_LIGHT_F_OFFSET:
	{
		int mask = 0, c;

		for (c = 0; c < 3; c++)
		{
			const float a = (l && l->has_offset) ? l->offset[c] : 0.0f;
			const float b = self->has_offset ? self->offset[c] : 0.0f;

			if (a != b)
				mask |= 1 << c;
		}
		return mask;
	}
	case QRE_LIGHT_F_COLOR:
	{
		const qboolean has = (l && l->has_color) ? true : false;

		if (has != self->has_color)
			return 1;
		if (has && (l->color[0] != self->color[0] || l->color[1] != self->color[1] || l->color[2] != self->color[2]))
			return 1;
		return 0;
	}
	case QRE_LIGHT_F_FRAST:
		return ((l && l->force_rasterize) ? true : false) != (self->force_rasterize ? true : false);
	case QRE_LIGHT_F_STYLE:
	{
		const qboolean has = (l && l->has_style) ? true : false;

		if (has != self->has_style)
			return 1;
		if (has && l->style != self->style)
			return 1;
		return 0;
	}
	default:
		return 0;
	}
}

static int QRE_LightGroupMixedMask (const rt_light_t *self, int field)
{
	char        base[MAX_QPATH];
	char        g2[MAX_QPATH];
	int         count = 0, i, mask = 0;
	rt_light_t *list = RT_LIGHT_List (&count);
	const int   kind = qre.sel_light.kind;

	RT_MAT_GroupBaseOf (self->name, base, sizeof (base));
	for (i = 0; i < count; i++)
	{
		const rt_light_t *l = &list[i];

		if (!l->valid || !l->group_edit || l == self)
			continue;
		RT_MAT_GroupBaseOf (l->name, g2, sizeof (g2));
		if (strcmp (g2, base))
			continue;

		mask |= QRE_LightFieldDiffers (l, self, kind, field);
	}

	{
		const rt_tracked_light_t *tracked = RT_TRACK_Lights (&count);

		for (i = 0; i < count; i++)
		{
			rt_light_t *l;

			if (!tracked[i].ready || !tracked[i].name[0] || !strcmp (tracked[i].name, self->name))
				continue;
			RT_MAT_GroupBaseOf (tracked[i].name, g2, sizeof (g2));
			if (strcmp (g2, base))
				continue;

			l = RT_LIGHT_Find (tracked[i].name);
			if (l == self)
				continue;
			if (l && (!l->valid || !l->group_edit))
				continue;

			mask |= QRE_LightFieldDiffers (l, self, tracked[i].kind, field);
		}
	}

	return mask;
}

// The light editor's panel: the dlight of the picked emitter. Its fields live in
// qray.lights.yaml (radius, intensity, offset); an emitter without an entry
// shows the global defaults, and authoring a value creates one.
// ---------------------------------------------------------------------------
// The global tab of the light editor: the sky, its clouds and the sun. These
// are engine cvars (the colors are cvars behind a console command, as the rest
// of the renderer uses them), so an edit takes effect at once and is archived
// in the config; Apply and Cancel do not own them.
// ---------------------------------------------------------------------------

enum
{
	QRE_G_BOOL,
	QRE_G_FLOAT,
	QRE_G_INT,
	QRE_G_COLOR,
	QRE_G_BUTTON, // a press sets the cvar in `action` (the row name is its caption)
};

enum
{
	QRE_COND_NONE = 0,
	QRE_COND_PHYSICAL_SKY,
};

// One row: the cvar, its kind and the range of its slider. A section name opens
// a group; the rows under it belong to it until the next name. A button row
// carries its caption in `name` and the cvar it writes in `action`.
typedef struct
{
	const char *section;
	const char *name;
	int         type;
	float       min, max;
	const char *tip;
	const char *action;
	const char *show_when;
	int         cond;
} qre_global_t;

static const qre_global_t qre_globals[] = {
	{ "Global", "rt_brightness",    QRE_G_FLOAT, 0, 3,
	  "The brightness of the whole ray-traced image." },
	{ NULL,  "rt_globallight",      QRE_G_COLOR, 0, 0,
	  "The color every light starts from, before its own color and the light tint are applied." },

	{ "Sky", "rt_sky",              QRE_G_FLOAT, 0, 8,
	  "Intensity of the sky; the classic sky texture is scaled by it." },
	{ NULL,  "rt_sky_brightness",   QRE_G_FLOAT, 0, 10,
	  "Brightness of the procedural sky." },
	{ NULL,  "rt_sky_light_mult",   QRE_G_FLOAT, 0, 8,
	  "How much light the sky casts on the level, separate from how bright the sky is drawn (rt_sky_brightness)." },
	{ NULL,  "rt_sky_color",        QRE_G_COLOR, 0, 0,
	  "The color the procedural sky is painted in, and the color of the light it casts." },
	{ NULL,  "rt_sky_ambient_lod",  QRE_G_INT,   0, 10,
	  "Mip level the ambient sky light is read from: lower is more directional, 10 a flat wash." },
	{ NULL,  "rt_sky_nee",          QRE_G_BOOL,  0, 0,
	  "Sample the sky as an explicit light source." },
	{ NULL,  "rt_physical_sky",     QRE_G_BOOL,  0, 0,
	  "1 draws the procedural sky (painted in rt_sky_color, with clouds and a sun disc); 0 draws the classic sky texture." },

	{ "Clouds", "rt_sky_clouds",        QRE_G_BOOL,  0, 0,
	  "Draw the volumetric clouds.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_color",     QRE_G_COLOR, 0, 0,
	  "The color the clouds are drawn in; they may be darker than the sky.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_alpha",      QRE_G_FLOAT, 0, 1,
	  "Opacity the clouds are composited over the sky with; 0 takes them out.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_coverage",   QRE_G_FLOAT, 0, 1,
	  "How much of the sky the clouds cover.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_density",    QRE_G_FLOAT, 0, 10,
	  "Optical density of the volumetric cloud layer at every quality level.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_speed",      QRE_G_FLOAT, 0, 4,
	  "How fast the cloud layer drifts.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_quality",    QRE_G_INT, 0, QR_SKY_CLOUDS_MAX_QUALITY,
	  "0 low, 1 medium, 2 high, 3 ultra; all levels use volumetric clouds.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_height",     QRE_G_FLOAT, 1, 300000,
	  "Height of the cloud layer over the camera, in world units.", NULL, NULL, QRE_COND_PHYSICAL_SKY },
	{ NULL,  "rt_sky_clouds_thickness",  QRE_G_FLOAT, 1, 300000,
	  "Depth of the cloud layer, in world units.", NULL, NULL, QRE_COND_PHYSICAL_SKY },

	{ "", "rt_physical_sun",        QRE_G_BOOL,  0, 0,
	  "1 enables sunlight and makes the sun the source of the god rays, following rt_sky_sun_pitch and rt_sky_sun_yaw; 0 disables sunlight and uses the bright areas of the classic sky texture for the god rays instead." },
	{ "Sun", "rt_sky_sun",              QRE_G_FLOAT, 0, 10,
	  "Strength of the sun: 1 is a usable daylight, 0 turns it off.", NULL, "rt_physical_sun" },
	{ NULL,  "rt_sky_sun_color",        QRE_G_COLOR, 0, 0,
	  "The color of the sun: its light, the disc in the procedural sky and everything that reads it (the indirect sun, the god rays, the fog's shafts).",
	  NULL },
	{ NULL,  "rt_sky_sun_size",         QRE_G_FLOAT, 0, 10,
	  "Sun disc size multiplier: 1 keeps the original size, 0 hides the disc without disabling sunlight." },
	{ NULL,  "rt_sky_sun_pitch",        QRE_G_FLOAT, -180, 180,
	  "The pitch the sun stands at." },
	{ NULL,  "rt_sky_sun_yaw",          QRE_G_FLOAT, -180, 180,
	  "The yaw the sun stands at." },
	{ NULL,  "Set sun position",    QRE_G_BUTTON, 0, 0,
	  "Place the sun by aiming: it follows the crosshair, and the fire button leaves it where it points (that press is swallowed).",
	  "rt_sky_sun_edit" },

	{ "", "rt_sky_godrays",                QRE_G_BOOL,  0, 0,
	  "Draw the sun shafts." },
	{ "God rays", "rt_sky_godrays_intensity", QRE_G_FLOAT, 0, 4,
	  "Strength of the sun shafts.", NULL, "rt_sky_godrays" },
	{ NULL,  "rt_sky_godrays_sky_threshold", QRE_G_FLOAT, 0, 1,
	  "How bright a sky area must be to pull the god rays to itself; the rays come from the centre of everything above it, and from the brightest point when nothing is (0 leaves the brightest point alone)." },

	{ "Volumetric fog", "rt_volume_type",    QRE_G_INT,   0, 2,
	  "0 off, 1 a simple depth-based fog (the density and the color below), 2 the volumetric pass the sky light feeds." },
	{ NULL,  "rt_volume_scatter",            QRE_G_FLOAT, 0, 1,
	  "Density of the simple depth-based fog (mode 1)." },
	{ NULL,  "rt_volume_ambient",            QRE_G_FLOAT, 0, 8,
	  "Brightness of the simple fog's color, which is the sky's flat color (mode 1)." },
	{ NULL,  "rt_volume_far",                QRE_G_FLOAT, 0, 4000,
	  "How far from the camera the volumetric volume reaches (mode 2)." },

	{ "", "rt_level_fog",          QRE_G_BOOL,  0, 0,
	  "Draw the level's own fog (the worldspawn \"fog\" key or the console `fog` command)." },
};

static char     qre_globals_snapshot[countof (qre_globals)][QRE_SNAPSHOT_MAX];
static qboolean qre_globals_snapshot_set[countof (qre_globals)];

static const char *QRE_GlobalCvarName (const qre_global_t *g)
{
	return (g->type == QRE_G_BUTTON) ? g->action : g->name;
}

static qboolean QRE_GlobalCondMet (int cond)
{
	switch (cond)
	{
	case QRE_COND_PHYSICAL_SKY:
	{
		cvar_t *var = Cvar_FindVar ("rt_physical_sky");

		return (var && CVAR_TO_BOOL (*var)) ? true : false;
	}
	default:
		return true;
	}
}

static void QRE_TakeGlobalsSnapshot (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_globals); i++)
	{
		const char *name = QRE_GlobalCvarName (&qre_globals[i]);
		cvar_t     *var = name ? Cvar_FindVar (name) : NULL;

		if (!var)
		{
			qre_globals_snapshot_set[i] = false;
			qre_globals_snapshot[i][0] = '\0';
			continue;
		}

		qre_globals_snapshot_set[i] = true;
		q_strlcpy (qre_globals_snapshot[i], var->string ? var->string : "", sizeof (qre_globals_snapshot[i]));
	}
}

static qboolean QRE_GlobalsTouched (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_globals); i++)
	{
		cvar_t *var;

		if (!qre_globals_snapshot_set[i])
			continue;

		var = Cvar_FindVar (QRE_GlobalCvarName (&qre_globals[i]));
		if (var && strcmp (var->string ? var->string : "", qre_globals_snapshot[i]))
			return true;
	}

	return false;
}

static void QRE_GlobalsRestore (void)
{
	int i;

	for (i = 0; i < (int)countof (qre_globals); i++)
	{
		if (qre_globals_snapshot_set[i])
			Cvar_Set (QRE_GlobalCvarName (&qre_globals[i]), qre_globals_snapshot[i]);
	}
}

static void QRE_GlobalColorGet (const char *name, float rgb[3])
{
	if (!strcmp (name, "rt_sky_color"))
		RT_GetSkyColor (rgb);
	else if (!strcmp (name, "rt_sky_sun_color"))
		RT_GetSunColor (rgb);
	else if (!strcmp (name, "rt_globallight"))
		RT_GetGlobalLightColor (rgb);
	else
		RT_GetSkyCloudsColor (rgb);
}

static void QRE_GlobalColorSet (const char *name, const float rgb[3])
{
	Cvar_Set (name, va ("%d %d %d",
	                    (int)(CLAMP (0.0f, rgb[0], 1.0f) * 255.0f + 0.5f),
	                    (int)(CLAMP (0.0f, rgb[1], 1.0f) * 255.0f + 0.5f),
	                    (int)(CLAMP (0.0f, rgb[2], 1.0f) * 255.0f + 0.5f)));
}

static qboolean QRE_GlobalSectionVisible (const char *condition)
{
	if (!condition)
		return true;

	const char *p = condition;

	while (*p)
	{
		char name[64];
		int  n = 0;

		while (*p && *p != '|' && n < (int)sizeof (name) - 1)
			name[n++] = *p++;
		name[n] = '\0';

		if (*p == '|')
			p++;

		cvar_t *var = Cvar_FindVar (name);

		if (var && CVAR_TO_BOOL (*var))
			return true;
	}

	return false;
}

static void QRE_LightGlobalTab (void)
{
	const char *section = NULL;
	qboolean    section_hidden = false;
	int         i;

	QR_GUI_LabelDim ("the light system itself: the sky, its clouds and the sun");
	QR_GUI_Spacing ();

	for (i = 0; i < (int)countof (qre_globals); i++)
	{
		const qre_global_t *g = &qre_globals[i];
		cvar_t             *var;

		if (!QRE_GlobalCondMet (g->cond))
			continue;

		if (g->section && (!section || strcmp (section, g->section)))
		{
			section = g->section;
			section_hidden = !QRE_GlobalSectionVisible (g->show_when);

			if (!section_hidden)
			{
				if (!g->show_when && g->cond == QRE_COND_NONE)
					QR_GUI_Separator ();
				if (section[0])
					QR_GUI_Label (section);
			}
		}

		if (section_hidden)
			continue;

		if (g->type == QRE_G_BUTTON)
		{
			// a mode rather than a value: the press turns the aiming on and the
			// fire button ends it, so the caption says when it is running
			cvar_t *mode = Cvar_FindVar (g->action);
			char    caption[128];

			if (mode && CVAR_TO_BOOL (*mode))
				q_snprintf (caption, sizeof (caption), "%s (aiming: fire places it)", g->name);
			else
				q_snprintf (caption, sizeof (caption), "%s", g->name);
			if (QR_GUI_Button (caption))
				Cvar_Set (g->action, "1");
			QR_GUI_Tooltip (g->tip);
			continue;
		}

		var = Cvar_FindVar (g->name);

		if (!var)
		{
			QR_GUI_LabelDim (va ("%s: no such cvar", g->name));
			continue;
		}

		switch (g->type)
		{
		case QRE_G_BOOL:
		{
			int value = CVAR_TO_BOOL (*var) ? 1 : 0;

			if (QR_GUI_Checkbox (g->name, &value, g->tip))
				Cvar_Set (g->name, value ? "1" : "0");
			break;
		}
		case QRE_G_FLOAT:
		{
			float value = var->value;

			if (QR_GUI_SliderFloat (g->name, &value, g->min, g->max, g->tip))
				Cvar_Set (g->name, va ("%.4g", value));
			break;
		}
		case QRE_G_INT:
		{
			int value = (int)(var->value + 0.5f);

			if (QR_GUI_SliderInt (g->name, &value, (int)g->min, (int)g->max, g->tip))
				Cvar_Set (g->name, va ("%d", value));
			break;
		}
		case QRE_G_COLOR:
		{
			float rgb[3];
			int   en = 1;

			QRE_GlobalColorGet (g->name, rgb);
			if (QR_GUI_ColorHex (g->name, rgb, &en, g->tip))
				QRE_GlobalColorSet (g->name, rgb);
			break;
		}
		default:
			break;
		}

		{
			const qboolean changed = qre_globals_snapshot_set[i] &&
			                         strcmp (var->string ? var->string : "", qre_globals_snapshot[i]) != 0;

			if (QR_GUI_ResetButton (g->name, changed))
				Cvar_Set (g->name, qre_globals_snapshot[i]);
		}
	}

	// The level's fog itself: the color and the density are map data, not cvars,
	// so they go through the `fog` command (the same path a map's key and the
	// console use) while the getters keep the widgets in step with the map.
	if (CVAR_TO_BOOL (rt_level_fog))
	{
		QR_GUI_Label ("Fog");

		float    color[4];
		float    density = Fog_GetDensity ();
		int      en = 1;
		qboolean changed = false;

		Fog_GetColor (color);
		color[3] = 1.0f;

		if (QR_GUI_ColorHex ("fog_color", color, &en, "The color of the level's fog."))
			changed = true;
		if (QR_GUI_ResetButton ("fog_color", color[0] != qre.snap_fog_color[0] ||
		                                        color[1] != qre.snap_fog_color[1] ||
		                                        color[2] != qre.snap_fog_color[2]))
		{
			VectorCopy (qre.snap_fog_color, color);
			changed = true;
		}
		if (QR_GUI_SliderFloat ("fog_density", &density, 0.0f, 4.0f, "How thick the level's fog is; 0 turns it off."))
			changed = true;
		if (QR_GUI_ResetButton ("fog_density", density != qre.snap_fog_density))
		{
			density = qre.snap_fog_density;
			changed = true;
		}

		if (changed)
			Cbuf_AddText (va ("fog %f %f %f %f\n", density,
			                  CLAMP (0.0f, color[0], 1.0f), CLAMP (0.0f, color[1], 1.0f), CLAMP (0.0f, color[2], 1.0f)));
	}

	QR_GUI_Spacing ();
	QR_GUI_LabelDim ("the values above are saved to the config by Save; Cancel puts them back");
	QR_GUI_LabelDim ("the fog is part of the level's session: it goes to qray.lights.yaml");
}

// ---------------------------------------------------------------------------
// The Custom tab: the lights this level does not have, authored here and stored
// in <gamedir>/qray/lights.yaml (one section per level).
// ---------------------------------------------------------------------------
static void QRE_SelectCustomLight (int index)
{
	int                count = 0;
	rt_custom_light_t *lights = RT_CustomLights (&count);

	if (index < 0 || index >= count)
	{
		qre.sel_light_valid = false;
		return;
	}

	qre.sel_light_valid = true;
	qre.sel_light.kind = RT_LIGHT_KIND_CUSTOM;
	qre.sel_light.uniqueID = (uint64_t)UINT32_MAX + 1 + (uint64_t)index;
	VectorCopy (lights[index].origin, qre.sel_light.position);
	if (lights[index].has_offset)
		VectorAdd (qre.sel_light.position, lights[index].offset, qre.sel_light.position);
	qre.sel_light.radius = lights[index].radius;
	VectorCopy (lights[index].color, qre.sel_light.color);

	{
		vec3_t dir, angles;

		VectorSubtract (qre.sel_light.position, qre.cam_origin, dir);
		if (VectorLength (dir) < 1.0f)
			return;

		VectorAngles (dir, NULL, angles);
		cl.viewangles[YAW] = angles[YAW];
		cl.viewangles[PITCH] = angles[PITCH];
	}
}

enum
{
	QRE_CUSTOM_F_RADIUS = 0,
	QRE_CUSTOM_F_INTENSITY,
	QRE_CUSTOM_F_ORIGIN,
	QRE_CUSTOM_F_OFFSET,
	QRE_CUSTOM_F_COLOR,
	QRE_CUSTOM_F_STYLE,
	QRE_CUSTOM_F_SPOT,
	QRE_CUSTOM_F_DIR,
	QRE_CUSTOM_F_ANGLE_INNER,
	QRE_CUSTOM_F_ANGLE_OUTER,
};

static qboolean QRE_CustomOriginal (int index, rt_custom_light_t *out)
{
	if (index >= 0 && index < qre.snap_custom_count)
	{
		*out = qre.snap_custom[index];
		return true;
	}

	memset (out, 0, sizeof (*out));
	out->radius = RT_CUSTOM_RADIUS_DEFAULT;
	out->intensity = RT_CUSTOM_INTENSITY_DEFAULT;
	out->color[0] = out->color[1] = out->color[2] = 1.0f;
	out->dir[0] = 1.0f;
	out->angle_outer = 30.0f;
	return false;
}

static qboolean QRE_CustomFieldChanged (const rt_custom_light_t *l, const rt_custom_light_t *orig, int field)
{
	switch (field)
	{
	case QRE_CUSTOM_F_RADIUS:    return l->radius != orig->radius;
	case QRE_CUSTOM_F_INTENSITY: return l->intensity != orig->intensity;
	case QRE_CUSTOM_F_ORIGIN:    return memcmp (l->origin, orig->origin, sizeof (l->origin)) != 0;
	case QRE_CUSTOM_F_OFFSET:    return l->has_offset != orig->has_offset ||
		                                (l->has_offset && memcmp (l->offset, orig->offset, sizeof (l->offset)) != 0);
	case QRE_CUSTOM_F_COLOR:     return memcmp (l->color, orig->color, sizeof (l->color)) != 0;
	case QRE_CUSTOM_F_STYLE:     return l->style != orig->style;
	case QRE_CUSTOM_F_SPOT:      return l->spot != orig->spot;
	case QRE_CUSTOM_F_DIR:       return memcmp (l->dir, orig->dir, sizeof (l->dir)) != 0;
	case QRE_CUSTOM_F_ANGLE_INNER: return l->angle_inner != orig->angle_inner;
	case QRE_CUSTOM_F_ANGLE_OUTER: return l->angle_outer != orig->angle_outer;
	default: return false;
	}
}

static void QRE_CustomResetField (rt_custom_light_t *l, const rt_custom_light_t *orig, int field)
{
	switch (field)
	{
	case QRE_CUSTOM_F_RADIUS:    l->radius = orig->radius; break;
	case QRE_CUSTOM_F_INTENSITY: l->intensity = orig->intensity; break;
	case QRE_CUSTOM_F_ORIGIN:    VectorCopy (orig->origin, l->origin); break;
	case QRE_CUSTOM_F_OFFSET:
		l->has_offset = orig->has_offset;
		VectorCopy (orig->offset, l->offset);
		break;
	case QRE_CUSTOM_F_COLOR:     VectorCopy (orig->color, l->color); break;
	case QRE_CUSTOM_F_STYLE:     l->style = orig->style; break;
	case QRE_CUSTOM_F_SPOT:      l->spot = orig->spot; break;
	case QRE_CUSTOM_F_DIR:       VectorCopy (orig->dir, l->dir); break;
	case QRE_CUSTOM_F_ANGLE_INNER: l->angle_inner = orig->angle_inner; break;
	case QRE_CUSTOM_F_ANGLE_OUTER: l->angle_outer = orig->angle_outer; break;
	default: break;
	}
	RT_CustomLightValidate (l);
}

static void QRE_CustomLightsTab (void)
{
	int                count = 0;
	rt_custom_light_t *lights = RT_CustomLights (&count);
	char               buf[96];
	int                i;

	QR_GUI_LabelDim ("lights the level does not have, kept in qray.lights.yaml");

	q_snprintf (buf, sizeof (buf), "%d of %d on this level", count, RT_CUSTOM_LIGHTS_MAX);
	QR_GUI_LabelDim (buf);
	QR_GUI_Spacing ();

	if (QR_GUI_Button ("Add light"))
	{
		// the light is created where the crosshair points: the cursor mode goes
		// off so the camera aims, and the fire button drops it (in_sdl.c)
		qre.custom_placing = true;
		QRE_CursorMode (false);
	}

	QR_GUI_Spacing ();

	for (i = 0; i < count; i++)
	{
		rt_custom_light_t *l = &lights[i];
		rt_custom_light_t  orig;
		qboolean           has_orig;
		char               label[48];
		int                style = CLAMP (0, l->style, RT_CUSTOM_STYLE_COUNT - 1);
		int                selected = (qre.sel_light_valid && qre.sel_light.kind == RT_LIGHT_KIND_CUSTOM &&
		                               qre.sel_light.uniqueID == (uint64_t)UINT32_MAX + 1 + (uint64_t)i);
		float              rgb[3];
		int                en = 1;

		has_orig = QRE_CustomOriginal (i, &orig);

		q_snprintf (label, sizeof (label), "Light %d", i + 1);
		QR_GUI_PushID (label);

		if (QR_GUI_SectionSelected (label, selected))
		{
			QRE_SelectCustomLight (i);
			selected = 1;
		}

		if (selected)
		{
			const char *tip = "A light the editor authored: uploaded like a dlight, with a style of its own.";
			float       offs[3];

			if (QR_GUI_SliderFloat ("light_radius", &l->radius, 0.0f, 10.0f,
			                        "The size of the light, in rt_dlight_radius units."))
			{
			}
			if (QR_GUI_ResetButton ("light_radius", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_RADIUS)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_RADIUS);

			if (QR_GUI_SliderFloat ("light_intensity", &l->intensity, 0.0f, 100.0f, tip))
			{
			}
			if (QR_GUI_ResetButton ("light_intensity", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_INTENSITY)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_INTENSITY);

			if (QR_GUI_Vec3Input ("origin", l->origin, -32768.0f, 32768.0f, "Where the light is, in Quake units."))
			{
			}
			if (QR_GUI_ResetButton ("origin", has_orig && QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_ORIGIN)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_ORIGIN);

			offs[0] = l->offset[0];
			offs[1] = l->offset[1];
			offs[2] = l->offset[2];
			if (QR_GUI_Vec3Input ("light_offset", offs, -128.0f, 128.0f, "A shift from the origin, X Y Z."))
			{
				l->has_offset = true;
				l->offset[0] = offs[0];
				l->offset[1] = offs[1];
				l->offset[2] = offs[2];
			}
			if (QR_GUI_ResetButton ("light_offset", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_OFFSET)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_OFFSET);

			rgb[0] = l->color[0];
			rgb[1] = l->color[1];
			rgb[2] = l->color[2];
			if (QR_GUI_ColorHex ("light_color", rgb, &en, "The color of the light."))
			{
				l->color[0] = rgb[0];
				l->color[1] = rgb[1];
				l->color[2] = rgb[2];
			}
			if (QR_GUI_ResetButton ("light_color", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_COLOR)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_COLOR);

			if (QR_GUI_Combo ("light_style", &style, rt_custom_style_names, RT_CUSTOM_STYLE_COUNT,
			                  "The light style the light flickers with, like the map's own lights (steady, candle, ...)."))
			{
				l->style = style;
			}
			if (QR_GUI_ResetButton ("light_style", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_STYLE)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_STYLE);

			{
				int spot = l->spot ? 1 : 0;

				if (QR_GUI_Checkbox ("light_spot", &spot,
				                     "Make the light a cone (a spotlight) instead of a sphere, aimed by its direction."))
				{
					l->spot = spot ? true : false;
					RT_CustomLightValidate (l);
				}
			}
			if (QR_GUI_ResetButton ("light_spot", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_SPOT)))
				QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_SPOT);

			if (l->spot)
			{
				if (QR_GUI_Vec3Input ("light_dir", l->dir, -1.0f, 1.0f,
				                      "The axis of the cone, X Y Z (normalized when uploaded)."))
				{
					RT_CustomLightValidate (l);
				}
				if (QR_GUI_ResetButton ("light_dir", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_DIR)))
					QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_DIR);

				if (QR_GUI_SliderFloat ("light_angle_inner", &l->angle_inner, 0.0f, l->angle_outer,
				                        "The cone's full-intensity core, in degrees."))
				{
					RT_CustomLightValidate (l);
				}
				if (QR_GUI_ResetButton ("light_angle_inner", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_ANGLE_INNER)))
					QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_ANGLE_INNER);

				if (QR_GUI_SliderFloat ("light_angle_outer", &l->angle_outer, l->angle_inner, 90.0f,
				                        "Where the cone falls to nothing, in degrees."))
				{
					RT_CustomLightValidate (l);
				}
				if (QR_GUI_ResetButton ("light_angle_outer", QRE_CustomFieldChanged (l, &orig, QRE_CUSTOM_F_ANGLE_OUTER)))
					QRE_CustomResetField (l, &orig, QRE_CUSTOM_F_ANGLE_OUTER);
			}

			if (QR_GUI_Button ("Duplicate"))
			{
				rt_custom_light_t *copy = RT_CustomLights_Ensure ();

				if (copy)
				{
					int new_count = 0;

					// every property, and a small step up so the copy is visible
					// next to the original straight away
					*copy = *l;
					copy->origin[2] += 32.0f;

					(void)RT_CustomLights (&new_count);
					QRE_SelectCustomLight (new_count - 1);
				}
				else
				{
					QRE_Notify ("no room for another custom light");
				}
			}
			QR_GUI_SameLine ();
			if (QR_GUI_Button ("Clone"))
			{
				qre.clone_source = *l;
				qre.custom_cloning = true;
				qre.custom_placing = false;
				QRE_CursorMode (false);
			}
			QR_GUI_SameLine ();
			if (QR_GUI_Button ("Remove"))
			{
				RT_CustomLights_Remove (i);
				qre.sel_light_valid = false;
				QR_GUI_PopID ();
				break;
			}
		}

		QR_GUI_PopID ();
	}
}

// The Entity tab: every generated light of the frame -- a material light, a
// legacy dlight, a light entity of the map -- the way the Custom tab lists its
// own lights. The list is the frame's, so it follows what is drawn.
static const char *QRE_LightKindName (int kind)
{
	switch (kind)
	{
	case RT_LIGHT_KIND_MATERIAL: return "material";
	case RT_LIGHT_KIND_DLIGHT:   return "dlight";
	case RT_LIGHT_KIND_MAP:      return "map";
	default:                     return "custom";
	}
}

static void QRE_SelectEntityLight (const rt_tracked_light_t *l)
{
	vec3_t dir, angles;

	qre.sel_light = *l;
	qre.sel_light_valid = true;

	VectorSubtract (qre.sel_light.position, qre.cam_origin, dir);
	if (VectorLength (dir) < 1.0f)
		return;

	VectorAngles (dir, NULL, angles);
	cl.viewangles[YAW] = angles[YAW];
	cl.viewangles[PITCH] = angles[PITCH];
}

static void QRE_LightEntityFields (void)
{
	char        ikey[MAX_QPATH];
	rt_light_t *inst = NULL;
	rt_light_t *shared = NULL;
	rt_light_t *light = NULL;

	if (qre.sel_light_valid && qre.sel_light.name[0])
	{
		RT_LIGHT_MakeKey (qre.sel_light.name, qre.sel_light.uniqueID, ikey, sizeof (ikey));
		inst = RT_LIGHT_Find (ikey);
		shared = RT_LIGHT_Ensure (qre.sel_light.name);
		light = inst ? inst : shared;
	}

	if (light)
	{
		const rt_light_t *orig = QRE_LightOriginal (light->name);
		qboolean          group = false;
		float             value;
		int               radius_mixed;

		// group_edit at the very top: whether an edit touches the whole group
		{
			int ge = inst ? 0 : 1;

			if (QR_GUI_Checkbox ("group_edit", &ge,
			                     "Edit every light of this group (the emitter's model) at once. Off gives this light an entry of its own; on drops it and takes the group's values back."))
			{
				if (ge)
				{
					// back into the group: the instance entry goes and the
					// group's values apply again
					RT_LIGHT_Remove (ikey);
					inst = NULL;
					light = RT_LIGHT_Ensure (qre.sel_light.name);
					if (light)
					{
						light->group_edit = true;
						if (!RT_LIGHT_HasFields (light))
							QRE_LightJoinGroup (light);
						else
							QRE_TouchLight (light->name);
					}
				}
				else
				{
					// its own entry: the values in effect move into it and the
					// group stops touching this light
					rt_light_t *own = RT_LIGHT_Ensure (ikey);

					if (own)
					{
						if (light)
						{
							own->has_radius = light->has_radius;
							own->radius = light->radius;
							own->has_intensity = light->has_intensity;
							own->intensity = light->intensity;
							own->has_offset = light->has_offset;
							VectorCopy (light->offset, own->offset);
							own->has_color = light->has_color;
							VectorCopy (light->color, own->color);
							own->has_style = light->has_style;
							own->style = light->style;
							own->force_rasterize = light->force_rasterize;
						}
						own->group_edit = false;
						QRE_TouchLight (own->name);
						inst = own;
						light = own;
					}
				}
			}
		}
		if (QR_GUI_ResetButton ("group_edit", inst != NULL ||
		                        (light->group_edit != (orig ? orig->group_edit : true))))
		{
			// the reset goes back to following the group
			if (inst)
			{
				RT_LIGHT_Remove (ikey);
				inst = NULL;
				light = RT_LIGHT_Ensure (qre.sel_light.name);
			}
			if (light)
			{
				light->group_edit = true;
				if (!RT_LIGHT_HasFields (light))
					QRE_LightJoinGroup (light);
				else
					QRE_TouchLight (light->name);
			}
		}

		group = (inst == NULL && light->group_edit) ? true : false;

		radius_mixed = group ? QRE_LightGroupMixedMask (light, QRE_LIGHT_F_RADIUS) : 0;
		value = light->has_radius ? light->radius : QRE_LightDefaultRadius (qre.sel_light.kind);
		if (QR_GUI_SliderFloatMixed ("light_radius", &value, 0.0f, 10.0f, radius_mixed,
		                             "The size of the light, in rt_dlight_radius units; the line under it is the radius the renderer draws."))
			QRE_LightWrite (light, QRE_LIGHT_F_RADIUS, value, 0, 0, false);
		if (QR_GUI_ResetButton ("light_radius", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_RADIUS)))
			QRE_LightResetField (light, QRE_LIGHT_F_RADIUS);
		if (!radius_mixed)
		{
			char buf[64];

			q_snprintf (buf, sizeof (buf), "radius %.1f game units", METRIC_TO_QUAKEUNIT (value));
			QR_GUI_LabelDim (buf);
		}

		value = light->has_intensity ? light->intensity : QRE_LightDefaultIntensity (qre.sel_light.kind);
		if (QR_GUI_SliderFloatMixed ("light_intensity", &value, 0.0f, 100.0f,
		                             (group && QRE_LightGroupMixedMask (light, QRE_LIGHT_F_INTENSITY)) ? 1 : 0,
		                             "The brightness of the light: a multiplier of its color."))
			QRE_LightWrite (light, QRE_LIGHT_F_INTENSITY, value, 0, 0, false);
		if (QR_GUI_ResetButton ("light_intensity", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_INTENSITY)))
			QRE_LightResetField (light, QRE_LIGHT_F_INTENSITY);

		{
			float         offs[3];
			unsigned char mixed[3];
			int           mask;

			offs[0] = light->has_offset ? light->offset[0] : 0.0f;
			offs[1] = light->has_offset ? light->offset[1] : 0.0f;
			offs[2] = light->has_offset ? light->offset[2] : 0.0f;

			mask = group ? QRE_LightGroupMixedMask (light, QRE_LIGHT_F_OFFSET) : 0;
			mixed[0] = (mask & 1) ? 1 : 0;
			mixed[1] = (mask & 2) ? 1 : 0;
			mixed[2] = (mask & 4) ? 1 : 0;

			int           changed;

			changed = QR_GUI_Vec3InputMixed ("light_offset", offs, -128.0f, 128.0f, mixed,
			                                 "The offset of the light from the emitter's pivot point (its origin), X Y Z.");
			if (changed & 1)
				QRE_LightWrite (light, QRE_LIGHT_F_OFFSET_X, offs[0], 0, 0, false);
			if (changed & 2)
				QRE_LightWrite (light, QRE_LIGHT_F_OFFSET_Y, offs[1], 0, 0, false);
			if (changed & 4)
				QRE_LightWrite (light, QRE_LIGHT_F_OFFSET_Z, offs[2], 0, 0, false);
			if (QR_GUI_ResetButton ("light_offset", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_OFFSET)))
				QRE_LightResetField (light, QRE_LIGHT_F_OFFSET);
		}

		{
			int   en = light->has_color ? 1 : 0;
			float rgb[3];

			VectorCopy (light->has_color ? light->color : vec3_origin, rgb);
			if (QR_GUI_ColorHexMixed ("light_color", rgb, &en,
			                          (group && QRE_LightGroupMixedMask (light, QRE_LIGHT_F_COLOR)) ? 1 : 0,
			                          "An explicit color of the light, replacing the emitter's own."))
			{
				if (!en)
					QRE_LightWrite (light, QRE_LIGHT_F_COLOR, -1.0f, 0, 0, false);
				else
					QRE_LightWrite (light, QRE_LIGHT_F_COLOR, rgb[0], rgb[1], rgb[2], false);
			}
			if (QR_GUI_ResetButton ("light_color", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_COLOR)))
				QRE_LightResetField (light, QRE_LIGHT_F_COLOR);
		}

		{
			const char *items[RT_CUSTOM_STYLE_COUNT + 1];
			int         style = light->has_style ? light->style + 1 : 0;
			int         i;

			items[0] = "NONE";
			for (i = 0; i < RT_CUSTOM_STYLE_COUNT; i++)
				items[i + 1] = rt_custom_style_names[i];

			if (group && QRE_LightGroupMixedMask (light, QRE_LIGHT_F_STYLE))
				style = -1;

			if (QR_GUI_Combo ("light_style", &style, (const char *const *)items, RT_CUSTOM_STYLE_COUNT + 1,
			                  "Force a light style on this emitter, overriding its own: the light follows that style's animation. NONE keeps the emitter's own."))
				QRE_LightWrite (light, QRE_LIGHT_F_STYLE, (float)(style - 1), 0, 0, false);
			if (QR_GUI_ResetButton ("light_style", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_STYLE)))
				QRE_LightResetField (light, QRE_LIGHT_F_STYLE);
		}

		if (qre.sel_light.kind == RT_LIGHT_KIND_MATERIAL || qre.sel_light.kind == RT_LIGHT_KIND_DLIGHT)
		{
			int fr = light->force_rasterize ? 1 : 0;

			if (QR_GUI_CheckboxMixed ("force_rasterize", &fr,
			                          (group && QRE_LightGroupMixedMask (light, QRE_LIGHT_F_FRAST)) ? 1 : 0,
			                          "Draw the emitter in the rasterized path."))
				QRE_LightWrite (light, QRE_LIGHT_F_FRAST, 0, 0, 0, fr != 0);
			if (QR_GUI_ResetButton ("force_rasterize", QRE_LightFieldChanged (light, orig, QRE_LIGHT_F_FRAST)))
				QRE_LightResetField (light, QRE_LIGHT_F_FRAST);
		}
	}
	else if (qre.sel_light_valid)
	{
		QR_GUI_LabelDim ("this light has no emitter name: there is nothing to save its fields to");
	}
	else
	{
		QR_GUI_Label ("nothing selected");
	}
}

static void QRE_LightEntityList (void)
{
	const rt_tracked_light_t *lights;
	int                       order[RT_TRACKED_LIGHTS_MAX];
	int                       count = 0, live = 0, i, j;
	int                       selected_any = 0;
	char                      buf[96];

	lights = RT_TRACK_Lights (&count);

	for (i = 0; i < count; i++)
	{
		const rt_tracked_light_t *l = &lights[i];

		if (l->kind == RT_LIGHT_KIND_CUSTOM)
			continue;

		for (j = 0; j < live; j++)
		{
			const rt_tracked_light_t *o = &lights[order[j]];

			if (o->kind == l->kind && o->uniqueID == l->uniqueID && !strcmp (o->name, l->name))
				break;
		}
		if (j < live)
			continue;

		for (j = live; j > 0; j--)
		{
			const rt_tracked_light_t *o = &lights[order[j - 1]];
			const int                 cmp = strcmp (o->name, l->name);

			if (cmp < 0 || (cmp == 0 && (o->kind < l->kind ||
			                             (o->kind == l->kind && o->uniqueID <= l->uniqueID))))
				break;

			order[j] = order[j - 1];
		}

		order[j] = i;
		live++;
	}

	q_snprintf (buf, sizeof (buf), "%d generated light%s in the frame", live, live == 1 ? "" : "s");
	QR_GUI_LabelDim (buf);
	QR_GUI_Spacing ();

	for (j = 0; j < live; j++)
	{
		const rt_tracked_light_t *l = &lights[order[j]];
		char                      label[MAX_QPATH + 32];
		char                      id[MAX_QPATH + 48];
		int                       selected;

		if (l->name[0])
			q_snprintf (label, sizeof (label), "%s  [%s]", l->name, QRE_LightKindName (l->kind));
		else
			q_snprintf (label, sizeof (label), "(no emitter)  [%s]", QRE_LightKindName (l->kind));

		q_snprintf (id, sizeof (id), "%s#%d", label, j);
		QR_GUI_PushID (id);

		selected = (qre.sel_light_valid && qre.sel_light.kind == l->kind &&
		            qre.sel_light.uniqueID == l->uniqueID && !strcmp (qre.sel_light.name, l->name)) ? 1 : 0;

		if (QR_GUI_SectionSelected (label, selected))
		{
			QRE_SelectEntityLight (l);
			selected = 1;
		}

		if (selected)
		{
			QRE_LightEntityFields ();
			selected_any = 1;
		}

		QR_GUI_PopID ();
	}

	if (!selected_any)
		QRE_LightEntityFields ();
}

static void QRE_BuildLightPanelGUI (void)
{
	int         panel_w = glwidth / 4;
	rt_light_t *inst = NULL;
	char        ikey[MAX_QPATH];

	if (panel_w < 352)
		panel_w = 352;

	QR_GUI_BeginPanel ("qr_light_editor", glwidth - panel_w, 0, panel_w, glheight);

	QR_GUI_Label ("LIGHT EDITOR");
	QRE_RefreshSelectedLight ();
	if (qre.sel_light_valid && qre.sel_light.name[0])
	{
		// "(own)" marks a light that left its group and has an entry of its
		// own, keyed by the emitter and the instance id (the fields below use
		// the same key).
		RT_LIGHT_MakeKey (qre.sel_light.name, qre.sel_light.uniqueID, ikey, sizeof (ikey));
		inst = RT_LIGHT_Find (ikey);
	}
	QR_GUI_Spacing ();

	{
		static const char *const tabs[] = { "Entity", "Custom", "Global" };
		int reset = 0;

		QR_GUI_Tabs ("light_tabs", tabs, (int)countof (tabs), &qre.light_tab, &reset);
		if (reset)
			qre.reset_prompt = true;
		QR_GUI_Spacing ();
	}

	QRE_PanelActionRow (QRE_RequestExit);

	if (qre.light_tab == 2)
	{
		// the global tab: the sky, its clouds, the sun and the fog
		QRE_LightGlobalTab ();
		QR_GUI_EndScroll ();
		QR_GUI_EndPanel ();
		return;
	}

	if (qre.light_tab == 1)
	{
		// the custom tab: the lights this level does not have
		QRE_CustomLightsTab ();
		QR_GUI_EndScroll ();
		QR_GUI_EndPanel ();
		return;
	}

	if (qre.sel_light_valid)
	{
		char buf[MAX_QPATH + 64];

		q_snprintf (buf, sizeof (buf), "light: %s%s", qre.sel_light.name[0] ? qre.sel_light.name : "(no emitter name)",
		            inst ? "  (own)" : "");
		QR_GUI_LabelDim (buf);
		q_snprintf (buf, sizeof (buf), "at %.0f %.0f %.0f   radius %.1f",
		            qre.sel_light.position[0], qre.sel_light.position[1], qre.sel_light.position[2],
		            qre.sel_light.radius);
		QR_GUI_LabelDim (buf);

		if (qre.sel_light.kind == RT_LIGHT_KIND_MATERIAL)
		{
			rt_material_t *m = qre.sel_light.name[0] ? RT_MAT_Find (qre.sel_light.name) : NULL;

			if (m && m->has_light_color)
				QR_GUI_LabelDim ("casts a dlight (light_color)");
			else
				QR_GUI_LabelDim ("no dlight: the material has no light_color");
		}
		else
		{
			QR_GUI_LabelDim (qre.sel_light.kind == RT_LIGHT_KIND_DLIGHT ? "a legacy dlight" : "a map light entity");
		}
	}
	else
	{
		QR_GUI_LabelDim ("aim at a light wireframe and press the fire button");
	}
	QR_GUI_Spacing ();

	QRE_LightEntityList ();

	QR_GUI_EndScroll ();
	QR_GUI_EndPanel ();
}

static void QRE_DrawHints (void)
{
	static const char *const lines[] = {
		"QR MATERIAL EDITOR",
		"LMB - select the face under the crosshair",
		"WASD + mouse - fly    Shift - faster    jump/movedown - up/down",
		"Tab - the cursor mode (the panel) / fly again",
		"Esc - editor menu    ~ - console",
	};
	static const char *const light_lines[] = {
		"QR LIGHT EDITOR",
		"LMB - pick a light in the list (Entity tab) or under the crosshair",
		"WASD + mouse - fly    Shift - faster    jump/movedown - up/down",
		"Tab - the cursor mode (the panel) / fly again",
		"Esc - editor menu    ~ - console",
	};
	const char *const *shown = (qre.mode == QRE_MODE_LIGHT) ? light_lines : lines;

	if (qre.choosing)
		return;

	QR_GUI_DrawHint (shown, (int)countof (light_lines));
}

static void QRE_BuildFlyingOverlay (void)
{
	QR_GUI_DrawCrosshair ();

	if (qre.light_dragging)
		QR_GUI_LabelBottomRight ("Press LMB to drop the light, Esc to return it");
	else if (qre.custom_cloning)
		QR_GUI_LabelBottomRight ("Press LMB to paste the cloned light source to crosshair");
	else if (qre.custom_placing)
		QR_GUI_LabelBottomRight ("Press LMB to add new light at crosshair position");
}

// ---------------------------------------------------------------------------
// Panel: per-frame bookkeeping
// ---------------------------------------------------------------------------

static void QRE_Frame (void)
{
	static keydest_t prev_key_dest = key_game;

	if (qre.stop_pending)
	{
		qre.stop_pending = false;
		QRE_StopEditor (qre.stop_pending_restore);
		return;
	}

	// the level went away under the editor: drop it (the material snapshot may
	// be stale relative to a freshly loaded map list, so nothing is restored)
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		QRE_StopEditor (false);
		return;
	}

	if (qre.reset_pending)
	{
		qre.reset_pending = false;
		QRE_ResetAll ();
	}

	// While the console is up it owns the input; coming back, the panel needs
	// its free cursor again (the console re-activated the relative mouse mode).
	if (qre.panel_open && prev_key_dest != key_game && key_dest == key_game)
	{
		IN_FreeCursorForGui ();
		SDL_ShowCursor (SDL_DISABLE);
	}
	prev_key_dest = key_dest;

	if (QR_Editor_Flying ())
		QRE_DoPick (false);

	if (qre.light_dragging)
		QRE_UpdateLightDrag ();

	// the normal path flushes before the render (QR_Editor_UpdateView); this
	// covers the frames in which V_CalcRefdef does not run (paused, intermission)
	QRE_FlushDirty ();
}

static void QRE_DrawChooser (void)
{
	int answer = QR_GUI_DialogVertical ("QuakeRay v." ENGINE_VER_STRING,
	                            "Choose the editor to run on this level (Esc closes).",
	                            "Material Editor", "Light Editor", "Exit");

	if (answer == 1)
		QRE_StartMode (QRE_MODE_MATERIAL);
	else if (answer == 2)
		QRE_StartMode (QRE_MODE_LIGHT);
	else if (answer == 3)
		QRE_RequestExit ();
}

void QR_Editor_DrawPanel (cb_context_t *cbx)
{
	(void)cbx;

	if (!qre.active)
		return;

	QRE_Frame ();

	// while the console is up it owns the screen; the editor state is kept
	if (key_dest != key_game)
		return;

	// The whole editor interface is ImGui: the panel, the crosshair and the
	// hints. SCR_UpdateScreen can run more than once per host frame.
	if (!QR_GUI_BeginFrame ((unsigned int)host_framecount, (float)host_frametime, glx, gly, glwidth, glheight, vid.height))
		return;

	qre.panel_drawing = true;

	if (qre.choosing)
	{
		QRE_DrawChooser ();
	}
	else if (qre.reset_prompt)
	{
		int answer = QR_GUI_DialogCentered ("QuakeRay v." ENGINE_VER_STRING,
		    "This will remove all your saved work and all materials/light and everything will be set to default. Are you REALLY SURE?",
		    "Yes", "No");

		if (answer != 0)
		{
			qre.reset_prompt = false;
			qre.reset_pending = (answer == 1);
		}
	}
	else if (qre.exit_prompt)
	{
		int answer = QR_GUI_DialogCentered ("QuakeRay v." ENGINE_VER_STRING,
		                                    "Save your changes?", "Yes", "No");

		if (answer == 1)
		{
			qre.exit_prompt = false;
			QRE_SessionSave ();
		}
		else if (answer == 2)
		{
			qre.exit_prompt = false;
			QRE_SessionDiscard ();
		}
	}
	else if (qre.panel_open)
	{
		if (qre.mode == QRE_MODE_LIGHT)
			QRE_BuildLightPanelGUI ();
		else
			QRE_BuildPanelGUI ();
	}
	else
		QRE_BuildFlyingOverlay ();

	QRE_DrawHints ();
	QRE_DrawOverlay ();
	RT_DtalDebugDrawGui (CVAR_TO_INT32 (rt_dtal_debug), (unsigned int)host_framecount, (float)host_frametime,
	                     glx, gly, glwidth, glheight, vid.height);

	qre.panel_drawing = false;

	QR_GUI_EndFrame ();
}

// ---------------------------------------------------------------------------
// Input hooks
// ---------------------------------------------------------------------------

qboolean QR_Editor_KeyEvent (int key, qboolean down)
{
	if (!qre.active)
		return false;

	// while the panel is open its events are consumed at the SDL level
	// (QR_Editor_GuiProcessEvent); only the flying mode is left here
	if (qre.panel_open)
		return false;

	if (key == K_TAB && down)
	{
		// the other half of the Tab toggle: the panel takes the cursor
		QRE_CursorMode (true);
		return true;
	}

	if (key == K_ESCAPE && down)
	{
		if (qre.custom_dragging)
		{
			QRE_CustomGizmoCancel ();
			return true;
		}
		if (qre.light_dragging)
		{
			QRE_CancelLightDrag ();
			return true;
		}
		QRE_RequestExit ();
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// The axis gizmo: the world-axis arrows of the selected custom light are the
// drag handles (they are drawn in QRE_DrawLightWireframes). A press near one of
// them in screen space starts a drag along that axis, the motion moves the light
// and the release ends it.
// ---------------------------------------------------------------------------

#define QRE_GIZMO_LEN 12.0f // the drawn length of an axis arrow, world units

static int QRE_CustomSelectedIndex (void)
{
	int count = 0;
	int index;

	if (!qre.sel_light_valid || qre.sel_light.kind != RT_LIGHT_KIND_CUSTOM ||
	    qre.sel_light.uniqueID <= (uint64_t)UINT32_MAX)
		return -1;

	(void)RT_CustomLights (&count);
	index = (int)(qre.sel_light.uniqueID - ((uint64_t)UINT32_MAX + 1));
	return (index >= 0 && index < count) ? index : -1;
}

static void QRE_GizmoOrigin (const rt_custom_light_t *l, vec3_t out)
{
	VectorCopy (l->origin, out);
	if (l->has_offset)
		VectorAdd (out, l->offset, out);
}

static void QRE_GizmoAxis (const rt_custom_light_t *l, int axis, vec3_t out)
{
	out[0] = out[1] = out[2] = 0.0f;
	out[axis] = 1.0f;
	if (l->spot && axis < 2)
	{
		const float length = sqrtf (l->dir[0] * l->dir[0] + l->dir[1] * l->dir[1]);

		if (length > 1e-6f)
		{
			out[0] = (axis == 0 ? l->dir[0] : -l->dir[1]) / length;
			out[1] = (axis == 0 ? l->dir[1] : l->dir[0]) / length;
		}
	}
}

#define QRE_SPOT_ARC_RADIUS (QRE_GIZMO_LEN * 1.2f)
#define QRE_SPOT_ARC_SEGS 24
#define QRE_SPOT_ARC_SWEEP 300.0f

static void QRE_SpotArcBasis (const rt_custom_light_t *light, int axis, vec3_t u, vec3_t v)
{
	QRE_GizmoAxis (light, (axis + 1) % 3, u);
	QRE_GizmoAxis (light, (axis + 2) % 3, v);
}

static qboolean QRE_SpotArcProject (const rt_custom_light_t *light, const vec3_t center, int axis, float *xy)
{
	const float deg2rad = 3.14159265f / 180.0f;
	vec3_t      u, v;
	int         i;

	QRE_SpotArcBasis (light, axis, u, v);

	for (i = 0; i <= QRE_SPOT_ARC_SEGS; i++)
	{
		const float a = QRE_SPOT_ARC_SWEEP * (float)i / (float)QRE_SPOT_ARC_SEGS * deg2rad;
		const float cs = cosf (a), sn = sinf (a);
		vec3_t      p;
		int         k;

		for (k = 0; k < 3; k++)
			p[k] = center[k] + QRE_SPOT_ARC_RADIUS * (cs * u[k] + sn * v[k]);

		if (!QRE_WorldToScreen (p, &xy[i * 2], &xy[i * 2 + 1]))
			return false;
	}

	return true;
}

static void QRE_DrawSpotDirArcs (const rt_custom_light_t *light, const vec3_t center, const uint32_t axis_color[3])
{
	const float deg2rad = 3.14159265f / 180.0f;
	const float head_angle = 28.0f * deg2rad;
	const float head_len = QRE_SPOT_ARC_RADIUS * 0.22f;
	const float end_angle = QRE_SPOT_ARC_SWEEP * deg2rad;
	float       xy[(QRE_SPOT_ARC_SEGS + 1) * 2];
	int         a;

	for (a = 1; a < 3; a++)
	{
		vec3_t u, v, end, radial, tangent;
		float  ex, ey, ec, es;
		int    i, k;

		if (!QRE_SpotArcProject (light, center, a, xy))
			continue;

		QR_GUI_DrawPolyline (xy, QRE_SPOT_ARC_SEGS + 1, axis_color[a], 8.0f);

		QRE_SpotArcBasis (light, a, u, v);
		ec = cosf (end_angle);
		es = sinf (end_angle);

		for (k = 0; k < 3; k++)
		{
			radial[k] = ec * u[k] + es * v[k];
			tangent[k] = -es * u[k] + ec * v[k];
			end[k] = center[k] + QRE_SPOT_ARC_RADIUS * radial[k];
		}

		if (!QRE_WorldToScreen (end, &ex, &ey))
			continue;

		for (i = 0; i < 2; i++)
		{
			const float side = (i == 0) ? 1.0f : -1.0f;
			const float ch = cosf (head_angle), sh = sinf (head_angle);
			vec3_t      wing;
			float       wx, wy;

			for (k = 0; k < 3; k++)
				wing[k] = end[k] + head_len * (-ch * tangent[k] + side * sh * radial[k]);

			if (!QRE_WorldToScreen (wing, &wx, &wy))
				continue;

			xy[0] = ex;
			xy[1] = ey;
			xy[2] = wx;
			xy[3] = wy;
			QR_GUI_DrawPolyline (xy, 2, axis_color[a], 8.0f);
		}
	}
}

static void QRE_DrawGizmoAxisArrows (const rt_custom_light_t *light, const vec3_t pos, const uint32_t axis_color[3])
{
	vec3_t adir, tup, tip, head;
	float  ox, oy;
	int    a, h;

	if (!QRE_WorldToScreen (pos, &ox, &oy))
		return;

	for (a = 0; a < 3; a++)
	{
		float tx, ty, hx, hy, w;
		float xy[4];

		QRE_GizmoAxis (light, a, adir);
		for (int k = 0; k < 3; k++)
			tip[k] = pos[k] + QRE_GIZMO_LEN * adir[k];
		if (!QRE_WorldToScreen (tip, &tx, &ty))
			continue;

		xy[0] = ox;
		xy[1] = oy;
		xy[2] = tx;
		xy[3] = ty;
		QR_GUI_DrawPolyline (xy, 2, axis_color[a], 8.0f);

		QRE_GizmoAxis (light, (a + 1) % 3, tup);

		w = CLAMP (0.4f, QRE_DepthToCamera (pos) * 0.003f, 8.0f);

		for (h = 0; h < 2; h++)
		{
			VectorCopy (tip, head);
			VectorMA (head, -QRE_GIZMO_LEN * 0.28f, adir, head);
			VectorMA (head, (h == 0 ? 1.0f : -1.0f) * w * 6.0f, tup, head);

			if (!QRE_WorldToScreen (head, &hx, &hy))
				continue;

			xy[0] = tx;
			xy[1] = ty;
			xy[2] = hx;
			xy[3] = hy;
			QR_GUI_DrawPolyline (xy, 2, axis_color[a], 8.0f);
		}
	}
}

static void QRE_DrawGizmoArrows (void)
{
	int                index = QRE_CustomSelectedIndex ();
	int                count = 0;
	rt_custom_light_t *custom;
	uint32_t           axis_color[3];
	vec3_t             pos;

	if (index < 0)
		return;

	custom = RT_CustomLights (&count);
	if (index >= count)
		return;

	axis_color[0] = RT_PackColorToUint32 (255, 64, 64, 255);
	axis_color[1] = RT_PackColorToUint32 (64, 255, 64, 255);
	axis_color[2] = RT_PackColorToUint32 (64, 128, 255, 255);

	QRE_GizmoOrigin (&custom[index], pos);
	QRE_DrawGizmoAxisArrows (&custom[index], pos, axis_color);

	if (custom[index].spot)
		QRE_DrawSpotDirArcs (&custom[index], pos, axis_color);
}

// World space to screen pixels, with the view the editor camera uses.
static qboolean QRE_WorldToScreen (const vec3_t p, float *out_x, float *out_y)
{
	const float deg2rad = 3.14159265f / 180.0f;
	vec3_t      forward, right, up, d;
	float       depth, tan_x, tan_y;

	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorSubtract (p, r_refdef.vieworg, d);

	depth = DotProduct (d, forward);
	if (depth <= 1.0f)
		return false;

	tan_x = tanf (r_refdef.fov_x * deg2rad * 0.5f);
	tan_y = tanf (r_refdef.fov_y * deg2rad * 0.5f);
	if (tan_x <= 0.0f || tan_y <= 0.0f)
		return false;

	*out_x = (0.5f + 0.5f * (DotProduct (d, right) / depth) / tan_x) * (float)glwidth;
	*out_y = (0.5f - 0.5f * (DotProduct (d, up) / depth) / tan_y) * (float)glheight;
	return true;
}

static float QRE_DistToSegment (float px, float py, float x0, float y0, float x1, float y1)
{
	const float dx = x1 - x0, dy = y1 - y0;
	const float len2 = dx * dx + dy * dy;
	float       t = 0.0f, qx, qy;

	if (len2 > 0.0f)
		t = CLAMP (0.0f, ((px - x0) * dx + (py - y0) * dy) / len2, 1.0f);

	qx = x0 + dx * t;
	qy = y0 + dy * t;
	return sqrtf ((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

static qboolean QRE_CustomGizmoBegin (float mx, float my)
{
	int                index = QRE_CustomSelectedIndex ();
	int                count = 0;
	rt_custom_light_t *lights;
	rt_custom_light_t *l;
	vec3_t             origin, tip;
	float              ox, oy;
	int                a, axis = -1;

	if (index < 0)
		return false;

	lights = RT_CustomLights (&count);
	l = &lights[index];
	QRE_GizmoOrigin (l, origin);

	if (!QRE_WorldToScreen (origin, &ox, &oy))
		return false;

	if (mx < 0.0f)
		return false;

	if (l->spot)
	{
		float best = 10.0f;
		float xy[(QRE_SPOT_ARC_SEGS + 1) * 2];

		for (a = 1; a < 3; a++)
		{
			float d = 1e30f;
			int   i;

			if (!QRE_SpotArcProject (l, origin, a, xy))
				continue;

			for (i = 0; i < QRE_SPOT_ARC_SEGS; i++)
			{
				const float seg = QRE_DistToSegment (mx, my, xy[i * 2], xy[i * 2 + 1],
				                                     xy[(i + 1) * 2], xy[(i + 1) * 2 + 1]);

				if (seg < d)
					d = seg;
			}

			if (d < best)
			{
				best = d;
				axis = a;
			}
		}
	}

	if (axis >= 0)
	{
		qre.custom_dragging = true;
		qre.custom_drag_dir = true;
		qre.custom_drag_axis = axis;
		qre.custom_drag_index = index;
		QRE_GizmoAxis (l, axis, qre.custom_drag_vector);
		VectorCopy (l->origin, qre.custom_drag_origin);
		VectorCopy (l->dir, qre.custom_drag_dir_start);

		if (qre.custom_drag_dir_start[0] == 0.0f && qre.custom_drag_dir_start[1] == 0.0f &&
		    qre.custom_drag_dir_start[2] == 0.0f)
		{
			qre.custom_drag_dir_start[0] = 1.0f;
			VectorCopy (qre.custom_drag_dir_start, l->dir);
		}

		qre.custom_drag_mouse[0] = mx;
		qre.custom_drag_mouse[1] = my;
		return true;
	}

	{
		float best = 10.0f;

		for (a = 0; a < 3; a++)
		{
			float ax, ay, d;
			vec3_t direction;

			QRE_GizmoAxis (l, a, direction);
			VectorMA (origin, QRE_GIZMO_LEN, direction, tip);
			if (!QRE_WorldToScreen (tip, &ax, &ay))
				continue;

			d = QRE_DistToSegment (mx, my, ox, oy, ax, ay);
			if (d < best)
			{
				best = d;
				axis = a;
			}
		}
	}

	if (axis < 0)
		return false;

	qre.custom_dragging = true;
	qre.custom_drag_dir = false;
	qre.custom_drag_axis = axis;
	qre.custom_drag_index = index;
	QRE_GizmoAxis (l, axis, qre.custom_drag_vector);
	VectorCopy (l->origin, qre.custom_drag_origin);
	qre.custom_drag_mouse[0] = mx;
	qre.custom_drag_mouse[1] = my;
	return true;
}

static void QRE_CustomGizmoMove (float mx, float my)
{
	int                count = 0;
	rt_custom_light_t *lights;
	rt_custom_light_t *l;
	vec3_t             origin, tip;
	float              ox, oy, ax, ay, dirx, diry, pixlen, delta;

	if (!qre.custom_dragging)
		return;

	lights = RT_CustomLights (&count);
	if (qre.custom_drag_index < 0 || qre.custom_drag_index >= count)
		return;

	l = &lights[qre.custom_drag_index];
	VectorCopy (qre.custom_drag_origin, origin);
	if (l->has_offset)
		VectorAdd (origin, l->offset, origin);

	if (qre.custom_drag_dir)
	{
		vec3_t axis, rot, forward, right, up;

		if (mx < 0.0f && !qre.gizmo_fly_drag)
			return;

		delta = (qre.custom_drag_axis == 1 ? my - qre.custom_drag_mouse[1]
		                                    : mx - qre.custom_drag_mouse[0]) * 0.5f;
		VectorCopy (qre.custom_drag_vector, axis);

		AngleVectors (r_refdef.viewangles, forward, right, up);
		if (DotProduct (forward, axis) < 0.0f)
			delta = -delta;

		RotatePointAroundVector (rot, axis, qre.custom_drag_dir_start, fmodf (delta, 360.0f));
		VectorNormalize (rot);
		VectorCopy (rot, l->dir);
		return;
	}

	if (!QRE_WorldToScreen (origin, &ox, &oy))
		return;

	VectorMA (origin, QRE_GIZMO_LEN, qre.custom_drag_vector, tip);
	if (!QRE_WorldToScreen (tip, &ax, &ay))
		return;

	if (mx < 0.0f)
		return;

	dirx = ax - ox;
	diry = ay - oy;
	pixlen = sqrtf (dirx * dirx + diry * diry);
	if (pixlen < 1.0f)
		return;

	dirx /= pixlen;
	diry /= pixlen;

	// how far the cursor moved along the axis' screen direction, in world units
	delta = ((mx - qre.custom_drag_mouse[0]) * dirx + (my - qre.custom_drag_mouse[1]) * diry) /
	        pixlen * QRE_GIZMO_LEN;

	VectorMA (qre.custom_drag_origin, floorf (delta + 0.5f), qre.custom_drag_vector, l->origin);
}

// Called for every SDL event before the engine handles it.
qboolean QR_Editor_GuiProcessEvent (const void *sdl_event)
{
	const SDL_Event *e = (const SDL_Event *)sdl_event;

	if (!qre.active || !qre.panel_open)
		return false;

	// while the console is up the engine owns the input
	if (key_dest != key_game)
		return false;

	// The axis gizmo of the Custom tab: the arrows of the selected custom light
	// are the drag handles, and the panel keeps its own clicks.
	if (qre.mode == QRE_MODE_LIGHT)
	{
		if (!qre.custom_dragging && e->type == SDL_MOUSEBUTTONDOWN &&
		    e->button.button == SDL_BUTTON_LEFT && !QR_GUI_WantsMouse ())
		{
			float gmx = -1.0f, gmy = -1.0f;

			QR_GUI_GetMousePos (&gmx, &gmy);
			if (gmx >= 0.0f && QRE_CustomGizmoBegin (gmx, gmy))
				return true;
		}

		if (qre.custom_dragging && !qre.gizmo_fly_drag)
		{
			if (e->type == SDL_MOUSEMOTION)
			{
				// the bridge owns the cursor position the drag reads: let it see
				// the motion first, then move the light and swallow the event
				float gmx = -1.0f, gmy = -1.0f;

				QR_GUI_ProcessEvent (e);
				QR_GUI_GetMousePos (&gmx, &gmy);
				QRE_CustomGizmoMove (gmx, gmy);
				return true;
			}
			if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT)
			{
				QR_GUI_ProcessEvent (e);
				qre.custom_dragging = false;
				return true;
			}
		}
	}

	// the console toggle key stays available unless an ImGui text field is
	// editing: the console is the way the editor is driven too
	if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) &&
	    e->key.keysym.scancode == SDL_SCANCODE_GRAVE && !QR_GUI_WantsKeyboard ())
		return false;

	// Tab hands the mouse back to the camera (the flying mode picks with the
	// fire button and flies again); a text field keeps its own Tab
	if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_TAB && !QR_GUI_WantsKeyboard ())
	{
		if (!qre.choosing && !qre.reset_prompt)
			QRE_CursorMode (false);
		return true;
	}

	// ESC dismisses the exit question, or closes the panel, unless an ImGui
	// text field is editing
	if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE && !QR_GUI_WantsKeyboard ())
	{
		if (qre.choosing)
		{
			QRE_RequestExit ();
		}
		else if (qre.reset_prompt)
		{
			qre.reset_prompt = false;
		}
		else if (qre.exit_prompt)
		{
			qre.exit_prompt = false; // back to editing
			if (qre.prompt_from_flying)
				QRE_ClosePanel ();
		}
		else
			QRE_ClosePanel ();
		return true;
	}

	return QR_GUI_ProcessEvent (sdl_event) ? true : false;
}

qboolean QR_Editor_TextEntryActive (void)
{
	// while the panel is open SDL text input must stay on for ImGui fields
	return qre.active && qre.panel_open;
}

// ---------------------------------------------------------------------------
// Apply / Cancel / Exit
// ---------------------------------------------------------------------------

static void QRE_ClosePanel (void)
{
	if (!qre.panel_open)
		return;

	qre.panel_open = false;

	IN_Activate ();
	SDL_ShowCursor (SDL_ENABLE);
	QR_GUI_SetMouseCursor (0);
}

static void QRE_Apply (void)
{
	const qboolean globals = (qre.mode == QRE_MODE_LIGHT) ? QRE_GlobalsTouched () : QRE_WaterTouched ();
	const qboolean touched = (qre.mode == QRE_MODE_LIGHT) ? QRE_LightSessionTouched () : (qre.touched_count > 0);
	const qboolean session = QRE_WriteSession ();

	if (!session && !globals)
	{
		QRE_Notify (touched ? "the session could not be written" : "nothing to save yet");
		return;
	}

	if (globals)
		Host_WriteConfiguration ();

	if (qre.mode == QRE_MODE_LIGHT)
	{
		QRE_TakeLightSnapshot (); // Cancel now reverts to the state just saved
		QRE_TakeGlobalsSnapshot ();

		if (session)
			QRE_Notify ("session written to qray.lights.editor.yaml");
		if (globals)
			QRE_Notify ("global settings written to the config");
		return;
	}

	QRE_TakeSnapshot (); // Cancel now reverts to the state just saved
	QRE_TakeWaterSnapshot ();
	if (session)
		QRE_Notify ("session written to qray.materials.editor.yaml");
	if (globals)
		QRE_Notify ("settings written to the config");
}

static void QRE_Cancel (void)
{
	if (qre.mode == QRE_MODE_LIGHT)
	{
		// the light uploads read the live lists every frame, so putting the
		// snapshots back is enough; the fog goes back through the fog command
		QRE_RestoreLightSnapshot ();

		remove (qre.editor_file);
		QRE_ClearSessionState ();

		QRE_Notify ("light overrides, custom lights, fog and global settings reverted");
		return;
	}

	QRE_RestoreSnapshot ();
	QRE_ReapplyTouched (); // put the yaml values back on screen
	QRE_WaterRestore ();

	qre.tmp_appended = false;

	// the restored lists may no longer contain the edited materials:
	// rebuild the group (a picked texture without a material goes back to the
	// detached defaults)
	if (qre.pick_glt)
	{
		char texname[MAX_QPATH];
		char *dot;

		RT_MAT_NormalizeName (qre.pick_glt->name, texname, sizeof (texname));
		dot = strrchr (texname, '.');
		if (dot && !strchr (dot, ':'))
			*dot = '\0';
		q_strlcpy (qre.pick_name, texname, sizeof (qre.pick_name));
		QRE_ResolveGroup (texname);
	}

	// nothing is pending after a cancel: the session files go away, so Exit
	// goes back to the chooser instead of asking to save them
	remove (qre.editor_file);
	QRE_ClearSessionState ();

	QRE_Notify ("materials and water settings reverted to the values from qray.materials.yaml and the config");
}

// ---------------------------------------------------------------------------
// Saving qray.materials.yaml
// ---------------------------------------------------------------------------

// Written to the top of qray.materials.yaml. Kept in sync with the header
// of renderer/Source/materials.yaml, which documents the accepted keys.
static const char *qre_yaml_header =
	"# Global material definitions for the qray ray-traced renderer.\n"
	"# `is_light: false` marks a *static* surface (textures/*) whose luma\n"
	"# texture should generate emissive triangle lights. Dynamic surfaces\n"
	"# (progs/* models and sprites) are gated separately by the engine.\n"
	"#\n"
	"# Emissive masks can be authored two ways:\n"
	"#   * `texture_emissive: textures/foo_luma.png` -- a hand-painted mask file:\n"
	"#     its pixels' luminance is the emission. Most precise; keeps the mask\n"
	"#     independent of the diffuse art.\n"
	"#   * `color_emissive:` -- no mask file: each block below matches its own\n"
	"#     color against the base texture, so only pixels close to it glow. A\n"
	"#     block carries its color and its own tone controls:\n"
	"#         color_emissive:\n"
	"#           - color: ff0000          # rrggbb\n"
	"#             threshold: 0.02        # 0..1 color-cube distance / sqrt(3)\n"
	"#             feather: 2             # pixels of outward edge softening\n"
	"#             emissive_factor: 1     # scales this block's glow (0..5)\n"
	"#             blend: screen          # cvar | off | normal | screen |\n"
	"#                                    # overlay | hard light | color dodge\n"
	"#     The synthesized mask is white where the pixel matches the color\n"
	"#     exactly and decays exponentially to black towards the threshold, so the\n"
	"#     glow fades out softly instead of ending in a hard edge; the feather\n"
	"#     extends that edge outward without comparing colors, and the pixels\n"
	"#     the threshold selected keep their value. Up to ten\n"
	"#     blocks may share one texture; a pixel glows when any of them matches\n"
	"#     it, and the block whose color is nearest supplies its blend mode.\n"
	"#     A block may carry `polygon: \"u,v u,v ...\"` (up to sixteen UV points,\n"
	"#     0..1) instead of a color: the pixels inside the polygon glow, so the\n"
	"#     mask follows the shape drawn on the texture; `threshold` does not\n"
	"#     apply, `feather`, `emissive_factor` and `blend` do.\n"
	"#     `emissive_factor` scales the result. Combine with `is_light: true`\n"
	"#     to also cast light (otherwise the surface only glows):\n"
	"#     e.g.  - name: textures/foo\n"
	"#             color_emissive:\n"
	"#               - color: ff0000\n"
	"#                 threshold: 0.02\n"
	"#                 feather: 3\n"
	"#                 emissive_factor: 2\n"
	"#                 blend: screen\n"
	"#             is_light: true\n"
	"# The old single-color keys (`color_emissive: ff0000,00ff00`,\n"
	"# `color_emissive_threshold`, `color_emissive_feather`) are still read: a\n"
	"# block without its own controls inherits them.\n"
	"# Precedence: an authored `texture_emissive` always wins -- while the key is\n"
	"# present in an entry, `color_emissive` in that same entry is ignored\n"
	"# entirely (even if the luma file fails to load, which is reported at\n"
	"# startup). The classic fullbright mask is merged rather than replaced, so\n"
	"# its pixels emit in addition to the luma mask.\n"
	"#\n"
	"# `emissive_blend: screen` (or a number) overrides the global `rt_emis_blend`\n"
	"# cvar for the emission that has no block of its own (a texture_emissive\n"
	"# mask); a color block's own `blend` wins for its pixels. The names are what\n"
	"# the shader does:\n"
	"#   0 - off: emission is not composited at all\n"
	"#   1 - normal: emission is used as coverage (this is the cvar default)\n"
	"#   2 - screen: added on top of the image (brightest, keeps saturation)\n"
	"#   3 - overlay: the overlay formula, driven by the underlying base color\n"
	"#   4 - hard light: the same formula, driven by the emission color\n"
	"#   5 - color dodge: base divided by the inverted emission (brightens)\n"
	"# Intended for emissive *mirrored* surfaces (stained glass, lit windows):\n"
	"# they read as washed out in the default mode, while the additive mode keeps\n"
	"# them bright and saturated.\n"
	"#     e.g.  - name: textures/window01_1\n"
	"#             mirror: true\n"
	"#             emissive_blend: screen\n"
	"#\n"
	"# `emissive_focus` (degrees, 0..90) confines the light a material casts to a\n"
	"# cone around its normal: full brightness up to the angle, nothing beyond.\n"
	"# No key keeps the default wide lobe; under a projector it is 45 degrees.\n"
	"# `emissive_focus_soft` (degrees, absolute) is the width of the cone's soft\n"
	"# edge: brightness holds to `emissive_focus - emissive_focus_soft` and then\n"
	"# falls smoothly (smoothstep squared) to zero at the focus angle, so the edge\n"
	"# always grows inward from it. No key uses 45 degrees under a projector and a\n"
	"# tenth of the focus angle otherwise; 0 is a nearly hard edge.\n"
	"# `emissive_projector: true` routes the light into the material's emissive\n"
	"# mask instead of dimming it -- a gobo: the mask is read along the direction\n"
	"# of each point it lights, over the same cone (`emissive_focus`; no key = 45\n"
	"# degrees), and `emissive_focus_soft` also blurs the projected pattern (the\n"
	"# mask is read from a blurrier mip as the edge grows).\n"
	"#     e.g.  - name: textures/window1_2\n"
	"#             emissive_focus: 45\n"
	"#             emissive_focus_soft: 8\n"
	"#             emissive_projector: true\n";

static void QRE_WriteColor (FILE *f, const char *key, const vec3_t rgb)
{
	fprintf (f, "    %s: %02x%02x%02x\n", key,
	         (int)(rgb[0] * 255.0f + 0.5f) & 0xff,
	         (int)(rgb[1] * 255.0f + 0.5f) & 0xff,
	         (int)(rgb[2] * 255.0f + 0.5f) & 0xff);
}

// The emissive blocks, one YAML mapping each: the color a block matches (or
// the polygon that selects its pixels) and the tone controls that work for it
// alone.
static void QRE_WriteEmissiveBlocks (FILE *f, const rt_material_t *m)
{
	int i;

	fprintf (f, "    color_emissive:\n");
	for (i = 0; i < m->color_emissive_count; i++)
	{
		const rt_emissive_t *b = &m->color_emissive[i];

		if (b->poly_count >= 3)
		{
			int p;

			fprintf (f, "      - polygon:");
			for (p = 0; p < b->poly_count; p++)
				fprintf (f, " %.4f,%.4f", b->poly_uv[p][0], b->poly_uv[p][1]);
			fprintf (f, "\n");
		}
		else
		{
			fprintf (f, "      - color: %02x%02x%02x\n",
			         (int)(b->color[0] * 255.0f + 0.5f) & 0xff,
			         (int)(b->color[1] * 255.0f + 0.5f) & 0xff,
			         (int)(b->color[2] * 255.0f + 0.5f) & 0xff);
			fprintf (f, "        threshold: %.6g\n", b->threshold);
		}
		fprintf (f, "        feather: %.6g\n", b->feather);
		fprintf (f, "        emissive_factor: %.6g\n", b->factor);
		fprintf (f, "        blend: %s\n", RT_MAT_EmissiveBlendName (b->blend));
	}
}

// Writes one material entry. Only values that differ from the defaults are
// emitted (plus texture paths and color flags), so the file stays readable and
// matches the style it was loaded from.
static void QRE_WriteMaterial (FILE *f, const rt_material_t *m)
{
	fprintf (f, "  - name: %s\n", m->name);
	if (m->filename_base[0])
		fprintf (f, "    texture_base: %s\n", m->filename_base);
	if (m->filename_normals[0])
		fprintf (f, "    texture_normals: %s\n", m->filename_normals);
	if (m->filename_emissive[0])
		fprintf (f, "    texture_emissive: %s\n", m->filename_emissive);
	if (m->filename_gloss[0])
		fprintf (f, "    texture_gloss: %s\n", m->filename_gloss);
	if (m->bump_scale != 1.0f)
		fprintf (f, "    bump_scale: %.6g\n", m->bump_scale);
	// mirror forces roughness_override to 0; the synthesis gives it the last
	// word, so the dead override is not perpetuated by a save
	if (!m->mirror && m->roughness_override != 0.0f)
		fprintf (f, "    roughness_override: %.6g\n", m->roughness_override);
	if (m->has_metalness_factor)
		fprintf (f, "    metalness_factor: %.6g\n", m->metalness_factor);
	if (m->metalness_from_normal_alpha)
		fprintf (f, "    metalness_from_normal_alpha: true\n");
	if (m->emissive_factor != 1.0f)
		fprintf (f, "    emissive_factor: %.6g\n", m->emissive_factor);
	if (m->emissive_blend >= 0)
		fprintf (f, "    emissive_blend: %s\n", RT_MAT_EmissiveBlendName (m->emissive_blend));
	if (m->base_factor != 1.0f)
		fprintf (f, "    base_factor: %.6g\n", m->base_factor);
	if (m->is_light)
		fprintf (f, "    is_light: true\n");
	if (m->light_styles)
		fprintf (f, "    light_styles: true\n");
	if (m->has_color_emissive && m->color_emissive_count > 0)
		QRE_WriteEmissiveBlocks (f, m);
	if (m->has_light_color)
		QRE_WriteColor (f, "light_color", m->light_color);
	if (m->light_brightness != 1.0f)
		fprintf (f, "    light_brightness: %.6g\n", m->light_brightness);
	if (m->light_upoffset != 0.0f)
		fprintf (f, "    light_upoffset: %.6g\n", m->light_upoffset);
	if (m->emissive_focus > 0.0f)
		fprintf (f, "    emissive_focus: %.6g\n", m->emissive_focus);
	if (m->emissive_focus_soft >= 0.0f)
		fprintf (f, "    emissive_focus_soft: %.6g\n", m->emissive_focus_soft);
	if (m->emissive_projector)
		fprintf (f, "    emissive_projector: true\n");
	if (m->mirror)
		fprintf (f, "    mirror: true\n");
	if (m->exact_normals)
		fprintf (f, "    exact_normals: true\n");
	if (m->force_rasterize)
		fprintf (f, "    force_rasterize: true\n");
	if (m->alpha_test)
		fprintf (f, "    alpha_test: true\n");
}

static qboolean QRE_FileExists (const char *path)
{
	FILE *f = fopen (path, "rb");

	if (!f)
		return false;
	fclose (f);
	return true;
}

static qboolean QRE_CopyFile (const char *from, const char *to)
{
	FILE    *in = fopen (from, "rb");
	FILE    *out;
	char     buf[8192];
	size_t   n;
	qboolean ok = true;

	if (!in)
		return false;

	out = fopen (to, "wb");
	if (!out)
	{
		fclose (in);
		return false;
	}

	while ((n = fread (buf, 1, sizeof (buf), in)) > 0)
	{
		if (fwrite (buf, 1, n, out) != n)
		{
			ok = false;
			break;
		}
	}
	if (ferror (in))
		ok = false;

	fclose (in);
	if (fflush (out) != 0)
		ok = false;
	fclose (out);
	return ok;
}

// The names the target qray.materials.yaml already carries: the session file is
// what replaces that file when it is saved, so those entries have to be written
// back (with their live, possibly edited values) or the save would drop them.
#define QRE_SESSION_NAMES_MAX 1024

// The touched entry of the current mode, written in the file's own format. A
// name the loader cannot resolve any more is not written at all.
static qboolean QRE_SessionEntryResolves (const char *name)
{
	if (qre.mode == QRE_MODE_LIGHT)
		return RT_LIGHT_HasFields (RT_LIGHT_Find (name)) ? true : false;
	return RT_MAT_Find (name) != NULL;
}

static void QRE_SessionWriteEntry (FILE *f, const char *name)
{
	if (qre.mode == QRE_MODE_LIGHT)
	{
		const rt_light_t *l = RT_LIGHT_Find (name);

		if (l)
			RT_LIGHT_WriteEntry (f, l);
	}
	else
	{
		const rt_material_t *m = RT_MAT_Find (name);

		if (m)
			QRE_WriteMaterial (f, m);
	}
}

// Creates every missing component of a directory path, without ending the game
// when one of them cannot be made: a save that cannot reach its directory reports
// the path and keeps the session. Sys_mkdir stays fatal for the directories the
// game cannot run without; a mod's own folder an editor save writes into is not
// one of them.
static qboolean QRE_CreateDir (const char *path)
{
	char  buf[MAX_OSPATH];
	char *ofs;

	q_strlcpy (buf, path, sizeof (buf));

	for (ofs = buf + 1; *ofs; ofs++)
	{
		if (*ofs == '/' || *ofs == '\\')
		{
			*ofs = '\0';
			if (!Sys_TryMkdir (buf))
				return false;
			*ofs = '/';
		}
	}

	return Sys_TryMkdir (buf);
}

// Writes the materials session file: the target file's own text with the blocks
// of the touched entries replaced, so comments, formatting and keys the loader
// does not understand survive a save. Entries the target does not carry are
// appended; a target that does not exist gets the standard header. Apply writes
// it, Save copies it over the target, Discard deletes it.
static qboolean QRE_WriteMergedSession (char (*touched)[MAX_QPATH], int touched_count)
{
	FILE    *in;
	FILE    *out;
	char     line[2048];
	char     written[QRE_TOUCHED_MAX];
	qboolean skipping = false;
	qboolean wrote = false;
	int      i;

	if (touched_count <= 0 || touched_count > QRE_TOUCHED_MAX)
		return false;

	memset (written, 0, sizeof (written));

	if (!QRE_CreateDir (com_gamedir))
	{
		QRE_Notify ("cannot create %s", com_gamedir);
		return false;
	}

	in = fopen (qre.target_file, "r");
	out = fopen (qre.editor_file, "w");
	if (!out)
	{
		if (in)
			fclose (in);
		QRE_Notify ("cannot write %s", qre.editor_file);
		return false;
	}

	if (!in)
	{
		// a target that does not exist yet: the standard header and the list key
		const qboolean light = (qre.mode == QRE_MODE_LIGHT);

		fprintf (out, "%s", light ? RT_LIGHT_Header () : qre_yaml_header);
		fprintf (out, "%s\n", light ? "qray_lights:" : "qray_materials:");
	}
	else
	{
		while (fgets (line, sizeof (line), in))
		{
			char *p = line;
			char  name[MAX_QPATH];

			while (*p == ' ' || *p == '\t')
				p++;

			if (!strncmp (p, "- name:", 7))
			{
				char *e;
				int   n;

				p += 7;
				while (*p == ' ' || *p == '\t')
					p++;
				e = p;
				while (*e && *e != '\r' && *e != '\n' && *e != ' ' && *e != '\t')
					e++;
				n = (int)(e - p);
				if (n >= MAX_QPATH)
					n = MAX_QPATH - 1;
				memcpy (name, p, (size_t)n);
				name[n] = '\0';
				q_strlwr (name);

				skipping = false;
				for (i = 0; i < touched_count; i++)
				{
					if (!strcmp (touched[i], name) && !written[i] && QRE_SessionEntryResolves (name))
					{
						QRE_SessionWriteEntry (out, name);
						written[i] = 1;
						skipping = true;
						wrote = true;
						break;
					}
				}
				if (skipping)
					continue;
			}
			else if (skipping && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r' || line[0] == '\n'))
			{
				continue; // the body of the block that was replaced
			}
			else
			{
				skipping = false;
			}

			if (line[0] != ' ' && line[0] != '\t')
			{
				if (!strncmp (line, "materials:", 10))
				{
					fputs ("qray_materials:\n", out); // migrate the old root key
					continue;
				}
				if (!strncmp (line, "lights:", 7))
				{
					fputs ("qray_lights:\n", out); // migrate the old root key
					continue;
				}
			}

			fputs (line, out);
		}
		fclose (in);
	}

	// the touched entries the target did not carry
	for (i = 0; i < touched_count; i++)
	{
		if (!written[i] && QRE_SessionEntryResolves (touched[i]))
		{
			QRE_SessionWriteEntry (out, touched[i]);
			wrote = true;
		}
	}

	if (!wrote)
	{
		fclose (out);
		remove (qre.editor_file); // an empty session is no session
		return false;
	}

	if (ferror (out) || fflush (out) != 0)
	{
		QRE_Notify ("write error in %s", qre.editor_file);
		fclose (out);
		remove (qre.editor_file);
		return false;
	}
	fclose (out);

	Con_Printf ("qr editor: session written to %s\n", qre.editor_file);
	return true;
}

// The current level's section: the fog that is in effect and the live custom
// lights, in the form RT_CustomLightsParse reads back (the key is the map's own
// level name).
static void QRE_CustomWriteLevel (FILE *out, const char *level)
{
	int                count = 0;
	rt_custom_light_t *lights = RT_CustomLights (&count);
	rt_custom_fog_t    fog;
	float              color[4];
	int                i;

	fprintf (out, "%s:\n", level);

	// The fog block is always part of the section: the Global tab authors the
	// level's fog, so the session has to carry it even when only a light
	// changed. "enabled" is the live rt_level_fog switch, so the section always
	// states whether the level's fog is drawn. (The color getter fills four
	// floats, hence the local.)
	memset (&fog, 0, sizeof (fog));
	fog.has_fog = true;
	fog.has_enabled = true;
	fog.enabled = CVAR_TO_BOOL (rt_level_fog);
	Fog_GetColor (color);
	fog.color[0] = color[0];
	fog.color[1] = color[1];
	fog.color[2] = color[2];
	fog.density = Fog_GetDensity ();

	fprintf (out, "  fog:\n");
	fprintf (out, "    enabled: %s\n", fog.enabled ? "true" : "false");
	fprintf (out, "    color: %02x%02x%02x\n",
	         (int)(CLAMP (0.0f, fog.color[0], 1.0f) * 255.0f + 0.5f) & 0xff,
	         (int)(CLAMP (0.0f, fog.color[1], 1.0f) * 255.0f + 0.5f) & 0xff,
	         (int)(CLAMP (0.0f, fog.color[2], 1.0f) * 255.0f + 0.5f) & 0xff);
	fprintf (out, "    density: %.6g\n", fog.density);

	if (count > 0)
	{
		fprintf (out, "  lights:\n");
		for (i = 0; i < count; i++)
			RT_CustomLights_WriteEntry (out, &lights[i]);
	}
}

// Whether the lines indented under a just-read key form an emitter list. The
// key name alone cannot tell the root list from a level named like it, so the
// body decides. The file position is the same after the call as before it.
static qboolean QRE_NextIsEmitterBody (FILE *f)
{
	char     probe[2048];
	qboolean emitter = false;
	long     pos = ftell (f);

	if (pos < 0)
		return false;

	while (fgets (probe, sizeof (probe), f))
	{
		char *p = probe;

		if (probe[0] == '#')
			continue;

		while (*p == ' ' || *p == '\t')
			p++;

		if (!strncmp (p, "- name:", 7))
		{
			emitter = true;
			break;
		}

		if (probe[0] != ' ' && probe[0] != '\t' && probe[0] != '\r' && probe[0] != '\n')
			break; // the next root line; the body was not an emitter list
	}

	fseek (f, pos, SEEK_SET);
	return emitter;
}

// Copies the level sections of a lights file into the session, leaving out the
// current level's section (when the Custom tab was touched) and, of a merged
// file, the emitter list under the root "qray_lights:" key.
static void QRE_WriteLevelBlocks (FILE *out, const char *source, const char *skip_level)
{
	FILE    *in = fopen (source, "r");
	char     line[2048];
	qboolean skipping = false;
	qboolean started = false;

	if (!in)
		return;

	while (fgets (line, sizeof (line), in))
	{
		if (!started)
		{
			if (line[0] == '#' || line[0] == ' ' || line[0] == '\t' ||
			    line[0] == '\r' || line[0] == '\n')
				continue;
			started = true;
		}

		if (skipping)
		{
			if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r' || line[0] == '\n')
				continue;
			skipping = false;
		}

		if (line[0] == '-' && !strncmp (line, "- name:", 7))
		{
			skipping = true;
			continue;
		}

		if (line[0] != ' ' && line[0] != '\t' && line[0] != '#' &&
		    line[0] != '\r' && line[0] != '\n')
		{
			char *colon = strchr (line, ':');

			if (colon)
			{
				char  key[64];
				char *e = colon;
				int   n;

				while (e > line && (e[-1] == ' ' || e[-1] == '\t'))
					e--;
				n = (int)(e - line);
				if (n >= (int)sizeof (key))
					n = (int)sizeof (key) - 1;
				memcpy (key, line, (size_t)n);
				key[n] = '\0';
				q_strlwr (key);

				if (!strcmp (key, "qray_lights"))
				{
					skipping = true;
					continue;
				}
				if (!strcmp (key, "lights") && QRE_NextIsEmitterBody (in))
				{
					skipping = true;
					continue;
				}
				if (skip_level && !strcmp (key, skip_level))
				{
					skipping = true;
					continue;
				}
			}
		}

		fputs (line, out);
	}

	fclose (in);
}

#define QRE_LIGHTS_ROOT_NONE   0
#define QRE_LIGHTS_ROOT_LEGACY 1 // the old root key, "lights:"
#define QRE_LIGHTS_ROOT_QRAY   2 // the namespaced root key, "qray_lights:"

// Which root emitter key the target carries, if any. "qray_lights:" is the
// namespaced root; a bare "lights:" is the root only when its body is an
// emitter list, so a section named "lights" belongs to that level.
static int QRE_FileLightsRootKind (const char *path)
{
	FILE *f = fopen (path, "r");
	char  line[2048];
	int   kind = QRE_LIGHTS_ROOT_NONE;

	if (!f)
		return QRE_LIGHTS_ROOT_NONE;

	while (fgets (line, sizeof (line), f))
	{
		if (!strncmp (line, "qray_lights:", 12))
		{
			kind = QRE_LIGHTS_ROOT_QRAY;
			break;
		}
		if (kind == QRE_LIGHTS_ROOT_NONE && !strncmp (line, "lights:", 7) &&
		    QRE_NextIsEmitterBody (f))
			kind = QRE_LIGHTS_ROOT_LEGACY;
	}

	fclose (f);
	return kind;
}

static int QRE_LightTouchedIndex (const char *name)
{
	int i;

	for (i = 0; i < qre.light_touched_count && i < QRE_TOUCHED_MAX; i++)
	{
		if (!q_strcasecmp (qre.light_touched[i], name))
			return i;
	}
	return -1;
}

// Writes one touched emitter entry when it still resolves; false when the
// writer has nothing to say about it.
static qboolean QRE_LightWriteTouched (FILE *out, qboolean *written, int index)
{
	rt_light_t *l;

	if (index < 0 || index >= qre.light_touched_count || written[index])
		return false;

	l = RT_LIGHT_Find (qre.light_touched[index]);
	if (!l || !RT_LIGHT_HasFields (l))
		return false;

	RT_LIGHT_WriteEntry (out, l);
	written[index] = true;
	return true;
}

// True when the entry is a touched one that resolves but no longer carries any
// field: the block in the target is stale and has to leave with the save.
static qboolean QRE_LightDropTouched (qboolean *written, int index)
{
	if (index < 0 || index >= qre.light_touched_count || written[index])
		return false;

	if (!RT_LIGHT_Find (qre.light_touched[index]))
		return false;

	written[index] = true;
	return true;
}

static int QRE_LightWriteMissing (FILE *out, qboolean *written)
{
	int i, count = 0;

	for (i = 0; i < qre.light_touched_count && i < QRE_TOUCHED_MAX; i++)
	{
		if (QRE_LightWriteTouched (out, written, i))
			count++;
	}

	return count;
}

// Writes qray.lights.editor.yaml: one file with the emitter overrides under the
// root "qray_lights:" key and one section per level for the custom lights and the
// fog. A merged target keeps its own text: only the touched emitter blocks and
// the current level's section are replaced, so comments and keys the loader
// does not understand survive a save.
static qboolean QRE_WriteLightSession (void)
{
	char        names[QRE_SESSION_NAMES_MAX][MAX_QPATH];
	int         name_count = 0;
	char        level[64];
	char        legacy_emitter[MAX_OSPATH];
	char        legacy_custom[MAX_OSPATH];
	const char *emitter_source;
	qboolean    custom_touched = QRE_CustomTouched ();
	qboolean    target_exists = QRE_FileExists (qre.target_file) ? true : false;
	int         root_kind = target_exists ? QRE_FileLightsRootKind (qre.target_file) : QRE_LIGHTS_ROOT_NONE;
	qboolean    merged_target = (root_kind != QRE_LIGHTS_ROOT_NONE);
	FILE       *out;
	qboolean    wrote = false;
	int         i, n;

	if (qre.light_touched_count <= 0 && !custom_touched)
		return false;

	if (!QRE_CreateDir (com_gamedir))
	{
		QRE_Notify ("cannot create %s", com_gamedir);
		return false;
	}

	q_snprintf (legacy_emitter, sizeof (legacy_emitter), "%s/lights.yaml", com_gamedir);
	q_snprintf (legacy_custom, sizeof (legacy_custom), "%s/qray/lights.yaml", com_gamedir);

	emitter_source = target_exists ? qre.target_file : legacy_emitter;

	if (QRE_FileExists (emitter_source))
		name_count = RT_LIGHT_ReadNames (emitter_source, names, QRE_SESSION_NAMES_MAX);

	for (i = 0; i < qre.light_touched_count && name_count < QRE_SESSION_NAMES_MAX; i++)
	{
		for (n = 0; n < name_count; n++)
		{
			if (!q_strcasecmp (names[n], qre.light_touched[i]))
				break;
		}
		if (n == name_count)
			q_strlcpy (names[name_count++], qre.light_touched[i], MAX_QPATH);
	}

	RT_CustomLights_LevelKey (cl.worldmodel ? cl.worldmodel->name : "", level, sizeof (level));

	out = fopen (qre.editor_file, "w");
	if (!out)
	{
		QRE_Notify ("cannot write %s", qre.editor_file);
		return false;
	}

	if (!merged_target)
	{
		// no merged target yet: write the canonical file and copy the level
		// sections the old files carry
		const char *level_source = target_exists ? qre.target_file : legacy_custom;

		fprintf (out, "%s", RT_LIGHT_Header ());
		fprintf (out, "qray_lights:\n");
		for (i = 0; i < name_count; i++)
		{
			rt_light_t *l = RT_LIGHT_Find (names[i]);

			if (l && RT_LIGHT_HasFields (l))
			{
				RT_LIGHT_WriteEntry (out, l);
				wrote = true;
			}
		}

		if (QRE_FileExists (level_source))
			QRE_WriteLevelBlocks (out, level_source, custom_touched ? level : NULL);

		if (custom_touched)
		{
			if (QRE_FileExists (level_source) && !wrote)
				fprintf (out, "\n");
			QRE_CustomWriteLevel (out, level);
			wrote = true;
		}
	}
	else
	{
		FILE    *in = fopen (qre.target_file, "r");
		char     line[2048];
		qboolean written[QRE_TOUCHED_MAX];
		qboolean skipping = false;
		qboolean in_lights = false;
		qboolean lights_closed = false;
		qboolean level_written = false;

		if (!in)
		{
			fclose (out);
			remove (qre.editor_file);
			return false;
		}

		memset (written, 0, sizeof (written));

		while (fgets (line, sizeof (line), in))
		{
			char *p = line;

			if (skipping)
			{
				char *q = line;

				while (*q == ' ' || *q == '\t')
					q++;

				if (!strncmp (q, "- name:", 7) || !strncmp (q, "name:", 5))
				{
					skipping = false; // the next block already starts
				}
				else if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r' || line[0] == '\n')
				{
					continue; // the body of the block that was replaced
				}
				else
				{
					skipping = false;
				}
			}

			while (*p == ' ' || *p == '\t')
				p++;

			if (!strncmp (p, "- name:", 7) || !strncmp (p, "name:", 5))
			{
				char  name[MAX_QPATH];
				char *e;
				int   len;
				int   touched;

				p = strchr (p, ':') + 1;
				while (*p == ' ' || *p == '\t')
					p++;
				e = p;
				while (*e && *e != '\r' && *e != '\n' && *e != ' ' && *e != '\t')
					e++;
				len = (int)(e - p);
				if (len >= MAX_QPATH)
					len = MAX_QPATH - 1;
				memcpy (name, p, (size_t)len);
				name[len] = '\0';
				q_strlwr (name);

				touched = QRE_LightTouchedIndex (name);
				if (touched >= 0)
				{
					if (written[touched] ||
					    QRE_LightWriteTouched (out, written, touched) ||
					    QRE_LightDropTouched (written, touched))
					{
						wrote = true;
						skipping = true;
						continue;
					}
				}
			}

			if (line[0] != ' ' && line[0] != '\t' && line[0] != '\r' && line[0] != '\n')
			{
				char *colon = strchr (line, ':');

				if (line[0] != '#' && colon)
				{
					char  key[64];
					char *e = colon;
					int   len;

					while (e > line && (e[-1] == ' ' || e[-1] == '\t'))
						e--;
					len = (int)(e - line);
					if (len >= (int)sizeof (key))
						len = (int)sizeof (key) - 1;
					memcpy (key, line, (size_t)len);
					key[len] = '\0';
					q_strlwr (key);

					if (!strcmp (key, "qray_lights"))
					{
						in_lights = true;
						fputs (line, out);
						continue;
					}
					if (!strcmp (key, "lights") && QRE_NextIsEmitterBody (in))
					{
						in_lights = true;
						fputs ("qray_lights:\n", out); // migrate the old root key
						continue;
					}

					if (in_lights && !lights_closed)
					{
						lights_closed = true;
						if (QRE_LightWriteMissing (out, written) > 0)
							wrote = true;
					}

					if (custom_touched && !level_written && !strcmp (key, level))
					{
						QRE_CustomWriteLevel (out, level);
						level_written = true;
						wrote = true;
						skipping = true;
						continue;
					}
				}
			}

			fputs (line, out);
		}

		if (!lights_closed)
		{
			fputc ('\n', out); // the file may not have ended on a newline
			if (QRE_LightWriteMissing (out, written) > 0)
				wrote = true;
		}

		if (custom_touched && !level_written)
		{
			fprintf (out, "\n");
			QRE_CustomWriteLevel (out, level);
			wrote = true;
		}

		if (in)
			fclose (in);
	}

	if (!wrote)
	{
		fclose (out);
		remove (qre.editor_file);
		return false;
	}

	if (ferror (out) || fflush (out) != 0)
	{
		QRE_Notify ("write error in %s", qre.editor_file);
		fclose (out);
		remove (qre.editor_file);
		return false;
	}
	fclose (out);

	Con_Printf ("qr editor: session written to %s\n", qre.editor_file);
	return true;
}

static qboolean QRE_WriteSession (void)
{
	if (qre.mode == QRE_MODE_LIGHT)
		return QRE_WriteLightSession ();
	return QRE_WriteMergedSession (qre.touched, qre.touched_count);
}

static void QRE_RestoreModeState (void)
{
	if (qre.mode == QRE_MODE_LIGHT)
	{
		QRE_RestoreLightSnapshot ();
	}
	else
	{
		QRE_RestoreSnapshot ();
		QRE_ReapplyTouched ();
		QRE_WaterRestore ();
	}
}

static void QRE_ClearSessionState (void)
{
	qre.touched_count = 0;
	qre.light_touched_count = 0;
	qre.dirty_count = 0;
	memset (qre.dirty_full, 0, sizeof (qre.dirty_full));
	memset (qre.dirty_light, 0, sizeof (qre.dirty_light));
}

static void QRE_DtalDebugOff (void)
{
	if (CVAR_TO_INT32 (rt_dtal_debug) != 0)
		Cvar_Set ("rt_dtal_debug", "0");
}

static void QRE_BackToChooser (void)
{
	QRE_CursorMode (true);

	qre.choosing = true;
	qre.exit_prompt = false;
	qre.reset_prompt = false;
	qre.reset_pending = false;
	qre.prompt_from_flying = false;
	qre.panel_open = true;

	qre.pick_model = NULL;
	qre.pick_surf = NULL;
	qre.pick_ent = NULL;
	qre.pick_glt = NULL;
	qre.hover_model = NULL;
	qre.hover_surf = NULL;
	qre.hover_ent = NULL;
	qre.hover_glt = NULL;
	qre.pick_name[0] = '\0';
	qre.hover_light = -1;
	qre.group_count = 0;
	qre.extra_count = 0;
	qre.tmp_appended = false;
	qre.sel_light_valid = false;
	qre.custom_placing = false;
	qre.custom_cloning = false;
	QRE_CustomGizmoCancel ();
	qre.custom_dragging = false;
	QRE_CancelLightDrag ();
	QRE_DtalDebugOff ();
	qre.mat_tab = 0;
	qre.light_tab = 0;

	QRE_FreePreview ();

	QRE_ClearSessionState ();
}

static void QRE_ResetCvar (const char *name)
{
	cvar_t *var = name ? Cvar_FindVar (name) : NULL;

	if (var && var->default_string && !(var->flags & CVAR_ROM))
		Cvar_SetQuick (var, var->default_string);
}

static qboolean QRE_ResetRemoveFile (const char *path)
{
	if (!path[0] || remove (path) == 0 || errno == ENOENT)
		return true;

	QRE_Notify ("cannot remove %s; reset failed", path);
	return false;
}

static void QRE_ResetRemoveOptional (const char *path)
{
	if (path && path[0])
		remove (path);
}

static void QRE_ResetRemoveLegacyMaterials (const char *gamedir)
{
	char            pattern[MAX_OSPATH];
	WIN32_FIND_DATAA fd;
	HANDLE           h;

	q_snprintf (pattern, sizeof (pattern), "%s/materials/*.yaml", gamedir);
	h = FindFirstFileA (pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	do
	{
		char path[MAX_OSPATH];

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		q_snprintf (path, sizeof (path), "%s/materials/%s", gamedir, fd.cFileName);
		remove (path);
	} while (FindNextFileA (h, &fd));

	FindClose (h);
}

static void QRE_ResetAll (void)
{
	const int mode = qre.mode;
	char      legacy[MAX_OSPATH];
	int       i;

	if (!QRE_ResetRemoveFile (qre.editor_file) ||
	    !QRE_ResetRemoveFile (qre.target_file))
		return;

	if (mode == QRE_MODE_LIGHT)
	{
		q_snprintf (legacy, sizeof (legacy), "%s/lights.yaml", com_gamedir);
		if (!QRE_ResetRemoveFile (legacy))
			return;
		q_snprintf (legacy, sizeof (legacy), "%s/qray/lights.yaml", com_gamedir);
		if (!QRE_ResetRemoveFile (legacy))
			return;

		q_snprintf (legacy, sizeof (legacy), "%s/lights.editor.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
		q_snprintf (legacy, sizeof (legacy), "%s/backup_lights.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
		q_snprintf (legacy, sizeof (legacy), "%s/qray/lights.editor.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
		q_snprintf (legacy, sizeof (legacy), "%s/qray/backup_lights.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
	}
	else
	{
		q_snprintf (legacy, sizeof (legacy), "%s/materials.yaml", com_gamedir);
		if (!QRE_ResetRemoveFile (legacy))
			return;

		q_snprintf (legacy, sizeof (legacy), "%s/materials.editor.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
		q_snprintf (legacy, sizeof (legacy), "%s/backup_materials.yaml", com_gamedir);
		QRE_ResetRemoveOptional (legacy);
		QRE_ResetRemoveLegacyMaterials (com_gamedir);
	}

	QRE_BackToChooser ();

	if (mode == QRE_MODE_LIGHT)
	{
		for (i = 0; i < (int)countof (qre_globals); i++)
			QRE_ResetCvar (QRE_GlobalCvarName (&qre_globals[i]));
		QRE_ResetCvar ("rt_sky_sun_edit");
		RT_LIGHT_Reload ();
		RT_CustomLights_ChangeMap (cl.mapname);
		Fog_NewMap ();
	}
	else
	{
		for (i = 0; i < (int)countof (qre_water); i++)
			QRE_ResetCvar (qre_water[i].name);
		for (i = 0; i < (int)countof (qre_mat_cvars); i++)
			QRE_ResetCvar (qre_mat_cvars[i]);
		QRE_ResetCvar ("rt_water_color");
		QRE_ResetCvar ("rt_water_acidcolor");
		RT_MAT_Reload ();
		TexMgr_ReloadAllImages ();
	}

	Atomic_StoreUInt32 (&rt_require_static_submit, true);
	Host_WriteConfiguration ();
	QRE_StartMode (mode);
	QRE_Notify ("all saved %s work and settings reset to defaults", mode == QRE_MODE_LIGHT ? "light" : "material");
}

// "Save" of the exit dialog: the target is backed up first, then the session
// file becomes the target (a mod's qray.materials.yaml / qray.lights.yaml, so
// they override id1's).
static void QRE_SessionSave (void)
{
	const qboolean light = (qre.mode == QRE_MODE_LIGHT) ? true : false;
	const qboolean globals = light ? QRE_GlobalsTouched () : QRE_WaterTouched ();
	const qboolean entries = light ? (qre.light_touched_count > 0 || QRE_CustomTouched ()) : (qre.touched_count > 0);
	const qboolean had_target = QRE_FileExists (qre.target_file);

	if (entries && !QRE_WriteSession ())
	{
		QRE_Notify ("the session could not be written");
		return;
	}

	if (globals)
		Host_WriteConfiguration ();

	if (!QRE_FileExists (qre.editor_file))
	{
		if (globals)
		{
			QRE_BackToChooser ();
			QRE_Notify ("settings written to the config");
		}
		else
		{
			QRE_Notify ("nothing to save");
		}
		return;
	}

	if (had_target && !QRE_CopyFile (qre.target_file, qre.backup_file))
	{
		QRE_Notify ("cannot write %s; the session is kept", qre.backup_file);
		return;
	}
	if (!QRE_CopyFile (qre.editor_file, qre.target_file))
	{
		QRE_Notify ("cannot write %s; the session is kept", qre.target_file);
		return;
	}

	remove (qre.editor_file);

	QRE_BackToChooser ();

	if (light)
		QRE_Notify (had_target ? "qray.lights.yaml saved; qray.backup_lights.yaml holds the previous file"
		                       : "qray.lights.yaml saved");
	else
		QRE_Notify (had_target ? "qray.materials.yaml saved; qray.backup_materials.yaml holds the previous file"
		                       : "qray.materials.yaml saved");

	if (globals)
		QRE_Notify ("settings written to the config");
}

// "Discard": the session files go away and the original values come back on
// screen; nothing on disk is touched.
static void QRE_SessionDiscard (void)
{
	QRE_RestoreModeState ();
	remove (qre.editor_file);
	QRE_BackToChooser ();
	QRE_Notify ("changes discarded");
}

// Exit (button, Esc, the console command): ask about the session when there is
// one, go back to the chooser when nothing was changed (while choosing, Esc and
// the chooser's Exit close the editor).
static void QRE_RequestExit (void)
{
	qboolean touched;

	if (qre.choosing)
	{
		QRE_StopEditor (false);
		return;
	}

	touched = (qre.mode == QRE_MODE_LIGHT) ? QRE_LightSessionTouched () : (qre.touched_count > 0 || QRE_WaterTouched ());

	if (!touched && !QRE_FileExists (qre.editor_file))
	{
		QRE_BackToChooser ();
		return;
	}

	if (!qre.panel_open)
	{
		qre.prompt_from_flying = true;
		QRE_CursorMode (true);
	}
	qre.exit_prompt = true;
}

// ---------------------------------------------------------------------------
// Texture browse (Win32 open dialog)
// ---------------------------------------------------------------------------

static qboolean QRE_BrowseTexture (char *out, size_t outsize)
{
	char          initdir[MAX_OSPATH];
	char          result[MAX_OSPATH];
	OPENFILENAMEA ofn;
	size_t        glen, i;

	q_snprintf (initdir, sizeof (initdir), "%s", com_gamedir);

	memset (&ofn, 0, sizeof (ofn));
	result[0] = '\0';
	ofn.lStructSize = sizeof (ofn);
	ofn.lpstrFilter = "Images (*.png;*.tga;*.jpg;*.jpeg)\0*.png;*.tga;*.jpg;*.jpeg\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = result;
	ofn.nMaxFile = sizeof (result);
	ofn.lpstrInitialDir = initdir;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameA (&ofn))
		return false;

	// The dialog returns backslashes while com_gamedir may carry forward
	// slashes: normalize before comparing, and store forward slashes.
	for (i = 0; result[i]; i++)
	{
		if (result[i] == '\\')
			result[i] = '/';
	}

	glen = strlen (com_gamedir);
	if (!q_strncasecmp (result, com_gamedir, glen) && result[glen] == '/')
	{
		q_strlcpy (out, result + glen + 1, outsize);
	}
	else
	{
		// outside the gamedir the material loader cannot find the file
		QRE_Notify ("the texture must be inside %s", com_gamedir);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void QRE_StopEditor (qboolean restore)
{
	if (!qre.active)
		return;

	if (qre.panel_drawing)
	{
		qre.stop_pending = true;
		qre.stop_pending_restore = restore;
		return;
	}

	// revert whatever was not applied, then restore the player's view
	QRE_CancelLightDrag ();
	QRE_CustomGizmoCancel ();
	QRE_DtalDebugOff ();
	if (restore)
		QRE_RestoreModeState ();

	qre.active = false;
	qre.choosing = false;
	qre.panel_open = false;
	qre.torch = false;
	qre.exit_prompt = false;
	qre.reset_prompt = false;
	qre.reset_pending = false;
	qre.pick_model = NULL;
	qre.pick_surf = NULL;
	qre.pick_ent = NULL;
	qre.pick_glt = NULL;
	qre.hover_model = NULL;
	qre.hover_surf = NULL;
	qre.hover_ent = NULL;
	qre.hover_glt = NULL;

	QRE_FreePreview ();

	// the world runs again: only lift the pause the editor itself set, so a
	// pause toggled meanwhile is left as the player left it; a session the
	// editor did not save goes away with it
	if (sv.paused)
		sv.paused = qre.sv_paused_prev;
	if (!restore)
		remove (qre.editor_file);

	VectorCopy (qre.player_viewangles, cl.viewangles);

	QRE_FreeSnapshot ();
	QRE_FreeLightSnapshot ();

	IN_Activate ();
	SDL_ShowCursor (SDL_ENABLE);
	QR_GUI_SetMouseCursor (0);

	QRE_Notify ("editor closed");
}

static void QRE_StartMode (int mode)
{
	const char *name = (mode == QRE_MODE_LIGHT) ? "light" : "material";

	if (mode == QRE_MODE_LIGHT)
		QRE_DtalDebugOff ();

	qre.choosing = false;
	qre.mode = mode;

	if (mode == QRE_MODE_LIGHT)
	{
		QRE_TakeLightSnapshot ();
		QRE_TakeGlobalsSnapshot ();

		// the light session: the gamedir's qray.lights.yaml holds both the
		// emitter overrides and the custom lights and fog, with the session file
		// carrying the edits until the exit dialog decides and the backup keeping
		// the target as it was before a save
		q_snprintf (qre.target_file, sizeof (qre.target_file), "%s/qray.lights.yaml", com_gamedir);
		q_snprintf (qre.editor_file, sizeof (qre.editor_file), "%s/qray.lights.editor.yaml", com_gamedir);
		q_snprintf (qre.backup_file, sizeof (qre.backup_file), "%s/qray.backup_lights.yaml", com_gamedir);
	}
	else
	{
		QRE_TakeSnapshot ();
		QRE_TakeWaterSnapshot ();

		// the session files: the target is the gamedir's own qray.materials.yaml
		// (for a mod that is the mod's file, which the loader reads after id1's
		// and lets override it), the session file carries the edits until the
		// exit dialog decides, and the backup keeps the target as it was before
		// a save
		q_snprintf (qre.target_file, sizeof (qre.target_file), "%s/qray.materials.yaml", com_gamedir);
		q_snprintf (qre.editor_file, sizeof (qre.editor_file), "%s/qray.materials.editor.yaml", com_gamedir);
		q_snprintf (qre.backup_file, sizeof (qre.backup_file), "%s/qray.backup_materials.yaml", com_gamedir);
	}

	// a session file left by a crash or a map change belongs to a session that
	// is over: it must not be saved by this one
	remove (qre.editor_file);

	Con_Printf ("qr %s editor: on (fly: WASD + mouse; LMB selects a face; ESC returns to the menu)\n", name);
}

static void QRE_StartEditor (void)
{
	if (qre.active)
	{
		Con_Printf ("qr editor: already running\n");
		return;
	}
	if (cls.state != ca_connected || !cl.worldmodel)
	{
		Con_Printf ("qr editor: a level must be loaded first\n");
		return;
	}
	if (!sv.active || svs.maxclients > 1 || cls.demoplayback)
	{
		Con_Printf ("qr editor: single player only (the world has to be frozen)\n");
		return;
	}
	if (CVAR_TO_FLOAT (rt_truelight) != 1.0f)
	{
		Con_Printf ("qr editor: rt_truelight must be 1 (the new light system)\n");
		return;
	}

	memset (&qre, 0, sizeof (qre));
	qre.active = true;
	qre.panel_open = false;
	qre.mode = QRE_MODE_MATERIAL;
	qre.choosing = true;

	VectorCopy (r_refdef.vieworg, qre.cam_origin);
	VectorCopy (cl.viewangles, qre.player_viewangles);

	// freeze the world: the server stops thinking and moving, cl.time stops
	// advancing (CL_ReadFromServer) and the frame time handed to the renderer is
	// the held clock, so poses, textures, particles, the water warp and the
	// clouds all stand still. cl.paused is deliberately not touched: the engine
	// skips V_CalcRefdef -- and with it the editor camera -- while it is set.
	qre.sv_paused_prev = sv.paused;
	sv.paused = true;

	QRE_CursorMode (true);

	Con_Printf ("qr editor: choose the mode (Material Editor / Light Editor)\n");
}

static void QR_Editor_Start_f (void)
{
	QRE_StartEditor ();
}

static void QR_Editor_Stop_f (void)
{
	if (qre.active)
		QRE_RequestExit ();
}

// Verbose reload diagnostics of the live material editor: logs every texture a
// reload reads (with its file offset) and dumps the synthesized albedo/RME/normal
// of the first reload of a material to <gamedir>/qre_dump.
cvar_t qr_material_editor_debug = { "qr_material_editor_debug", "0", CVAR_NONE };

void QR_Editor_Init (void)
{
	static qboolean qr_editor_registered = false;
	int             font_handle = -1;
	int             font_size = 0;
	void           *font_data = NULL;

	if (qr_editor_registered)
		return;
	qr_editor_registered = true;

	Cvar_RegisterVariable (&qr_material_editor_debug);

	Cmd_AddCommand ("qr_editor", QR_Editor_Start_f);
	Cmd_AddCommand ("qr_editor_stop", QR_Editor_Stop_f);

	// the font is part of the game data, next to the cursor artwork
	font_size = COM_OpenFile ("gfx/Roboto-Regular.ttf", &font_handle, NULL);
	if (font_handle != -1 && font_size > 0)
	{
		font_data = malloc ((size_t)font_size);
		if (font_data)
			Sys_FileRead (font_handle, font_data, font_size);
		COM_CloseFile (font_handle);
	}

	QR_GUI_Init (VID_GetWindow (), vulkan_globals.instance, font_data, font_size);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

qboolean QR_Editor_Active (void)
{
	return qre.active;
}

qboolean QR_Editor_PanelOpen (void)
{
	return qre.active && qre.panel_open;
}

qboolean QR_Editor_Flying (void)
{
	return qre.active && !qre.panel_open;
}

void QR_Editor_SunPlacement (qboolean on)
{
	if (!qre.active)
		return;

	QRE_CursorMode (!on);
}

qboolean QR_Editor_TorchOn (void)
{
	return qre.active && qre.torch;
}

void QR_Editor_TorchOrigin (vec3_t out)
{
	vec3_t fwd, right, up;

	AngleVectors (cl.viewangles, fwd, right, up);
	VectorCopy (qre.cam_origin, out);
	VectorMA (out, 24.0f, fwd, out);
	VectorMA (out, 8.0f, up, out);
}

void QR_Editor_Pick (void)
{
	if (!QR_Editor_Flying ())
		return;
	QRE_DoPick (true);
}

// The Custom tab's placement mode: while it is on, the fire button drops the new
// light where the crosshair hits (the last hover pick's hit point, just in front
// of the surface); with nothing under the crosshair it goes a step ahead of the
// camera. Either way the editor comes back to the cursor mode with the new light
// selected.
static int QRE_CustomLightIndexForUnique (uint64_t uniqueID)
{
	if (uniqueID <= (uint64_t)UINT32_MAX)
		return -1;
	return (int)(uniqueID - (uint64_t)UINT32_MAX - 1);
}

static void QRE_UpdateLightDrag (void)
{
	vec3_t target;

	if (!qre.light_dragging)
		return;

	if (qre.pick_impact_frame == (unsigned)host_framecount && (qre.pick_surf || qre.pick_ent))
		VectorMA (qre.pick_impact, -8.0f, vpn, target);
	else
		VectorMA (qre.cam_origin, 128.0f, vpn, target);

	if (qre.light_drag_custom >= 0)
	{
		int                count = 0;
		rt_custom_light_t *custom = RT_CustomLights (&count);

		if (custom && qre.light_drag_custom < count)
			VectorCopy (target, custom[qre.light_drag_custom].origin);
	}
	else if (qre.light_drag_entry)
	{
		VectorSubtract (target, qre.light_drag_emitter, qre.light_drag_entry->offset);
		qre.light_drag_entry->has_offset = true;
	}
}

static void QRE_CancelLightDrag (void)
{
	if (!qre.light_dragging)
		return;

	if (qre.light_drag_custom >= 0)
	{
		int                count = 0;
		rt_custom_light_t *custom = RT_CustomLights (&count);

		if (custom && qre.light_drag_custom < count)
			VectorCopy (qre.light_drag_custom_origin, custom[qre.light_drag_custom].origin);
	}
	else if (qre.light_drag_entry)
	{
		if (qre.light_drag_created)
			RT_LIGHT_Remove (qre.light_drag_key);
		else if (qre.light_drag_backup_valid)
			*qre.light_drag_entry = qre.light_drag_backup;
	}

	qre.light_dragging = false;
	qre.light_drag_entry = NULL;
}

qboolean QR_Editor_LightDragActive (void)
{
	return (qre.active && qre.light_dragging) ? true : false;
}

void QR_Editor_LightGrab (void)
{
	const rt_tracked_light_t *lights;
	int                       count = 0, index;

	if (!qre.active || !QR_Editor_Flying () || qre.light_dragging)
		return;

	index = QRE_LightUnderCrosshair ();
	if (index < 0)
		return;

	lights = RT_TRACK_Lights (&count);
	if (index >= count || !lights[index].ready)
		return;

	qre.light_drag_kind = lights[index].kind;
	q_strlcpy (qre.light_drag_name, lights[index].name, sizeof (qre.light_drag_name));
	qre.light_drag_custom = (lights[index].kind == RT_LIGHT_KIND_CUSTOM)
	                            ? QRE_CustomLightIndexForUnique (lights[index].uniqueID)
	                            : -1;
	qre.light_drag_entry = NULL;
	qre.light_drag_created = false;
	qre.light_drag_backup_valid = false;

	if (qre.light_drag_custom >= 0)
	{
		rt_custom_light_t *custom = RT_CustomLights (&count);

		if (!custom || qre.light_drag_custom >= count)
			return;

		VectorCopy (custom[qre.light_drag_custom].origin, qre.light_drag_custom_origin);
	}
	else
	{
		rt_light_t *inst;

		RT_LIGHT_MakeKey (lights[index].name, lights[index].uniqueID, qre.light_drag_key, sizeof (qre.light_drag_key));

		inst = RT_LIGHT_Find (qre.light_drag_key);
		if (inst)
		{
			qre.light_drag_backup = *inst;
			qre.light_drag_backup_valid = true;
		}
		else
		{
			rt_light_t *shared = RT_LIGHT_Ensure (lights[index].name);

			if (!shared)
				return;

			inst = RT_LIGHT_Ensure (qre.light_drag_key);
			if (!inst)
				return;

			*inst = *shared;
			qre.light_drag_created = true;
		}

		inst->group_edit = false;
		qre.light_drag_entry = inst;

		if (inst->has_offset)
		{
			VectorSubtract (lights[index].position, inst->offset, qre.light_drag_emitter);
		}
		else
		{
			VectorCopy (lights[index].position, qre.light_drag_emitter);
		}
	}

	qre.light_dragging = true;
}

void QR_Editor_LightDrop (void)
{
	if (!qre.active || !qre.light_dragging)
		return;

	QRE_UpdateLightDrag ();

	if (qre.light_drag_custom < 0 && qre.light_drag_entry)
		QRE_TouchLight (qre.light_drag_key);

	qre.light_dragging = false;
	qre.light_drag_entry = NULL;
	QRE_Notify ("light moved");
}

static void QRE_CustomGizmoCancel (void)
{
	int                count = 0;
	rt_custom_light_t *lights;

	if (!qre.custom_dragging || qre.custom_drag_index < 0)
		return;

	lights = RT_CustomLights (&count);
	if (lights && qre.custom_drag_index < count)
		VectorCopy (qre.custom_drag_origin, lights[qre.custom_drag_index].origin);

	qre.custom_dragging = false;
	qre.gizmo_fly_drag = false;
}

qboolean QR_Editor_GizmoDragActive (void)
{
	return (qre.active && qre.custom_dragging && qre.gizmo_fly_drag) ? true : false;
}

qboolean QR_Editor_GizmoPress (void)
{
	if (!qre.active || !QR_Editor_Flying () || qre.custom_dragging)
		return false;

	if (!QRE_CustomGizmoBegin ((float)glwidth * 0.5f, (float)glheight * 0.5f))
		return false;

	qre.gizmo_fly_drag = true;
	qre.gizmo_fly_mouse[0] = (float)glwidth * 0.5f;
	qre.gizmo_fly_mouse[1] = (float)glheight * 0.5f;

	{
		qre.gizmo_fly_anchor_local[0] = qre.gizmo_fly_anchor_local[1] = qre.gizmo_fly_anchor_local[2] = 0.0f;

		if (!qre.custom_drag_dir)
		{
			int                count = 0;
			rt_custom_light_t *custom = RT_CustomLights (&count);

			if (custom && qre.custom_drag_index >= 0 && qre.custom_drag_index < count)
			{
				vec3_t origin, w0, e, anchor;
				float  b, d, ee, denom, t;

				QRE_GizmoOrigin (&custom[qre.custom_drag_index], origin);
				VectorCopy (qre.custom_drag_vector, e);
				VectorSubtract (r_refdef.vieworg, origin, w0);
				b = DotProduct (vpn, e);
				d = DotProduct (vpn, w0);
				ee = DotProduct (e, w0);
				denom = 1.0f - b * b;
				t = (denom > 1e-4f) ? (ee - b * d) / denom : 0.0f;
				VectorMA (origin, t, e, anchor);
				VectorSubtract (anchor, origin, qre.gizmo_fly_anchor_local);
			}
		}
	}

	return true;
}

static void QRE_GizmoFlyFollow (void)
{
	int                count = 0;
	rt_custom_light_t *lights;
	rt_custom_light_t *l;
	vec3_t             origin, anchor, dir, angles;

	if (qre.custom_drag_dir || qre.custom_drag_index < 0)
		return;

	lights = RT_CustomLights (&count);
	if (qre.custom_drag_index >= count)
		return;

	l = &lights[qre.custom_drag_index];
	QRE_GizmoOrigin (l, origin);
	VectorAdd (origin, qre.gizmo_fly_anchor_local, anchor);
	VectorSubtract (anchor, r_refdef.vieworg, dir);

	if (VectorLength (dir) < 1.0f)
		return;

	VectorAngles (dir, NULL, angles);
	cl.viewangles[YAW] = angles[YAW];
	cl.viewangles[PITCH] = angles[PITCH];
}

void QR_Editor_GizmoMotion (int dx, int dy)
{
	if (!QR_Editor_GizmoDragActive ())
		return;

	qre.gizmo_fly_mouse[0] += (float)dx;
	qre.gizmo_fly_mouse[1] += (float)dy;
	QRE_CustomGizmoMove (qre.gizmo_fly_mouse[0], qre.gizmo_fly_mouse[1]);
	QRE_GizmoFlyFollow ();
}

void QR_Editor_GizmoRelease (void)
{
	if (!qre.custom_dragging)
		return;

	qre.custom_dragging = false;
	qre.gizmo_fly_drag = false;
}

void QR_Editor_PlaceAtCrosshair (void)
{
	rt_custom_light_t *l;
	const qboolean     cloning = qre.custom_cloning;

	if (!qre.active || (!qre.custom_placing && !qre.custom_cloning))
		return;

	l = RT_CustomLights_Ensure ();
	if (!l)
	{
		QRE_Notify ("no room for another custom light");
		qre.custom_placing = false;
		qre.custom_cloning = false;
		return;
	}

	if (cloning)
		*l = qre.clone_source;

	if (qre.pick_impact_frame == (unsigned)host_framecount && (qre.pick_surf || qre.pick_ent))
		VectorMA (qre.pick_impact, -8.0f, vpn, l->origin);
	else
		VectorMA (qre.cam_origin, 128.0f, vpn, l->origin);

	// the new light is the editor's selection: its row is open and the axis
	// arrows sit on it right away
	{
		int count = 0;

		(void)RT_CustomLights (&count);
		QRE_SelectCustomLight (count - 1);
	}

	qre.custom_placing = false;
	qre.custom_cloning = false;
	qre.light_tab = 1;
	QRE_CursorMode (true);
	QRE_Notify (cloning ? "light cloned; tune it in the Custom tab" : "light added; tune it in the Custom tab");
}

qboolean QR_Editor_PlacePending (void)
{
	return (qre.active && (qre.custom_placing || qre.custom_cloning)) ? true : false;
}

void QR_Editor_OnNewMap (void)
{
	if (qre.active)
	{
		Con_Printf ("qr editor: closed by a map change\n");
		QRE_StopEditor (false);
	}
}

void QR_Editor_Shutdown (void)
{
	// The host is going down without a session: the editor's own buffers (the
	// material snapshots and the preview pixels and their materials) go back
	// here, since QRE_StopEditor -- the usual owner of that teardown -- also
	// re-applies the touched materials and the renderer is what is leaving.
	if (qre.active)
	{
		QRE_FreePreview ();
		QRE_FreeSnapshot ();
		QRE_FreeLightSnapshot ();
		qre.active = false;
		qre.panel_open = false;
		qre.torch = false;
	}

	QR_GUI_Shutdown ();
}
