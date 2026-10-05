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

#include "quakedef.h"
#include "bgmusic.h"
#include "snd_openal.h"
#include "snd_eq.h"
#include <stdbool.h>

void (*vid_menucmdfn) (void); // johnfitz
void (*vid_menukeyfn) (int key);
void (*vid_menudrawfn) (cb_context_t *cbx);

enum m_state_e m_state;

static void M_Menu_SinglePlayer_f (void);
static void M_Menu_Load_f (void);
static void M_Menu_Save_f (void);
static void M_Menu_MultiPlayer_f (void);
static void M_Menu_Setup_f (void);
static void M_Menu_Net_f (void);
static void M_Menu_LanConfig_f (void);
static void M_Menu_MPGameOptions_f (void);
static void M_Menu_Search_f (enum slistScope_e scope);
static void M_Menu_ServerList_f (void);
static void M_Menu_Keys_f (void);
static void M_Menu_Help_f (void);
static void M_Menu_Mods_f (void);
static void M_Menu_Maps_f (void);
static void M_Menu_Skill_f (void);

static void M_Main_Draw (cb_context_t *cbx);
static void M_SinglePlayer_Draw (cb_context_t *cbx);
static void M_Load_Draw (cb_context_t *cbx);
static void M_Save_Draw (cb_context_t *cbx);
static void M_MultiPlayer_Draw (cb_context_t *cbx);
static void M_Setup_Draw (cb_context_t *cbx);
static void M_Net_Draw (cb_context_t *cbx);
static void M_LanConfig_Draw (cb_context_t *cbx);
static void M_MPGameOptions_Draw (cb_context_t *cbx);
static void M_Search_Draw (cb_context_t *cbx);
static void M_ServerList_Draw (cb_context_t *cbx);
static void M_Options_Draw (cb_context_t *cbx);
static void M_Mods_Draw (cb_context_t *cbx);
static void M_Maps_Draw (cb_context_t *cbx);
static void M_Skill_Draw (cb_context_t *cbx);
static void M_Keys_Draw (cb_context_t *cbx);
static void M_Help_Draw (cb_context_t *cbx);
static void M_Quit_Draw (cb_context_t *cbx);

static void M_Main_Key (int key);
static void M_SinglePlayer_Key (int key);
static void M_Load_Key (int key);
static void M_Save_Key (int key);
static void M_MultiPlayer_Key (int key);
static void M_Setup_Key (int key);
static void M_Net_Key (int key);
static void M_LanConfig_Key (int key);
static void M_MPGameOptions_Key (int key);
static void M_Search_Key (int key);
static void M_ServerList_Key (int key);
static void M_Options_Key (int key);
static void M_Keys_Key (int key);
static void M_Help_Key (int key);
static void M_Mods_Key (int key);
static void M_Maps_Key (int key);
static void M_Skill_Key (int key);
static void M_Quit_Key (int key);

qboolean		m_entersound; // play after drawing a frame, so caching
							  // won't disrupt the sound
static qboolean m_recursiveDraw;

qboolean m_is_quitting = false; // prevents SDL_StartTextInput during quit

enum m_state_e m_return_state;
qboolean	   m_return_onerror;
char		   m_return_reason[32];

#define StartingGame (m_multiplayer_cursor == 1)
#define JoiningGame	 (m_multiplayer_cursor == 0)
#define IPXConfig	 (m_net_cursor == 0)
#define TCPIPConfig	 (m_net_cursor == 1)

static int			  m_main_cursor;
static qboolean		  m_mouse_moved;
static enum m_state_e m_mouse_hover_state = m_none;
static int			 *m_mouse_hover_cursor;
static int			  m_mouse_hover_value;
static qboolean		  menu_changed;
static int			  m_mouse_x = -1;
static int			  m_mouse_y = -1;
static int			  m_mouse_x_pixels = -1;
static int			  m_mouse_y_pixels = -1;

static int scrollbar_x;
static int scrollbar_y;
static int scrollbar_size;

cvar_t ui_live_preview = {"ui_live_preview", "1", CVAR_ARCHIVE};

typedef enum
{
	PREVIEW_WORLD,
	PREVIEW_CENTERPRINT,
	PREVIEW_UNDERWATER
} preview_kind_t;

static struct
{
	enum m_state_e menu;
	int			   row;
	preview_kind_t kind;
	float		   fraction, target, hold;
} menu_preview;

static void M_UpdatePreview (void);

float M_MenuPreviewFraction (void)
{
	if (!ui_live_preview.value || key_dest != key_menu || m_state != menu_preview.menu)
		return 0.0f;
	return menu_preview.fraction;
}

qboolean M_ForcedUnderwater (void)
{
	return M_MenuPreviewFraction () > 0.0f && menu_preview.kind == PREVIEW_UNDERWATER;
}

static void M_PreviewRow (cb_context_t *cbx, int row, int y)
{
	float fraction = M_MenuPreviewFraction ();
	if (row == menu_preview.row)
	{
		Draw_SetOpacity (1.0f);
		if (fraction > 0.0f)
			Draw_Fill (cbx, MENU_CURSOR_X - 4, y - 4, 320 - MENU_CURSOR_X, CHARACTER_SIZE + 8, 0, 0.5f * fraction);
	}
	else
		Draw_SetOpacity (1.0f - fraction);
}

static void M_BeginPreview (int row, preview_kind_t kind)
{
	if (!ui_live_preview.value || cls.state != ca_connected || cls.signon != SIGNONS || (kind == PREVIEW_CENTERPRINT && cl.intermission))
		return;
	menu_preview.menu = m_state;
	menu_preview.row = row;
	menu_preview.kind = kind;
	menu_preview.target = 1.0f;
	menu_preview.hold = kind == PREVIEW_UNDERWATER ? 2.25f : 1.25f;
}

cvar_t ui_mouse = {"ui_mouse", "1", CVAR_ARCHIVE};

void		M_ConfigureNetSubsystem (void);
static void M_SetSkillMenuMap (const char *name);

extern qboolean keydown[256];

extern cvar_t scr_fov;
extern cvar_t scr_showfps;
extern cvar_t r_rtshadows;
extern cvar_t r_particles;
extern cvar_t rt_hud_minimal;
extern cvar_t r_smoke;
extern cvar_t ui_cursor;
extern cvar_t r_softparticles;
extern cvar_t r_oit;
extern cvar_t r_enhancedmodels;
extern cvar_t r_lerpmodels;
extern cvar_t r_enhancedmodels;
extern cvar_t r_lerpmove;
extern cvar_t r_lerpturn;
extern cvar_t vid_filter;
extern cvar_t rt_bloom_intensity;
extern cvar_t rt_bloom_quality;
extern cvar_t rt_bloom_threshold;
extern cvar_t rt_dof_near;
extern cvar_t rt_tonemap_power;
extern cvar_t rt_tonemap;
extern cvar_t rt_exposure_bias;
extern cvar_t rt_ef_damage_strength;
extern cvar_t rt_ef_liquid_strength;
extern cvar_t rt_sharpen_strength;
extern cvar_t rt_sharpen;
extern cvar_t rt_vignette;
extern cvar_t rt_filmgrain;
extern cvar_t rt_local_exposure;
extern cvar_t rt_sky_godrays;
extern cvar_t rt_sky_godrays_quality;
extern cvar_t rt_sky_sun_size;
extern cvar_t rt_sky_clouds;
extern cvar_t rt_sky_clouds_quality;
extern cvar_t rt_gi_level;
extern cvar_t rt_truelight;
extern cvar_t rt_reflrefr_depth;
extern cvar_t scr_guifilter;
extern cvar_t vid_palettize;
extern cvar_t vid_anisotropic;
extern cvar_t vid_fsaa;
extern cvar_t vid_fsaamode;
extern cvar_t host_maxfps;
extern cvar_t cl_bob;
extern cvar_t cl_rollangle;
extern cvar_t v_gunkick;
extern cvar_t crosshair;
extern cvar_t crosshair_def;
extern cvar_t crosshair_size;
extern cvar_t crosshair_color;
extern cvar_t crosshair_alpha;

static qboolean slider_grab;
static qboolean scrollbar_grab;

// clang-format off
// crosshair_definitions
static const crosshair_t crosshair_defs[] = 
{
	{"Modern 020", 0, 0, 0, 0, 0, "gfx/crosshair-020.png"},
	{"Modern 000", 0, 0, 0, 0, 0, "gfx/crosshair-000.png"},
	{"Modern 001", 0, 0, 0, 0, 0, "gfx/crosshair-001.png"},
	{"Modern 021", 0, 0, 0, 0, 0, "gfx/crosshair-021.png"},
	{"Modern 148", 0, 0, 0, 0, 0, "gfx/crosshair-148.png"},
	{"Classic +", '+', - CHARACTER_SIZE * 0.5f, - CHARACTER_SIZE * 0.5f, 0, 0, NULL},
	{"Classic dot", '.', - CHARACTER_SIZE * 0.5f + 1.75f, - CHARACTER_SIZE * 0.5f - 1.5f, 2, -1, NULL},
	{"Classic x", 'x', - CHARACTER_SIZE * 0.5f, - CHARACTER_SIZE * 0.5f, 0, 0, NULL},
	{"Classic ring", 'o', - CHARACTER_SIZE * 0.5f, - CHARACTER_SIZE * 0.5f, 0, 0, NULL},
	{"Classic up", '^', - CHARACTER_SIZE * 0.5f, - CHARACTER_SIZE * 0.5f + 5.0f, 0, 2, NULL},
	{"Classic down", 'v', - CHARACTER_SIZE * 0.5f - .65f, - CHARACTER_SIZE * 0.5f + .75f, 0, 0, NULL}
};

static const size_t num_crosshair_defs = countof (crosshair_defs);
// clang-format on

/*
================
M_GetCrosshairDef
================
*/
crosshair_t M_GetCrosshairDef (float crosshair_def_value)
{
	return crosshair_defs[CLAMP (0, (int)crosshair_def_value, (int)num_crosshair_defs - 1)];
}

static const char *const crosshair_color_names[] = {"White", "Gray", "Green", "Cyan", "Yellow", "Red", "Magenta"};
static const float		 crosshair_colors[][3] = {{1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.7f, 0.25f}, {0.2f, 0.7f, 0.7f},
												  {0.7f, 0.7f, 0.2f}, {0.7f, 0.2f, 0.2f}, {0.7f, 0.2f, 0.7f}};

const char *M_GetCrosshairColorName (float crosshair_color_value)
{
	return crosshair_color_names[CLAMP (0, (int)crosshair_color_value, (int)countof (crosshair_color_names) - 1)];
}

void M_GetCrosshairColor (float crosshair_color_value, float *rgb)
{
	memcpy (rgb, crosshair_colors[CLAMP (0, (int)crosshair_color_value, (int)countof (crosshair_colors) - 1)], sizeof (crosshair_colors[0]));
}

void M_DrawCrosshair (cb_context_t *cbx, float x, float y, float size)
{
	crosshair_t current = M_GetCrosshairDef (crosshair_def.value);
	const float alpha = CLAMP (0.0f, crosshair_alpha.value, 1.0f);
	float		rgb[3];
	M_GetCrosshairColor (crosshair_color.value, rgb);

	if (current.pic_path)
	{
		qpic_t *pic = Draw_TryCachePic (current.pic_path, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_MIPMAP);
		if (!pic)
		{
			GL_SetCanvasColor (rgb[0], rgb[1], rgb[2], alpha);
			Draw_Character (cbx, x + current.viewport_x_offset, y + current.viewport_y_offset, current.crosshair_char);
			GL_SetCanvasColor (1.0f, 1.0f, 1.0f, 1.0f);
			return;
		}
		const float draw_size = size * pic->width / 128.0f;
		Draw_SubPicLinearBlend (cbx, x - draw_size * 0.5f, y - draw_size * 0.5f, draw_size, draw_size, pic, 0, 0, 1, 1, rgb, alpha);
	}
	else
	{
		GL_SetCanvasColor (rgb[0], rgb[1], rgb[2], alpha);
		Draw_Character (cbx, x + current.viewport_x_offset, y + current.viewport_y_offset, current.crosshair_char);
		GL_SetCanvasColor (1.0f, 1.0f, 1.0f, 1.0f);
	}
}

/*
================
M_GetScale
================
*/
float M_GetScale ()
{
	static float latched_menuscale;
	if (!slider_grab)
		latched_menuscale = scr_menuscale.value;
	return latched_menuscale;
}

/*
================
M_PixelToMenuCanvasCoord
================
*/
static void M_PixelToMenuCanvasCoord (int *x, int *y)
{
	float s = q_min ((float)glwidth / 320.0, (float)glheight / 200.0);
	s = CLAMP (1.0, M_GetScale (), s);
	*x = (*x - (glwidth - 320 * s) / 2) / s;
	*y = (*y - (glheight - 200 * s) / 2) / s;
}

#define MENU_OPTION_STRING (str)

/*
================
M_PrintHighlighted
================
*/
static void M_PrintHighlighted (cb_context_t *cbx, int cx, int cy, const char *str)
{
	while (*str)
	{
		Draw_Character (cbx, cx, cy, (*str));
		str++;
		cx += CHARACTER_SIZE;
	}
}

/*
================
M_Print
================
*/
void M_DrawCharacter (cb_context_t *cbx, int cx, int line, int num)
{
	Draw_Character (cbx, cx, line, num);
}

void M_Print (cb_context_t *cbx, int cx, int cy, const char *str)
{
	while (*str)
	{
		Draw_Character (cbx, cx, cy, (*str) + 128);
		str++;
		cx += CHARACTER_SIZE;
	}
}

/*
================
M_PrintElided
================
*/
void M_PrintElided (cb_context_t *cbx, int cx, int cy, const char *str, const int max_length)
{
	int i = 0;
	while (str[i] && i < max_length)
	{
		Draw_Character (cbx, cx, cy, str[i] + 128);
		++i;
		cx += CHARACTER_SIZE;
	}
	if (str[i] != 0)
	{
		for (int j = 0; j < 3; ++j)
		{
			Draw_Character (cbx, cx, cy, '.' + 128);
			cx += CHARACTER_SIZE / 2;
		}
	}
}

/*
================
M_PrintWhite
================
*/
void M_PrintWhite (cb_context_t *cbx, int cx, int cy, const char *str)
{
	while (*str)
	{
		Draw_Character (cbx, cx, cy, *str);
		str++;
		cx += CHARACTER_SIZE;
	}
}

/*
================
M_DrawTransPic
================
*/
void M_DrawTransPic (cb_context_t *cbx, int x, int y, qpic_t *pic)
{
	Draw_Pic (cbx, x, y, pic, 1.0f, false);
}

/*
================
M_DrawPic
================
*/
void M_DrawPic (cb_context_t *cbx, int x, int y, qpic_t *pic)
{
	Draw_Pic (cbx, x, y, pic, 1.0f, false);
}

/*
================
M_DrawTransPicTranslate
================
*/
static void M_DrawTransPicTranslate (cb_context_t *cbx, int x, int y, qpic_t *pic, int top, int bottom) // johnfitz -- more parameters
{
	Draw_TransPicTranslate (cbx, x, y, pic, top, bottom); // johnfitz -- simplified becuase centering is handled elsewhere
}

/*
================
M_DrawTextBox
================
*/
void M_DrawTextBoxAlpha (cb_context_t *cbx, int x, int y, int width, int lines, float alpha)
{
	qpic_t *p;
	int		cx, cy;
	int		n;

	// draw left side
	cx = x;
	cy = y;
	p = Draw_CachePic ("gfx/box_tl.lmp");
	Draw_Pic (cbx, cx, cy, p, alpha, true);
	p = Draw_CachePic ("gfx/box_ml.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		Draw_Pic (cbx, cx, cy, p, alpha, true);
	}
	p = Draw_CachePic ("gfx/box_bl.lmp");
	Draw_Pic (cbx, cx, cy + 8, p, alpha, true);

	// draw middle
	cx += 8;
	while (width > 0)
	{
		cy = y;
		p = Draw_CachePic ("gfx/box_tm.lmp");
		Draw_Pic (cbx, cx, cy, p, alpha, true);
		p = Draw_CachePic ("gfx/box_mm.lmp");
		for (n = 0; n < lines; n++)
		{
			cy += 8;
			if (n == 1)
				p = Draw_CachePic ("gfx/box_mm2.lmp");
			Draw_Pic (cbx, cx, cy, p, alpha, true);
		}
		p = Draw_CachePic ("gfx/box_bm.lmp");
		Draw_Pic (cbx, cx, cy + 8, p, alpha, true);
		width -= 2;
		cx += 16;
	}

	// draw right side
	cy = y;
	p = Draw_CachePic ("gfx/box_tr.lmp");
	Draw_Pic (cbx, cx, cy, p, alpha, true);
	p = Draw_CachePic ("gfx/box_mr.lmp");
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		Draw_Pic (cbx, cx, cy, p, alpha, true);
	}
	p = Draw_CachePic ("gfx/box_br.lmp");
	Draw_Pic (cbx, cx, cy + 8, p, alpha, true);
}

static void M_DrawTextBox (cb_context_t *cbx, int x, int y, int width, int lines)
{
	M_DrawTextBoxAlpha (cbx, x, y, width, lines, 1.0f);
}

/*
================
M_MenuChanged
================
*/
void M_MenuChanged ()
{
	m_entersound = true;
	m_mouse_hover_state = m_none;
	menu_changed = true;
}

/*
================
M_DrawSlider
================
*/
static void M_DrawSlider (cb_context_t *cbx, int x, int y, float value, const char *label)
{
	value = CLAMP (0.0f, value, 1.0f);
	Draw_Character (cbx, x - CHARACTER_SIZE, y, 128);

	for (int i = 0; i < MENU_SLIDER_SIZE; i++)
		Draw_Character (cbx, x + i * CHARACTER_SIZE, y, 129);

	Draw_Character (cbx, x + MENU_SLIDER_SIZE * CHARACTER_SIZE, y, 130);
	Draw_Character (cbx, x + (MENU_SLIDER_SIZE - 1) * CHARACTER_SIZE * value, y, 131);

	M_Print (cbx, x + (MENU_SLIDER_SIZE + 1) * CHARACTER_SIZE, y, label);
}

/*
================
M_GetSliderPos
================
*/
static float
M_GetSliderPos (float low, float high, float current, qboolean backward, qboolean mouse, float clamped_mouse, int dir, float step, float snap_start)
{
	float f;

	if (mouse)
	{
		if (backward)
			f = high + (low - high) * (clamped_mouse - MENU_SLIDER_START) / MENU_SLIDER_EXTENT;
		else
			f = low + (high - low) * (clamped_mouse - MENU_SLIDER_START) / MENU_SLIDER_EXTENT;
	}
	else
	{
		if (backward)
			f = current - dir * step;
		else
			f = current + dir * step;
	}
	if (!mouse || f > snap_start)
		f = (int)(f / step + 0.5f) * step;
	if (f < low)
		f = low;
	else if (f > high)
		f = high;

	return f;
}

/*
================
M_DrawScrollbar
================
*/
static void M_DrawScrollbar (cb_context_t *cbx, int x, int y, float value, float size)
{
	scrollbar_x = x;
	scrollbar_y = y - 8;
	scrollbar_size = (size + 2) * CHARACTER_SIZE;
	value = CLAMP (0.0f, value, 1.0f);
	Draw_Character (cbx, x, y - CHARACTER_SIZE, 128 + 256);
	for (int i = 0; i < size; i++)
		Draw_Character (cbx, x, y + i * CHARACTER_SIZE, 129 + 256);
	Draw_Character (cbx, x, y + size * CHARACTER_SIZE, 130 + 256);
	Draw_Character (cbx, x, y + (size - 1) * CHARACTER_SIZE * value, 131 + 256);
}

/*
================
M_DrawCheckbox
================
*/
void M_DrawCheckbox (cb_context_t *cbx, int x, int y, int on)
{
	if (on)
		M_Print (cbx, x, y, "on");
	else
		M_Print (cbx, x, y, "off");
}

//=============================================================================

int m_save_demonum;

/*
================
M_ToggleMenu_f
================
*/
void M_ToggleMenu_f (void)
{
	M_MenuChanged ();

	if (key_dest == key_menu)
	{
		if (m_state != m_main)
		{
			M_Menu_Main_f ();
			return;
		}

		IN_Activate ();
		key_dest = key_game;
		m_state = m_none;
		return;
	}
	if (key_dest == key_console)
	{
		Con_ToggleConsole_f ();
	}
	else
	{
		M_Menu_Main_f ();
	}
}

/*
================
M_InScrollbar
================
*/
static qboolean M_InScrollbar ()
{
	return scrollbar_grab || (scrollbar_size && (m_mouse_x >= scrollbar_x) && (m_mouse_x <= (scrollbar_x + 8)) && (m_mouse_y >= scrollbar_y) &&
							  (m_mouse_y <= (scrollbar_y + scrollbar_size)) && (m_mouse_y <= (scrollbar_y + scrollbar_size)));
}

/*
================
M_HandleScrollBarKeys
================
*/
qboolean M_HandleScrollBarKeys (const int key, int *cursor, int *first_drawn, const int num_total, const int max_on_screen)
{
	const int prev_cursor = *cursor;
	qboolean  handled_mouse = false;

	if (num_total == 0)
	{
		*cursor = 0;
		*first_drawn = 0;
		return false;
	}

	switch (key)
	{
	case K_MOUSE1:
		if (M_InScrollbar () && ((num_total - max_on_screen) > 0) && !slider_grab)
		{
			handled_mouse = true;
			scrollbar_grab = true;
			int clamped_mouse = CLAMP (scrollbar_y + 8, m_mouse_y, scrollbar_y + scrollbar_size - 8);
			*first_drawn = ((float)clamped_mouse - scrollbar_y - 8) / (scrollbar_size - 16) * (num_total - max_on_screen) + 0.5f;
			if (*cursor < *first_drawn)
				*cursor = *first_drawn;
			else if (*cursor >= *first_drawn + max_on_screen)
				*cursor = *first_drawn + max_on_screen - 1;
		}
		break;

	case K_HOME:
		*cursor = 0;
		*first_drawn = 0;
		break;

	case K_END:
		*cursor = num_total - 1;
		*first_drawn = num_total - max_on_screen;
		break;

	case K_PGUP:
		*cursor = q_max (0, *cursor - max_on_screen);
		*first_drawn = q_max (0, *first_drawn - max_on_screen);
		break;

	case K_PGDN:
		*cursor = q_min (num_total - 1, *cursor + max_on_screen);
		*first_drawn = q_min (*first_drawn + max_on_screen, num_total - max_on_screen);
		break;

	case K_UPARROW:
		if (*cursor == 0)
			*cursor = num_total - 1;
		else
			--*cursor;
		break;

	case K_DOWNARROW:
		if (*cursor == num_total - 1)
			*cursor = 0;
		else
			++*cursor;
		break;

	case K_MWHEELUP:
		*first_drawn = q_max (0, *first_drawn - 1);
		*cursor = q_min (*cursor, *first_drawn + max_on_screen - 1);
		break;

	case K_MWHEELDOWN:
		*first_drawn = q_min (*first_drawn + 1, num_total - max_on_screen);
		*cursor = q_max (*cursor, *first_drawn);
		break;
	}

	if (*cursor != prev_cursor)
		S_LocalSound ("misc/menu1.wav");

	if (num_total <= max_on_screen)
		*first_drawn = 0;
	else
		*first_drawn = CLAMP (*cursor - max_on_screen + 1, *first_drawn, *cursor);

	return handled_mouse;
}

