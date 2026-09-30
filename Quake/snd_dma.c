/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>
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

// snd_dma.c -- main control for any streaming sound output device

#include "quakedef.h"
#include "snd_codec.h"
#include "bgmusic.h"
#include "snd_openal.h"

static void S_Play (void);
static void S_PlayVol (void);
static void S_SoundList (void);
void        S_StopAllSounds (qboolean clear);
static void S_StopAllSoundsC (void);

// =======================================================================
// Internal sound data & structures
// =======================================================================

channel_t snd_channels[MAX_CHANNELS];
int       total_channels;

static int      snd_blocked = 0;
static qboolean snd_initialized = false;

vec3_t listener_origin;
vec3_t listener_forward;
vec3_t listener_right;
vec3_t listener_up;

#define sound_nominal_clip_dist 1000.0

int paintedtime; // sample PAIRS

int                   s_rawend;
portable_samplepair_t s_rawsamples[MAX_RAW_SAMPLES];

#define MAX_SFX 1024
static sfx_t *known_sfx = NULL; // hunk allocated [MAX_SFX]
static int    num_sfx;

static sfx_t *ambient_sfx[NUM_AMBIENTS];

static qboolean sound_started = false;
static qboolean sound_ready = false;

static void S_BackendStart (void);
static void S_BackendShutdown (void);
static void S_OpenALChanged (cvar_t *var);
static void S_MixSpeedChanged (cvar_t *var);

SDL_mutex *snd_mutex;

cvar_t bgmvolume = {"bgmvolume", "1", CVAR_ARCHIVE};
cvar_t sfxvolume = {"volume", "0.7", CVAR_ARCHIVE};

cvar_t precache = {"precache", "1", CVAR_NONE};
cvar_t loadas8bit = {"loadas8bit", "0", CVAR_NONE};

cvar_t snd_mixspeed = {"snd_mixspeed", "44100", CVAR_ARCHIVE};

static cvar_t nosound = {"nosound", "0", CVAR_NONE};
static cvar_t ambient_level = {"ambient_level", "0.3", CVAR_NONE};
static cvar_t ambient_fade = {"ambient_fade", "100", CVAR_NONE};
static cvar_t snd_show = {"snd_show", "0", CVAR_NONE};

cvar_t s_openal_hrtf = {"s_openal_hrtf", "2", CVAR_ARCHIVE};
cvar_t s_openal_max_sources = {"s_openal_max_sources", "256", CVAR_ARCHIVE};

static void S_SoundInfo_f (void)
{
	if (!sound_started || !snd_output.ready)
	{
		Con_Printf ("sound system not started\n");
		return;
	}

	Con_Printf ("OpenAL Soft: %d bit, %s, %d Hz\n", snd_output.samplebits, (snd_output.channels == 2) ? "stereo" : "mono", snd_output.speed);
	Con_Printf ("%5d total_channels\n", total_channels);
}

static void S_BackendStart (void)
{
	sound_started = SNDAL_Init ();
}

static void S_BackendShutdown (void)
{
	SNDAL_Shutdown ();
}

static void S_RestartBackend (void)
{
	int oldspeed;

	if (!snd_initialized)
		return;
	if (!sound_ready)
	{
		Con_Printf ("Sound was never initialized; restart the game to change the audio settings\n");
		return;
	}

	oldspeed = snd_output.speed;
	Con_Printf ("Restarting sound backend\n");
	if (sound_started)
		S_StopAllSounds (true);

	SDL_LockMutex (snd_mutex);
	S_BackendShutdown ();
	paintedtime = 0;
	s_rawend = 0;
	S_BackendStart ();
	if (sound_started)
	{
		Con_Printf ("Audio: %d bit, %s, %d Hz\n", snd_output.samplebits, (snd_output.channels == 2) ? "stereo" : "mono", snd_output.speed);
		if (snd_output.speed != oldspeed)
			S_ClearAll ();
	}
	else
	{
		Con_Printf ("Failed initializing sound\n");
		BGM_Stop ();
	}
	SDL_UnlockMutex (snd_mutex);
}

static void S_OpenALChanged (cvar_t *var)
{
	S_RestartBackend ();
}

