/*
 * snd_eq.c -- five-band equalizer for the OpenAL output
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
#include "snd_openal.h"

cvar_t s_eq_60 = {"s_eq_60", "0", CVAR_ARCHIVE};
cvar_t s_eq_230 = {"s_eq_230", "0", CVAR_ARCHIVE};
cvar_t s_eq_910 = {"s_eq_910", "0", CVAR_ARCHIVE};
cvar_t s_eq_3600 = {"s_eq_3600", "0", CVAR_ARCHIVE};
cvar_t s_eq_14000 = {"s_eq_14000", "0", CVAR_ARCHIVE};

static cvar_t       *sndeq_cvars[SNDEQ_BANDS];
static const int     sndeq_freqs[SNDEQ_BANDS] = {60, 230, 910, 3600, 14000};
static qboolean      sndeq_dirty;
static double        sndeq_lastbuild;

static void SNDEQ_ConfigureBand (sndeq_band_t *band, double freq, double gain, int rate)
{
	double a, w, cosw, alpha, a0;

	if (rate <= 0 || freq >= rate * 0.5 || fabs (gain) <= 0.001 || !isfinite (gain))
	{
		memset (band, 0, sizeof (*band));
		band->b0 = 1.0;
		return;
	}

	a = pow (10.0, gain / 40.0);
	w = 2.0 * M_PI * freq / rate;
	cosw = cos (w);
	alpha = sin (w) / (2.0 * 0.9);
	a0 = 1.0 + alpha / a;

	band->b0 = (1.0f + alpha * a) / a0;
	band->b1 = (-2.0f * cosw) / a0;
	band->b2 = (1.0f - alpha * a) / a0;
	band->a1 = (-2.0f * cosw) / a0;
	band->a2 = (1.0f - alpha / a) / a0;
}

static void SNDEQ_Changed (cvar_t *var)
{
	if (!isfinite (var->value))
		Cvar_SetQuick (var, "0");
	else if (var->value < -12.0f)
		Cvar_SetQuick (var, "-12");
	else if (var->value > 12.0f)
		Cvar_SetQuick (var, "12");
	SNDEQ_RequestRebuild ();
}

void SNDEQ_Init (void)
{
	int i;

	sndeq_cvars[0] = &s_eq_60;
	sndeq_cvars[1] = &s_eq_230;
	sndeq_cvars[2] = &s_eq_910;
	sndeq_cvars[3] = &s_eq_3600;
	sndeq_cvars[4] = &s_eq_14000;
	for (i = 0; i < SNDEQ_BANDS; i++)
	{
		Cvar_RegisterVariable (sndeq_cvars[i]);
		Cvar_SetCallback (sndeq_cvars[i], SNDEQ_Changed);
	}
	SNDEQ_GuiInit ();
}

void SNDEQ_RequestRebuild (void)
{
	sndeq_dirty = true;
}

void SNDEQ_Update (void)
{
	if (!sndeq_dirty)
		return;
	if (Sys_DoubleTime () - sndeq_lastbuild < 0.2)
		return;
	sndeq_dirty = false;
	sndeq_lastbuild = Sys_DoubleTime ();
	SNDAL_ClearAll ();
}

qboolean SNDEQ_Active (void)
{
	int i;

	for (i = 0; i < SNDEQ_BANDS; i++)
	{
		if (fabsf (sndeq_cvars[i]->value) > 0.001f)
			return true;
	}
	return false;
}

void SNDEQ_Process (sndeq_state_t *state, short *pcm, int frames, int channels, int rate)
{
	int i, b, c;

	if (!SNDEQ_Active ())
	{
		memset (state, 0, sizeof (*state));
		return;
	}
	if (rate <= 0 || channels <= 0 || channels > 2)
		return;

	for (b = 0; b < SNDEQ_BANDS; b++)
		SNDEQ_ConfigureBand (&state->band[b], (float)sndeq_freqs[b], sndeq_cvars[b]->value, rate);

	for (i = 0; i < frames; i++)
	{
		for (c = 0; c < channels; c++)
		{
			double x = pcm[i * channels + c];

			for (b = 0; b < SNDEQ_BANDS; b++)
			{
				sndeq_band_t *f = &state->band[b];
				double        y = f->b0 * x + f->z1[c];

				f->z1[c] = f->b1 * x - f->a1 * y + f->z2[c];
				f->z2[c] = f->b2 * x - f->a2 * y;
				x = y;
			}

			if (x > 32767.0f)
				x = 32767.0f;
			else if (x < -32768.0f)
				x = -32768.0f;
			pcm[i * channels + c] = (short)(x >= 0.0f ? x + 0.5f : x - 0.5f);
		}
	}
}

void SNDEQ_FilterBuffer (short *pcm, int frames, int rate)
{
	sndeq_state_t state;

	memset (&state, 0, sizeof (state));
	SNDEQ_Process (&state, pcm, frames, 1, rate);
}

float SNDEQ_ResponseDb (float freq, int rate)
{
	double db = 0.0;
	int   b;

	if (rate <= 0 || freq <= 0.0f || freq >= rate * 0.5f)
		return 0.0f;

	for (b = 0; b < SNDEQ_BANDS; b++)
	{
		sndeq_band_t f;
		double       w, cw, sw, c2w, s2w;
		double       nr, ni, dr, di, power;

		memset (&f, 0, sizeof (f));
		SNDEQ_ConfigureBand (&f, (float)sndeq_freqs[b], sndeq_cvars[b]->value, rate);

		w = 2.0 * M_PI * freq / rate;
		cw = cos (w);
		sw = sin (w);
		c2w = cos (2.0 * w);
		s2w = sin (2.0 * w);

		nr = f.b0 + f.b1 * cw + f.b2 * c2w;
		ni = -(f.b1 * sw + f.b2 * s2w);
		dr = 1.0f + f.a1 * cw + f.a2 * c2w;
		di = -(f.a1 * sw + f.a2 * s2w);

		power = (nr * nr + ni * ni) / (dr * dr + di * di);
		if (power > 1.0e-12f)
			db += 10.0 * log10 (power);
	}

	return (float)db;
}

int SNDEQ_BandCount (void)
{
	return SNDEQ_BANDS;
}

int SNDEQ_BandFrequency (int band)
{
	return sndeq_freqs[band];
}

cvar_t *SNDEQ_BandCvar (int band)
{
	return sndeq_cvars[band];
}