/*
================
M_Mouse_InRect
================
*/
static qboolean M_Mouse_InRect (int left, int right, int top, int bottom)
{
	return m_mouse_x >= left && m_mouse_x <= right && m_mouse_y >= top && m_mouse_y <= bottom;
}

/*
================
M_Mouse_UpdateListCursor
================
*/
static void M_Mouse_UpdateListCursor (int *cursor, int left, int right, int top, int item_height, int num_items, int scroll_offset)
{
	if (!scrollbar_grab && !slider_grab && num_items > 0 && M_Mouse_InRect (left, right, top, top + item_height * num_items))
	{
		m_mouse_hover_state = m_state;
		m_mouse_hover_cursor = cursor;
		m_mouse_hover_value = scroll_offset + CLAMP (0, (m_mouse_y - top) / item_height, num_items - 1);
		if (m_mouse_moved)
			*cursor = m_mouse_hover_value;
	}
}

/*
================
M_Mouse_UpdateCursor
================
*/
void M_Mouse_UpdateCursor (int *cursor, int left, int right, int top, int item_height, int index)
{
	if (M_Mouse_InRect (left, right, top, top + item_height))
	{
		m_mouse_hover_state = m_state;
		m_mouse_hover_cursor = cursor;
		m_mouse_hover_value = index;
		if (m_mouse_moved)
			*cursor = index;
	}
}

//=============================================================================
/* MAIN MENU */

#define MAIN_ITEMS 5

void M_Menu_Main_f (void)
{
	M_MenuChanged ();
	if (key_dest != key_menu)
	{
		m_save_demonum = cls.demonum;
		cls.demonum = -1;
	}
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_main;
}

static qpic_t *Get_Menu2 ()
{
	qboolean base_game = COM_GetGameNames (false)[0] == 0;
	// Check if user has actually installed vkquake.pak, otherwise fall back to old menu
	return (base_game && registered.value) ? Draw_TryCachePic ("gfx/mainmenu2.lmp", TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP) : NULL;
}

void M_Main_Draw (cb_context_t *cbx)
{
	int		f;
	qpic_t *p;
	qpic_t *menu2 = Get_Menu2 ();
	int		main_items = MAIN_ITEMS + (menu2 ? 1 : 0);

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/ttl_main.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_DrawTransPic (cbx, 72, 32, menu2 ? menu2 : Draw_CachePic ("gfx/mainmenu.lmp"));

	f = (int)(realtime * 10) % 6;

	M_Mouse_UpdateListCursor (&m_main_cursor, 70, 320, 32, 20, main_items, 0);
	M_DrawTransPic (cbx, 54, 32 + m_main_cursor * 20, Draw_CachePic (va ("gfx/menudot%i.lmp", f + 1)));
}

void M_Main_Key (int key)
{
	qpic_t *menu2 = Get_Menu2 ();

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		IN_Activate ();
		key_dest = key_game;
		m_state = m_none;
		cls.demonum = m_save_demonum;
		if (!cl_startdemos.value) /* QuakeSpasm customization: */
			break;
		if (cls.demonum != -1 && !cls.demoplayback && cls.state != ca_connected)
			CL_NextDemo ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_main_cursor >= (MAIN_ITEMS + (menu2 ? 1 : 0)))
			m_main_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_main_cursor < 0)
			m_main_cursor = (MAIN_ITEMS + (menu2 ? 1 : 0)) - 1;
		break;

	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
	case K_MOUSE1:
		switch (m_main_cursor)
		{
		case 0:
			M_Menu_SinglePlayer_f ();
			break;

		case 1:
			M_Menu_MultiPlayer_f ();
			break;

		case 2:
			M_Menu_Options_f ();
			break;

		case 3:
			M_Menu_Help_f ();
			break;

		case 4:
			if (menu2)
				M_Menu_Mods_f ();
			else
				M_Menu_Quit_f ();
			break;
		case 5:
			M_Menu_Quit_f ();
			break;
		}
	}
}

typedef struct
{
	double scroll_time;
	double scroll_wait_time;
} menuticker_t;

static void M_Ticker_Init (menuticker_t *ticker)
{
	ticker->scroll_time = 0.0;
	ticker->scroll_wait_time = 1.0;
}

static void M_Ticker_Update (menuticker_t *ticker)
{
	if (ticker->scroll_wait_time <= 0.0)
		ticker->scroll_time += host_rawframetime;
	else
		ticker->scroll_wait_time = q_max (0.0, ticker->scroll_wait_time - host_rawframetime);
}

static qboolean M_Ticker_Key (menuticker_t *ticker, int key)
{
	switch (key)
	{
	case K_RIGHTARROW:
		ticker->scroll_time += 0.25;
		ticker->scroll_wait_time = 1.5;
		return true;

	case K_LEFTARROW:
		ticker->scroll_time -= 0.25;
		ticker->scroll_wait_time = 1.5;
		return true;

	default:
		return false;
	}
}

/*
================
M_PrintScroll
================
*/
static void M_PrintScroll (cb_context_t *cbx, int x, int y, int maxwidth, const char *str, double time, qboolean color)
{
	int	 maxchars = maxwidth / CHARACTER_SIZE;
	int	 len = strlen (str);
	int	 i, ofs;
	char mask = color ? 0x80 : 0;

	if (len <= maxchars)
	{
		if (color)
			M_Print (cbx, x, y, str);
		else
			M_PrintWhite (cbx, x, y, str);
		return;
	}

	ofs = (int)floor (time * 4.0);
	ofs %= len + 5;
	if (ofs < 0)
		ofs += len + 5;

	for (i = 0; i < maxchars; i++)
	{
		char c = (ofs < len) ? str[ofs] : " /// "[ofs - len];
		Draw_Character (cbx, x, y, c ^ mask);
		x += CHARACTER_SIZE;
		if (++ofs >= len + 5)
			ofs = 0;
	}
}

//=============================================================================
/* SINGLE PLAYER MENU */

int				m_singleplayer_cursor;
static qboolean m_singleplayer_showlevels;
#define SINGLEPLAYER_ITEMS (3 + (m_singleplayer_showlevels ? 1 : 0))

static void M_Menu_SinglePlayer_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_singleplayer;
	if (m_singleplayer_cursor >= SINGLEPLAYER_ITEMS)
		m_singleplayer_cursor = 0;
}

static void M_SinglePlayer_Draw (cb_context_t *cbx)
{
	int		f;
	qpic_t *p;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/ttl_sgl.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);
	M_DrawTransPic (cbx, 72, 32, Draw_CachePic ("gfx/sp_menu.lmp"));
	if (m_singleplayer_showlevels)
		M_DrawTransPic (cbx, 72, 92, Draw_CachePic ("gfx/sp_maps.lmp"));

	f = (int)(realtime * 10) % 6;

	M_Mouse_UpdateListCursor (&m_singleplayer_cursor, 70, 320, 32, 20, SINGLEPLAYER_ITEMS, 0);
	M_DrawTransPic (cbx, 54, 32 + m_singleplayer_cursor * 20, Draw_CachePic (va ("gfx/menudot%i.lmp", f + 1)));
}

static void M_SinglePlayer_Key (int key)
{
	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_singleplayer_cursor >= SINGLEPLAYER_ITEMS)
			m_singleplayer_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_singleplayer_cursor < 0)
			m_singleplayer_cursor = SINGLEPLAYER_ITEMS - 1;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;

		switch (m_singleplayer_cursor)
		{
		case 0:
			if (sv.active)
				if (!SCR_ModalMessage ("Are you sure you want to\nstart a new game? (y/n)\n", 0.0f))
					break;
			SCR_BeginLoadingPlaque ();
			IN_Activate ();
			key_dest = key_game;
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Cbuf_AddText ("maxplayers 1\n");
			Cbuf_AddText ("deathmatch 0\n"); // johnfitz
			Cbuf_AddText ("coop 0\n");		 // johnfitz
			Cbuf_AddText ("map start\n");
			break;

		case 1:
			M_Menu_Load_f ();
			break;

		case 2:
			M_Menu_Save_f ();
			break;

		case 3:
			M_Menu_Maps_f ();
			break;
		}
	}
}

//=============================================================================
/* LOAD/SAVE MENU */

int load_cursor; // 0 < load_cursor < MAX_SAVEGAMES

#define MAX_SAVEGAMES 20 /* johnfitz -- increased from 12 */
char			m_filenames[MAX_SAVEGAMES][SAVEGAME_COMMENT_LENGTH + 1];
int				loadable[MAX_SAVEGAMES];

static qboolean M_ScanSave (const char *save_name, char *comment, size_t comment_size)
{
	int	   version;
	size_t k;
	char   path[MAX_OSPATH];
	char   save_comment[SAVEGAME_COMMENT_LENGTH + 1];
	FILE  *f;

	q_snprintf (path, sizeof (path), "%s/%s.sav", com_gamedir, save_name);
	f = fopen (path, "r");
	if (!f)
		return false;
	if (fscanf (f, "%i\n", &version) != 1 || (version != 5 && version != 6) || (version == 6 && fscanf (f, "%*1023s\n") == EOF) ||
		fscanf (f, "%" QS_STRINGIFY (SAVEGAME_COMMENT_LENGTH) "s\n", save_comment) != 1)
	{
		fclose (f);
		return false;
	}
	fclose (f);

	if (comment && comment_size)
	{
		q_strlcpy (comment, save_comment, comment_size);
		for (k = 0; comment[k]; k++)
		{
			if (comment[k] == '_')
				comment[k] = ' ';
		}
	}
	return true;
}

static void M_ScanSaves (void)
{
	int	 i;
	char save_name[16];

	for (i = 0; i < MAX_SAVEGAMES; i++)
	{
		q_strlcpy (m_filenames[i], "--- UNUSED SLOT ---", sizeof (m_filenames[i]));
		q_snprintf (save_name, sizeof (save_name), "s%i", i);
		loadable[i] = M_ScanSave (save_name, m_filenames[i], sizeof (m_filenames[i]));
	}
}

static void M_PrintSavegame (cb_context_t *cbx, int x, int y, const char *comment, const char *title)
{
	char   level_name[SAVEGAME_LEVEL_LENGTH + 1];
	size_t comment_length = strlen (comment);
	size_t level_length = q_min (comment_length, SAVEGAME_LEVEL_LENGTH);

	if (!title)
	{
		memcpy (level_name, comment, level_length);
		while (level_length && level_name[level_length - 1] == ' ')
			level_length--;
		level_name[level_length] = '\0';
		title = level_name;
	}

	M_PrintElided (cbx, x, y, title, SAVEGAME_LEVEL_LENGTH - 2);
	if (comment_length > SAVEGAME_LEVEL_LENGTH)
		M_Print (cbx, x + SAVEGAME_LEVEL_LENGTH * CHARACTER_SIZE, y, comment + SAVEGAME_LEVEL_LENGTH);
}

static void M_Menu_Load_f (void)
{
	M_MenuChanged ();
	m_state = m_load;

	IN_DeactivateForMenu ();
	key_dest = key_menu;
	M_ScanSaves ();
	if (load_cursor >= MAX_SAVEGAMES)
		load_cursor = MAX_SAVEGAMES - 1;
}

static void M_Menu_Save_f (void)
{
	if (!sv.active)
		return;
	if (cl.intermission)
		return;
	if (svs.maxclients != 1)
		return;
	M_MenuChanged ();
	m_state = m_save;

	IN_DeactivateForMenu ();
	key_dest = key_menu;
	M_ScanSaves ();
	if (load_cursor >= MAX_SAVEGAMES)
		load_cursor = MAX_SAVEGAMES - 1;
}

static void M_Load_Draw (cb_context_t *cbx)
{
	int		i;
	qpic_t *p;

	p = Draw_CachePic ("gfx/p_load.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	for (i = 0; i < MAX_SAVEGAMES; i++)
		M_PrintSavegame (cbx, 16, 32 + 8 * i, m_filenames[i], NULL);

	// line cursor
	M_Mouse_UpdateListCursor (&load_cursor, 16, 320, 32, 8, MAX_SAVEGAMES, 0);
	Draw_Character (cbx, 8, 32 + load_cursor * 8, 12 + ((int)(realtime * 4) & 1));
}

static void M_Save_Draw (cb_context_t *cbx)
{
	int		i;
	qpic_t *p;

	p = Draw_CachePic ("gfx/p_save.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	for (i = 0; i < MAX_SAVEGAMES; i++)
		M_PrintSavegame (cbx, 16, 32 + 8 * i, m_filenames[i], NULL);

	// line cursor
	M_Mouse_UpdateListCursor (&load_cursor, 16, 320, 32, 8, MAX_SAVEGAMES, 0);
	Draw_Character (cbx, 8, 32 + load_cursor * 8, 12 + ((int)(realtime * 4) & 1));
}

static void M_Load_Key (int k)
{
	int num_items = MAX_SAVEGAMES;
	int save_slot = load_cursor;

	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_SinglePlayer_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		S_LocalSound ("misc/menu2.wav");
		if (save_slot < 0 || save_slot >= MAX_SAVEGAMES || !loadable[save_slot])
			return;

		// Draw before leaving the menu so disconnected loads don't expose the console.
		SCR_BeginLoadingPlaque ();

		m_state = m_none;
		IN_Activate ();
		key_dest = key_game;

		// issue the load command
		Cbuf_AddText (va ("load s%i\n", save_slot));
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = num_items - 1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor++;
		if (load_cursor >= num_items)
			load_cursor = 0;
		break;
	}
}

static void M_Save_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_SinglePlayer_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_state = m_none;
		IN_Activate ();
		key_dest = key_game;
		Cbuf_AddText (va ("save s%i\n", load_cursor));
		return;

	case K_UPARROW:
	case K_LEFTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor--;
		if (load_cursor < 0)
			load_cursor = MAX_SAVEGAMES - 1;
		break;

	case K_DOWNARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		load_cursor++;
		if (load_cursor >= MAX_SAVEGAMES)
			load_cursor = 0;
		break;
	}
}

//=============================================================================
/* MULTIPLAYER MENU */

int m_multiplayer_cursor;
#define MULTIPLAYER_ITEMS 3

static void M_Menu_MultiPlayer_f (void)
{
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_multiplayer;
	m_entersound = true;
}

static void M_MultiPlayer_Draw (cb_context_t *cbx)
{
	int		f;
	qpic_t *p;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);
	M_DrawTransPic (cbx, 72, 32, Draw_CachePic ("gfx/mp_menu.lmp"));

	f = (int)(realtime * 10) % 6;

	M_Mouse_UpdateListCursor (&m_multiplayer_cursor, 70, 320, 32, 20, MULTIPLAYER_ITEMS, 0);
	M_DrawTransPic (cbx, 54, 32 + m_multiplayer_cursor * 20, Draw_CachePic (va ("gfx/menudot%i.lmp", f + 1)));

	if (ipxAvailable || ipv4Available || ipv6Available)
		return;
	M_PrintWhite (cbx, (320 / 2) - ((27 * 8) / 2), 148, "No Communications Available");
}

static void M_MultiPlayer_Key (int key)
{
	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Main_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_multiplayer_cursor >= MULTIPLAYER_ITEMS)
			m_multiplayer_cursor = 0;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_multiplayer_cursor < 0)
			m_multiplayer_cursor = MULTIPLAYER_ITEMS - 1;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		switch (m_multiplayer_cursor)
		{
		case 0:
			if (ipxAvailable || ipv4Available || ipv6Available)
				M_Menu_Net_f ();
			break;

		case 1:
			if (ipxAvailable || ipv4Available || ipv6Available)
				M_Menu_Net_f ();
			break;

		case 2:
			M_Menu_Setup_f ();
			break;
		}
	}
}

//=============================================================================
/* SETUP MENU */

int setup_cursor = 4;
int setup_cursor_table[] = {40, 56, 80, 104, 140};

char setup_hostname[16];
char setup_myname[16];
int	 setup_oldtop;
int	 setup_oldbottom;
int	 setup_top;
int	 setup_bottom;

#define NUM_SETUP_CMDS 5

static void M_Menu_Setup_f (void)
{
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_setup;
	m_entersound = true;
	q_strlcpy (setup_myname, cl_name.string, sizeof (setup_myname));
	q_strlcpy (setup_hostname, hostname.string, sizeof (setup_hostname));
	setup_top = setup_oldtop = ((int)cl_color.value) >> 4;
	setup_bottom = setup_oldbottom = ((int)cl_color.value) & 15;
}

static void M_Setup_Draw (cb_context_t *cbx)
{
	qpic_t *p;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_Print (cbx, 64, 40, "Hostname");
	M_DrawTextBox (cbx, 160, 32, 16, 1);
	M_Print (cbx, 168, 40, setup_hostname);

	M_Print (cbx, 64, 56, "Your name");
	M_DrawTextBox (cbx, 160, 48, 16, 1);
	M_Print (cbx, 168, 56, setup_myname);

	M_Print (cbx, 64, 80, "Shirt color");
	M_Print (cbx, 64, 104, "Pants color");

	M_DrawTextBox (cbx, 64, 140 - 8, 14, 1);
	M_Print (cbx, 72, 140, "Accept Changes");

	p = Draw_CachePic ("gfx/bigbox.lmp");
	M_DrawTransPic (cbx, 160, 64, p);
	p = Draw_CachePic ("gfx/menuplyr.lmp");
	M_DrawTransPicTranslate (cbx, 172, 72, p, setup_top, setup_bottom);

	for (int i = 0; i < 5; ++i)
		M_Mouse_UpdateCursor (&setup_cursor, 0, 400, setup_cursor_table[i], 8, i);
	Draw_Character (cbx, 56, setup_cursor_table[setup_cursor], 12 + ((int)(realtime * 4) & 1));

	if (setup_cursor == 0)
		Draw_Character (cbx, 168 + 8 * strlen (setup_hostname), setup_cursor_table[setup_cursor], 10 + ((int)(realtime * 4) & 1));

	if (setup_cursor == 1)
		Draw_Character (cbx, 168 + 8 * strlen (setup_myname), setup_cursor_table[setup_cursor], 10 + ((int)(realtime * 4) & 1));
}

static void M_Setup_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_MultiPlayer_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		setup_cursor--;
		if (setup_cursor < 0)
			setup_cursor = NUM_SETUP_CMDS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		setup_cursor++;
		if (setup_cursor >= NUM_SETUP_CMDS)
			setup_cursor = 0;
		break;

	case K_LEFTARROW:
		if (setup_cursor < 2)
			return;
		S_LocalSound ("misc/menu3.wav");
		if (setup_cursor == 2)
			setup_top = setup_top - 1;
		if (setup_cursor == 3)
			setup_bottom = setup_bottom - 1;
		break;
	case K_RIGHTARROW:
		if (setup_cursor < 2)
			return;
	forward:
		S_LocalSound ("misc/menu3.wav");
		if (setup_cursor == 2)
			setup_top = setup_top + 1;
		if (setup_cursor == 3)
			setup_bottom = setup_bottom + 1;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		if (setup_cursor == 0 || setup_cursor == 1)
			return;

		if (setup_cursor == 2 || setup_cursor == 3)
			goto forward;

		// setup_cursor == 4 (OK)
		if (strcmp (cl_name.string, setup_myname) != 0)
			Cbuf_AddText (va ("name \"%s\"\n", setup_myname));
		if (strcmp (hostname.string, setup_hostname) != 0)
			Cvar_Set ("hostname", setup_hostname);
		if (setup_top != setup_oldtop || setup_bottom != setup_oldbottom)
			Cbuf_AddText (va ("color %i %i\n", setup_top, setup_bottom));
		m_entersound = true;
		M_Menu_MultiPlayer_f ();
		break;

	case K_BACKSPACE:
		if (setup_cursor == 0)
		{
			if (strlen (setup_hostname))
				setup_hostname[strlen (setup_hostname) - 1] = 0;
		}

		if (setup_cursor == 1)
		{
			if (strlen (setup_myname))
				setup_myname[strlen (setup_myname) - 1] = 0;
		}
		break;
	}

	if (setup_top > 13)
		setup_top = 0;
	if (setup_top < 0)
		setup_top = 13;
	if (setup_bottom > 13)
		setup_bottom = 0;
	if (setup_bottom < 0)
		setup_bottom = 13;
}

static void M_Setup_Char (int k)
{
	int l;

	switch (setup_cursor)
	{
	case 0:
		l = strlen (setup_hostname);
		if (l < 15)
		{
			setup_hostname[l + 1] = 0;
			setup_hostname[l] = k;
		}
		break;
	case 1:
		l = strlen (setup_myname);
		if (l < 15)
		{
			setup_myname[l + 1] = 0;
			setup_myname[l] = k;
		}
		break;
	}
}

qboolean M_Setup_TextEntry (void)
{
	return (setup_cursor == 0 || setup_cursor == 1);
}

//=============================================================================
/* NET MENU */

int m_net_cursor;
int m_first_net_item;
int m_net_items;

static const char *net_helpMessage[] = {
	/* .........1.........2.... */
	" Novell network LANs    ", " or Windows 95 DOS-box. ", "                        ", "(LAN=Local Area Network)",

	" Commonly used to play  ", " over the Internet, but ", " also used on a Local   ", " Area Network.          "};

static void M_Menu_Net_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_net;

	m_net_items = 2;
	m_first_net_item = 0;
	if (!ipxAvailable)
	{
		m_net_items -= 1;
		m_first_net_item += 1;
	}
	if (!ipv4Available && !ipv6Available)
		m_net_items -= 1;

	m_net_cursor = CLAMP (m_first_net_item, m_net_cursor, m_first_net_item + m_net_items - 1);
}