static void S_MixSpeedChanged (cvar_t *var)
{
	if (var->value < 8000 || var->value > 192000)
	{
		Con_Printf ("snd_mixspeed must be between 8000 and 192000\n");
		Cvar_SetQuick (&snd_mixspeed, "44100");
		return;
	}
	S_RestartBackend ();
}

/*
================
S_Startup
================
*/
void S_Startup (void)
{
	if (!snd_initialized)
		return;

	S_BackendStart ();

	if (!sound_started)
		Con_Printf ("Failed initializing sound\n");
	else
		Con_Printf ("Audio: %d bit, %s, %d Hz\n", snd_output.samplebits, (snd_output.channels == 2) ? "stereo" : "mono", snd_output.speed);
}

/*
================
S_Init
================
*/
void S_Init (void)
{
	int i;

	if (snd_initialized)
	{
		Con_Printf ("Sound is already initialized\n");
		return;
	}

	snd_mutex = SDL_CreateMutex ();

	Cvar_RegisterVariable (&nosound);
	Cvar_RegisterVariable (&sfxvolume);
	Cvar_RegisterVariable (&precache);
	Cvar_RegisterVariable (&loadas8bit);
	Cvar_RegisterVariable (&bgmvolume);
	Cvar_RegisterVariable (&ambient_level);
	Cvar_RegisterVariable (&ambient_fade);
	Cvar_RegisterVariable (&snd_show);
	Cvar_RegisterVariable (&s_openal_hrtf);
	Cvar_RegisterVariable (&s_openal_max_sources);
	Cvar_RegisterVariable (&snd_mixspeed);

	if (safemode || COM_CheckParm ("-nosound"))
		return;

	Con_Printf ("\nSound Initialization\n");

	Cmd_AddCommand ("play", S_Play);
	Cmd_AddCommand ("playvol", S_PlayVol);
	Cmd_AddCommand ("stopsound", S_StopAllSoundsC);
	Cmd_AddCommand ("soundlist", S_SoundList);
	Cmd_AddCommand ("soundinfo", S_SoundInfo_f);

	i = COM_CheckParm ("-mixspeed");
	if (i && i < com_argc - 1)
	{
		Cvar_SetQuick (&snd_mixspeed, com_argv[i + 1]);
	}

	Cvar_SetCallback (&s_openal_hrtf, S_OpenALChanged);
	Cvar_SetCallback (&s_openal_max_sources, S_OpenALChanged);
	Cvar_SetCallback (&snd_mixspeed, S_MixSpeedChanged);

	known_sfx = (sfx_t *)Mem_Alloc (MAX_SFX * sizeof (sfx_t));
	num_sfx = 0;

	snd_initialized = true;

	S_Startup ();
	if (sound_started == 0)
		return;

	ambient_sfx[AMBIENT_WATER] = S_PrecacheSound ("ambience/water1.wav");
	ambient_sfx[AMBIENT_SKY] = S_PrecacheSound ("ambience/wind2.wav");

	S_CodecInit ();

	S_StopAllSounds (true);

	sound_ready = true;
}

// =======================================================================
// Shutdown sound engine
// =======================================================================
void S_Shutdown (void)
{
	if (!sound_started)
		return;

	sound_started = 0;
	snd_blocked = 0;

	S_CodecShutdown ();

	S_BackendShutdown ();
}

// =======================================================================
// Load a sound
// =======================================================================

/*
==================
S_FindName

==================
*/
static sfx_t *S_FindName (const char *name)
{
	int    i;
	sfx_t *sfx;

	if (!name)
		Sys_Error ("S_FindName: NULL");

	if (strlen (name) >= MAX_QPATH)
		Sys_Error ("Sound name too long: %s", name);

	// see if already loaded
	for (i = 0; i < num_sfx; i++)
	{
		if (!strcmp (known_sfx[i].name, name))
		{
			return &known_sfx[i];
		}
	}

	if (num_sfx == MAX_SFX)
		Sys_Error ("S_FindName: out of sfx_t");

	sfx = &known_sfx[i];
	q_strlcpy (sfx->name, name, sizeof (sfx->name));

	num_sfx++;

	return sfx;
}

/*
==================
S_TouchSound

==================
*/
void S_TouchSound (const char *name)
{
	if (!sound_started)
		return;
	S_FindName (name);
}

