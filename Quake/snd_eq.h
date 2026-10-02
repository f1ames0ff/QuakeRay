/*
 * snd_eq.h -- five-band equalizer for the OpenAL output
 *
 * Copyright (C) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#ifndef __SND_EQ__
#define __SND_EQ__

#include "q_sound.h"

#define SNDEQ_BANDS 5

typedef struct
{
	double b0, b1, b2, a1, a2;
	double z1[2], z2[2];
} sndeq_band_t;

typedef struct
{
	sndeq_band_t band[SNDEQ_BANDS];
} sndeq_state_t;

void SNDEQ_Init (void);
void SNDEQ_GuiInit (void);
void SNDEQ_Update (void);
void SNDEQ_RequestRebuild (void);

qboolean SNDEQ_Active (void);
void     SNDEQ_Process (sndeq_state_t *state, short *pcm, int frames, int channels, int rate);
void     SNDEQ_FilterBuffer (short *pcm, int frames, int rate);
float    SNDEQ_ResponseDb (float freq, int rate);

int     SNDEQ_BandCount (void);
int     SNDEQ_BandFrequency (int band);
cvar_t *SNDEQ_BandCvar (int band);

qboolean SNDEQ_DialogActive (void);
void     SNDEQ_OpenDialog (void);
void     SNDEQ_CloseDialog (void);
void     SNDEQ_DrawDialog (void);
qboolean SNDEQ_GuiProcessEvent (const void *sdl_event);

#endif /* __SND_EQ__ */