static void M_Net_Draw (cb_context_t *cbx)
{
	int		f;
	qpic_t *p;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	f = 32;

	if (ipxAvailable)
		p = Draw_CachePic ("gfx/netmen3.lmp");
	else
		p = Draw_CachePic ("gfx/dim_ipx.lmp");
	M_DrawTransPic (cbx, 72, f, p);

	f += 19;
	if (ipv4Available || ipv6Available)
		p = Draw_CachePic ("gfx/netmen4.lmp");
	else
		p = Draw_CachePic ("gfx/dim_tcp.lmp");
	M_DrawTransPic (cbx, 72, f, p);

	f = (320 - 26 * 8) / 2;
	M_DrawTextBox (cbx, f, 96, 24, 4);
	f += 8;
	M_Print (cbx, f, 104, net_helpMessage[m_net_cursor * 4 + 0]);
	M_Print (cbx, f, 112, net_helpMessage[m_net_cursor * 4 + 1]);
	M_Print (cbx, f, 120, net_helpMessage[m_net_cursor * 4 + 2]);
	M_Print (cbx, f, 128, net_helpMessage[m_net_cursor * 4 + 3]);

	f = (int)(realtime * 10) % 6;
	M_Mouse_UpdateListCursor (&m_net_cursor, 70, 320, 32 + m_first_net_item * 20, 20, m_net_items, m_first_net_item);
	M_DrawTransPic (cbx, 54, 32 + m_net_cursor * 20, Draw_CachePic (va ("gfx/menudot%i.lmp", f + 1)));
}

static void M_Net_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_MultiPlayer_f ();
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_net_cursor >= m_first_net_item + m_net_items)
			m_net_cursor = m_first_net_item;
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_net_cursor < m_first_net_item)
			m_net_cursor = m_first_net_item + m_net_items - 1;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		M_Menu_LanConfig_f ();
		break;
	}
}

//=============================================================================
/* GAME OPTIONS MENU */

enum
{
	GAME_OPT_SCALE,
	GAME_OPT_SBALPHA,
	GAME_OPT_MOUSESPEED,
	GAME_OPT_VIEWBOB,
	GAME_OPT_VIEWROLL,
	GAME_OPT_GUNKICK,
	GAME_OPT_SHOWGUN,
	GAME_OPT_ALWAYRUN,
	GAME_OPT_INVMOUSE,
	GAME_OPT_HUD_DETAIL,
	GAME_OPT_CROSSHAIR,
	GAME_OPT_CROSSHAIR_SIZE,
	GAME_OPT_CROSSHAIR_COLOR,
	GAME_OPT_CROSSHAIR_OPACITY,
	GAME_OPT_STARTUP_DEMOS,
	GAME_OPT_SHOWFPS,
	GAME_OPT_LIVE_PREVIEW,
	GAME_OPTIONS_ITEMS
};

#define GAME_OPTIONS_PER_PAGE MAX_MENU_LINES
static int game_options_cursor = 0;
static int first_game_option = 0;

static void M_Menu_GameOptions_f (void)
{
	memset (&menu_preview, 0, sizeof (menu_preview));
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_game;
	m_entersound = true;
}

static void M_GameOptions_AdjustSliders (int dir, qboolean mouse)
{
	if (scrollbar_grab)
		return;

	float f, clamped_mouse = CLAMP (MENU_SLIDER_START, (float)m_mouse_x, MENU_SLIDER_END);

	if (fabsf (clamped_mouse - (float)m_mouse_x) > 12.0f)
		mouse = false;

	if (dir)
		S_LocalSound ("misc/menu3.wav");

	if (mouse)
		slider_grab = true;

	switch (game_options_cursor)
	{
	case GAME_OPT_SCALE: // console and menu scale
		if (scr_relativescale.value)
		{
			f = M_GetSliderPos (1, 3.0f, scr_relativescale.value, false, mouse, clamped_mouse, dir, 0.1, 999);
			Cvar_SetValue ("scr_relativescale", f);
		}
		else
		{
			f = M_GetSliderPos (1, ((vid.width + 31) / 32) / 10.0, scr_conscale.value, false, mouse, clamped_mouse, dir, 0.1, 999);
			Cvar_SetValue ("scr_conscale", f);
			Cvar_SetValue ("scr_menuscale", f);
			Cvar_SetValue ("scr_sbarscale", f);
		}
		break;
	case GAME_OPT_MOUSESPEED: // mouse speed
		f = M_GetSliderPos (1, 11, sensitivity.value, false, mouse, clamped_mouse, dir, 0.5, 999);
		Cvar_SetValue ("sensitivity", f);
		break;
	case GAME_OPT_SBALPHA: // statusbar alpha
		f = M_GetSliderPos (0, 1, 1.0f - scr_sbaralpha.value, true, mouse, clamped_mouse, dir, 0.05, 999);
		Cvar_SetValue ("scr_sbaralpha", 1.0f - f);
		break;
	case GAME_OPT_VIEWBOB: // statusbar alpha
		f = (1.0f - M_GetSliderPos (0, 1, 1.0f - (cl_bob.value * 20.0f), true, mouse, clamped_mouse, dir, 0.05, 999)) / 20.0f;
		Cvar_SetValue ("cl_bob", f);
		break;
	case GAME_OPT_VIEWROLL: // statusbar alpha
		f = (1.0f - M_GetSliderPos (0, 1, 1.0f - (cl_rollangle.value * 0.2f), true, mouse, clamped_mouse, dir, 0.05, 999)) / 0.2f;
		Cvar_SetValue ("cl_rollangle", f);
		break;
	case GAME_OPT_GUNKICK: // gun kick
		Cvar_SetValue ("v_gunkick", ((int)v_gunkick.value + 3 + dir) % 3);
		break;
	case GAME_OPT_SHOWGUN: // gun kick
		Cvar_SetValue ("r_drawviewmodel", ((int)r_drawviewmodel.value + 2 + dir) % 2);
		break;
	case GAME_OPT_ALWAYRUN: // always run
		Cvar_SetValue ("cl_alwaysrun", !(cl_alwaysrun.value || cl_forwardspeed.value > 200));

		// The past vanilla "always run" option set these two CVARs, so reset them too when
		// changing in case the user previously used that option.
		Cvar_SetValue ("cl_forwardspeed", 200);
		Cvar_SetValue ("cl_backspeed", 200);
		break;
	case GAME_OPT_INVMOUSE:
		Cvar_SetValue ("m_pitch", -m_pitch.value);
		break;
	case GAME_OPT_CROSSHAIR:
		if (crosshair.value)
		{
			if ((crosshair_def.value == num_crosshair_defs - 1) && (dir == 1))
			{
				// fold to off
				Cvar_SetValue ("crosshair", 0.0f);
				Cvar_SetValue ("crosshair_def", 0.0f);
			}
			else if ((crosshair_def.value == 0) && (dir == -1))
			{
				// fold to off
				Cvar_SetValue ("crosshair", 0.0f);
			}
			else
			{
				Cvar_SetValue ("crosshair_def", ((int)crosshair_def.value + num_crosshair_defs + dir) % num_crosshair_defs);
			}
		}
		else // off => on
		{
			if (dir == -1)
			{
				// fold to max
				Cvar_SetValue ("crosshair", 1.0f);
				Cvar_SetValue ("crosshair_def", (float)(num_crosshair_defs - 1));
			}
			else if (dir == 1)
			{
				Cvar_SetValue ("crosshair", 1.0f);
				Cvar_SetValue ("crosshair_def", 0.0f);
			}
		}
		break;
	case GAME_OPT_CROSSHAIR_COLOR:
		Cvar_SetValue ("crosshair_color", ((int)crosshair_color.value + (int)countof (crosshair_color_names) + dir) % (int)countof (crosshair_color_names));
		break;
	case GAME_OPT_CROSSHAIR_SIZE:
		f = M_GetSliderPos (6, 64, crosshair_size.value, false, mouse, clamped_mouse, dir, 1, 999);
		Cvar_SetValue ("crosshair_size", f);
		break;
	case GAME_OPT_CROSSHAIR_OPACITY:
		f = M_GetSliderPos (0, 1, crosshair_alpha.value, false, mouse, clamped_mouse, dir, 0.1, 999);
		Cvar_SetValue ("crosshair_alpha", f);
		break;
	case GAME_OPT_HUD_DETAIL: // interface detail
		// cycles through 100 (classic full), 110 (classic), 110 + minimal, 120 (none)
		if (scr_viewsize.value < 110.0f)
		{
			if (dir > 0)
			{
				Cvar_SetValue ("viewsize", 110.0f);
				Cvar_SetValue ("rt_hud_minimal", 0.0f);
			}
		}
		else if (scr_viewsize.value < 120.0f && !CVAR_TO_BOOL (rt_hud_minimal))
		{
			if (dir < 0)
			{
				Cvar_SetValue ("viewsize", 100.0f);
				Cvar_SetValue ("rt_hud_minimal", 0.0f);
			}
			else
				Cvar_SetValue ("rt_hud_minimal", 1.0f);
		}
		else if (scr_viewsize.value < 120.0f && CVAR_TO_BOOL (rt_hud_minimal))
		{
			if (dir < 0)
				Cvar_SetValue ("rt_hud_minimal", 0.0f);
			else
			{
				Cvar_SetValue ("viewsize", 120.0f);
				Cvar_SetValue ("rt_hud_minimal", 0.0f);
			}
		}
		else if (dir > 0)
		{
			Cvar_SetValue ("viewsize", 110.0f);
			Cvar_SetValue ("rt_hud_minimal", 1.0f);
		}
		break;
	case GAME_OPT_STARTUP_DEMOS:
		Cvar_SetValue ("cl_startdemos", ((int)cl_startdemos.value + 2 + dir) % 2);
		break;
	case GAME_OPT_SHOWFPS:
		Cvar_SetValue ("scr_showfps", ((int)scr_showfps.value + 2 + dir) % 2);
		break;
	case GAME_OPT_LIVE_PREVIEW:
		Cvar_SetValueQuick (&ui_live_preview, !ui_live_preview.value);
		break;
	}
	switch (game_options_cursor)
	{
	case GAME_OPT_SCALE:
	case GAME_OPT_SBALPHA:
	case GAME_OPT_HUD_DETAIL:
	default:
		menu_preview.target = menu_preview.hold = 0.0f;
		break;
	}
}

static void M_GameOptions_Key (int k)
{
	if (M_HandleScrollBarKeys (k, &game_options_cursor, &first_game_option, GAME_OPTIONS_ITEMS, GAME_OPTIONS_PER_PAGE))
		return;

	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		M_GameOptions_AdjustSliders (1, k == K_MOUSE1);
		return;

	case K_LEFTARROW:
		M_GameOptions_AdjustSliders (-1, false);
		break;

	case K_RIGHTARROW:
		M_GameOptions_AdjustSliders (1, false);
		break;
	}
}
static void M_GameOptions_Draw (cb_context_t *cbx)
{
	float	  l, r;
	qpic_t	 *p;
	const int top = MENU_TOP;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	// Draw the items in the order of the enum defined above:

	for (int i = 0; i < GAME_OPTIONS_PER_PAGE && i < (int)GAME_OPTIONS_ITEMS; i++)
	{
		const int y = top + i * CHARACTER_SIZE;
		M_PreviewRow (cbx, i + first_game_option, y);
		switch (i + first_game_option)
		{
		case GAME_OPT_SCALE:
			M_Print (cbx, MENU_LABEL_X, y, "Interface Scale");
			l = scr_relativescale.value ? 2.0f : ((vid.width / 320.0) - 1);
			r = l > 0 ? ((scr_relativescale.value ? scr_relativescale.value : scr_conscale.value) - 1) / l : 0;
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.1f", r));
			break;

		case GAME_OPT_SBALPHA:
			M_Print (cbx, MENU_LABEL_X, y, "HUD Opacity");
			r = scr_sbaralpha.value; // scr_sbaralpha range is 1.0 to 0.0
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.2f", r));
			break;

		case GAME_OPT_MOUSESPEED:
			M_Print (cbx, MENU_LABEL_X, y, "Mouse Speed");
			r = (sensitivity.value - 1) / 10;
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.1f", r));
			break;

		case GAME_OPT_VIEWBOB:
			M_Print (cbx, MENU_LABEL_X, y, "View Bob");
			r = cl_bob.value * 20.0f;
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.2f", r));
			break;

		case GAME_OPT_VIEWROLL:
			M_Print (cbx, MENU_LABEL_X, y, "View Roll");
			r = cl_rollangle.value * 0.2f;
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.2f", r));
			break;

		case GAME_OPT_GUNKICK:
			M_Print (cbx, MENU_LABEL_X, y, "Gun Kick");
			M_Print (cbx, MENU_VALUE_X, y, (v_gunkick.value == 2) ? "smooth" : (v_gunkick.value == 1) ? "classic" : "off");
			break;

		case GAME_OPT_SHOWGUN:
			M_Print (cbx, MENU_LABEL_X, y, "Show Gun");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, r_drawviewmodel.value);
			break;

		case GAME_OPT_ALWAYRUN:
			M_Print (cbx, MENU_LABEL_X, y, "Always Run");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, cl_alwaysrun.value || cl_forwardspeed.value > 200.0);
			break;

		case GAME_OPT_INVMOUSE:
			M_Print (cbx, MENU_LABEL_X, y, "Invert Mouse");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, m_pitch.value < 0);
			break;

		case GAME_OPT_HUD_DETAIL:
			M_Print (cbx, MENU_LABEL_X, y, "HUD Detail");
			if (scr_viewsize.value >= 120.0f)
				M_Print (cbx, MENU_VALUE_X, y, "none");
			else if (CVAR_TO_BOOL (rt_hud_minimal))
				M_Print (cbx, MENU_VALUE_X, y, "minimal");
			else if (scr_viewsize.value >= 110.0f)
				M_Print (cbx, MENU_VALUE_X, y, "classic");
			else
				M_Print (cbx, MENU_VALUE_X, y, "classic full");
			break;


		case GAME_OPT_CROSSHAIR:
			M_Print (cbx, MENU_LABEL_X, y, "Crosshair");
			if (!crosshair.value)
			{
				M_Print (cbx, MENU_VALUE_X, y, "off");
			}
			else
			{
				M_DrawCrosshair (cbx, MENU_VALUE_X + CHARACTER_SIZE * 0.5f, y + CHARACTER_SIZE * 0.5f, CHARACTER_SIZE);
			}
			break;

		case GAME_OPT_CROSSHAIR_SIZE:
			M_Print (cbx, MENU_LABEL_X, y, "Crosshair Size");
			r = (crosshair_size.value - 6.0f) / 58.0f;
			M_DrawSlider (cbx, MENU_SLIDER_X, y, r, va ("%.0f", crosshair_size.value));
			break;

		case GAME_OPT_CROSSHAIR_COLOR:
			M_Print (cbx, MENU_LABEL_X, y, "Crosshair Color");
			M_Print (cbx, MENU_VALUE_X, y, M_GetCrosshairColorName (crosshair_color.value));
			break;

		case GAME_OPT_CROSSHAIR_OPACITY:
			M_Print (cbx, MENU_LABEL_X, y, "Crosshair Opacity");
			M_DrawSlider (cbx, MENU_SLIDER_X, y, crosshair_alpha.value, va ("%.1f", crosshair_alpha.value));
			break;



		case GAME_OPT_STARTUP_DEMOS:
			M_Print (cbx, MENU_LABEL_X, y, "Startup Demos");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, cl_startdemos.value);
			break;

		case GAME_OPT_SHOWFPS:
			M_Print (cbx, MENU_LABEL_X, y, "Show FPS");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, scr_showfps.value);
			break;

		case GAME_OPT_LIVE_PREVIEW:
			M_Print (cbx, MENU_LABEL_X, y, "Live Preview");
			M_DrawCheckbox (cbx, MENU_VALUE_X, y, ui_live_preview.value);
			break;
		}
	}

	Draw_SetOpacity (1.0f - M_MenuPreviewFraction ());
	if (GAME_OPTIONS_ITEMS > GAME_OPTIONS_PER_PAGE)
		M_DrawScrollbar (
			cbx, MENU_SCROLLBAR_X, MENU_TOP + CHARACTER_SIZE, (float)(first_game_option) / (GAME_OPTIONS_ITEMS - GAME_OPTIONS_PER_PAGE),
			GAME_OPTIONS_PER_PAGE - 2);

	// cursor
	M_Mouse_UpdateListCursor (&game_options_cursor, MENU_CURSOR_X, 320, top, CHARACTER_SIZE, GAME_OPTIONS_PER_PAGE, first_game_option);
	Draw_SetOpacity (1.0f);
	Draw_Character (cbx, MENU_CURSOR_X, top + (game_options_cursor - first_game_option) * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}


//=============================================================================
/* SOUND OPTIONS MENU */

enum
{
	SOUND_OPT_SNDVOL,
	SOUND_OPT_MUSICVOL,
	SOUND_OPT_MUSICEXT,
	SOUND_OPT_SPATIAL,
	SOUND_OPT_FREQUENCY,
	SOUND_OPT_EQUALIZER,
	SOUND_OPTIONS_ITEMS
};

static const int sound_frequencies[] = {11025, 22050, 44100, 48000, 96000, 192000};

static int sound_options_cursor = 0;

static void M_Menu_SoundOptions_f (void)
{
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_sound;
	m_entersound = true;
}

static void M_SoundOptions_AdjustSliders (int dir, qboolean mouse)
{
	float f, clamped_mouse = CLAMP (MENU_SLIDER_START, (float)m_mouse_x, MENU_SLIDER_END);

	if (fabsf (clamped_mouse - (float)m_mouse_x) > 12.0f)
		mouse = false;

	if (dir)
		S_LocalSound ("misc/menu3.wav");

	if (mouse)
		slider_grab = true;

	switch (sound_options_cursor)
	{
	case SOUND_OPT_SNDVOL:
		f = M_GetSliderPos (0, 1, sfxvolume.value, false, mouse, clamped_mouse, dir, 0.01, 999);
		Cvar_SetValue ("volume", f);
		break;
	case SOUND_OPT_MUSICVOL:
		f = M_GetSliderPos (0, 1, bgmvolume.value, false, mouse, clamped_mouse, dir, 0.01, 999);
		Cvar_SetValue ("bgmvolume", f);
		break;
	case SOUND_OPT_MUSICEXT:
		Cvar_SetValueQuick (&bgm_extmusic, (float)(((int)bgm_extmusic.value + 2 + dir) % 2));
		break;
	case SOUND_OPT_SPATIAL:
		if ((int)s_openal_hrtf.value == 2)
			Cvar_SetValueQuick (&s_openal_hrtf, SNDAL_HrtfEnabled () ? 0.0f : 1.0f);
		else
			Cvar_SetValueQuick (&s_openal_hrtf, s_openal_hrtf.value ? 0.0f : 1.0f);
		break;
	case SOUND_OPT_FREQUENCY:
	{
		int i, index = 0;

		for (i = 0; i < (int)countof (sound_frequencies); i++)
		{
			if (sound_frequencies[i] == (int)snd_mixspeed.value)
			{
				index = i;
				break;
			}
		}
		index = (index + (dir >= 0 ? 1 : (int)countof (sound_frequencies) - 1)) % (int)countof (sound_frequencies);
		Cvar_SetValueQuick (&snd_mixspeed, (float)sound_frequencies[index]);
		break;
	}
	}
}

static void M_SoundOptions_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		if (sound_options_cursor == SOUND_OPT_EQUALIZER)
			SNDEQ_OpenDialog ();
		else
			M_SoundOptions_AdjustSliders (1, k == K_MOUSE1);
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		sound_options_cursor--;
		if (sound_options_cursor < 0)
			sound_options_cursor = SOUND_OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		sound_options_cursor++;
		if (sound_options_cursor >= SOUND_OPTIONS_ITEMS)
			sound_options_cursor = 0;
		break;

	case K_LEFTARROW:
		M_SoundOptions_AdjustSliders (-1, false);
		break;

	case K_RIGHTARROW:
		M_SoundOptions_AdjustSliders (1, false);
		break;
	}
}
static void M_SoundOptions_Draw (cb_context_t *cbx)
{
	qpic_t	 *p;
	const int top = MENU_TOP;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	// Draw the items in the order of the enum defined above:
	float label_value;

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_SNDVOL, "Sound Volume");
	label_value = sfxvolume.value * 100.f;
	M_DrawSlider (cbx, MENU_SLIDER_X, top + CHARACTER_SIZE * SOUND_OPT_SNDVOL, sfxvolume.value, va ("%.0f%%", label_value));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_MUSICVOL, "Music Volume");
	label_value = bgmvolume.value * 100.f;
	M_DrawSlider (cbx, MENU_SLIDER_X, top + CHARACTER_SIZE * SOUND_OPT_MUSICVOL, bgmvolume.value, va ("%.0f%%", label_value));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_MUSICEXT, "External Music");
	M_DrawCheckbox (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * SOUND_OPT_MUSICEXT, bgm_extmusic.value);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_SPATIAL, "Spatial sound");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * SOUND_OPT_SPATIAL, SNDAL_HrtfEnabled () ? "on" : "off");

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_FREQUENCY, "Sound frequency");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * SOUND_OPT_FREQUENCY, va ("%.1f kHz", snd_mixspeed.value / 1000.0));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * SOUND_OPT_EQUALIZER, "Equalizer");


	// cursor
	M_Mouse_UpdateListCursor (&sound_options_cursor, MENU_CURSOR_X, 320, top, CHARACTER_SIZE, SOUND_OPTIONS_ITEMS, 0);
	Draw_Character (cbx, MENU_CURSOR_X, top + sound_options_cursor * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}


//=============================================================================
/* QUALITY LADDERS */

static const char *M_GetQualityName (const cvar_t *var)
{
	const int maximum = var == &rt_sky_clouds_quality ? QR_SKY_CLOUDS_MAX_QUALITY : 4;
	switch (CLAMP (0, (int)var->value, maximum))
	{
	case 0:  return "low";
	case 1:  return "medium";
	case 3:  return "ultra";
	case 4:  return "extreme";
	default: return "high";
	}
}

static void M_StepQuality (cvar_t *var, int dir)
{
	const int maximum = var == &rt_sky_clouds_quality ? QR_SKY_CLOUDS_MAX_QUALITY : 4;
	Cvar_SetValueQuick (var, (float)CLAMP (0, (int)var->value + dir, maximum));
}

static const char *M_GetGiLevelName (void)
{
	const float v = CVAR_TO_FLOAT (rt_gi_level);

	if (v < 0.25f)
		return "off";
	if (v < 0.75f)
		return "low";
	if (v < 1.5f)
		return "medium";

	return "high";
}

static void M_StepGiLevel (int dir)
{
	static const float levels[] = { 0.0f, 0.5f, 1.0f, 2.0f };
	const int numlevels = (int)(sizeof (levels) / sizeof (levels[0]));
	const float cur = CVAR_TO_FLOAT (rt_gi_level);

	int   idx = 2; // medium
	float best = 1e9f;

	for (int i = 0; i < numlevels; i++)
	{
		const float d = fabsf (levels[i] - cur);

		if (d < best)
		{
			best = d;
			idx = i;
		}
	}

	idx = CLAMP (0, idx + (dir > 0 ? 1 : -1), numlevels - 1);

	Cvar_SetValueQuick (&rt_gi_level, levels[idx]);
}