/*
==================
S_PrecacheSound

==================
*/
sfx_t *S_PrecacheSound (const char *name)
{
	sfx_t *sfx;

	if (!sound_started || nosound.value)
		return NULL;

	sfx = S_FindName (name);

	// cache it in
	if (precache.value)
		S_LoadSound (sfx);

	return sfx;
}

//=============================================================================

/*
=================
SND_PickChannel

picks a channel based on priorities, empty slots, number of channels
=================
*/
channel_t *SND_PickChannel (int entnum, int entchannel)
{
	int ch_idx;
	int first_to_die;
	int life_left;

	// Check for replacement sound, or find the best one to replace
	first_to_die = -1;
	life_left = 0x7fffffff;
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++)
	{
		if (entchannel != 0 // channel 0 never overrides
		    && snd_channels[ch_idx].entnum == entnum && (snd_channels[ch_idx].entchannel == entchannel || entchannel == -1))
		{ // always override sound from same entity
			first_to_die = ch_idx;
			break;
		}

		// don't let monster sounds override player sounds
		if (snd_channels[ch_idx].entnum == cl.viewentity && entnum != cl.viewentity && snd_channels[ch_idx].sfx)
			continue;

		if (snd_channels[ch_idx].end - paintedtime < life_left)
		{
			life_left = snd_channels[ch_idx].end - paintedtime;
			first_to_die = ch_idx;
		}
	}

	if (first_to_die == -1)
		return NULL;

	if (snd_channels[first_to_die].sfx)
		snd_channels[first_to_die].sfx = NULL;

	return &snd_channels[first_to_die];
}

/*
=================
SND_Spatialize

spatializes a channel
=================
*/
void SND_Spatialize (channel_t *ch)
{
	vec_t  dot;
	vec_t  dist;
	vec_t  lscale, rscale, scale;
	vec3_t source_vec;

	// anything coming from the view entity will always be full volume
	if (ch->entnum == cl.viewentity)
	{
		ch->leftvol = ch->master_vol;
		ch->rightvol = ch->master_vol;
		return;
	}

	// calculate stereo seperation and distance attenuation
	VectorSubtract (ch->origin, listener_origin, source_vec);
	dist = VectorNormalize (source_vec) * ch->dist_mult;
	dot = DotProduct (listener_right, source_vec);

	rscale = 1.0 + dot;
	lscale = 1.0 - dot;

	// add in distance effect
	scale = (1.0 - dist) * rscale;
	ch->rightvol = (int)(ch->master_vol * scale);
	if (ch->rightvol < 0)
		ch->rightvol = 0;

	scale = (1.0 - dist) * lscale;
	ch->leftvol = (int)(ch->master_vol * scale);
	if (ch->leftvol < 0)
		ch->leftvol = 0;
}

// =======================================================================
// Start a sound effect
// =======================================================================

