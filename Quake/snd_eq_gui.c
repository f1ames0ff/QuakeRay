/*
 * snd_eq_gui.c -- the equalizer's ImGui dialog
 *
 * Copyright (C) 2026 QuakeRay contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "snd_eq.h"
#include "qr_gui.h"

static qboolean eq_dialog_open;
static int      eq_drag_band = -1;

static void SNDEQ_Command_f (void)
{
	key_dest = key_menu;
	SNDEQ_OpenDialog ();
}

void SNDEQ_GuiInit (void)
{
	Cmd_AddCommand ("equalizer", SNDEQ_Command_f);
}

static void SNDEQ_SetDialogOpen (qboolean open)
{
	if (open == eq_dialog_open)
		return;

	eq_dialog_open = open;

	if (open)
	{
		IN_FreeCursorForGui ();
		SDL_ShowCursor (SDL_DISABLE);
		QR_GUI_SetMouseCursor (1);
	}
	else
	{
		SDL_ShowCursor (SDL_ENABLE);
		QR_GUI_SetMouseCursor (0);
		eq_drag_band = -1;
		IN_DeactivateForMenu ();
	}
}

qboolean SNDEQ_DialogActive (void)
{
	return eq_dialog_open;
}

void SNDEQ_OpenDialog (void)
{
	SNDEQ_SetDialogOpen (true);
}

void SNDEQ_CloseDialog (void)
{
	SNDEQ_SetDialogOpen (false);
}

#define EQ_GRAPH_W 816.0f
#define EQ_GRAPH_H 336.0f
#define EQ_DB_MAX  18.0f
#define EQ_DB_MIN  (-18.0f)
#define EQ_F_MIN   20.0f
#define EQ_F_MAX   20000.0f

static const float sndeq_grid_freqs[] = {20, 30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000};
static const float sndeq_grid_dbs[] = {12, 6, 0, -6, -12};

static uint32_t SNDEQ_Color (int r, int g, int b, int a)
{
	return ((uint32_t)(a & 0xFF) << 24) | ((uint32_t)(b & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(r & 0xFF);
}

static float SNDEQ_FreqToX (float freq)
{
	return (logf (freq / EQ_F_MIN) / logf (EQ_F_MAX / EQ_F_MIN)) * EQ_GRAPH_W;
}

static float SNDEQ_DbToY (float db)
{
	return (EQ_DB_MAX - db) / (EQ_DB_MAX - EQ_DB_MIN) * EQ_GRAPH_H;
}

static float SNDEQ_XToFreq (float x)
{
	return EQ_F_MIN * powf (EQ_F_MAX / EQ_F_MIN, x / EQ_GRAPH_W);
}

static float SNDEQ_YToDb (float y)
{
	return EQ_DB_MAX - y / EQ_GRAPH_H * (EQ_DB_MAX - EQ_DB_MIN);
}

static void SNDEQ_ResetBands (void)
{
	int i;

	for (i = 0; i < SNDEQ_BandCount (); i++)
		Cvar_SetValueQuick (SNDEQ_BandCvar (i), 0.0f);
	SNDEQ_RequestRebuild ();
}

static void SNDEQ_DrawGrid (void)
{
	int i;

	for (i = 0; i < (int)countof (sndeq_grid_freqs); i++)
	{
		float f = sndeq_grid_freqs[i];
		float x = SNDEQ_FreqToX (f);
		float w, tx;
		char  label[16];

		QR_GUI_CanvasLine (x, 0.0f, x, EQ_GRAPH_H, SNDEQ_Color (255, 255, 255, 28), 1.0f);

		if (f >= 1000.0f)
			q_snprintf (label, sizeof (label), "%gk", f / 1000.0f);
		else
			q_snprintf (label, sizeof (label), "%d", (int)f);

		w = QR_GUI_TextWidth (label);
		tx = x + 3.0f;
		if (tx + w > EQ_GRAPH_W)
			tx = x - w - 3.0f;
		QR_GUI_CanvasText (tx, EQ_GRAPH_H - 17.0f, SNDEQ_Color (170, 180, 200, 200), label);
	}

	for (i = 0; i < (int)countof (sndeq_grid_dbs); i++)
	{
		float db = sndeq_grid_dbs[i];
		float y = SNDEQ_DbToY (db);
		char  label[16];

		QR_GUI_CanvasLine (0.0f, y, EQ_GRAPH_W, y, SNDEQ_Color (255, 255, 255, db == 0.0f ? 60 : 24), 1.0f);
		q_snprintf (label, sizeof (label), "%+d", (int)db);
		QR_GUI_CanvasText (4.0f, y - 16.0f, SNDEQ_Color (170, 180, 200, 200), label);
	}
}

void SNDEQ_DrawDialog (void)
{
	float gx = 0.0f, gy = 0.0f, mouse_x = 0.0f, mouse_y = 0.0f;
	float hx[SNDEQ_BANDS], hy[SNDEQ_BANDS];
	int   canvas, i, rate;

	if (!eq_dialog_open || !QR_GUI_Ready ())
		return;

	QR_GUI_Backdrop (0.7f);
	QR_GUI_PushWindowPadding (24.0f, 16.0f);
	if (QR_GUI_BeginDialog ("Equalizer", EQ_GRAPH_W + 48.0f) == 0)
	{
		QR_GUI_EndDialog ();
		QR_GUI_PopWindowPadding ();
		return;
	}

	QR_GUI_LabelDim ("Five bands over ten octaves; drag a handle to set its gain, -12 to +12 dB.");
	QR_GUI_Spacing ();

	canvas = QR_GUI_Canvas ("##eq_graph", EQ_GRAPH_W, EQ_GRAPH_H, &gx, &gy);
	QR_GUI_GetMousePos (&mouse_x, &mouse_y);
	mouse_x -= gx;
	mouse_y -= gy;

	for (i = 0; i < SNDEQ_BandCount (); i++)
	{
		hx[i] = SNDEQ_FreqToX ((float)SNDEQ_BandFrequency (i));
		hy[i] = SNDEQ_DbToY (SNDEQ_BandCvar (i)->value);
	}

	if (canvas & 2)
	{
		if (eq_drag_band == -1)
		{
			for (i = 0; i < SNDEQ_BandCount (); i++)
			{
				if (fabsf (mouse_x - hx[i]) < 14.0f && fabsf (mouse_y - hy[i]) < 14.0f)
				{
					if (QR_GUI_CtrlDown ())
					{
						Cvar_SetValueQuick (SNDEQ_BandCvar (i), 0.0f);
						SNDEQ_RequestRebuild ();
						hy[i] = SNDEQ_DbToY (0.0f);
						eq_drag_band = -2;
					}
					else
						eq_drag_band = i;
					break;
				}
			}
		}
		if (eq_drag_band >= 0)
		{
			float db = SNDEQ_YToDb (mouse_y);

			if (db > 12.0f)
				db = 12.0f;
			else if (db < -12.0f)
				db = -12.0f;
			Cvar_SetValueQuick (SNDEQ_BandCvar (eq_drag_band), db);
			hy[eq_drag_band] = SNDEQ_DbToY (db);
		}
	}
	else
		eq_drag_band = -1;

	QR_GUI_CanvasRect (0.0f, 0.0f, EQ_GRAPH_W, EQ_GRAPH_H, SNDEQ_Color (13, 15, 19, 255));
	SNDEQ_DrawGrid ();

	for (i = 0; i < SNDEQ_BandCount (); i++)
		QR_GUI_CanvasLine (hx[i], 0.0f, hx[i], EQ_GRAPH_H, SNDEQ_Color (250, 210, 90, 40), 1.0f);

	rate = (snd_output.speed > 0) ? snd_output.speed : 48000;

	for (i = 0; i < (int)EQ_GRAPH_W; i += 2)
	{
		float db0 = SNDEQ_ResponseDb (SNDEQ_XToFreq ((float)i), rate);
		float db1 = SNDEQ_ResponseDb (SNDEQ_XToFreq ((float)(i + 2)), rate);
		float y0 = SNDEQ_DbToY (db0);
		float y1 = SNDEQ_DbToY (db1);
		float zero = SNDEQ_DbToY (0.0f);
		float top = (y0 < y1 ? y0 : y1);
		float bot = (y0 < y1 ? y1 : y0);

		if (top < 0.0f)
			top = 0.0f;
		if (bot > EQ_GRAPH_H)
			bot = EQ_GRAPH_H;

		QR_GUI_CanvasRect ((float)i, top < zero ? top : zero, (float)(i + 2), bot > zero ? bot : zero, SNDEQ_Color (250, 210, 90, 34));
		QR_GUI_CanvasLine ((float)i, y0, (float)(i + 2), y1, SNDEQ_Color (250, 210, 90, 230), 2.0f);
	}

	for (i = 0; i < SNDEQ_BandCount (); i++)
	{
		char  label[32];
		int   hot = (eq_drag_band == i || (fabsf (mouse_x - hx[i]) < 14.0f && fabsf (mouse_y - hy[i]) < 14.0f)) ? 1 : 0;
		float radius = hot ? 9.0f : 7.0f;

		QR_GUI_CanvasCircle (hx[i], hy[i], radius + 2.0f, SNDEQ_Color (250, 210, 90, hot ? 90 : 40), 0.0f, 1);
		QR_GUI_CanvasCircle (hx[i], hy[i], radius, SNDEQ_Color (28, 32, 40, 255), 0.0f, 1);
		QR_GUI_CanvasCircle (hx[i], hy[i], radius, SNDEQ_Color (250, 210, 90, 255), hot ? 2.5f : 2.0f, 0);

		q_snprintf (label, sizeof (label), "%+.1f", SNDEQ_BandCvar (i)->value);
		QR_GUI_CanvasText (hx[i] + radius + 4.0f, hy[i] - 8.0f, SNDEQ_Color (240, 230, 200, 230), label);
	}

	QR_GUI_Spacing ();

	if (QR_GUI_Button ("Reset"))
		SNDEQ_ResetBands ();
	QR_GUI_SameLine ();
	if (QR_GUI_Button ("Close"))
		SNDEQ_SetDialogOpen (false);
	QR_GUI_SameLine ();
	QR_GUI_LabelDim (SNDEQ_Active () ? "EQ active" : "flat");
	QR_GUI_LabelRight ("Drag: gain | Ctrl+Click: reset band | Esc: close");

	QR_GUI_EndDialog ();
	QR_GUI_PopWindowPadding ();
}

qboolean SNDEQ_GuiProcessEvent (const void *sdl_event)
{
	const SDL_Event *e = (const SDL_Event *)sdl_event;

	if (!eq_dialog_open || !QR_GUI_Ready ())
		return false;
	if (key_dest == key_console || key_dest == key_message)
		return false;

	switch (e->type)
	{
	case SDL_KEYDOWN:
		if (e->key.keysym.sym == SDLK_ESCAPE)
		{
			SNDEQ_SetDialogOpen (false);
			return true;
		}
		break;
	case SDL_KEYUP:
	case SDL_TEXTINPUT:
	case SDL_TEXTEDITING:
	case SDL_MOUSEMOTION:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
		break;
	default:
		return false;
	}

	QR_GUI_ProcessEvent (e);
	return true;
}