static const char *M_GetReflDepthName (void)
{
	const int depth = (int)(CVAR_TO_FLOAT (rt_reflrefr_depth) + 0.5f);

	if (depth <= 0)
		return "off";
	if (depth == 1)
		return "1 bounce";

	return va ("%d bounces", depth);
}

static void M_StepReflDepth (int dir)
{
	static const float depths[] = { 0.0f, 1.0f, 2.0f, 4.0f, 8.0f };
	const int numdepths = (int)(sizeof (depths) / sizeof (depths[0]));
	const float cur = CVAR_TO_FLOAT (rt_reflrefr_depth);

	int   idx = 2; // 2 bounces
	float best = 1e9f;

	for (int i = 0; i < numdepths; i++)
	{
		const float d = fabsf (depths[i] - cur);

		if (d < best)
		{
			best = d;
			idx = i;
		}
	}

	idx = CLAMP (0, idx + (dir > 0 ? 1 : -1), numdepths - 1);

	Cvar_SetValueQuick (&rt_reflrefr_depth, depths[idx]);
}

//=============================================================================
/* GRAPHICS OPTIONS MENU */

enum
{
	GRAPHICS_OPT_FILTER,
	GRAPHICS_OPT_MODELS,
	GRAPHICS_OPT_PARTICLES,
	GRAPHICS_OPT_CLOUDS,
	GRAPHICS_OPT_CLOUDS_QUALITY,
	GRAPHICS_OPT_REFLECT,
	GRAPHICS_OPT_SMOKE,
	GRAPHICS_OPT_CURSOR,
	GRAPHICS_OPTIONS_ITEMS
};

static int graphics_options_cursor = 0;

static void M_Menu_GraphicsOptions_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_graphics;
}

static void M_GraphicsOptions_Adjust (int dir)
{
	int value;

	if (dir)
		S_LocalSound ("misc/menu3.wav");

	switch (graphics_options_cursor)
	{
	case GRAPHICS_OPT_FILTER:
		Cvar_SetValue ("vid_filter", (Cvar_VariableValue ("vid_filter") == 0.0) ? 1.0f : 0.0f);
		break;
	case GRAPHICS_OPT_MODELS:
		Cvar_SetValueQuick (&r_enhancedmodels, (float)(((int)r_enhancedmodels.value + 2 + dir) % 2));
		break;
	case GRAPHICS_OPT_PARTICLES:
		value = (int)r_particles.value;
		if (dir > 0)
			value = (value == 0) ? 2 : ((value == 2) ? 1 : 0);
		else
			value = (value == 0) ? 1 : ((value == 2) ? 0 : 2);
		Cvar_SetValueQuick (&r_particles, (float)value);
		break;
	case GRAPHICS_OPT_CLOUDS:
		Cvar_SetValueQuick (&rt_sky_clouds, !CVAR_TO_BOOL (rt_sky_clouds));
		break;
	case GRAPHICS_OPT_CLOUDS_QUALITY:
		M_StepQuality (&rt_sky_clouds_quality, dir);
		break;
	case GRAPHICS_OPT_REFLECT:
		M_StepReflDepth (dir);
		break;
	case GRAPHICS_OPT_SMOKE:
		Cvar_SetValueQuick (&r_smoke, !CVAR_TO_BOOL (r_smoke));
		break;
	case GRAPHICS_OPT_CURSOR:
		Cvar_SetValueQuick (&ui_cursor, !CVAR_TO_BOOL (ui_cursor));
		break;
	}
}

static void M_GraphicsOptions_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		M_GraphicsOptions_Adjust (1);
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		graphics_options_cursor--;
		if (graphics_options_cursor < 0)
			graphics_options_cursor = GRAPHICS_OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		graphics_options_cursor++;
		if (graphics_options_cursor >= GRAPHICS_OPTIONS_ITEMS)
			graphics_options_cursor = 0;
		break;

	case K_LEFTARROW:
		M_GraphicsOptions_Adjust (-1);
		break;

	case K_RIGHTARROW:
		M_GraphicsOptions_Adjust (1);
		break;
	}
}

static void M_GraphicsOptions_Draw (cb_context_t *cbx)
{
	qpic_t	 *p;
	const int top = MENU_TOP;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_FILTER, "Texture filtering");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_FILTER, (Cvar_VariableValue ("vid_filter") == 0.0) ? "smooth" : "classic");

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_MODELS, "Models");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_MODELS, (r_enhancedmodels.value == 0) ? "classic" : "enhanced");

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_PARTICLES, "Particles");
	M_Print (
		cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_PARTICLES,
		((int)r_particles.value == 0) ? "none" : (((int)r_particles.value == 2) ? "classic" : "circle"));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CLOUDS, "Volumetric clouds");
	M_DrawCheckbox (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CLOUDS, CVAR_TO_BOOL (rt_sky_clouds));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CLOUDS_QUALITY, "Clouds quality");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CLOUDS_QUALITY,
		M_GetQualityName (&rt_sky_clouds_quality));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_REFLECT, "Reflections");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_REFLECT, M_GetReflDepthName ());

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_SMOKE, "Smoke type");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_SMOKE, CVAR_TO_BOOL (r_smoke) ? "shader" : "classic");

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CURSOR, "Mouse cursor");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * GRAPHICS_OPT_CURSOR, CVAR_TO_BOOL (ui_cursor) ? "default" : "axe");

	M_Mouse_UpdateListCursor (&graphics_options_cursor, MENU_CURSOR_X, 320, top, CHARACTER_SIZE, GRAPHICS_OPTIONS_ITEMS, 0);
	Draw_Character (cbx, MENU_CURSOR_X, top + graphics_options_cursor * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}


//=============================================================================
/* LIGHTING OPTIONS MENU */

enum
{
	LIGHTING_OPT_SYSTEM,
	LIGHTING_OPT_GI,
	LIGHTING_OPT_SUN_SIZE,
	LIGHTING_OPT_GODRAYS,
	LIGHTING_OPT_GODRAYS_QUALITY,
	LIGHTING_OPTIONS_ITEMS
};

static int lighting_options_cursor = 0;

static void M_Menu_LightingOptions_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_lighting;
}

static void M_LightingOptions_Adjust (int dir)
{
	if (dir)
		S_LocalSound ("misc/menu3.wav");

	switch (lighting_options_cursor)
	{
	case LIGHTING_OPT_SYSTEM:
		// the light system is a choice, not a checkbox: new (1) or old (0)
		Cvar_SetValueQuick (&rt_truelight, CVAR_TO_FLOAT (rt_truelight) > 0.0f ? 0.0f : 1.0f);
		break;
	case LIGHTING_OPT_GI:
		M_StepGiLevel (dir);
		break;
	case LIGHTING_OPT_SUN_SIZE:
		Cvar_SetValueQuick (&rt_sky_sun_size, CLAMP (0.0f, CVAR_TO_FLOAT (rt_sky_sun_size) + dir * 0.1f, 10.0f));
		break;
	case LIGHTING_OPT_GODRAYS:
		Cvar_SetValueQuick (&rt_sky_godrays, !CVAR_TO_BOOL (rt_sky_godrays));
		break;
	case LIGHTING_OPT_GODRAYS_QUALITY:
		M_StepQuality (&rt_sky_godrays_quality, dir);
		break;
	}
}

static void M_LightingOptions_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		M_LightingOptions_Adjust (1);
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		lighting_options_cursor--;
		if (lighting_options_cursor < 0)
			lighting_options_cursor = LIGHTING_OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		lighting_options_cursor++;
		if (lighting_options_cursor >= LIGHTING_OPTIONS_ITEMS)
			lighting_options_cursor = 0;
		break;

	case K_LEFTARROW:
		M_LightingOptions_Adjust (-1);
		break;

	case K_RIGHTARROW:
		M_LightingOptions_Adjust (1);
		break;
	}
}

static void M_LightingOptions_Draw (cb_context_t *cbx)
{
	qpic_t	 *p;
	const int top = MENU_TOP;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * LIGHTING_OPT_SYSTEM, "Light system");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * LIGHTING_OPT_SYSTEM,
		CVAR_TO_FLOAT (rt_truelight) > 0.0f ? "new" : "old");

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * LIGHTING_OPT_GI, "Indirect lighting");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * LIGHTING_OPT_GI, M_GetGiLevelName ());

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * LIGHTING_OPT_SUN_SIZE, "Sun disc size");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * LIGHTING_OPT_SUN_SIZE,
		va ("%.1fx", CVAR_TO_FLOAT (rt_sky_sun_size)));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * LIGHTING_OPT_GODRAYS, "God rays");
	M_DrawCheckbox (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * LIGHTING_OPT_GODRAYS, CVAR_TO_BOOL (rt_sky_godrays));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * LIGHTING_OPT_GODRAYS_QUALITY, "God rays quality");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * LIGHTING_OPT_GODRAYS_QUALITY,
		M_GetQualityName (&rt_sky_godrays_quality));

	M_Mouse_UpdateListCursor (&lighting_options_cursor, MENU_CURSOR_X, 320, top, CHARACTER_SIZE, LIGHTING_OPTIONS_ITEMS, 0);
	Draw_Character (cbx, MENU_CURSOR_X, top + lighting_options_cursor * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}


enum
{
	EFFECTS_OPT_BLOOM,
	EFFECTS_OPT_BLOOM_QUALITY,
	EFFECTS_OPT_NEAR_DOF,
	EFFECTS_OPT_TONEMAP_TYPE,
	EFFECTS_OPT_TONEMAPPING,
	EFFECTS_OPT_EXPOSURE,
	EFFECTS_OPT_DAMAGE,
	EFFECTS_OPT_LIQUID,
	EFFECTS_OPT_SHARPEN,
	EFFECTS_OPT_VIGNETTE,
	EFFECTS_OPT_FILM_GRAIN,
	EFFECTS_OPT_LOCAL_EXPOSURE,
	EFFECTS_OPT_RESET,
	EFFECTS_OPTIONS_ITEMS
};

static int effects_options_cursor = 0;

static const char *effects_tonemap_names[] = {"Off", "Q2RTX", "Reinhard", "ACES", "AgX"};

#define EFFECTS_TONEMAP_TYPES ((int)(sizeof (effects_tonemap_names) / sizeof (effects_tonemap_names[0])))

static void M_EffectsOptions_DrawSlider (cb_context_t *cbx, int row, const cvar_t *var, float maximum)
{
	const float value = CLAMP (0.0f, CVAR_TO_FLOAT (*var), maximum);
	const float fraction = value / maximum;

	M_DrawSlider (cbx, MENU_SLIDER_X, MENU_TOP + CHARACTER_SIZE * row, fraction, va ("%.0f%%", fraction * 100.0f));
}

static void M_Menu_EffectsOptions_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_effects;
}

static void M_EffectsOptions_AdjustSliders (int dir, qboolean mouse)
{
	float f, clamped_mouse = CLAMP (MENU_SLIDER_START, (float)m_mouse_x, MENU_SLIDER_END);

	if (fabsf (clamped_mouse - (float)m_mouse_x) > 12.0f)
		mouse = false;

	if (dir)
		S_LocalSound ("misc/menu3.wav");

	if (mouse)
		slider_grab = true;

	switch (effects_options_cursor)
	{
	case EFFECTS_OPT_BLOOM:
		f = M_GetSliderPos (0, 0.2f, CVAR_TO_FLOAT (rt_bloom_intensity), false, mouse, clamped_mouse, dir, 0.002f, 0);
		Cvar_SetValueQuick (&rt_bloom_intensity, f);
		break;
	case EFFECTS_OPT_BLOOM_QUALITY:
		f = M_GetSliderPos (0, 2, CVAR_TO_FLOAT (rt_bloom_quality), false, mouse, clamped_mouse, dir, 1.0f, 0);
		Cvar_SetValueQuick (&rt_bloom_quality, f);
		break;
	case EFFECTS_OPT_NEAR_DOF:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_dof_near), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_dof_near, f);
		break;
	case EFFECTS_OPT_TONEMAP_TYPE:
		Cvar_SetValueQuick (&rt_tonemap,
			(float)(((int)rt_tonemap.value + EFFECTS_TONEMAP_TYPES + dir) % EFFECTS_TONEMAP_TYPES));
		break;
	case EFFECTS_OPT_TONEMAPPING:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_tonemap_power), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_tonemap_power, f);
		break;
	case EFFECTS_OPT_EXPOSURE:
		f = M_GetSliderPos (-3, 3, CVAR_TO_FLOAT (rt_exposure_bias), false, mouse, clamped_mouse, dir, 0.1f, 0);
		Cvar_SetValueQuick (&rt_exposure_bias, floorf (f * 10.0f + 0.5f) / 10.0f);
		break;
	case EFFECTS_OPT_DAMAGE:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_ef_damage_strength), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_ef_damage_strength, f);
		break;
	case EFFECTS_OPT_LIQUID:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_ef_liquid_strength), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_ef_liquid_strength, f);
		break;
	case EFFECTS_OPT_SHARPEN:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_sharpen_strength), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_sharpen_strength, f);
		Cvar_SetValueQuick (&rt_sharpen, f > 0.0f ? 2.0f : 0.0f);
		break;
	case EFFECTS_OPT_VIGNETTE:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_vignette), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_vignette, f);
		break;
	case EFFECTS_OPT_FILM_GRAIN:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_filmgrain), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_filmgrain, f);
		break;
	case EFFECTS_OPT_LOCAL_EXPOSURE:
		f = M_GetSliderPos (0, 1, CVAR_TO_FLOAT (rt_local_exposure), false, mouse, clamped_mouse, dir, 0.01f, 0);
		Cvar_SetValueQuick (&rt_local_exposure, f);
		break;
	case EFFECTS_OPT_RESET:
		Cvar_SetValueQuick (&rt_bloom_intensity, 0.08f);
		Cvar_SetValueQuick (&rt_bloom_threshold, 6.0f);
		Cvar_SetValueQuick (&rt_bloom_quality, 2.0f);
		Cvar_SetValueQuick (&rt_dof_near, 0.8f);
		Cvar_SetValueQuick (&rt_tonemap_power, 0.8f);
		Cvar_SetValueQuick (&rt_tonemap, 2.0f);
		Cvar_SetValueQuick (&rt_exposure_bias, 0.0f);
		Cvar_SetValueQuick (&rt_ef_damage_strength, 0.5f);
		Cvar_SetValueQuick (&rt_ef_liquid_strength, 0.51f);
		Cvar_SetValueQuick (&rt_sharpen_strength, 0.5f);
		Cvar_SetValueQuick (&rt_sharpen, 2.0f);
		Cvar_SetValueQuick (&rt_vignette, 0.5f);
		Cvar_SetValueQuick (&rt_filmgrain, 0.5f);
		Cvar_SetValueQuick (&rt_local_exposure, 0.2f);
		break;
	}
}

static void M_EffectsOptions_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		M_EffectsOptions_AdjustSliders (1, k == K_MOUSE1);
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		effects_options_cursor--;
		if (effects_options_cursor < 0)
			effects_options_cursor = EFFECTS_OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		effects_options_cursor++;
		if (effects_options_cursor >= EFFECTS_OPTIONS_ITEMS)
			effects_options_cursor = 0;
		break;

	case K_LEFTARROW:
		M_EffectsOptions_AdjustSliders (-1, false);
		break;

	case K_RIGHTARROW:
		M_EffectsOptions_AdjustSliders (1, false);
		break;
	}
}

static void M_EffectsOptions_Draw (cb_context_t *cbx)
{
	qpic_t	 *p;
	const int top = MENU_TOP;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_BLOOM, "Bloom");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_BLOOM, &rt_bloom_intensity, 0.2f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_BLOOM_QUALITY, "Bloom quality");
	M_DrawSlider (cbx, MENU_SLIDER_X, top + CHARACTER_SIZE * EFFECTS_OPT_BLOOM_QUALITY,
		CLAMP (0.0f, CVAR_TO_FLOAT (rt_bloom_quality) / 2.0f, 1.0f), M_GetQualityName (&rt_bloom_quality));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_NEAR_DOF, "Near DOF");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_NEAR_DOF, &rt_dof_near, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_TONEMAP_TYPE, "Tonemap type");
	M_Print (cbx, MENU_VALUE_X, top + CHARACTER_SIZE * EFFECTS_OPT_TONEMAP_TYPE,
		effects_tonemap_names[CLAMP (0, (int)rt_tonemap.value, EFFECTS_TONEMAP_TYPES - 1)]);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_TONEMAPPING, "Tonemap power");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_TONEMAPPING, &rt_tonemap_power, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_EXPOSURE, "Exposure bias");
	M_DrawSlider (cbx, MENU_SLIDER_X, top + CHARACTER_SIZE * EFFECTS_OPT_EXPOSURE,
		(CLAMP (-3.0f, CVAR_TO_FLOAT (rt_exposure_bias), 3.0f) + 3.0f) / 6.0f,
		va ("%+.1f EV", CVAR_TO_FLOAT (rt_exposure_bias)));

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_DAMAGE, "Damage aberration");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_DAMAGE, &rt_ef_damage_strength, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_LIQUID, "Liquid aberration");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_LIQUID, &rt_ef_liquid_strength, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_SHARPEN, "Sharpen");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_SHARPEN, &rt_sharpen_strength, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_VIGNETTE, "Vignette");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_VIGNETTE, &rt_vignette, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_FILM_GRAIN, "Film grain");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_FILM_GRAIN, &rt_filmgrain, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_LOCAL_EXPOSURE, "Local exposure");
	M_EffectsOptions_DrawSlider (cbx, EFFECTS_OPT_LOCAL_EXPOSURE, &rt_local_exposure, 1.0f);

	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * EFFECTS_OPT_RESET, "Reset effects defaults");

	M_Mouse_UpdateListCursor (&effects_options_cursor, MENU_CURSOR_X, 320, top, CHARACTER_SIZE, EFFECTS_OPTIONS_ITEMS, 0);
	Draw_Character (cbx, MENU_CURSOR_X, top + effects_options_cursor * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}


//=============================================================================
/* OPTIONS MENU */


enum
{
	OPT_GAME = 0,
	OPT_CONTROLS,
	OPT_VIDEO,
	OPT_GRAPHICS,
	OPT_EFFECTS,
	OPT_LIGHTING,
	OPT_SOUND,
	OPT_BENCHMARK,
	OPT_PADDING,
	OPT_DEFAULTS,
	OPTIONS_ITEMS
};

static int options_cursor;

void M_Menu_Options_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_options;
}

static void M_Options_Draw (cb_context_t *cbx)
{
	qpic_t	 *p;
	const int top = 40;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	// Draw the items in the order of the enum defined above:
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_GAME, "Game");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_CONTROLS, "Key Bindings");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_VIDEO, "Video");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_GRAPHICS, "Graphics");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_EFFECTS, "Effects");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_LIGHTING, "Lighting");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_SOUND, "Sound");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_BENCHMARK, "Benchmark");
	M_Print (cbx, MENU_LABEL_X, top + CHARACTER_SIZE * OPT_DEFAULTS, "Reset config");

	// cursor
	M_Mouse_UpdateListCursor (&options_cursor, MENU_LABEL_X, 320, top, CHARACTER_SIZE, OPT_PADDING, 0);
	M_Mouse_UpdateCursor (&options_cursor, MENU_LABEL_X, 320, top + OPT_DEFAULTS * CHARACTER_SIZE, CHARACTER_SIZE, OPT_DEFAULTS);
	if (options_cursor == OPT_PADDING)
		options_cursor = OPT_SOUND;
	Draw_Character (cbx, MENU_CURSOR_X, top + options_cursor * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
}

void M_Options_Key (int k)
{
	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Main_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		m_mouse_x_pixels = -1;
		switch (options_cursor)
		{
		case OPT_GAME:
			M_Menu_GameOptions_f ();
			break;
		case OPT_CONTROLS:
			M_Menu_Keys_f ();
			break;
		case OPT_DEFAULTS:
			if (SCR_ModalMessage (
					"This will reset all controls\n"
					"and stored cvars. Continue? (y/n)\n",
					15.0f))
			{
				Cbuf_AddText ("resetcfg\n");
				Cbuf_AddText ("exec default.cfg\n");
			}
			break;
		case OPT_VIDEO:
			M_Menu_Video_f ();
			break;
		case OPT_GRAPHICS:
			M_Menu_GraphicsOptions_f ();
			break;
		case OPT_EFFECTS:
			M_Menu_EffectsOptions_f ();
			break;
		case OPT_LIGHTING:
			M_Menu_LightingOptions_f ();
			break;
		case OPT_BENCHMARK:
		M_Menu_Benchmark_f ();
		break;
	case OPT_SOUND:
			M_Menu_SoundOptions_f ();
			break;
		}
		return;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		--options_cursor;
		if (options_cursor == OPT_PADDING)
			--options_cursor;
		if (options_cursor < 0)
			options_cursor = OPTIONS_ITEMS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		++options_cursor;
		if (options_cursor == OPT_PADDING)
			++options_cursor;
		if (options_cursor >= OPTIONS_ITEMS)
			options_cursor = 0;
		break;
	}
}

//=============================================================================
/* KEYS MENU */

#define QUICKSAVE "echo Quicksaving...; wait; save quick"
#define QUICKLOAD "echo Quickloading...; wait; load quick"

typedef struct
{
	const char *command;
	const char *description;
} menukeybind_t;

static const menukeybind_t default_keybinds[] = {
	{"+forward", "Move Forward"},
	{"+back", "Move Backward"},
	{"+moveleft", "Strafe Left"},
	{"+moveright", "Strafe Right"},
	{"+jump", "Jump / Swim up"},
	{"+speed", "Run"},
	{"+zoom", "Quick zoom"},
	{"+gyroaction", "Gyro switch"},
	{"+altmodifier", "Alt modifier"},
	{"+moveup", "Swim up"},
	{"+movedown", "Swim down"},
	{"", ""},
	{"*", ""}, // insertion point for bindlist.lst entries
	{"", ""},
	{"+attack", "Attack"},
	{"impulse 10", "Next weapon"},
	{"impulse 12", "Previous weapon"},
	{"impulse 1", "Axe"},
	{"impulse 2", "Shotgun"},
	{"impulse 3", "Super Shotgun"},
	{"impulse 4", "Nailgun"},
	{"impulse 5", "Super Nailgun"},
	{"impulse 6", "Grenade Launcher"},
	{"impulse 7", "Rocket Launcher"},
	{"impulse 8", "Thunderbolt"},
	{"impulse 225", "Laser Cannon"},
	{"impulse 226", "Mjolnir"},
	{"", ""},
	{"*", ""}, // end of gameplay bindings
	{"", ""},
	{QUICKSAVE, "Quick save"},
	{QUICKLOAD, "Quick load"},
	{"menu_save", "Save menu"},
	{"menu_load", "Load menu"},
	{"menu_options", "Options menu"},
	{"menu_multiplayer", "Multiplayer menu"},
	{"quit", "Quit"},
	{"help", "Help"},
	{"screenshot", "Screenshot"},
	{"+showscores", "Show score"},
	{"messagemode", "Text chat"},
	{"toggleconsole", "Toggle console"},
};