void S_StartSound (int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)
{
	channel_t  *target_chan, *check;
	sfxcache_t *sc;
	int         ch_idx;
	int         skip;

	SDL_LockMutex (snd_mutex);
	if (!sound_started || !sfx || nosound.value)
		goto unlock_mutex;

	// pick a channel to play on
	target_chan = SND_PickChannel (entnum, entchannel);
	if (!target_chan)
		goto unlock_mutex;

	// spatialize
	memset (target_chan, 0, sizeof (*target_chan));
	VectorCopy (origin, target_chan->origin);
	target_chan->dist_mult = attenuation / sound_nominal_clip_dist;
	target_chan->master_vol = (int)(fvol * 255);
	target_chan->entnum = entnum;
	target_chan->entchannel = entchannel;
	SND_Spatialize (target_chan);

	if (!target_chan->leftvol && !target_chan->rightvol)
		goto unlock_mutex;

	// new channel
	sc = S_LoadSound (sfx);
	if (!sc)
	{
		target_chan->sfx = NULL;
		goto unlock_mutex; // couldn't load the sound's data
	}

	target_chan->sfx = sfx;
	target_chan->pos = 0.0;
	target_chan->end = paintedtime + sc->length;

	// if an identical sound has also been started this frame, offset the pos
	// a bit to keep it from just making the first one louder
	check = &snd_channels[NUM_AMBIENTS];
	for (ch_idx = NUM_AMBIENTS; ch_idx < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ch_idx++, check++)
	{
		if (check == target_chan)
			continue;
		if (check->sfx == sfx && !check->pos)
		{
			/* LordHavoc: fixed skip calculations */
			skip = 0.1 * snd_output.speed;
			if (skip > sc->length)
				skip = sc->length;
			if (skip > 0)
				skip = rand () % skip;
			target_chan->pos += skip;
			target_chan->end -= skip;
			break;
		}
	}

	SNDAL_StartChannel (target_chan);

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

void S_StopSound (int entnum, int entchannel)
{
	int i;

	SDL_LockMutex (snd_mutex);

	for (i = 0; i < MAX_DYNAMIC_CHANNELS; i++)
	{
		if (snd_channels[i].entnum == entnum && snd_channels[i].entchannel == entchannel)
		{
			snd_channels[i].end = 0;
			snd_channels[i].sfx = NULL;
			SNDAL_StopChannel (&snd_channels[i]);
			goto unlock_mutex;
		}
	}

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

void S_StopAllSounds (qboolean clear)
{
	int i;

	if (!snd_initialized)
		return;

	SDL_LockMutex (snd_mutex);
	if (!sound_started)
		goto unlock_mutex;

	total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS; // no statics

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (snd_channels[i].sfx)
			snd_channels[i].sfx = NULL;
	}

	memset (snd_channels, 0, MAX_CHANNELS * sizeof (channel_t));

	SNDAL_StopAll ();

	if (clear)
		S_ClearBuffer ();

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

static void S_StopAllSoundsC (void)
{
	S_StopAllSounds (true);
}

void S_ClearBuffer (void)
{
	SDL_LockMutex (snd_mutex);

	if (!sound_started || !snd_output.ready)
		goto unlock_mutex;

	s_rawend = 0;
	SNDAL_ClearBuffer ();

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

/*
=================
S_StaticSound
=================
*/
void S_StaticSound (sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
	channel_t  *ss;
	sfxcache_t *sc;

	if (!sfx)
		return;

	SDL_LockMutex (snd_mutex);

	if (total_channels == MAX_CHANNELS)
	{
		Con_Printf ("total_channels == MAX_CHANNELS\n");
		goto unlock_mutex;
	}

	ss = &snd_channels[total_channels];
	total_channels++;

	sc = S_LoadSound (sfx);
	if (!sc)
		goto unlock_mutex;

	if (sc->loopstart == -1)
	{
		Con_Printf ("Sound %s not looped\n", sfx->name);
		goto unlock_mutex;
	}

	ss->sfx = sfx;
	VectorCopy (origin, ss->origin);
	ss->master_vol = (int)vol;
	ss->dist_mult = (attenuation / 64) / sound_nominal_clip_dist;
	ss->end = paintedtime + sc->length;

	SND_Spatialize (ss);

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

//=============================================================================

/*
===================
S_UpdateAmbientSounds
===================
*/
static void S_UpdateAmbientSounds (void)
{
	mleaf_t     *l;
	int          ambient_channel;
	channel_t   *chan;
	static float vol, levels[NUM_AMBIENTS]; // Spike: fixing ambient levels not changing at high enough framerates due to integer precison.

	SDL_LockMutex (snd_mutex);

	// no ambients when disconnected
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		goto unlock_mutex;
	// calc ambient sound levels
	if (!cl.worldmodel || cl.worldmodel->needload)
		goto unlock_mutex;

	l = Mod_PointInLeaf (listener_origin, cl.worldmodel);
	if (!l || !ambient_level.value)
	{
		for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
			snd_channels[ambient_channel].sfx = NULL;
		goto unlock_mutex;
	}

	for (ambient_channel = 0; ambient_channel < NUM_AMBIENTS; ambient_channel++)
	{
		chan = &snd_channels[ambient_channel];
		chan->sfx = ambient_sfx[ambient_channel];

		vol = (int)(ambient_level.value * l->ambient_sound_level[ambient_channel]);
		if (vol < 8)
			vol = 0;

		// don't adjust volume too fast
		if (levels[ambient_channel] < vol)
		{
			levels[ambient_channel] += (host_frametime * ambient_fade.value);
			if (levels[ambient_channel] > vol)
				levels[ambient_channel] = vol;
		}
		else if (chan->master_vol > vol)
		{
			levels[ambient_channel] -= (host_frametime * ambient_fade.value);
			if (levels[ambient_channel] < vol)
				levels[ambient_channel] = vol;
		}

		chan->leftvol = chan->rightvol = chan->master_vol = levels[ambient_channel];
	}

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

/*
===================
S_RawSamples		(from QuakeII)

Streaming music support. Byte swapping
of data must be handled by the codec.
Expects data in signed 16 bit, or unsigned
8 bit format.
===================
*/
int S_RawSamplesCursor (void)
{
	return SNDAL_RawPosition ();
}

void S_RawSamples (int samples, int rate, int width, int channels, byte *data, float volume)
{
	int   i;
	int   src, dst;
	float scale;
	int   intVolume;

	if (!snd_output.ready)
		return;

	if (s_rawend < S_RawSamplesCursor ())
		s_rawend = S_RawSamplesCursor ();

	scale = (float)rate / snd_output.speed;
	intVolume = (int)(256 * volume);

	if (channels == 2 && width == 2)
	{
		for (i = 0;; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			s_rawsamples[dst].left = ((short *)data)[src * 2] * intVolume;
			s_rawsamples[dst].right = ((short *)data)[src * 2 + 1] * intVolume;
		}
	}
	else if (channels == 1 && width == 2)
	{
		for (i = 0;; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			s_rawsamples[dst].left = ((short *)data)[src] * intVolume;
			s_rawsamples[dst].right = ((short *)data)[src] * intVolume;
		}
	}
	else if (channels == 2 && width == 1)
	{
		intVolume *= 256;

		for (i = 0;; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			//	s_rawsamples [dst].left = ((signed char *) data)[src * 2] * intVolume;
			//	s_rawsamples [dst].right = ((signed char *) data)[src * 2 + 1] * intVolume;
			s_rawsamples[dst].left = (((byte *)data)[src * 2] - 128) * intVolume;
			s_rawsamples[dst].right = (((byte *)data)[src * 2 + 1] - 128) * intVolume;
		}
	}
	else if (channels == 1 && width == 1)
	{
		intVolume *= 256;

		for (i = 0;; i++)
		{
			src = i * scale;
			if (src >= samples)
				break;
			dst = s_rawend & (MAX_RAW_SAMPLES - 1);
			s_rawend++;
			//	s_rawsamples [dst].left = ((signed char *) data)[src] * intVolume;
			//	s_rawsamples [dst].right = ((signed char *) data)[src] * intVolume;
			s_rawsamples[dst].left = (((byte *)data)[src] - 128) * intVolume;
			s_rawsamples[dst].right = (((byte *)data)[src] - 128) * intVolume;
		}
	}
}

/*
============
S_Update

Called once each time through the main loop
============
*/
void S_Update (vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	int        i;
	int        total;
	channel_t *ch;

	SDL_LockMutex (snd_mutex);
	if (!sound_started || (snd_blocked > 0))
		goto unlock_mutex;

	VectorCopy (origin, listener_origin);
	VectorCopy (forward, listener_forward);
	VectorCopy (right, listener_right);
	VectorCopy (up, listener_up);

	// update general area ambient sound sources
	S_UpdateAmbientSounds ();

	// update spatialization for static and dynamic sounds
	ch = snd_channels + NUM_AMBIENTS;
	for (i = NUM_AMBIENTS; i < total_channels; i++, ch++)
	{
		if (!ch->sfx)
			continue;
		SND_Spatialize (ch); // respatialize channel
	}

	//
	// debugging output
	//
	if (snd_show.value)
	{
		total = 0;
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (ch->sfx && (ch->leftvol || ch->rightvol))
			{
				//	Con_Printf ("%3i %3i %s\n", ch->leftvol, ch->rightvol, ch->sfx->name);
				total++;
			}
		}

		Con_Printf ("----(%i)----\n", total);
	}

	// add raw data from streamed samples
	//	BGM_Update();	// moved to the main loop just before S_Update ()

	// mix some sound
	SNDAL_Update ();

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

void S_ExtraUpdate (void)
{
	if (!snd_initialized)
		return;

	SDL_LockMutex (snd_mutex);
	SNDAL_ExtraUpdate ();
	SDL_UnlockMutex (snd_mutex);
}

void S_BlockSound (void)
{
	SDL_LockMutex (snd_mutex);
	/* FIXME: do we really need the blocking at the
	 * driver level?
	 */
	if (sound_started && snd_blocked == 0) /* ++snd_blocked == 1 */
	{
		snd_blocked = 1;
		SNDAL_BlockSound ();
	}
	SDL_UnlockMutex (snd_mutex);
}

void S_UnblockSound (void)
{
	SDL_LockMutex (snd_mutex);

	if (!sound_started || !snd_blocked)
		goto unlock_mutex;
	if (snd_blocked == 1) /* --snd_blocked == 0 */
	{
		snd_blocked = 0;
		SNDAL_UnblockSound ();
	}

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

/*
===============================================================================
S_ClearAll
===============================================================================
*/
void S_ClearAll (void)
{
	SDL_LockMutex (snd_mutex);

	SNDAL_ClearAll ();

	for (int i = 0; i < num_sfx; ++i)
	{
		if (known_sfx[i].cache)
		{
			Mem_Free (known_sfx[i].cache);
			known_sfx[i].cache = NULL;
		}
	}

	SDL_UnlockMutex (snd_mutex);
}

/*
===============================================================================

console functions

===============================================================================
*/

static void S_Play (void)
{
	static int hash = 345;
	int        i;
	char       name[256];
	sfx_t     *sfx;

	i = 1;
	while (i < Cmd_Argc ())
	{
		q_strlcpy (name, Cmd_Argv (i), sizeof (name));
		if (!strrchr (Cmd_Argv (i), '.'))
		{
			q_strlcat (name, ".wav", sizeof (name));
		}
		sfx = S_PrecacheSound (name);
		S_StartSound (hash++, 0, sfx, listener_origin, 1.0, 1.0);
		i++;
	}
}

static void S_PlayVol (void)
{
	static int hash = 543;
	int        i;
	float      vol;
	char       name[256];
	sfx_t     *sfx;

	i = 1;
	while (i < Cmd_Argc ())
	{
		q_strlcpy (name, Cmd_Argv (i), sizeof (name));
		if (!strrchr (Cmd_Argv (i), '.'))
		{
			q_strlcat (name, ".wav", sizeof (name));
		}
		sfx = S_PrecacheSound (name);
		vol = atof (Cmd_Argv (i + 1));
		S_StartSound (hash++, 0, sfx, listener_origin, vol, 1.0);
		i += 2;
	}
}

static void S_SoundList (void)
{
	int         i;
	sfx_t      *sfx;
	sfxcache_t *sc;
	int         size, total;

	total = 0;
	for (sfx = known_sfx, i = 0; i < num_sfx; i++, sfx++)
	{
		sc = (sfxcache_t *)sfx->cache;
		if (!sc)
			continue;
		size = sc->length * sc->width * (sc->stereo + 1);
		total += size;
		if (sc->loopstart >= 0)
			Con_SafePrintf ("L"); // johnfitz -- was Con_Printf
		else
			Con_SafePrintf (" ");                                             // johnfitz -- was Con_Printf
		Con_SafePrintf ("(%2db) %6i : %s\n", sc->width * 8, size, sfx->name); // johnfitz -- was Con_Printf
	}
	Con_Printf ("%i sounds, %i bytes\n", num_sfx, total); // johnfitz -- added count
}

void S_LocalSound (const char *name)
{
	sfx_t *sfx;

	SDL_LockMutex (snd_mutex);

	if (nosound.value)
		goto unlock_mutex;
	if (!sound_started)
		goto unlock_mutex;

	sfx = S_PrecacheSound (name);
	if (!sfx)
	{
		Con_Printf ("S_LocalSound: can't cache %s\n", name);
		goto unlock_mutex;
	}
	S_StartSound (cl.viewentity, -1, sfx, vec3_origin, 1, 1);

unlock_mutex:
	SDL_UnlockMutex (snd_mutex);
}

void S_ClearPrecache (void) {}

void S_BeginPrecaching (void) {}

void S_EndPrecaching (void) {}