// current bind names for the current game
menukeybind_t		 *bindnames;
static menukeybind_t *custom_bindnames;

static int		keys_cursor;
static int		first_key;
static qboolean bind_grab;

static void M_Keys_AddCustomEntry (const char *command, const char *description)
{
	static const char *const deprecated_commands[] = {
		"+klook",
		"+mlook",
	};
	qboolean filter_default = true;

	// bindlist.lst uses "-" for separators.
	if (command[0] == '-' && command[1] == '\0')
		command++;

	if (command[0])
	{
		COM_Parse (command);
		if (!Cmd_Exists (com_token) && !Cmd_AliasExists (com_token))
		{
			Con_DPrintf ("Skipping unsupported key binding: \"%s\" = \"%s\"\n", description, command);
			return;
		}

		for (int i = 0; i < countof (deprecated_commands); i++)
		{
			if (!strcmp (deprecated_commands[i], command))
			{
				Con_DPrintf ("Skipping deprecated key binding: \"%s\" = \"%s\"\n", description, command);
				return;
			}
		}

		// Custom gameplay entries may replace built-in weapon labels, but not
		// standard movement or menu bindings outside the marked section.
		for (int i = 0; i < countof (default_keybinds); i++)
		{
			if (!default_keybinds[i].command[0])
				continue;
			if (!strcmp (default_keybinds[i].command, "*"))
			{
				filter_default = !filter_default;
				continue;
			}
			if (filter_default && !strcmp (default_keybinds[i].command, command))
				return;
		}
	}

	menukeybind_t item = {.command = q_strdup (command), .description = q_strdup (description)};
	VEC_PUSH (custom_bindnames, item);
}

static void M_Keys_AddItem (const menukeybind_t *item)
{
	if (item->command[0])
	{
		for (int i = 0; i < VEC_SIZE (bindnames); i++)
			if (bindnames[i].command[0] && !strcmp (bindnames[i].command, item->command))
				return;
	}

	// Collapse adjacent separators.
	if (VEC_SIZE (bindnames) && !bindnames[VEC_SIZE (bindnames) - 1].command[0] && !item->command[0])
		return;

	menukeybind_t copy = {.command = q_strdup (item->command), .description = q_strdup (item->description)};
	VEC_PUSH (bindnames, copy);
}

static void M_Keys_Populate (void)
{
	// free current binds
	for (int i = 0; i < VEC_SIZE (bindnames); i++)
	{
		SAFE_FREE (bindnames[i].command);
		SAFE_FREE (bindnames[i].description);
	}
	VEC_CLEAR (bindnames);

	qboolean added_custom_entries = false;

	// Add applicable binds to the current game.
	for (int i = 0; i < countof (default_keybinds); i++)
	{
		const menukeybind_t *item = &default_keybinds[i];

		// Filter-out items not applicable for the current game:
		if (!hipnotic && !mg3 && strcmp (item->command, "impulse 225") == 0)
			continue;
		if (!hipnotic && strcmp (item->command, "impulse 226") == 0)
			continue;

		if (!strcmp (item->command, "*"))
		{
			if (!added_custom_entries)
			{
				added_custom_entries = true;
				for (int j = 0; j < VEC_SIZE (custom_bindnames); j++)
					M_Keys_AddItem (&custom_bindnames[j]);
			}
			continue;
		}

		M_Keys_AddItem (item);
	}
}

void M_Menu_Keys_f (void)
{
	for (int i = 0; i < VEC_SIZE (custom_bindnames); i++)
	{
		SAFE_FREE (custom_bindnames[i].command);
		SAFE_FREE (custom_bindnames[i].description);
	}
	VEC_CLEAR (custom_bindnames);

	char *file = (char *)COM_LoadFile ("bindlist.lst", NULL);
	if (file)
	{
		char *text = file;
		char *line;
		while (COM_ParseMutableLine (&text, &line))
		{
			Cmd_TokenizeString (line);
			// Ignore blank/comment-only lines; separators must use "-".
			if (!Cmd_Argv (0)[0])
				continue;
			M_Keys_AddCustomEntry (Cmd_Argv (0), Cmd_Argv (1));
		}
		Mem_Free (file);
	}

	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_keys;

	M_Keys_Populate ();
	keys_cursor = 0;
	first_key = 0;
}

void M_FindKeysForCommand (const char *command, int *twokeys)
{
	int	  count;
	int	  j;
	char *b;

	twokeys[0] = twokeys[1] = -1;
	count = 0;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[j];
		if (!b)
			continue;
		if (!strcmp (b, command))
		{
			twokeys[count] = j;
			count++;
			if (count == 2)
				break;
		}
	}
}

void M_UnbindCommand (const char *command)
{
	int	  j;
	char *b;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[j];
		if (!b)
			continue;
		if (!strcmp (b, command))
			Key_SetBinding (j, NULL);
	}
}

extern qpic_t *pic_up, *pic_down;

#define BINDS_PER_PAGE 19

static void M_Keys_Draw (cb_context_t *cbx)
{
	int			i, x, y;
	int			keys[2];
	const char *name;
	qpic_t	   *p;

	p = Draw_CachePic ("gfx/ttl_cstm.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	if (bind_grab)
		M_Print (cbx, 12, 32, "Press a key or button for this action");
	else
		M_Print (cbx, 18, 32, "Enter to change, backspace to clear");

	// search for known bindings
	for (i = 0; i < BINDS_PER_PAGE && i < (int)VEC_SIZE (bindnames); i++)
	{
#define KEY_STRING_DRAW_POS (160)
		y = 48 + 8 * i;

		M_Print (cbx, 10, y, bindnames[i + first_key].description);
		if (bindnames[i + first_key].command[0])
			M_Mouse_UpdateCursor (&keys_cursor, 12, 400, y, 8, i + first_key);

		M_FindKeysForCommand (bindnames[i + first_key].command, keys);

		// do not draw anything if the bindnames is empty, it means a plceholder separator.
		if (strlen (bindnames[i + first_key].command) && (keys[0] == -1))
		{
			M_Print (cbx, KEY_STRING_DRAW_POS, y, "???");
		}
		else
		{
			name = Key_KeynumToString (keys[0]);
			M_Print (cbx, KEY_STRING_DRAW_POS, y, name);
			x = strlen (name) * 8;
			if (keys[1] != -1)
			{
				name = Key_KeynumToString (keys[1]);
				M_PrintHighlighted (cbx, (KEY_STRING_DRAW_POS - 2) + x, y, ",");
				M_Print (cbx, (KEY_STRING_DRAW_POS - 2) + x + 12, y, name);
				x = x + 12 + strlen (name) * 8;
			}
		}
	}

	if (VEC_SIZE (bindnames) > BINDS_PER_PAGE)
		M_DrawScrollbar (cbx, MENU_SCROLLBAR_X, 56, (float)(first_key) / (VEC_SIZE (bindnames) - BINDS_PER_PAGE), BINDS_PER_PAGE - 2);

	if (bind_grab)
		Draw_Character (cbx, (KEY_STRING_DRAW_POS - 10), 48 + (keys_cursor - first_key) * 8, '=');
	else
	{
		Draw_Character (cbx, 0, 48 + (keys_cursor - first_key) * 8, 12 + ((int)(realtime * 4) & 1));
	}
}

void M_Keys_Key (int k)
{
	char cmd[80];
	int	 keys[2];

	if (bind_grab)
	{ // defining a key
		S_LocalSound ("misc/menu1.wav");
		if ((k != K_ESCAPE) && (k != '`'))
		{
			q_snprintf (cmd, sizeof (cmd), "bind \"%s\" \"%s\"\n", Key_KeynumToString (k), bindnames[keys_cursor].command);
			Cbuf_InsertText (cmd);
		}

		bind_grab = false;
		IN_DeactivateForMenu (); // deactivate because we're returning to the menu
		return;
	}

	if (M_HandleScrollBarKeys (k, &keys_cursor, &first_key, (int)VEC_SIZE (bindnames), BINDS_PER_PAGE))
		return;

	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		break;

	case K_MOUSE1:
	case K_ENTER: // go into bind mode
	case K_KP_ENTER:
	case K_ABUTTON:
		M_FindKeysForCommand (bindnames[keys_cursor].command, keys);
		// if bindnames is empty, it means as a placeholder separator
		if (!strlen (bindnames[keys_cursor].command))
			return;
		S_LocalSound ("misc/menu2.wav");
		if (keys[1] != -1)
			M_UnbindCommand (bindnames[keys_cursor].command);
		bind_grab = true;
		IN_Activate (); // activate to allow mouse key binding
		break;

	case K_BACKSPACE: // delete bindings
	case K_DEL:
		S_LocalSound ("misc/menu2.wav");
		M_UnbindCommand (bindnames[keys_cursor].command);
		break;
	}
}

//=============================================================================
/* HELP MENU */

int help_page;
#define NUM_HELP_PAGES 6

static void M_Menu_Help_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_help;
	m_entersound = true;
	help_page = 0;
}

static void M_Help_Draw (cb_context_t *cbx)
{
	M_DrawPic (cbx, 0, 0, Draw_CachePic (va ("gfx/help%i.lmp", help_page)));
}

static void M_Help_Key (int key)
{
	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Main_f ();
		break;

	case K_MOUSE1:
	case K_MWHEELDOWN:
	case K_UPARROW:
	case K_RIGHTARROW:
		m_entersound = true;
		if (++help_page >= NUM_HELP_PAGES)
			help_page = 0;
		break;

	case K_MWHEELUP:
	case K_DOWNARROW:
	case K_LEFTARROW:
		m_entersound = true;
		if (--help_page < 0)
			help_page = NUM_HELP_PAGES - 1;
		break;
	}
}

//=============================================================================
/* MODS MENU */

#define MAX_MODS_ON_SCREEN MAX_MENU_LINES

static int				 num_mods = 0;
static int				 first_mod = 0;
static int				 mods_cursor = 0;
int						 mods_prev_cursor = 0;
static int				 mod_loaded_from_menu = 0;
static menuticker_t		 m_mods_ticker;
static filelist_item_t **mods_sorted;

static int M_Mods_Compare (const void *a, const void *b)
{
	const filelist_item_t *left = *(filelist_item_t *const *)a;
	const filelist_item_t *right = *(filelist_item_t *const *)b;
	const char			  *left_name = Modlist_GetFullName (left);
	const char			  *right_name = Modlist_GetFullName (right);
	int					   result = q_strcasecmp (left_name ? left_name : left->name, right_name ? right_name : right->name);

	return result ? result : q_strcasecmp (left->name, right->name);
}

static void M_Menu_Mods_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_mods;
	m_entersound = true;
	num_mods = 0;
	VEC_CLEAR (mods_sorted);
	for (filelist_item_t *item = modlist; item; item = item->next)
	{
		VEC_PUSH (mods_sorted, item);
		++num_mods;
	}
	if (num_mods > 1)
		qsort (mods_sorted, num_mods, sizeof (*mods_sorted), M_Mods_Compare);
	first_mod = 0;
	mods_cursor = 0;
	mods_prev_cursor = 0;

	M_Ticker_Init (&m_mods_ticker);
}

static void M_Mods_Draw (cb_context_t *cbx)
{
	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	qpic_t *p = Draw_CachePic ("gfx/p_mods.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);
	int mod_index = -first_mod;
	int mods_height = q_min (MAX_MODS_ON_SCREEN, num_mods - first_mod);

	if (mods_prev_cursor != mods_cursor)
	{
		mods_prev_cursor = mods_cursor;
		M_Ticker_Init (&m_mods_ticker);
	}
	else
		M_Ticker_Update (&m_mods_ticker);

	// to trigger scroll faster
	M_Ticker_Update (&m_mods_ticker);

	for (int i = 0; i < num_mods; ++i)
	{
		filelist_item_t *item = mods_sorted[i];
		if (mod_index >= MAX_MODS_ON_SCREEN)
			break;
		if (mod_index >= 0)
		{
			const char *fullname = Modlist_GetFullName (item);

			const qboolean selected = (mods_cursor - first_mod == mod_index);

			M_PrintScroll (
				cbx, MENU_LABEL_X, 32 + mod_index * CHARACTER_SIZE, 32 * CHARACTER_SIZE, fullname ? fullname : item->name,
				selected ? m_mods_ticker.scroll_time : 0.0, true);
		}
		++mod_index;
	}

	M_Mouse_UpdateListCursor (&mods_cursor, 12, 400, 32, CHARACTER_SIZE, mods_height, first_mod);
	Draw_Character (cbx, MENU_CURSOR_X, 32 + (mods_cursor - first_mod) * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
	if (num_mods > MAX_MODS_ON_SCREEN)
		M_DrawScrollbar (cbx, MENU_SCROLLBAR_X, 32 + 8, (float)(first_mod) / (float)(num_mods - MAX_MODS_ON_SCREEN), MAX_MODS_ON_SCREEN - 2);
}

static void M_Mods_Key (int key)
{
	if (M_Ticker_Key (&m_mods_ticker, key))
		return;

	if (M_HandleScrollBarKeys (key, &mods_cursor, &first_mod, num_mods, MAX_MODS_ON_SCREEN))
		return;

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Main_f ();
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		if (mods_cursor < num_mods)
		{
			Cbuf_AddText ("game \"");
			Cbuf_AddText (mods_sorted[mods_cursor]->name);
			Cbuf_AddText ("\"\n");
			mod_loaded_from_menu = 1;
			m_state = m_main;
		}
		break;
	}
}

//=============================================================================
/* MAPS MENU (from Ironwail) */

#define MAPLIST_X		 8
#define MAPLIST_TOP		 32
#define MAPLIST_COLS	 (38 + 6)
#define MAPLIST_NAMECOLS (14 + 4)
#define MAPLIST_VIEWSIZE 19

/*
================
M_DrawQuakeBar
================
*/
static void M_DrawQuakeBar (cb_context_t *cbx, int x, int y, int cols)
{
	Draw_Character (cbx, x, y, '\35');
	x += CHARACTER_SIZE;
	cols -= 2;
	while (cols-- > 0)
	{
		Draw_Character (cbx, x, y, '\36');
		x += CHARACTER_SIZE;
	}
	Draw_Character (cbx, x, y, '\37');
}

/*
================
M_DrawEllipsisBar
================
*/
static void M_DrawEllipsisBar (cb_context_t *cbx, int x, int y, int cols)
{
	while (cols > 0)
	{
		Draw_Character (cbx, x, y, '.' | 0x80);
		cols -= 2;
		x += CHARACTER_SIZE * 2;
	}
}

typedef struct
{
	const char			  *name;
	const filelist_item_t *source;
	int					   mapidx;
	qboolean			   active;
} mapitem_t;

static struct
{
	int			 cursor;
	int			 scroll;
	int			 numitems;
	int			 mapcount; // not all items represent actual maps!
	int			 prev_cursor;
	menuticker_t ticker;
	mapitem_t	*items;
	struct
	{
		int	 len;
		char text[33];
	} search;
} mapsmenu;

static const char *M_Maps_GetMessage (const mapitem_t *item)
{
	if (!item->source)
		return item->name;
	return ExtraMaps_GetMessage (item->source);
}

static qboolean M_Maps_IsActive (const char *map)
{
	return cls.state == ca_connected && cls.signon == SIGNONS && !strcmp (cl.mapname, map);
}

static void M_Maps_AddDecoration (const char *text)
{
	mapitem_t item;
	memset (&item, 0, sizeof (item));
	item.name = text;
	item.mapidx = -1;
	VEC_PUSH (mapsmenu.items, item);
	mapsmenu.numitems++;
}

static void M_Maps_AddSeparator (maptype_t before, maptype_t after)
{
#define QBAR "\35\36\37"

	if (after >= MAPTYPE_ID_START)
	{
		if (before < MAPTYPE_ID_START)
		{
			M_Maps_AddDecoration ("");
			M_Maps_AddDecoration (QBAR " Original Quake levels " QBAR);
		}
		M_Maps_AddDecoration ("");
	}
	else if (after >= MAPTYPE_CUSTOM_ID_START && before < MAPTYPE_CUSTOM_ID_START)
	{
		M_Maps_AddDecoration ("");
		M_Maps_AddDecoration (QBAR " Custom Quake levels " QBAR);
		M_Maps_AddDecoration ("");
	}
	else if (after >= MAPTYPE_MOD_START && before < MAPTYPE_MOD_START)
	{
		M_Maps_AddDecoration ("");
		M_Maps_AddDecoration (QBAR " Official mod levels " QBAR);
		M_Maps_AddDecoration ("");
	}

#undef QBAR
}

static qboolean M_Maps_IsSelectable (int index)
{
	return mapsmenu.items[index].source != NULL;
}

static qboolean M_Maps_Match (int index)
{
	return mapsmenu.items[index].mapidx >= 0 && ExtraMaps_Match (mapsmenu.items[index].source, mapsmenu.search.text);
}

static void M_Maps_ClearSearch (void)
{
	mapsmenu.search.len = 0;
	mapsmenu.search.text[0] = '\0';
}

static int M_Maps_GetOverflow (void)
{
	return mapsmenu.numitems - MAPLIST_VIEWSIZE;
}

static void M_Maps_ClampScroll (void)
{
	mapsmenu.scroll = CLAMP (0, mapsmenu.scroll, q_max (M_Maps_GetOverflow (), 0));
}

static void M_Maps_AutoScroll (void)
{
	if (mapsmenu.numitems <= MAPLIST_VIEWSIZE)
		return;
	if (mapsmenu.cursor < mapsmenu.scroll)
	{
		mapsmenu.scroll = mapsmenu.cursor;
		// show decorations right above the selected item (e.g. a section header)
		while (mapsmenu.scroll > 0 && mapsmenu.scroll > mapsmenu.cursor - MAPLIST_VIEWSIZE + 1 && !M_Maps_IsSelectable (mapsmenu.scroll - 1))
			--mapsmenu.scroll;
	}
	else if (mapsmenu.cursor >= mapsmenu.scroll + MAPLIST_VIEWSIZE)
		mapsmenu.scroll = mapsmenu.cursor - MAPLIST_VIEWSIZE + 1;
	M_Maps_ClampScroll ();
}

static void M_Maps_CenterCursor (void)
{
	if (mapsmenu.cursor >= MAPLIST_VIEWSIZE)
		mapsmenu.scroll = mapsmenu.cursor - MAPLIST_VIEWSIZE / 2; // keep centered
	else
		mapsmenu.scroll = 0;
	M_Maps_ClampScroll ();
}

static qboolean M_Maps_SelectNextMatch (qboolean (*match_fn) (int idx), int start, int dir, qboolean wrap)
{
	int i, j;

	if (mapsmenu.numitems <= 0)
		return false;

	if (!wrap)
		start = CLAMP (0, start, mapsmenu.numitems - 1);

	for (i = 0, j = start; i < mapsmenu.numitems; i++, j += dir)
	{
		if (j < 0)
		{
			if (!wrap)
				return false;
			j = mapsmenu.numitems - 1;
		}
		else if (j >= mapsmenu.numitems)
		{
			if (!wrap)
				return false;
			j = 0;
		}
		if (!M_Maps_IsSelectable (j))
			continue;
		if (!match_fn || match_fn (j))
		{
			mapsmenu.cursor = j;
			M_Maps_AutoScroll ();
			return true;
		}
	}

	return false;
}

static qboolean M_Maps_SelectNextSearchMatch (int start, int dir)
{
	return M_Maps_SelectNextMatch (M_Maps_Match, start, dir, true);
}

static qboolean M_Maps_SelectNextActive (int start, int dir, qboolean wrap)
{
	return M_Maps_SelectNextMatch (NULL, start, dir, wrap);
}

static void M_Maps_UpdateMouseSelection (void)
{
	if (mapsmenu.cursor < mapsmenu.scroll)
		M_Maps_SelectNextActive (mapsmenu.scroll, 1, false);
	else if (mapsmenu.cursor >= mapsmenu.scroll + MAPLIST_VIEWSIZE)
		M_Maps_SelectNextActive (mapsmenu.scroll + MAPLIST_VIEWSIZE - 1, -1, false);
}

static void M_Maps_Init (void)
{
	int				 i, active;
	maptype_t		 type, prev_type;
	filelist_item_t *item;

	M_Maps_ClearSearch ();
	mapsmenu.cursor = -1;
	mapsmenu.scroll = 0;
	mapsmenu.numitems = 0;
	mapsmenu.mapcount = 0;
	VEC_CLEAR (mapsmenu.items);

	M_Ticker_Init (&mapsmenu.ticker);

	for (i = 0, active = -1, prev_type = (maptype_t)-1; (item = ExtraMaps_NextLevel (&i)) != NULL;)
	{
		mapitem_t map;

		type = ExtraMaps_GetType (item);
		if (prev_type != (maptype_t)-1 && prev_type != type)
			M_Maps_AddSeparator (prev_type, type);
		prev_type = type;

		map.name = item->name;
		map.active = M_Maps_IsActive (item->name);
		map.source = item;
		map.mapidx = mapsmenu.mapcount++;
		if (map.active)
			active = VEC_SIZE (mapsmenu.items);
		if ((map.active && !cls.demoplayback) || (mapsmenu.cursor == -1 && ExtraMaps_IsStart (type)))
			mapsmenu.cursor = VEC_SIZE (mapsmenu.items);
		VEC_PUSH (mapsmenu.items, map);
		mapsmenu.numitems++;
	}

	if (mapsmenu.cursor == -1)
		mapsmenu.cursor = (active != -1) ? active : 0;

	M_Maps_CenterCursor ();

	mapsmenu.prev_cursor = mapsmenu.cursor;
}

static void M_Menu_Maps_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_maps;
	m_entersound = true;
	M_Maps_Init ();
}

static void M_Menu_Maps_Cmd_f (void)
{
	M_Menu_Maps_f ();

	// handle optional map argument
	if (Cmd_Argc () >= 2)
	{
		char   mapname[MAX_QPATH];
		size_t i;

		COM_StripExtension (Cmd_Argv (1), mapname, sizeof (mapname));

		for (i = 0; i < VEC_SIZE (mapsmenu.items); i++)
			if (q_strcasecmp (mapname, mapsmenu.items[i].name) == 0)
				break;

		if (i == VEC_SIZE (mapsmenu.items))
		{
			Con_SafePrintf ("Couldn't find map \"%s\".\n", mapname);
			return;
		}

		mapsmenu.cursor = i;
		M_Maps_CenterCursor ();
		M_SetSkillMenuMap (mapname);
		M_Menu_Skill_f ();
	}
}

static void M_Maps_UpdateMouse (void)
{
	int i, yrel, numvis;

	if (scrollbar_grab || slider_grab)
		return;
	if (m_mouse_x < MAPLIST_X - CHARACTER_SIZE || m_mouse_x > MAPLIST_X + MAPLIST_COLS * CHARACTER_SIZE)
		return;

	yrel = m_mouse_y - MAPLIST_TOP;
	numvis = q_min (mapsmenu.scroll + MAPLIST_VIEWSIZE, mapsmenu.numitems) - mapsmenu.scroll;
	if (!numvis || yrel < 0)
		return;
	i = yrel / CHARACTER_SIZE;
	if (i >= numvis)
		return;

	i += mapsmenu.scroll;
	if (M_Maps_IsSelectable (i))
	{
		m_mouse_hover_state = m_state;
		m_mouse_hover_cursor = &mapsmenu.cursor;
		m_mouse_hover_value = i;
	}
	if (!m_mouse_moved)
		return;
	if (mapsmenu.cursor == i)
		return;

	if (!M_Maps_IsSelectable (i))
	{
		// snap to the closest selectable item instead (from Ironwail)
		int firstvis = mapsmenu.scroll;
		int before, after;
		yrel += firstvis * CHARACTER_SIZE;

		for (before = i - 1; before >= firstvis; before--)
			if (M_Maps_IsSelectable (before))
				break;
		for (after = i + 1; after < firstvis + numvis; after++)
			if (M_Maps_IsSelectable (after))
				break;

		if (before >= firstvis && after < firstvis + numvis)
		{
			int distbefore = yrel - CHARACTER_SIZE / 2 - before * CHARACTER_SIZE;
			int distafter = after * CHARACTER_SIZE + CHARACTER_SIZE / 2 - yrel;
			i = distbefore < distafter ? before : after;
		}
		else if (before >= firstvis)
			i = before;
		else if (after < firstvis + numvis)
			i = after;
		else
			return;

		if (mapsmenu.cursor == i)
			return;
	}

	mapsmenu.cursor = i;
}

static void M_Maps_Draw (cb_context_t *cbx)
{
	const char *str;
	int			x, y, i, j, cols;
	int			firstvis, numvis;
	int			firstvismap, numvismaps;
	int			namecols, desccols;

	M_Maps_UpdateMouse ();

	x = MAPLIST_X;
	cols = MAPLIST_COLS;
	namecols = MAPLIST_NAMECOLS;
	desccols = cols - 1 - namecols;

	if (mapsmenu.prev_cursor != mapsmenu.cursor)
	{
		mapsmenu.prev_cursor = mapsmenu.cursor;
		M_Ticker_Init (&mapsmenu.ticker);
	}
	else
		M_Ticker_Update (&mapsmenu.ticker);

	// to trigger scroll faster
	M_Ticker_Update (&mapsmenu.ticker);

	M_PrintWhite (cbx, x, 8, "Levels");
	M_DrawQuakeBar (cbx, x - 8, 16, namecols + 1);
	M_DrawQuakeBar (cbx, x + namecols * CHARACTER_SIZE, 16, cols + 1 - namecols);

	y = MAPLIST_TOP;

	firstvismap = -1;
	numvismaps = 0;
	firstvis = mapsmenu.scroll;
	numvis = q_min (firstvis + MAPLIST_VIEWSIZE, mapsmenu.numitems) - firstvis;
	for (i = 0; i < numvis; i++)
	{
		int				 idx = i + firstvis;
		const mapitem_t *item = &mapsmenu.items[idx];
		const char		*message = M_Maps_GetMessage (item);
		int				 mask = item->active ? 128 : 0;
		qboolean		 selected = (idx == mapsmenu.cursor);

		if (!item->source)
		{
			M_PrintWhite (cbx, x + (cols - strlen (item->name)) / 2 * CHARACTER_SIZE, y + i * CHARACTER_SIZE, item->name);
		}
		else
		{
			char buf[256];
			if (mapsmenu.search.len > 0)
				COM_TintSubstring (item->name, mapsmenu.search.text, buf, sizeof (buf));
			else
				q_strlcpy (buf, item->name, sizeof (buf));

			if (firstvismap == -1)
				firstvismap = item->mapidx;
			numvismaps++;

			for (j = 0; j < namecols - 2 && buf[j]; j++)
				Draw_Character (cbx, x + j * CHARACTER_SIZE, y + i * CHARACTER_SIZE, buf[j] ^ mask);

			if (!message || message[0])
			{
				if (!message) // still parsing, show a fully dotted line
				{
					memset (buf, '.' | 0x80, desccols);
					buf[desccols] = '\0';
				}
				else if (mapsmenu.search.len > 0)
					COM_TintSubstring (message, mapsmenu.search.text, buf, sizeof (buf));
				else
					q_strlcpy (buf, message, sizeof (buf));

				GL_SetCanvasColor (1, 1, 1, 0.375f);
				for (/**/; j < namecols; j++)
					Draw_Character (cbx, x + j * CHARACTER_SIZE, y + i * CHARACTER_SIZE, '.' | mask);
				if (message)
					GL_SetCanvasColor (1, 1, 1, 1);

				M_PrintScroll (
					cbx, x + namecols * CHARACTER_SIZE, y + i * CHARACTER_SIZE, desccols * CHARACTER_SIZE, buf, selected ? mapsmenu.ticker.scroll_time : 0.0,
					true);

				if (!message)
					GL_SetCanvasColor (1, 1, 1, 1);
			}
		}

		if (selected)
			Draw_Character (cbx, x - CHARACTER_SIZE, y + i * CHARACTER_SIZE, 12 + ((int)(realtime * 4) & 1));
	}

	str = va ("%d-%d of %d", firstvismap + 1, firstvismap + numvismaps, mapsmenu.mapcount);
	M_Print (cbx, x + (cols - strlen (str)) * CHARACTER_SIZE, 8, str);

	if (M_Maps_GetOverflow () > 0)
	{
		M_DrawScrollbar (
			cbx, x + cols * CHARACTER_SIZE - CHARACTER_SIZE, y + CHARACTER_SIZE, (float)mapsmenu.scroll / (float)M_Maps_GetOverflow (), MAPLIST_VIEWSIZE - 2);

		if (mapsmenu.scroll > 0)
			M_DrawEllipsisBar (cbx, x, y - CHARACTER_SIZE, cols);
		if (mapsmenu.scroll + MAPLIST_VIEWSIZE < mapsmenu.numitems)
			M_DrawEllipsisBar (cbx, x, y + MAPLIST_VIEWSIZE * CHARACTER_SIZE, cols);
	}

	if (mapsmenu.search.len > 0)
	{
		int ofs = q_max (0, mapsmenu.search.len + 1 - namecols);
		int cy = y + MAPLIST_VIEWSIZE * CHARACTER_SIZE + 4;
		M_DrawTextBox (cbx, x - CHARACTER_SIZE, cy - CHARACTER_SIZE, namecols, 1);
		for (i = ofs; i < mapsmenu.search.len; i++)
			Draw_Character (cbx, x + (i - ofs) * CHARACTER_SIZE, cy, mapsmenu.search.text[i]);
		Draw_Character (cbx, x + (i - ofs) * CHARACTER_SIZE, cy, 10 + ((int)(realtime * 4) & 1));
	}
}

static qboolean M_Maps_ListKey (int key)
{
	qboolean overflow = M_Maps_GetOverflow () > 0;

	switch (key)
	{
	case K_BACKSPACE:
		if (mapsmenu.search.len)
		{
			if (keydown[K_CTRL])
				M_Maps_ClearSearch ();
			else
			{
				mapsmenu.search.len--;
				mapsmenu.search.text[mapsmenu.search.len] = '\0';
			}
			return true;
		}
		return false;

	case K_ESCAPE:
	case K_BBUTTON:
	case K_MOUSE4:
	case K_MOUSE2:
		if (mapsmenu.search.len)
		{
			M_Maps_ClearSearch ();
			return true;
		}
		return false;

	case K_HOME:
	case K_KP_HOME:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (0, 1);
		else
		{
			M_Maps_SelectNextActive (0, 1, false);
			mapsmenu.scroll = 0;
			M_Maps_AutoScroll ();
		}
		return true;

	case K_END:
	case K_KP_END:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (mapsmenu.numitems - 1, -1);
		else
			M_Maps_SelectNextActive (mapsmenu.numitems - 1, -1, false);
		return true;

	case K_PGDN:
	case K_KP_PGDN:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (mapsmenu.cursor + 1, 1);
		else
		{
			qboolean sel;
			if (mapsmenu.cursor - mapsmenu.scroll < MAPLIST_VIEWSIZE - 1)
				sel = M_Maps_SelectNextActive (mapsmenu.scroll + MAPLIST_VIEWSIZE - 1, 1, false);
			else
				sel = M_Maps_SelectNextActive (mapsmenu.cursor + MAPLIST_VIEWSIZE - 1, 1, false);
			if (!sel)
				M_Maps_SelectNextActive (mapsmenu.numitems - 1, -1, false);
		}
		return true;

	case K_PGUP:
	case K_KP_PGUP:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (mapsmenu.cursor - 1, -1);
		else
		{
			qboolean sel;
			if (mapsmenu.cursor > mapsmenu.scroll)
				sel = M_Maps_SelectNextActive (mapsmenu.scroll, -1, false);
			else
				sel = M_Maps_SelectNextActive (mapsmenu.cursor - MAPLIST_VIEWSIZE + 1, -1, false);
			if (!sel)
				M_Maps_SelectNextActive (0, 1, false);
		}
		return true;

	case K_UPARROW:
	case K_KP_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (mapsmenu.cursor - 1, -1);
		else
			M_Maps_SelectNextActive (mapsmenu.cursor - 1, -1, true);
		return true;

	case K_DOWNARROW:
	case K_KP_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (mapsmenu.search.len)
			M_Maps_SelectNextSearchMatch (mapsmenu.cursor + 1, 1);
		else
			M_Maps_SelectNextActive (mapsmenu.cursor + 1, 1, true);
		return true;

	case K_MWHEELUP:
		if (!overflow)
			return false;
		mapsmenu.scroll -= 3;
		M_Maps_ClampScroll ();
		M_Maps_UpdateMouseSelection ();
		return true;

	case K_MWHEELDOWN:
		if (!overflow)
			return false;
		mapsmenu.scroll += 3;
		M_Maps_ClampScroll ();
		M_Maps_UpdateMouseSelection ();
		return true;

	default:
		return false;
	}
}

static void M_Maps_Key (int key)
{
	if (M_Maps_ListKey (key))
		return;

	if (M_Ticker_Key (&mapsmenu.ticker, key))
		return;

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_SinglePlayer_f ();
		break;

	case K_MOUSE1:
		if (M_InScrollbar () && M_Maps_GetOverflow () > 0 && !slider_grab)
		{
			scrollbar_grab = true;
			int clamped_mouse = CLAMP (scrollbar_y + 8, m_mouse_y, scrollbar_y + scrollbar_size - 8);
			mapsmenu.scroll = (int)(((float)clamped_mouse - scrollbar_y - 8) / (scrollbar_size - 16) * M_Maps_GetOverflow () + 0.5f);
			M_Maps_ClampScroll ();
			M_Maps_UpdateMouseSelection ();
			break;
		}
		/* fall through */
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		if (mapsmenu.numitems > 0 && mapsmenu.items[mapsmenu.cursor].source)
		{
			const char *mapname = mapsmenu.items[mapsmenu.cursor].name;
			M_Maps_ClearSearch ();
			m_entersound = true;
			M_SetSkillMenuMap (mapname);
			M_Menu_Skill_f ();
		}
		else
			S_LocalSound ("misc/menu3.wav");
		break;

	default:
		break;
	}
}

static void M_Maps_Char (int key)
{
	int start;

	if (mapsmenu.numitems <= 0)
		return;

	// don't allow starting with a space
	if (mapsmenu.search.len <= 0 && key == ' ')
		return;

	if (mapsmenu.search.len >= (int)sizeof (mapsmenu.search.text) - 1)
	{
		S_LocalSound ("misc/menu2.wav");
		return;
	}

	mapsmenu.search.text[mapsmenu.search.len++] = (char)key;
	mapsmenu.search.text[mapsmenu.search.len] = '\0';

	if (mapsmenu.cursor < 0)
		mapsmenu.cursor = 0;

	start = mapsmenu.cursor;
	if (mapsmenu.search.len == 1)
		start++;

	if (!M_Maps_SelectNextSearchMatch (start, 1))
	{
		mapsmenu.search.len--;
		mapsmenu.search.text[mapsmenu.search.len] = '\0';
		S_LocalSound ("misc/menu2.wav");
	}
}

static qboolean M_Maps_TextEntry (void)
{
	return true;
}

//=============================================================================
/* SKILL MENU */

static int			  m_skill_cursor;
static qboolean		  m_skill_usegfx;
static qboolean		  m_skill_usecustomtitle;
static char			  m_skill_mapname[MAX_QPATH];
static char			  m_skill_maptitle[1024];
static menuticker_t	  m_skill_ticker;
static enum m_state_e m_skill_prevmenu;

static void M_SetSkillMenuMap (const char *name)
{
	q_strlcpy (m_skill_mapname, name, sizeof (m_skill_mapname));
	if (!Mod_LoadMapDescription (m_skill_maptitle, sizeof (m_skill_maptitle), name) || !m_skill_maptitle[0])
		q_strlcpy (m_skill_maptitle, name, sizeof (m_skill_maptitle));
}

static void M_Menu_Skill_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_skill_prevmenu = m_state;
	m_state = m_skill;
	m_entersound = true;
	M_Ticker_Init (&m_skill_ticker);

	m_skill_cursor = (int)skill.value;
	m_skill_cursor = CLAMP (0, m_skill_cursor, 3);
}

static void M_Skill_Draw (cb_context_t *cbx)
{
	int		x, y, f;
	qpic_t *p;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic (m_skill_usecustomtitle ? "gfx/p_skill.lmp" : "gfx/ttl_sgl.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	x = 72;
	y = 32;

	M_Ticker_Update (&m_skill_ticker);
	M_PrintScroll (cbx, x, y, 30 * CHARACTER_SIZE, m_skill_maptitle, m_skill_ticker.scroll_time, false);

	y += 16;

	if (m_skill_usegfx)
	{
		M_DrawTransPic (cbx, x, y, Draw_CachePic ("gfx/skillmenu.lmp"));
		M_Mouse_UpdateListCursor (&m_skill_cursor, x, 320, y, 20, 4, 0);
		f = (int)(realtime * 10) % 6;
		M_DrawTransPic (cbx, x - 18, y + m_skill_cursor * 20, Draw_CachePic (va ("gfx/menudot%i.lmp", f + 1)));
	}
	else
	{
		static const char *const skills[] = {
			"EASY",
			"NORMAL",
			"HARD",
			"NIGHTMARE",
		};

		for (f = 0; f < 4; f++)
			M_Print (cbx, x, y + f * 16 + 2, skills[f]);

		M_Mouse_UpdateListCursor (&m_skill_cursor, x, 320, y, 16, 4, 0);
		Draw_Character (cbx, x - 16, y + m_skill_cursor * 16 + 4, 12 + ((int)(realtime * 4) & 1));
	}
}

static void M_Skill_Key (int key)
{
	if (M_Ticker_Key (&m_skill_ticker, key))
		return;

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		m_state = m_skill_prevmenu;
		m_entersound = true;
		break;

	case K_DOWNARROW:
	case K_KP_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		if (++m_skill_cursor > 3)
			m_skill_cursor = 0;
		break;

	case K_UPARROW:
	case K_KP_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		if (--m_skill_cursor < 0)
			m_skill_cursor = 3;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		IN_Activate ();
		key_dest = key_game;
		m_state = m_none;
		if (sv.active)
			Cbuf_AddText ("disconnect\n");
		Cbuf_AddText (va ("skill %d\n", m_skill_cursor));
		Cbuf_AddText ("maxplayers 1\n");
		Cbuf_AddText ("deathmatch 0\n");
		Cbuf_AddText ("coop 0\n");
		Cbuf_AddText (va ("map \"%s\"\n", m_skill_mapname));
		break;
	}
}

//=============================================================================
/* QUIT MENU */

static int			  msg_number;
static enum m_state_e m_quit_prevstate;
static qboolean		  was_in_menus;

void M_Menu_Quit_f (void)
{
	if (m_state == m_quit)
		return;
	if (!mod_loaded_from_menu)
	{
		was_in_menus = (key_dest == key_menu);
		IN_DeactivateForMenu ();
		key_dest = key_menu;
		m_quit_prevstate = m_state;
		m_state = m_quit;
		m_entersound = true;
		msg_number = COM_Rand () & 7;
	}
	else
	{
		mod_loaded_from_menu = 0;
		Cbuf_AddText ("game " GAMENAME "\n");
	}
}

static void M_Quit_Cancel (void)
{
	if (was_in_menus)
	{
		m_state = m_quit_prevstate;
		m_entersound = true;
	}
	else
	{
		IN_Activate ();
		key_dest = key_game;
		m_state = m_none;
	}
}

static void M_Quit_Key (int key)
{
	switch (key)
	{
	case 'n':
	case 'N':
	case K_ESCAPE:
		M_Quit_Cancel ();
		break;

	case 'y':
	case 'Y':
	case K_SPACE:
		m_is_quitting = true;
		IN_DeactivateForMenu ();
		key_dest = key_console;
		Cbuf_InsertText ("quit");
		break;

	default:
		break;
	}
}

static void M_Quit_Draw (cb_context_t *cbx) // johnfitz -- modified for new quit message
{
	char msg1[40];
	char msg2[] = "by Artem \"f1ames0ff\""; /* msg2/msg3 are mostly [40] */
	char msg3[] = "Press space or Y to exit";
	int	 boxlen;

	if (was_in_menus)
	{
		m_state = m_quit_prevstate;
		m_recursiveDraw = true;
		M_Draw (cbx);
		m_state = m_quit;
	}

	q_snprintf (msg1, sizeof (msg1), ENGINE_NAME_AND_VER);

	// okay, this is kind of fucked up.  M_DrawTextBox will always act as if
	// width is even. Also, the width and lines values are for the interior of the box,
	// but the x and y values include the border.
	boxlen = q_max (strlen (msg1), q_max ((sizeof (msg2) - 1), (sizeof (msg3) - 1))) + 1;
	if (boxlen & 1)
		boxlen++;
	M_DrawTextBox (cbx, 160 - 4 * (boxlen + 2), 76, boxlen, 5);

	// now do the text
	M_Print (cbx, 160 - 4 * strlen (msg1), 88, msg1);
	M_Print (cbx, 160 - 4 * (sizeof (msg2) - 1), 96, msg2);
	M_PrintWhite (cbx, 160 - 4 * (sizeof (msg3) - 1), 112, msg3);
}

//=============================================================================
/* LAN CONFIG MENU */

static int lan_config_cursor = -1;
#define NUM_LANCONFIG_CMDS 4

static int	lan_config_port;
static char lan_config_portname[5 + 1];
static char lan_config_joinname[36 + 1];

static void M_Menu_LanConfig_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_lanconfig;
	if (lan_config_cursor == -1)
	{
		if (JoiningGame && TCPIPConfig)
			lan_config_cursor = 2;
		else
			lan_config_cursor = 1;
	}
	if (StartingGame && lan_config_cursor >= 2)
		lan_config_cursor = 1;
	lan_config_port = DEFAULTnet_hostport;
	q_snprintf (lan_config_portname, sizeof (lan_config_portname), "%u", lan_config_port);

	m_return_onerror = false;
	m_return_reason[0] = 0;
}

static void M_LanConfig_Draw (cb_context_t *cbx)
{
	qpic_t	   *p;
	int			basex;
	int			y;
	int			numaddresses, i;
	qhostaddr_t addresses[16];
	const char *startJoin;
	const char *protocol;

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_multi.lmp");
	basex = (320 - p->width) / 2;
	M_DrawPic (cbx, basex, 4, p);
	//
	basex = 72 - (8 * 2);

	if (StartingGame)
		startJoin = "New Game";
	else
		startJoin = "Join Game";
	if (IPXConfig)
		protocol = "IPX";
	else
		protocol = "TCP/IP";
	M_Print (cbx, basex + 8 * 4, 32, va ("%s - %s", startJoin, protocol));
	basex += 8;

	y = 52;
	M_Print (cbx, basex, y, "Address:");
	numaddresses = NET_ListAddresses (addresses, countof (addresses));
	if (!numaddresses)
	{
		M_Print (cbx, basex + 9 * 8, y, "NONE KNOWN");
		y += 8;
	}
	else
		for (i = 0; i < numaddresses; i++)
		{
			M_Print (cbx, basex + 9 * 8, y, addresses[i]);
			y += 8;
		}

	y += 8; // for the port's box
	M_Print (cbx, basex, y, "Port");
	M_DrawTextBox (cbx, basex + 8 * 8, y - 8, countof (lan_config_portname), 1);
	M_Print (cbx, basex + 9 * 8, y, lan_config_portname);
	M_Mouse_UpdateCursor (&lan_config_cursor, basex, 320, y, 8, 0);
	if (lan_config_cursor == 0)
	{
		Draw_Character (cbx, basex + 9 * 8 + 8 * strlen (lan_config_portname), y, 10 + ((int)(realtime * 4) & 1));
		Draw_Character (cbx, basex - 8, y, 12 + ((int)(realtime * 4) & 1));
	}
	y += 20;

	if (JoiningGame)
	{
		M_Print (cbx, basex, y, "Search for local games...");
		M_Mouse_UpdateCursor (&lan_config_cursor, basex, 320, y, 8, 1);
		if (lan_config_cursor == 1)
			Draw_Character (cbx, basex - 8, y, 12 + ((int)(realtime * 4) & 1));
		y += 8;

		M_Print (cbx, basex, y, "Search for public games...");
		M_Mouse_UpdateCursor (&lan_config_cursor, basex, 320, y, 8, 2);
		if (lan_config_cursor == 2)
			Draw_Character (cbx, basex - 8, y, 12 + ((int)(realtime * 4) & 1));
		y += 24;

		M_Print (cbx, basex, y, "Join game at:");
		y += 12;
		M_DrawTextBox (cbx, basex + 8, y - 8, countof (lan_config_joinname), 1);
		M_Print (cbx, basex + 16, y, lan_config_joinname);
		M_Mouse_UpdateCursor (&lan_config_cursor, basex, 320, y, 8, 3);
		if (lan_config_cursor == 3)
		{
			Draw_Character (cbx, basex + 16 + 8 * strlen (lan_config_joinname), y, 10 + ((int)(realtime * 4) & 1));
			Draw_Character (cbx, basex - 8, y, 12 + ((int)(realtime * 4) & 1));
		}
		y += 16;
	}
	else
	{
		M_DrawTextBox (cbx, basex, y - 8, 2, 1);
		M_Print (cbx, basex + 8, y, "OK");
		M_Mouse_UpdateCursor (&lan_config_cursor, basex, 320, y, 8, 1);
		if (lan_config_cursor == 1)
			Draw_Character (cbx, basex - 8, y, 12 + ((int)(realtime * 4) & 1));
		y += 16;
	}

	if (*m_return_reason)
		M_PrintWhite (cbx, basex, 148, m_return_reason);
}

static void validate_LanConfig (void)
{
	char raw_join_address[countof (lan_config_joinname)];

	// make a copy of lan_config_joinname because of q_strsplit / q_strtrim on-place modification.
	q_strlcpy (raw_join_address, lan_config_joinname, sizeof (lan_config_joinname));

	// Check if the resulting raw_join_address is of form 'address:port', in this case overwrite lan_config_portname with it
	size_t nb_parts = 0;

	char **split_address = q_strsplit (raw_join_address, ":", &nb_parts);

	if (nb_parts == 2 && atoi (split_address[1]) > 0 && atoi (split_address[1]) <= 65535)
	{
		// set join name from the first part:
		q_strlcpy (lan_config_joinname, q_strtrim (split_address[0]), sizeof (lan_config_joinname));

		// overwrite existing port value from the second part:
		q_strlcpy (lan_config_portname, q_strtrim (split_address[1]), sizeof (lan_config_portname));
	}
	else
	{
		q_strlcpy (lan_config_joinname, q_strtrim (raw_join_address), sizeof (lan_config_joinname));
	}
	Mem_Free (split_address);
}

static void M_LanConfig_Key (int key)
{
	int l;

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		lan_config_cursor--;
		if (lan_config_cursor < 0)
			lan_config_cursor = NUM_LANCONFIG_CMDS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		lan_config_cursor++;
		if (lan_config_cursor >= NUM_LANCONFIG_CMDS)
			lan_config_cursor = 0;
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		if (lan_config_cursor == 0)
			break;

		m_entersound = true;

		M_ConfigureNetSubsystem ();

		if (StartingGame)
		{
			if (lan_config_cursor == 1)
				M_Menu_MPGameOptions_f ();
		}
		else
		{
			if (lan_config_cursor == 1)
				M_Menu_Search_f (SLIST_LAN);
			else if (lan_config_cursor == 2)
				M_Menu_Search_f (SLIST_INTERNET);
			else if (lan_config_cursor == 3)
			{
				validate_LanConfig ();
				m_return_state = m_state;
				m_return_onerror = true;
				IN_Activate ();
				key_dest = key_game;
				m_state = m_none;
				Cbuf_AddText (va ("connect \"%s\"\n", lan_config_joinname));
			}
		}

		break;

	case K_BACKSPACE:
		if (lan_config_cursor == 0)
		{
			if (strlen (lan_config_portname))
				lan_config_portname[strlen (lan_config_portname) - 1] = 0;
		}

		if (lan_config_cursor == 3)
		{
			if (strlen (lan_config_joinname))
				lan_config_joinname[strlen (lan_config_joinname) - 1] = 0;
		}
		break;

	case 'v':
	case 'V':
		// Ctrl + v : paste a hostname
		if (lan_config_cursor == 3 &&
			(keydown[K_CTRL])
		)
		{
			const int current_joinname_size = strlen (lan_config_joinname);

			const int joinname_remaining_room_size = countof (lan_config_joinname) - 1 - current_joinname_size;

			if (joinname_remaining_room_size > 0)
			{
				char *clipboard_text = PL_GetClipboardData ();

				// append the existing clipboard text
				if (clipboard_text)
					q_strlcpy (lan_config_joinname + current_joinname_size, clipboard_text, joinname_remaining_room_size);

				// Check if the resulting raw_join_address is of form 'address:port', in this case overwrite lan_config_portname with it
				validate_LanConfig ();

				Mem_Free (clipboard_text);
			} // end if enough room to Ctrl+v
		}
		break;
	}

	if (StartingGame && lan_config_cursor >= 2)
	{
		if (key == K_UPARROW)
			lan_config_cursor = 1;
		else
			lan_config_cursor = 0;
	}

	l = atoi (lan_config_portname);
	if (l > 65535)
		l = lan_config_port;
	else
		lan_config_port = l;
	q_snprintf (lan_config_portname, sizeof (lan_config_portname), "%u", lan_config_port);
}

static void M_LanConfig_Char (int key)
{
	int l;

	switch (lan_config_cursor)
	{
	case 0:
		if (key < '0' || key > '9')
			return;
		l = strlen (lan_config_portname);
		// append one character, assure null-termination
		if (l < countof (lan_config_portname) - 1)
		{
			lan_config_portname[l + 1] = 0;
			lan_config_portname[l] = key;
		}
		break;
	case 3:
		l = strlen (lan_config_joinname);
		if (l < countof (lan_config_joinname) - 1)
		{
			lan_config_joinname[l + 1] = 0;
			lan_config_joinname[l] = key;
		}
		break;
	}
}

static qboolean M_LanConfig_TextEntry (void)
{
	return (lan_config_cursor == 0 || lan_config_cursor == 3);
}

//=============================================================================
/* GAME OPTIONS MENU */

typedef struct
{
	const char *name;
	const char *description;
} level_t;

static level_t levels[] = {
	{"start", "Entrance"}, // 0

	{"e1m1", "Slipgate Complex"}, // 1
	{"e1m2", "Castle of the Damned"},
	{"e1m3", "The Necropolis"},
	{"e1m4", "The Grisly Grotto"},
	{"e1m5", "Gloom Keep"},
	{"e1m6", "The Door To Chthon"},
	{"e1m7", "The House of Chthon"},
	{"e1m8", "Ziggurat Vertigo"},

	{"e2m1", "The Installation"}, // 9
	{"e2m2", "Ogre Citadel"},
	{"e2m3", "Crypt of Decay"},
	{"e2m4", "The Ebon Fortress"},
	{"e2m5", "The Wizard's Manse"},
	{"e2m6", "The Dismal Oubliette"},
	{"e2m7", "Underearth"},

	{"e3m1", "Termination Central"}, // 16
	{"e3m2", "The Vaults of Zin"},
	{"e3m3", "The Tomb of Terror"},
	{"e3m4", "Satan's Dark Delight"},
	{"e3m5", "Wind Tunnels"},
	{"e3m6", "Chambers of Torment"},
	{"e3m7", "The Haunted Halls"},

	{"e4m1", "The Sewage System"}, // 23
	{"e4m2", "The Tower of Despair"},
	{"e4m3", "The Elder God Shrine"},
	{"e4m4", "The Palace of Hate"},
	{"e4m5", "Hell's Atrium"},
	{"e4m6", "The Pain Maze"},
	{"e4m7", "Azure Agony"},
	{"e4m8", "The Nameless City"},

	{"end", "Shub-Niggurath's Pit"}, // 31

	{"dm1", "Place of Two Deaths"}, // 32
	{"dm2", "Claustrophobopolis"},
	{"dm3", "The Abandoned Base"},
	{"dm4", "The Bad Place"},
	{"dm5", "The Cistern"},
	{"dm6", "The Dark Zone"}};

// MED 01/06/97 added hipnotic levels
static level_t hipnoticlevels[] = {
	{"start", "Command HQ"}, // 0

	{"hip1m1", "The Pumping Station"}, // 1
	{"hip1m2", "Storage Facility"},
	{"hip1m3", "The Lost Mine"},
	{"hip1m4", "Research Facility"},
	{"hip1m5", "Military Complex"},

	{"hip2m1", "Ancient Realms"}, // 6
	{"hip2m2", "The Black Cathedral"},
	{"hip2m3", "The Catacombs"},
	{"hip2m4", "The Crypt"},
	{"hip2m5", "Mortum's Keep"},
	{"hip2m6", "The Gremlin's Domain"},

	{"hip3m1", "Tur Torment"}, // 12
	{"hip3m2", "Pandemonium"},
	{"hip3m3", "Limbo"},
	{"hip3m4", "The Gauntlet"},

	{"hipend", "Armagon's Lair"}, // 16

	{"hipdm1", "The Edge of Oblivion"} // 17
};

// PGM 01/07/97 added rogue levels
// PGM 03/02/97 added dmatch level
static level_t roguelevels[] = {{"start", "Split Decision"},   {"r1m1", "Deviant's Domain"}, {"r1m2", "Dread Portal"},		{"r1m3", "Judgement Call"},
								{"r1m4", "Cave of Death"},	   {"r1m5", "Towers of Wrath"},	 {"r1m6", "Temple of Pain"},	{"r1m7", "Tomb of the Overlord"},
								{"r2m1", "Tempus Fugit"},	   {"r2m2", "Elemental Fury I"}, {"r2m3", "Elemental Fury II"}, {"r2m4", "Curse of Osiris"},
								{"r2m5", "Wizard's Keep"},	   {"r2m6", "Blood Sacrifice"},	 {"r2m7", "Last Bastion"},		{"r2m8", "Source of Evil"},
								{"ctf1", "Division of Change"}};

typedef struct
{
	const char *description;
	int			firstLevel;
	int			levels;
} episode_t;

static episode_t episodes[] = {{"Welcome to Quake", 0, 1}, {"Doomed Dimension", 1, 8}, {"Realm of Black Magic", 9, 7}, {"Netherworld", 16, 7},
							   {"The Elder World", 23, 8}, {"Final Level", 31, 1},	   {"Deathmatch Arena", 32, 6}};

// MED 01/06/97  added hipnotic episodes
static episode_t hipnoticepisodes[] = {{"Scourge of Armagon", 0, 1}, {"Fortress of the Dead", 1, 5}, {"Dominion of Darkness", 6, 6},
									   {"The Rift", 12, 4},			 {"Final Level", 16, 1},		 {"Deathmatch Arena", 17, 1}};

// PGM 01/07/97 added rogue episodes
// PGM 03/02/97 added dmatch episode
static episode_t rogueepisodes[] = {{"Introduction", 0, 1}, {"Hell's Fortress", 1, 7}, {"Corridors of Time", 8, 8}, {"Deathmatch Arena", 16, 1}};

static int startepisode;
static int startlevel;
static int maxplayers;

static int mpgameoptions_cursor_table[] = {40, 56, 64, 72, 80, 88, 96, 112, 120};
#define NUM_MPGAMEOPTIONS 9
static int mpgameoptions_cursor;

static void M_Menu_MPGameOptions_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_mpgameoptions;
	if (maxplayers == 0)
		maxplayers = svs.maxclients;
	if (maxplayers < 2)
		maxplayers = 4;
}

static void M_MPGameOptions_Draw (cb_context_t *cbx)
{
	qpic_t *p;

#define OPTION_NAMES_CX	 64
#define OPTION_VALUES_CX 176

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	M_DrawTextBox (cbx, OPTION_NAMES_CX - 4, 32, 10, 1);
	M_Print (cbx, OPTION_NAMES_CX + 4, 40, "begin game");

	M_Print (cbx, OPTION_NAMES_CX, 56, "Max players");
	M_Print (cbx, OPTION_VALUES_CX, 56, va ("%i", maxplayers));

	M_Print (cbx, OPTION_NAMES_CX, 64, "Game Type");
	if (coop.value)
		M_Print (cbx, OPTION_VALUES_CX, 64, "Cooperative");
	else
		M_Print (cbx, OPTION_VALUES_CX, 64, "Deathmatch");

	M_Print (cbx, OPTION_NAMES_CX, 72, "Teamplay");
	if (rogue)
	{
		const char *msg;

		switch ((int)teamplay.value)
		{
		case 1:
			msg = "No Friendly Fire";
			break;
		case 2:
			msg = "Friendly Fire";
			break;
		case 3:
			msg = "Tag";
			break;
		case 4:
			msg = "Capture the Flag";
			break;
		case 5:
			msg = "One Flag CTF";
			break;
		case 6:
			msg = "Three Team CTF";
			break;
		default:
			msg = "Off";
			break;
		}
		M_Print (cbx, OPTION_VALUES_CX, 72, msg);
	}
	else
	{
		const char *msg;

		switch ((int)teamplay.value)
		{
		case 1:
			msg = "No Friendly Fire";
			break;
		case 2:
			msg = "Friendly Fire";
			break;
		default:
			msg = "Off";
			break;
		}
		M_Print (cbx, OPTION_VALUES_CX, 72, msg);
	}

	M_Print (cbx, OPTION_NAMES_CX, 80, "Skill");
	if (skill.value == 0)
		M_Print (cbx, OPTION_VALUES_CX, 80, "Easy difficulty");
	else if (skill.value == 1)
		M_Print (cbx, OPTION_VALUES_CX, 80, "Normal difficulty");
	else if (skill.value == 2)
		M_Print (cbx, OPTION_VALUES_CX, 80, "Hard difficulty");
	else
		M_Print (cbx, OPTION_VALUES_CX, 80, "Nightmare difficulty");

	M_Print (cbx, OPTION_NAMES_CX, 88, "Frag Limit");
	if (fraglimit.value == 0)
		M_Print (cbx, OPTION_VALUES_CX, 88, "none");
	else
		M_Print (cbx, OPTION_VALUES_CX, 88, va ("%i frags", (int)fraglimit.value));

	M_Print (cbx, OPTION_NAMES_CX, 96, "Time Limit");
	if (timelimit.value == 0)
		M_Print (cbx, OPTION_VALUES_CX, 96, "none");
	else
		M_Print (cbx, OPTION_VALUES_CX, 96, va ("%i minutes", (int)timelimit.value));

	M_Print (cbx, OPTION_NAMES_CX, 112, "Episode");
	// MED 01/06/97 added hipnotic episodes
	if (hipnotic)
		M_Print (cbx, OPTION_VALUES_CX, 112, hipnoticepisodes[startepisode].description);
	// PGM 01/07/97 added rogue episodes
	else if (rogue)
		M_Print (cbx, OPTION_VALUES_CX, 112, rogueepisodes[startepisode].description);
	else
		M_Print (cbx, OPTION_VALUES_CX, 112, episodes[startepisode].description);

	M_Print (cbx, OPTION_NAMES_CX, 120, "Level");
	// MED 01/06/97 added hipnotic episodes
	if (hipnotic)
	{
		M_Print (cbx, OPTION_VALUES_CX, 120, hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].description);
		M_Print (cbx, OPTION_VALUES_CX, 128, hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].name);
	}
	// PGM 01/07/97 added rogue episodes
	else if (rogue)
	{
		M_Print (cbx, OPTION_VALUES_CX, 120, roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].description);
		M_Print (cbx, OPTION_VALUES_CX, 128, roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].name);
	}
	else
	{
		M_Print (cbx, OPTION_VALUES_CX, 120, levels[episodes[startepisode].firstLevel + startlevel].description);
		M_Print (cbx, OPTION_VALUES_CX, 128, levels[episodes[startepisode].firstLevel + startlevel].name);
	}

	// line cursor
	for (int i = 0; i < NUM_MPGAMEOPTIONS; ++i)
		M_Mouse_UpdateCursor (&mpgameoptions_cursor, 0, 400, mpgameoptions_cursor_table[i], 8, i);
	Draw_Character (cbx, OPTION_NAMES_CX - 8, mpgameoptions_cursor_table[mpgameoptions_cursor], 12 + ((int)(realtime * 4) & 1));

#undef OPTION_NAMES_CX
#undef OPTION_VALUES_CX
}

static void M_NetStart_Change (int dir)
{
	int	  count;
	float f;

	switch (mpgameoptions_cursor)
	{
	case 1:
		maxplayers += dir;
		if (maxplayers > svs.maxclientslimit)
			maxplayers = svs.maxclientslimit;
		if (maxplayers < 2)
			maxplayers = 2;
		break;

	case 2:
		Cvar_Set ("coop", coop.value ? "0" : "1");
		break;

	case 3:
		count = (rogue) ? 6 : 2;
		f = teamplay.value + dir;
		if (f > count)
			f = 0;
		else if (f < 0)
			f = count;
		Cvar_SetValue ("teamplay", f);
		break;

	case 4:
		f = skill.value + dir;
		if (f > 3)
			f = 0;
		else if (f < 0)
			f = 3;
		Cvar_SetValue ("skill", f);
		break;

	case 5:
		f = fraglimit.value + dir * 10;
		if (f > 100)
			f = 0;
		else if (f < 0)
			f = 100;
		Cvar_SetValue ("fraglimit", f);
		break;

	case 6:
		f = timelimit.value + dir * 5;
		if (f > 60)
			f = 0;
		else if (f < 0)
			f = 60;
		Cvar_SetValue ("timelimit", f);
		break;

	case 7:
		startepisode += dir;
		// MED 01/06/97 added hipnotic count
		if (hipnotic)
			count = 6;
		// PGM 01/07/97 added rogue count
		// PGM 03/02/97 added 1 for dmatch episode
		else if (rogue)
			count = 4;
		else if (registered.value)
			count = 7;
		else
			count = 2;

		if (startepisode < 0)
			startepisode = count - 1;

		if (startepisode >= count)
			startepisode = 0;

		startlevel = 0;
		break;

	case 8:
		startlevel += dir;
		// MED 01/06/97 added hipnotic episodes
		if (hipnotic)
			count = hipnoticepisodes[startepisode].levels;
		// PGM 01/06/97 added hipnotic episodes
		else if (rogue)
			count = rogueepisodes[startepisode].levels;
		else
			count = episodes[startepisode].levels;

		if (startlevel < 0)
			startlevel = count - 1;

		if (startlevel >= count)
			startlevel = 0;
		break;
	}
}

static void M_MPGameOptions_Key (int key)
{
	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Net_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		mpgameoptions_cursor--;
		if (mpgameoptions_cursor < 0)
			mpgameoptions_cursor = NUM_MPGAMEOPTIONS - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		mpgameoptions_cursor++;
		if (mpgameoptions_cursor >= NUM_MPGAMEOPTIONS)
			mpgameoptions_cursor = 0;
		break;

	case K_LEFTARROW:
		if (mpgameoptions_cursor == 0)
			break;
		S_LocalSound ("misc/menu3.wav");
		M_NetStart_Change (-1);
		break;

	case K_RIGHTARROW:
		if (mpgameoptions_cursor == 0)
			break;
		S_LocalSound ("misc/menu3.wav");
		M_NetStart_Change (1);
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		S_LocalSound ("misc/menu2.wav");
		if (mpgameoptions_cursor == 0)
		{
			if (sv.active)
				Cbuf_AddText ("disconnect\n");
			Cbuf_AddText ("listen 0\n"); // so host_netport will be re-examined
			Cbuf_AddText (va ("maxplayers %u\n", maxplayers));
			SCR_BeginLoadingPlaque ();

			if (hipnotic)
				Cbuf_AddText (va ("map %s\n", hipnoticlevels[hipnoticepisodes[startepisode].firstLevel + startlevel].name));
			else if (rogue)
				Cbuf_AddText (va ("map %s\n", roguelevels[rogueepisodes[startepisode].firstLevel + startlevel].name));
			else
				Cbuf_AddText (va ("map %s\n", levels[episodes[startepisode].firstLevel + startlevel].name));

			return;
		}

		M_NetStart_Change (1);
		break;
	}
}

//=============================================================================
/* SEARCH MENU */

static qboolean			 search_complete = false;
static double			 search_complete_time;
static enum slistScope_e search_last_scope = SLIST_LAN;

static void M_Menu_Search_f (enum slistScope_e scope)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_search;
	slistSilent = true;
	slistScope = search_last_scope = scope;
	search_complete = false;
	NET_Slist_f ();
}

static void M_Search_Draw (cb_context_t *cbx)
{
	qpic_t *p;
	int		x;

	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);
	x = (320 / 2) - ((12 * 8) / 2) + 4;
	M_DrawTextBox (cbx, x - 8, 32, 12, 1);
	M_Print (cbx, x, 40, "Searching...");

	if (slistInProgress)
	{
		NET_Poll ();
		return;
	}

	if (!search_complete)
	{
		search_complete = true;
		search_complete_time = realtime;
	}

	if (hostCacheCount)
	{
		M_Menu_ServerList_f ();
		return;
	}

	M_PrintWhite (cbx, (320 / 2) - ((22 * 8) / 2), 64, "No Quake servers found");
	if ((realtime - search_complete_time) < 3.0)
		return;

	M_Menu_LanConfig_f ();
}

static void M_Search_Key (int key) {}

//=============================================================================
/* SLIST MENU */

static int		slist_cursor;
static int		slist_first;
static qboolean slist_sorted;
#define SERVER_LIST_MAX_ON_SCREEN 21

static void M_Menu_ServerList_f (void)
{
	M_MenuChanged ();
	IN_DeactivateForMenu ();
	key_dest = key_menu;
	m_state = m_slist;
	slist_cursor = 0;
	slist_first = 0;
	m_return_onerror = false;
	m_return_reason[0] = 0;
	slist_sorted = false;
}

static void M_ServerList_Draw (cb_context_t *cbx)
{
	size_t	n;
	qpic_t *p;

	if (!slist_sorted)
	{
		slist_sorted = true;
		NET_SlistSort ();
	}

	if (hostCacheCount > SERVER_LIST_MAX_ON_SCREEN)
		M_DrawScrollbar (cbx, MENU_SCROLLBAR_X, 40, (float)(slist_first) / (hostCacheCount - SERVER_LIST_MAX_ON_SCREEN), SERVER_LIST_MAX_ON_SCREEN - 2);
	const int server_list_height = CLAMP (0, (int)hostCacheCount - slist_first, SERVER_LIST_MAX_ON_SCREEN);
	M_Mouse_UpdateListCursor (&slist_cursor, 12, 400, 32, 8, server_list_height, slist_first);

	p = Draw_CachePic ("gfx/p_multi.lmp");
	M_DrawPic (cbx, (320 - p->width) / 2, 4, p);

	for (n = 0; n < (size_t)server_list_height; n++)
	{
		M_Print (cbx, 28 - CHARACTER_SIZE, 32 + 8 * n, NET_SlistPrintServer (slist_first + n));
	}

	Draw_Character (cbx, 16 - CHARACTER_SIZE, 32 + (slist_cursor - slist_first) * 8, 12 + ((int)(realtime * 4) & 1));

	if (*m_return_reason)
		M_PrintWhite (cbx, 16, 148, m_return_reason);
}

static void M_ServerList_Key (int k)
{
	if (M_HandleScrollBarKeys (k, &slist_cursor, &slist_first, hostCacheCount, SERVER_LIST_MAX_ON_SCREEN))
		return;

	switch (k)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_LanConfig_f ();
		break;

	case K_SPACE:
		M_Menu_Search_f (search_last_scope);
		break;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		S_LocalSound ("misc/menu2.wav");
		m_return_state = m_state;
		m_return_onerror = true;
		slist_sorted = false;
		IN_Activate ();
		key_dest = key_game;
		m_state = m_none;
		Cbuf_AddText (va ("connect \"%s\"\n", NET_SlistPrintServerName (slist_cursor)));
		break;

	default:
		break;
	}
}

//=============================================================================
/* Custom gfx checks (from Ironwail) */

static qboolean M_CheckCustomGfx (const char *custompath, const char *basepath, int knownlength, const unsigned int *hashes, int numhashes)
{
	unsigned int id_custom, id_base;
	int			 h;
	int			 length;
	qboolean	 ret = false;

	if (!COM_FileExists (custompath, &id_custom))
		return false;

	length = COM_OpenFile (basepath, &h, &id_base);
	if (length == -1)
		return false;

	if (id_custom >= id_base)
		ret = true;
	else if (length == knownlength)
	{
		byte *data = (byte *)Mem_Alloc (length);
		if (length == Sys_FileRead (h, data, length))
		{
			unsigned int hash = COM_HashBlock (data, length);
			while (numhashes-- > 0 && !ret)
				if (hash == *hashes++)
					ret = true;
		}
		Mem_Free (data);
	}

	COM_CloseFile (h);

	return ret;
}

void M_CheckMods (void)
{
	const unsigned int sp_hashes[] = {0x86a6f086}, sgl_hashes[] = {0x7bba813d};

	m_singleplayer_showlevels = M_CheckCustomGfx ("gfx/sp_maps.lmp", "gfx/sp_menu.lmp", 14856, sp_hashes, countof (sp_hashes));
	m_skill_usegfx = M_CheckCustomGfx ("gfx/skillmenu.lmp", "gfx/sp_menu.lmp", 14856, sp_hashes, countof (sp_hashes));
	m_skill_usecustomtitle = M_CheckCustomGfx ("gfx/p_skill.lmp", "gfx/ttl_sgl.lmp", 6728, sgl_hashes, countof (sgl_hashes));
}

//=============================================================================
/* Credits menu -- used by the 2021 re-release */

static void M_Menu_Credits_f (void) {}

//=============================================================================
/* Menu Subsystem */

void M_Init (void)
{
	Cmd_AddCommand ("togglemenu", M_ToggleMenu_f);

	Cmd_AddCommand ("menu_main", M_Menu_Main_f);
	Cmd_AddCommand ("menu_singleplayer", M_Menu_SinglePlayer_f);
	Cmd_AddCommand ("menu_load", M_Menu_Load_f);
	Cmd_AddCommand ("menu_save", M_Menu_Save_f);
	Cmd_AddCommand ("menu_maps", M_Menu_Maps_Cmd_f);
	Cmd_AddCommand ("menu_multiplayer", M_Menu_MultiPlayer_f);
	Cmd_AddCommand ("menu_setup", M_Menu_Setup_f);
	Cmd_AddCommand ("menu_options", M_Menu_Options_f);
	Cmd_AddCommand ("menu_keys", M_Menu_Keys_f);
	Cmd_AddCommand ("menu_video", M_Menu_Video_f);
	Cmd_AddCommand ("help", M_Menu_Help_f);
	Cmd_AddCommand ("menu_quit", M_Menu_Quit_f);
	Cmd_AddCommand ("menu_credits", M_Menu_Credits_f); // needed by the 2021 re-release

	Cvar_RegisterVariable (&ui_mouse);
	Cvar_RegisterVariable (&ui_live_preview);
}

void M_NewGame (void)
{
	m_main_cursor = 0;
	if (m_state == m_maps || m_state == m_skill) // the map list is about to be rebuilt
		m_state = m_main;
}

static void M_UpdatePreview (void)
{
	float dt = q_min (host_rawframetime, 1.0 / 30.0);
	int	  row = m_state == m_sound ? sound_options_cursor : game_options_cursor;

	if (!ui_live_preview.value || key_dest != key_menu || m_state != menu_preview.menu || cls.state != ca_connected || cls.signon != SIGNONS)
	{
		memset (&menu_preview, 0, sizeof (menu_preview));
		return;
	}
	if (row != menu_preview.row)
		menu_preview.target = menu_preview.hold = 0.0f;
	if (menu_preview.fraction < menu_preview.target)
		menu_preview.fraction = q_min (menu_preview.target, menu_preview.fraction + dt / 0.125f);
	else if (menu_preview.fraction > menu_preview.target)
		menu_preview.fraction = q_max (menu_preview.target, menu_preview.fraction - dt / 0.125f);
	else if (menu_preview.hold > 0.0f && !slider_grab)
	{
		menu_preview.hold -= dt;
		if (menu_preview.hold <= 0.0f)
			menu_preview.target = 0.0f;
	}
}

/* BENCHMARK MENU */

#define MAX_DEMOS_ON_SCREEN 12
#define MAX_BENCH_DEMOS     64
static char demo_names[MAX_BENCH_DEMOS][MAX_QPATH];
static int  num_demos = 0;
static int  first_demo = 0;
static int  demo_cursor = 0;
static char bench_error[64];

qboolean M_Benchmark_DemoExists (const char *name)
{
	char  path[MAX_QPATH];
	FILE *file = NULL;

	q_strlcpy (path, name, sizeof (path));
	COM_AddExtension (path, ".dem", sizeof (path));
	COM_FOpenFile (path, &file, NULL);
	if (!file)
		return false;

	fclose (file);
	return true;
}

void M_Benchmark_AddDemo (const char *name)
{
	char base[MAX_QPATH];

	if (!name || !name[0])
		return;

	// the playback appends the extension itself, so keep the bare name
	q_strlcpy (base, name, sizeof (base));
	COM_StripExtension (base, base, sizeof (base));
	if (!base[0])
		return;

	for (int i = 0; i < num_demos; i++)
		if (!q_strcasecmp (demo_names[i], base))
			return;

	if (num_demos < MAX_BENCH_DEMOS)
		q_strlcpy (demo_names[num_demos++], base, sizeof (demo_names[0]));
}

void M_Menu_Benchmark_f (void)
{
	IN_Deactivate (modestate == MS_WINDOWED);
	key_dest = key_menu;
	m_state = m_benchmark;
	m_entersound = true;

	num_demos = 0;

	// the demo list below skips the files inside the paks on purpose, so the
	// standard demos and the intro loop are probed for and listed first: a
	// long list of loose demos must not push them out
	for (int i = 1; i <= 3; i++)
	{
		char name[MAX_QPATH];

		q_snprintf (name, sizeof (name), "demo%i", i);
		if (M_Benchmark_DemoExists (name))
			M_Benchmark_AddDemo (name);
	}

	for (int i = 0; i < MAX_DEMOS; i++)
		if (cls.demos[i][0] && M_Benchmark_DemoExists (cls.demos[i]))
			M_Benchmark_AddDemo (cls.demos[i]);

	// a demo recorded in this session is in the list too
	DemoList_Rebuild ();
	for (filelist_item_t *item = demolist; item; item = item->next)
		M_Benchmark_AddDemo (item->name);

	first_demo = 0;
	demo_cursor = 0;
	bench_error[0] = 0;
}

void M_Benchmark_Draw (cb_context_t *cbx)
{
	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	M_PrintWhite (cbx, 124, 8, "BENCHMARK");

	if (num_demos <= 0)
	{
		M_Print (cbx, 105, 40, "no demos found");
		return;
	}

	int demo_index = -first_demo;

	for (int i = 0; i < num_demos; i++)
	{
		if (demo_index >= MAX_DEMOS_ON_SCREEN)
			break;
		if (demo_index >= 0)
			M_Print (cbx, 105, 32 + demo_index * 8, demo_names[i]);
		++demo_index;
	}

	M_Mouse_UpdateListCursor (&demo_cursor, 90, 320, 32, 8, q_min (num_demos - first_demo, MAX_DEMOS_ON_SCREEN), first_demo);
	M_DrawCharacter (cbx, 90, 32 + (demo_cursor - first_demo) * 8, 12 + ((int)(realtime * 4) & 1));
	if (num_demos > MAX_DEMOS_ON_SCREEN)
		M_DrawScrollbar (cbx, 220, 32 + 8, (float)(first_demo) / (float)(num_demos - MAX_DEMOS_ON_SCREEN), MAX_DEMOS_ON_SCREEN - 2);

	if (bench_error[0])
		M_PrintWhite (cbx, 40, 140, bench_error);
}

void M_Benchmark_Key (int key)
{
	int prev_demo_cursor = demo_cursor;

	switch (key)
	{
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
		M_Menu_Options_f ();
		return;

	case K_MOUSE1:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		if (demo_cursor < 0 || demo_cursor >= num_demos)
			return;

		{
			char     name[MAX_OSPATH];
			FILE    *file = NULL;
			int      forcetrack;
			qboolean ok = false;

			/* Check the demo before the playback tears the running game down. */
			q_strlcpy (name, demo_names[demo_cursor], sizeof (name));
			COM_AddExtension (name, ".dem", sizeof (name));

			COM_FOpenFile (name, &file, NULL);
			if (file)
			{
				ok = (fscanf (file, "%i", &forcetrack) == 1 && fgetc (file) == '\n');
				fclose (file);
			}

			if (!ok || !CL_BenchStart (name, true))
			{
				q_snprintf (bench_error, sizeof (bench_error), "could not open %s", demo_names[demo_cursor]);
				return;
			}

			m_state = m_none;
			IN_Activate ();
			key_dest = key_game;
		}
		return;

	case K_HOME:
		demo_cursor = 0;
		first_demo = 0;
		break;

	case K_END:
		demo_cursor = num_demos - 1;
		first_demo = q_max (0, num_demos - MAX_DEMOS_ON_SCREEN);
		break;

	case K_PGUP:
		demo_cursor -= MAX_DEMOS_ON_SCREEN;
		first_demo = q_max (0, first_demo - MAX_DEMOS_ON_SCREEN);
		break;

	case K_PGDN:
		demo_cursor += MAX_DEMOS_ON_SCREEN;
		first_demo = q_max (0, q_min (first_demo + MAX_DEMOS_ON_SCREEN, num_demos - 1 - MAX_DEMOS_ON_SCREEN));
		break;

	case K_UPARROW:
		--demo_cursor;
		break;

	case K_DOWNARROW:
		++demo_cursor;
		break;
	}

	if (num_demos <= 0)
		return;

	demo_cursor = CLAMP (0, demo_cursor, num_demos - 1);
	if (demo_cursor != prev_demo_cursor)
		S_LocalSound ("misc/menu1.wav");
	first_demo = q_max (0, CLAMP (demo_cursor - MAX_DEMOS_ON_SCREEN + 1, first_demo, demo_cursor));
}

//=============================================================================
/* BENCHMARK RESULTS */

static int benchmark_results_cursor = 0;

void M_Menu_BenchmarkResults_f (void)
{
	IN_Deactivate (modestate == MS_WINDOWED);
	key_dest = key_menu;
	m_state = m_bench_results;
	m_entersound = true;
}

void M_BenchmarkResults_Draw (cb_context_t *cbx)
{
	const rt_bench_result_t *r = &rt_bench_result;
	char                     line[64];

	M_DrawTransPic (cbx, 16, 4, Draw_CachePic ("gfx/qplaque.lmp"));
	M_PrintWhite (cbx, 96, 8, "BENCHMARK RESULT");

	if (!r->valid)
	{
		M_Print (cbx, 105, 48, "no result");
		return;
	}

	M_Print (cbx, 72, 44, r->demo);
	q_snprintf (line, sizeof (line), "%d frames, %.2f seconds", r->frames, r->seconds);
	M_Print (cbx, 72, 56, line);

	M_PrintWhite (cbx, 72, 76, "FPS");
	q_snprintf (line, sizeof (line), "min %5.1f   max %5.1f   avg %5.1f", r->fpsMin, r->fpsMax, r->fpsAvg);
	M_Print (cbx, 72, 86, line);

	M_PrintWhite (cbx, 72, 102, "FRAMETIME, ms");
	q_snprintf (line, sizeof (line), "min %5.2f   max %5.2f   avg %5.2f", r->frameMinMs, r->frameMaxMs, r->frameAvgMs);
	M_Print (cbx, 72, 112, line);

	M_Mouse_UpdateCursor (&benchmark_results_cursor, 88, 320, 140, CHARACTER_SIZE, 0);
	M_PrintWhite (cbx, 88, 140, "Press ENTER to continue");
}

void M_BenchmarkResults_Key (int key)
{
	switch (key)
	{
	case K_MOUSE1:
	case K_MOUSE2:
	case K_ESCAPE:
	case K_BBUTTON:
	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		S_LocalSound ("misc/menu2.wav");
		M_Menu_Options_f ();
		break;
	}
}

void M_UpdateMouse (void)
{
	M_UpdatePreview ();
	// IN_GetMousePos scales window coordinates to drawable pixels, which is what
	// M_PixelToMenuCanvasCoord expects; the two differ on high pixel density displays
	int new_mouse_x;
	int new_mouse_y;

	if (!ui_mouse.value)
	{
		m_mouse_moved = false;
		m_mouse_x = m_mouse_y = INT_MIN;
		scrollbar_grab = slider_grab = false;
		scrollbar_size = 0;
		return;
	}

	IN_GetMousePos (&new_mouse_x, &new_mouse_y);

	m_mouse_moved = !menu_changed && ((m_mouse_x_pixels != new_mouse_x) || (m_mouse_y_pixels != new_mouse_y));
	m_mouse_x_pixels = new_mouse_x;
	m_mouse_y_pixels = new_mouse_y;
	menu_changed = false;

	m_mouse_x = new_mouse_x;
	m_mouse_y = new_mouse_y;
	M_PixelToMenuCanvasCoord (&m_mouse_x, &m_mouse_y);

	if (scrollbar_grab)
	{
		if (keydown[K_MOUSE1] && M_InScrollbar ())
			M_Keydown (K_MOUSE1, false);
		else
			scrollbar_grab = false;
	}
	else if (slider_grab)
	{
		const bool game_option_has_sliders = ((game_options_cursor >= GAME_OPT_SCALE) && (game_options_cursor <= GAME_OPT_VIEWROLL)) ||
											 (game_options_cursor == GAME_OPT_CROSSHAIR_SIZE) || (game_options_cursor == GAME_OPT_CROSSHAIR_OPACITY);

		if (keydown[K_MOUSE1] && (m_state == m_game) && game_option_has_sliders)
			M_GameOptions_AdjustSliders (0, true);
		else if (keydown[K_MOUSE1] && (m_state == m_sound) && (sound_options_cursor >= SOUND_OPT_SNDVOL) && (sound_options_cursor <= SOUND_OPT_MUSICVOL))
			M_SoundOptions_AdjustSliders (0, true);
		else if (keydown[K_MOUSE1] && (m_state == m_effects) && (effects_options_cursor != EFFECTS_OPT_RESET))
			M_EffectsOptions_AdjustSliders (0, true);
		else
			slider_grab = false;
	}

	scrollbar_size = 0;
}

void M_Draw (cb_context_t *cbx)
{
	m_mouse_hover_state = m_none;
	m_mouse_hover_cursor = NULL;

	if (m_state == m_none || key_dest != key_menu)
		return;

	if (menu_preview.kind == PREVIEW_CENTERPRINT && M_MenuPreviewFraction () > 0.0f)
		SCR_DrawCenterPrintPreview (cbx, M_MenuPreviewFraction ());
	Draw_SetOpacity (1.0f - M_MenuPreviewFraction ());
	if (!m_recursiveDraw)
	{
		if (scr_con_current)
		{
			Draw_ConsoleBackground (cbx);
			S_ExtraUpdate ();
		}

		Draw_FadeScreen (cbx); // johnfitz -- fade even if console fills screen
	}
	else
	{
		m_recursiveDraw = false;
	}

	GL_SetCanvas (cbx, CANVAS_MENU); // johnfitz

	switch (m_state)
	{
	case m_none:
		break;

	case m_main:
		M_Main_Draw (cbx);
		break;

	case m_singleplayer:
		M_SinglePlayer_Draw (cbx);
		break;

	case m_load:
		M_Load_Draw (cbx);
		break;

	case m_save:
		M_Save_Draw (cbx);
		break;

	case m_multiplayer:
		M_MultiPlayer_Draw (cbx);
		break;

	case m_setup:
		M_Setup_Draw (cbx);
		break;

	case m_net:
		M_Net_Draw (cbx);
		break;

	case m_options:
		M_Options_Draw (cbx);
		break;


	case m_game:
		M_GameOptions_Draw (cbx);
		break;

	case m_keys:
		M_Keys_Draw (cbx);
		break;

	case m_video:
		M_Video_Draw (cbx);
		break;

	case m_graphics:
		M_GraphicsOptions_Draw (cbx);
		break;

	case m_effects:
		M_EffectsOptions_Draw (cbx);
		break;

	case m_lighting:
		M_LightingOptions_Draw (cbx);
		break;


	case m_sound:
		M_SoundOptions_Draw (cbx);
		break;

	case m_help:
		M_Help_Draw (cbx);
		break;

	case m_benchmark:
		M_Benchmark_Draw (cbx);
		break;

	case m_bench_results:
		M_BenchmarkResults_Draw (cbx);
		break;

	case m_mods:
		M_Mods_Draw (cbx);
		break;

	case m_maps:
		M_Maps_Draw (cbx);
		break;

	case m_skill:
		M_Skill_Draw (cbx);
		break;

	case m_quit:
		M_Quit_Draw (cbx);
		break;

	case m_lanconfig:
		M_LanConfig_Draw (cbx);
		break;

	case m_mpgameoptions:
		M_MPGameOptions_Draw (cbx);
		break;

	case m_search:
		M_Search_Draw (cbx);
		break;

	case m_slist:
		M_ServerList_Draw (cbx);
		break;
	}

	Draw_SetOpacity (1.0f);

	if (m_entersound)
	{
		S_LocalSound ("misc/menu2.wav");
		m_entersound = false;
	}

	S_ExtraUpdate ();
}

static qboolean M_Mouse_ClickValid (void)
{
	return bind_grab || m_state == m_help || m_mouse_hover_state == m_state || M_InScrollbar ();
}

static qboolean M_IsMouseKey (int key)
{
	switch (key)
	{
	case K_MOUSE1:
	case K_MOUSE2:
	case K_MOUSE3:
	case K_MOUSE4:
	case K_MOUSE5:
	case K_MWHEELUP:
	case K_MWHEELDOWN:
		return true;
	default:
		return false;
	}
}

void M_Keydown (int key, qboolean repeat)
{
	// mouse buttons can still be bound in the keys menu
	if (!ui_mouse.value && !bind_grab && M_IsMouseKey (key))
		return;

	// Repeat navigation and editing, but never menu activation or binding capture.
	if (repeat)
	{
		if (bind_grab)
			return;
		switch (key)
		{
		case K_UPARROW:
		case K_DOWNARROW:
		case K_LEFTARROW:
		case K_RIGHTARROW:
		case K_PGUP:
		case K_PGDN:
		case K_HOME:
		case K_END:
		case K_MWHEELUP:
		case K_MWHEELDOWN:
		case K_BACKSPACE:
		case K_DEL:
			break;
		default:
			return;
		}
	}

	if (key == K_MOUSE1 && !M_Mouse_ClickValid ())
		return;
	if (key == K_MOUSE1 && m_mouse_hover_state == m_state && m_mouse_hover_cursor)
		*m_mouse_hover_cursor = m_mouse_hover_value;

	switch (m_state)
	{
	case m_none:
		return;

	case m_main:
		M_Main_Key (key);
		return;

	case m_singleplayer:
		M_SinglePlayer_Key (key);
		return;

	case m_load:
		M_Load_Key (key);
		return;

	case m_save:
		M_Save_Key (key);
		return;

	case m_multiplayer:
		M_MultiPlayer_Key (key);
		return;

	case m_setup:
		M_Setup_Key (key);
		return;

	case m_net:
		M_Net_Key (key);
		return;

	case m_options:
		M_Options_Key (key);
		return;


	case m_game:
		M_GameOptions_Key (key);
		return;


	case m_benchmark:
		M_Benchmark_Key (key);
		break;

	case m_bench_results:
		M_BenchmarkResults_Key (key);
		break;

	case m_mods:
		M_Mods_Key (key);
		break;

	case m_maps:
		M_Maps_Key (key);
		break;

	case m_skill:
		M_Skill_Key (key);
		break;

	case m_keys:
		M_Keys_Key (key);
		return;

	case m_video:
		M_Video_Key (key);
		return;

	case m_graphics:
		M_GraphicsOptions_Key (key);
		return;

	case m_effects:
		M_EffectsOptions_Key (key);
		return;

	case m_lighting:
		M_LightingOptions_Key (key);
		return;

	case m_sound:
		M_SoundOptions_Key (key);
		return;

	case m_help:
		M_Help_Key (key);
		return;

	case m_quit:
		M_Quit_Key (key);
		return;

	case m_lanconfig:
		M_LanConfig_Key (key);
		return;

	case m_mpgameoptions:
		M_MPGameOptions_Key (key);
		return;

	case m_search:
		M_Search_Key (key);
		break;

	case m_slist:
		M_ServerList_Key (key);
		return;
	}
}

void M_Charinput (int key)
{
	switch (m_state)
	{
	case m_setup:
		M_Setup_Char (key);
		return;
	case m_maps:
		M_Maps_Char (key);
		return;
	case m_lanconfig:
		M_LanConfig_Char (key);
		return;
	default:
		return;
	}
}

qboolean M_TextEntry (void)
{
	switch (m_state)
	{
	case m_setup:
		return M_Setup_TextEntry ();
	case m_maps:
		return M_Maps_TextEntry ();
	case m_lanconfig:
		return M_LanConfig_TextEntry ();
	default:
		return false;
	}
}

qboolean M_WaitingForKeyBinding (void)
{
	return key_dest == key_menu && m_state == m_keys && bind_grab;
}

void M_ConfigureNetSubsystem (void)
{
	// enable/disable net systems to match desired config
	Cbuf_AddText ("stopdemo\n");

	if (IPXConfig || TCPIPConfig)
		net_hostport = lan_config_port;
}
