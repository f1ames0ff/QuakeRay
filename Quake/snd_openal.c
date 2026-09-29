/*
 * snd_openal.c -- OpenAL Soft output backend
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
#include "snd_openal.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

extern cvar_t s_openal_max_sources;

snd_output_t snd_output;

#define SNDAL_MAX_BUFFERS   1024
#define SNDAL_MUSIC_BUFFERS 8
#define SNDAL_MUSIC_SAMPLES 1024

#ifndef ALC_HRTF_SOFT
#define ALC_HRTF_SOFT                    0x1992
#define ALC_DONT_CARE_SOFT               0x0002
#define ALC_HRTF_STATUS_SOFT             0x1993
#define ALC_HRTF_DISABLED_SOFT           0x0000
#define ALC_HRTF_ENABLED_SOFT            0x0001
#define ALC_HRTF_DENIED_SOFT             0x0002
#define ALC_HRTF_REQUIRED_SOFT           0x0003
#define ALC_HRTF_HEADPHONES_DETECTED_SOFT 0x0004
#define ALC_HRTF_UNSUPPORTED_FORMAT_SOFT 0x0005
#endif

typedef struct
{
	ALuint      buffer;
	sfx_t      *sfx;
	sfxcache_t *cache;
} sndal_buffer_t;

typedef struct
{
	ALuint     source;
	channel_t *channel;
	int        index;
	qboolean   started;
	qboolean   looping;
} sndal_source_t;

static ALCdevice         *sndal_device;
static ALCcontext        *sndal_context;
static qboolean           sndal_context_current;
static qboolean           sndal_active;
static qboolean           sndal_blocked;
static qboolean           sndal_has_direct_channels;
static qboolean           sndal_has_pause_device;
static int                sndal_hrtf_status = -1;
static int                sndal_rawpos;
static double             sndal_clockfrac;
static double             sndal_lasttime;

static sndal_source_t     sndal_sources[MAX_CHANNELS];
static int                sndal_numsources;
static int                sndal_binding[MAX_CHANNELS];

static sndal_buffer_t     sndal_buffers[SNDAL_MAX_BUFFERS];
static int                sndal_numbuffers;

static ALuint             sndal_music_source;
static ALuint             sndal_music_buffers[SNDAL_MUSIC_BUFFERS];
static ALuint             sndal_music_free[SNDAL_MUSIC_BUFFERS];
static int                sndal_music_numfree;
static int                sndal_music_queued;
static short              sndal_music_pcm[SNDAL_MUSIC_SAMPLES * 2];

static LPALCDEVICEPAUSESOFT  sndal_pause_device;
static LPALCDEVICERESUMESOFT sndal_resume_device;

static int SNDAL_AllocSource (void);
static void SNDAL_ReleaseSlot (int slot);
static ALuint SNDAL_GetBuffer (channel_t *ch);
static void SNDAL_DeleteBuffers (void);
static void SNDAL_FlushMusic (void);
static void SNDAL_MusicShutdown (void);

static const char *SNDAL_HrtfStatusName (int status)
{
	switch (status)
	{
	case ALC_HRTF_DISABLED_SOFT:
		return "disabled";
	case ALC_HRTF_ENABLED_SOFT:
		return "enabled";
	case ALC_HRTF_DENIED_SOFT:
		return "denied";
	case ALC_HRTF_REQUIRED_SOFT:
		return "required";
	case ALC_HRTF_HEADPHONES_DETECTED_SOFT:
		return "enabled (headphones detected)";
	case ALC_HRTF_UNSUPPORTED_FORMAT_SOFT:
		return "unsupported format";
	default:
		return "not supported";
	}
}

static qboolean SNDAL_IsLooping (sfxcache_t *sc)
{
	return (sc->loopstart >= 0 && sc->loopstart < sc->length) ? true : false;
}

static float SNDAL_ChannelDistance (channel_t *ch)
{
	int    index = (int)(ch - snd_channels);
	vec3_t direction;

	if (ch->entnum == cl.viewentity || index < NUM_AMBIENTS)
		return 0.0f;
	VectorSubtract (ch->origin, listener_origin, direction);
	return VectorLength (direction) * ch->dist_mult;
}

static qboolean SNDAL_ChannelAudible (channel_t *ch)
{
	return (SNDAL_ChannelDistance (ch) < 1.0f) ? true : false;
}

static float SNDAL_ChannelGain (channel_t *ch)
{
	float gain = SNDAL_ChannelDistance (ch);

	if (gain >= 1.0f)
		return 0.0f;
	gain = (1.0f - gain) * (ch->master_vol / 510.0f) * sfxvolume.value;
	if (gain < 0.0f)
		gain = 0.0f;
	else if (gain > 1.0f)
		gain = 1.0f;
	return gain;
}

static void SNDAL_SetupSource (channel_t *ch, int slot)
{
	ALuint    source = sndal_sources[slot].source;
	int       index = (int)(ch - snd_channels);
	qboolean  relative = (index < NUM_AMBIENTS || ch->entnum == cl.viewentity) ? true : false;

	alSourcei (source, AL_SOURCE_RELATIVE, relative ? AL_TRUE : AL_FALSE);
	if (relative)
		alSource3f (source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	else
		alSource3f (source, AL_POSITION, ch->origin[0], ch->origin[1], ch->origin[2]);
	alSourcef (source, AL_GAIN, SNDAL_ChannelGain (ch));
}

static int SNDAL_AllocSource (void)
{
	int i;
	int candidate = -1;

	for (i = 0; i < sndal_numsources; i++)
	{
		if (!sndal_sources[i].channel)
			return i;
		if (sndal_sources[i].index >= MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS && !SNDAL_ChannelAudible (sndal_sources[i].channel))
			candidate = i;
	}
	if (candidate >= 0)
	{
		SNDAL_ReleaseSlot (candidate);
		return candidate;
	}
	return -1;
}

static void SNDAL_ReleaseSlot (int slot)
{
	if (slot < 0 || slot >= sndal_numsources)
		return;
	if (!sndal_sources[slot].channel)
		return;

	alSourceStop (sndal_sources[slot].source);
	alSourcei (sndal_sources[slot].source, AL_BUFFER, 0);
	alSourcei (sndal_sources[slot].source, AL_LOOPING, AL_FALSE);
	sndal_binding[sndal_sources[slot].index] = -1;
	sndal_sources[slot].channel = NULL;
	sndal_sources[slot].index = -1;
	sndal_sources[slot].started = false;
	sndal_sources[slot].looping = false;
}

static ALuint SNDAL_GetBuffer (channel_t *ch)
{
	sfx_t      *sfx = ch->sfx;
	sfxcache_t *sc = sfx->cache;
	ALuint      buffer;
	ALenum      error;
	int         i;

	if (!sc)
		return 0;

	for (i = 0; i < sndal_numbuffers; i++)
	{
		if (sndal_buffers[i].sfx == sfx && sndal_buffers[i].cache == sc)
			return sndal_buffers[i].buffer;
	}
	if (sndal_numbuffers >= SNDAL_MAX_BUFFERS)
		return 0;

	alGetError ();
	alGenBuffers (1, &buffer);
	if (!buffer)
		return 0;

	if (sc->width == 1)
	{
		short *pcm = (short *)Mem_Alloc ((size_t)sc->length * sizeof (short));

		if (!pcm)
		{
			alDeleteBuffers (1, &buffer);
			return 0;
		}
		for (i = 0; i < sc->length; i++)
			pcm[i] = (short)(((signed char *)sc->data)[i] << 8);
		alBufferData (buffer, AL_FORMAT_MONO16, pcm, (ALsizei)(sc->length * (int)sizeof (short)), sc->speed);
		Mem_Free (pcm);
	}
	else
	{
		alBufferData (buffer, AL_FORMAT_MONO16, sc->data, (ALsizei)(sc->length * 2), sc->speed);
	}

	if (SNDAL_IsLooping (sc))
	{
		ALint points[2];

		points[0] = sc->loopstart;
		points[1] = sc->length;
		alBufferiv (buffer, AL_LOOP_POINTS_SOFT, points);
	}

	error = alGetError ();
	if (error != AL_NO_ERROR)
	{
		alDeleteBuffers (1, &buffer);
		return 0;
	}

	sndal_buffers[sndal_numbuffers].buffer = buffer;
	sndal_buffers[sndal_numbuffers].sfx = sfx;
	sndal_buffers[sndal_numbuffers].cache = sc;
	sndal_numbuffers++;
	return buffer;
}

static void SNDAL_DeleteBuffers (void)
{
	int i;

	for (i = 0; i < sndal_numbuffers; i++)
	{
		if (sndal_buffers[i].buffer)
			alDeleteBuffers (1, &sndal_buffers[i].buffer);
	}
	sndal_numbuffers = 0;
}

static void SNDAL_ConfigureSource (channel_t *ch, int slot)
{
	ALuint      source;
	sfxcache_t *sc = ch->sfx->cache;
	ALuint      buffer;

	buffer = SNDAL_GetBuffer (ch);
	if (!buffer)
	{
		SNDAL_ReleaseSlot (slot);
		return;
	}

	source = sndal_sources[slot].source;
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)buffer);
	alSourcei (source, AL_LOOPING, SNDAL_IsLooping (sc) ? AL_TRUE : AL_FALSE);
	alSourcei (source, AL_SAMPLE_OFFSET, ch->pos);
	SNDAL_SetupSource (ch, slot);

	sndal_sources[slot].started = false;
	sndal_sources[slot].looping = SNDAL_IsLooping (sc);
	if (!sndal_blocked)
	{
		alSourcePlay (source);
		sndal_sources[slot].started = true;
	}
}

void SNDAL_StartChannel (channel_t *ch)
{
	int index, slot;

	if (!sndal_active || !ch || !ch->sfx || !ch->sfx->cache)
		return;

	index = (int)(ch - snd_channels);
	if (index < 0 || index >= MAX_CHANNELS)
		return;

	slot = sndal_binding[index];
	if (slot < 0)
	{
		slot = SNDAL_AllocSource ();
		if (slot < 0)
			return;
		sndal_sources[slot].channel = ch;
		sndal_sources[slot].index = index;
		sndal_binding[index] = slot;
	}
	SNDAL_ConfigureSource (ch, slot);
}

void SNDAL_StopChannel (channel_t *ch)
{
	int index;

	if (!sndal_active || !ch)
		return;
	index = (int)(ch - snd_channels);
	if (index < 0 || index >= MAX_CHANNELS)
		return;
	if (sndal_binding[index] >= 0)
		SNDAL_ReleaseSlot (sndal_binding[index]);
}

void SNDAL_StopAll (void)
{
	int i;

	if (!sndal_active)
		return;
	for (i = 0; i < sndal_numsources; i++)
	{
		if (sndal_sources[i].channel)
			SNDAL_ReleaseSlot (i);
	}
}

void SNDAL_ClearBuffer (void)
{
	if (!sndal_active)
		return;
	SNDAL_StopAll ();
	SNDAL_FlushMusic ();
}

void SNDAL_ClearAll (void)
{
	if (!sndal_active)
		return;
	SNDAL_StopAll ();
	SNDAL_DeleteBuffers ();
}

static void SNDAL_SyncSource (int slot)
{
	channel_t  *ch = sndal_sources[slot].channel;
	ALuint      source = sndal_sources[slot].source;
	sfxcache_t *sc;
	ALint       state = 0;
	ALint       offset = 0;
	qboolean    audible;

	if (!ch)
		return;
	if (!ch->sfx)
	{
		SNDAL_ReleaseSlot (slot);
		return;
	}
	if (!ch->sfx->cache && !S_LoadSound (ch->sfx))
	{
		SNDAL_ReleaseSlot (slot);
		return;
	}
	sc = ch->sfx->cache;
	audible = SNDAL_ChannelAudible (ch);

	alGetSourcei (source, AL_SOURCE_STATE, &state);
	if (!sndal_sources[slot].looping)
	{
		ALint remaining = ch->end - paintedtime;

		if (sndal_sources[slot].started && state == AL_STOPPED)
		{
			ch->sfx = NULL;
			SNDAL_ReleaseSlot (slot);
			return;
		}
		if (state != AL_PLAYING && remaining <= 0)
		{
			ch->sfx = NULL;
			SNDAL_ReleaseSlot (slot);
			return;
		}
		if (state != AL_PLAYING && sc->length - ch->pos > remaining)
		{
			ch->pos = sc->length - remaining;
			alSourcei (source, AL_SAMPLE_OFFSET, ch->pos);
		}
	}

	SNDAL_SetupSource (ch, slot);

	if (!audible)
	{
		if (state == AL_PLAYING)
			alSourcePause (source);
		return;
	}
	if (!sndal_blocked && state != AL_PLAYING)
	{
		alSourcePlay (source);
		sndal_sources[slot].started = true;
		state = AL_PLAYING;
	}

	if (state == AL_PLAYING)
	{
		alGetSourcei (source, AL_SAMPLE_OFFSET, &offset);
		if (offset >= 0 && offset <= sc->length)
			ch->pos = (int)offset;
	}
	if (sndal_sources[slot].looping)
		ch->end = paintedtime + (sc->length - ch->pos);
}

static void SNDAL_UpdateListener (void)
{
	ALfloat orientation[6];
	ALfloat length;

	alListenerfv (AL_POSITION, listener_origin);

	length = listener_forward[0] * listener_forward[0] + listener_forward[1] * listener_forward[1] + listener_forward[2] * listener_forward[2];
	length += listener_up[0] * listener_up[0] + listener_up[1] * listener_up[1] + listener_up[2] * listener_up[2];
	if (length > 0.0001f)
	{
		orientation[0] = listener_forward[0];
		orientation[1] = listener_forward[1];
		orientation[2] = listener_forward[2];
		orientation[3] = listener_up[0];
		orientation[4] = listener_up[1];
		orientation[5] = listener_up[2];
		alListenerfv (AL_ORIENTATION, orientation);
	}
}

static void SNDAL_UpdateMusic (void)
{
	ALint  processed = 0;
	ALint  state = 0;
	ALuint buffer;
	int    available, i, n, sample, left, right;

	if (!sndal_music_source)
		return;

	alGetSourcei (sndal_music_source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0)
	{
		buffer = 0;
		alSourceUnqueueBuffers (sndal_music_source, 1, &buffer);
		if (!buffer)
			break;
		if (sndal_music_numfree < SNDAL_MUSIC_BUFFERS)
			sndal_music_free[sndal_music_numfree++] = buffer;
		sndal_music_queued--;
	}
	if (sndal_music_queued < 0)
		sndal_music_queued = 0;

	available = s_rawend - sndal_rawpos;
	while (sndal_music_numfree > 0 && available > 0)
	{
		n = (available > SNDAL_MUSIC_SAMPLES) ? SNDAL_MUSIC_SAMPLES : available;
		for (i = 0; i < n; i++)
		{
			sample = (sndal_rawpos + i) & (MAX_RAW_SAMPLES - 1);
			left = s_rawsamples[sample].left / 512;
			right = s_rawsamples[sample].right / 512;
			if (left < -32768)
				left = -32768;
			else if (left > 32767)
				left = 32767;
			if (right < -32768)
				right = -32768;
			else if (right > 32767)
				right = 32767;
			sndal_music_pcm[i * 2] = (short)left;
			sndal_music_pcm[i * 2 + 1] = (short)right;
		}

		buffer = sndal_music_free[--sndal_music_numfree];
		alGetError ();
		alBufferData (buffer, AL_FORMAT_STEREO16, sndal_music_pcm, (ALsizei)(n * 4), snd_output.speed);
		if (alGetError () != AL_NO_ERROR)
		{
			sndal_music_free[sndal_music_numfree++] = buffer;
			break;
		}
		alSourceQueueBuffers (sndal_music_source, 1, &buffer);
		if (alGetError () != AL_NO_ERROR)
		{
			sndal_music_free[sndal_music_numfree++] = buffer;
			break;
		}
		sndal_music_queued++;
		sndal_rawpos += n;
		available -= n;
	}

	if (sndal_rawpos > 0x40000000)
	{
		sndal_rawpos -= 0x40000000;
		s_rawend -= 0x40000000;
	}

	if (sndal_music_queued > 0 && !sndal_blocked)
	{
		alGetSourcei (sndal_music_source, AL_SOURCE_STATE, &state);
		if (state != AL_PLAYING)
			alSourcePlay (sndal_music_source);
	}
}

static void SNDAL_FlushMusic (void)
{
	int i;

	if (!sndal_music_source)
		return;

	alSourceStop (sndal_music_source);
	alSourcei (sndal_music_source, AL_BUFFER, 0);
	for (i = 0; i < SNDAL_MUSIC_BUFFERS; i++)
		sndal_music_free[i] = sndal_music_buffers[i];
	sndal_music_numfree = SNDAL_MUSIC_BUFFERS;
	sndal_music_queued = 0;
	sndal_rawpos = 0;
}

static void SNDAL_AdvanceClock (void)
{
	double now, delta;
	int    elapsed;

	if (!snd_output.ready || snd_output.speed <= 0)
		return;

	now = Sys_DoubleTime ();
	if (sndal_lasttime <= 0.0)
	{
		sndal_lasttime = now;
		return;
	}
	delta = now - sndal_lasttime;
	sndal_lasttime = now;
	if (delta < 0.0)
		delta = 0.0;
	else if (delta > 1.0)
		delta = 1.0;

	sndal_clockfrac += delta * snd_output.speed;
	elapsed = (int)sndal_clockfrac;
	if (elapsed <= 0)
		return;
	sndal_clockfrac -= elapsed;
	paintedtime += elapsed;

	if (paintedtime > 0x40000000)
	{
		int wrap = 0x40000000;
		int i;

		paintedtime -= wrap;
		for (i = 0; i < total_channels; i++)
			snd_channels[i].end -= wrap;
	}
}

void SNDAL_Update (void)
{
	int i, slot;

	if (!sndal_active)
		return;

	SNDAL_AdvanceClock ();
	SNDAL_UpdateListener ();
	SNDAL_UpdateMusic ();

	for (i = 0; i < total_channels; i++)
	{
		channel_t *ch = &snd_channels[i];

		if (!ch->sfx || (!ch->leftvol && !ch->rightvol))
			continue;
		if (!ch->sfx->cache && !S_LoadSound (ch->sfx))
			continue;
		if (sndal_binding[i] >= 0)
			continue;
		if (!SNDAL_IsLooping (ch->sfx->cache) && ch->end <= paintedtime)
		{
			ch->sfx = NULL;
			continue;
		}
		slot = SNDAL_AllocSource ();
		if (slot < 0)
			continue;
		sndal_sources[slot].channel = ch;
		sndal_sources[slot].index = i;
		sndal_binding[i] = slot;
		SNDAL_ConfigureSource (ch, slot);
	}

	for (i = 0; i < sndal_numsources; i++)
	{
		if (sndal_sources[i].channel)
			SNDAL_SyncSource (i);
	}
}

void SNDAL_ExtraUpdate (void)
{
	if (!sndal_active)
		return;
	SNDAL_UpdateMusic ();
}

int SNDAL_RawPosition (void)
{
	return sndal_rawpos;
}

qboolean SNDAL_HrtfEnabled (void)
{
	return (sndal_hrtf_status == ALC_HRTF_ENABLED_SOFT || sndal_hrtf_status == ALC_HRTF_HEADPHONES_DETECTED_SOFT) ? true : false;
}

void SNDAL_BlockSound (void)
{
	int i;

	if (!sndal_active)
		return;

	if (sndal_has_pause_device)
	{
		sndal_pause_device (sndal_device);
		return;
	}

	sndal_blocked = true;
	for (i = 0; i < sndal_numsources; i++)
	{
		if (sndal_sources[i].channel)
			alSourcePause (sndal_sources[i].source);
	}
	if (sndal_music_source && sndal_music_queued > 0)
		alSourcePause (sndal_music_source);
}

void SNDAL_UnblockSound (void)
{
	if (!sndal_active)
		return;

	if (sndal_has_pause_device)
	{
		sndal_resume_device (sndal_device);
		return;
	}

	sndal_blocked = false;
}

qboolean SNDAL_Init (void)
{
	ALCint        attributes[8];
	int           nattributes = 0;
	ALCint        frequency = 0;
	ALCint        status = -1;
	int           want, i;
	const char   *version, *device;
	ALboolean     has_hrtf;

	if (sndal_active)
		return true;

	sndal_device = alcOpenDevice (NULL);
	if (!sndal_device)
		goto fail;

	has_hrtf = alcIsExtensionPresent (sndal_device, "ALC_SOFT_HRTF");
	if (has_hrtf == AL_TRUE)
	{
		int mode = (int)s_openal_hrtf.value;

		attributes[nattributes++] = ALC_HRTF_SOFT;
		attributes[nattributes++] = (mode <= 0) ? ALC_FALSE : ((mode == 1) ? ALC_TRUE : ALC_DONT_CARE_SOFT);
	}
	attributes[nattributes++] = ALC_FREQUENCY;
	attributes[nattributes++] = (ALCint)snd_mixspeed.value;
	attributes[nattributes] = 0;

	sndal_context = alcCreateContext (sndal_device, attributes);
	if (!sndal_context)
		sndal_context = alcCreateContext (sndal_device, NULL);
	if (!sndal_context)
		goto fail;
	if (alcMakeContextCurrent (sndal_context) != ALC_TRUE)
	{
		alcDestroyContext (sndal_context);
		sndal_context = NULL;
		goto fail;
	}
	sndal_context_current = true;

	alGetError ();

	alDopplerFactor (0.0f);
	if (alIsExtensionPresent ("AL_SOFT_loop_points") != AL_TRUE)
	{
		Con_Printf ("OpenAL: AL_SOFT_loop_points is required\n");
		goto fail;
	}
	sndal_has_direct_channels = (alIsExtensionPresent ("AL_SOFT_direct_channels") == AL_TRUE) ? true : false;

	alcGetIntegerv (sndal_device, ALC_FREQUENCY, 1, &frequency);
	snd_output.speed = (frequency > 0) ? (int)frequency : (int)snd_mixspeed.value;
	snd_output.channels = 2;
	snd_output.samplebits = 16;
	snd_output.ready = true;

	for (i = 0; i < MAX_CHANNELS; i++)
		sndal_binding[i] = -1;

	alGenSources (1, &sndal_music_source);
	if (!sndal_music_source)
		goto fail;
	alSourcei (sndal_music_source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f (sndal_music_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef (sndal_music_source, AL_ROLLOFF_FACTOR, 0.0f);
	alSourcef (sndal_music_source, AL_GAIN, 1.0f);
	if (sndal_has_direct_channels)
		alSourcei (sndal_music_source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);

	alGenBuffers (SNDAL_MUSIC_BUFFERS, sndal_music_buffers);
	for (i = 0; i < SNDAL_MUSIC_BUFFERS; i++)
	{
		if (!sndal_music_buffers[i])
			goto fail;
		sndal_music_free[i] = sndal_music_buffers[i];
	}
	sndal_music_numfree = SNDAL_MUSIC_BUFFERS;
	sndal_music_queued = 0;
	sndal_rawpos = 0;
	sndal_clockfrac = 0.0;
	sndal_lasttime = Sys_DoubleTime ();

	want = (int)s_openal_max_sources.value;
	if (want < 1)
		want = 1;
	else if (want > MAX_CHANNELS)
		want = MAX_CHANNELS;

	sndal_numsources = 0;
	for (i = 0; i < want; i++)
	{
		ALuint source = 0;

		alGenSources (1, &source);
		if (alGetError () != AL_NO_ERROR || !source)
			break;
		alSourcef (source, AL_ROLLOFF_FACTOR, 0.0f);
		sndal_sources[sndal_numsources].source = source;
		sndal_sources[sndal_numsources].channel = NULL;
		sndal_sources[sndal_numsources].index = -1;
		sndal_numsources++;
	}
	if (!sndal_numsources)
		goto fail;

	sndal_has_pause_device = false;
	if (alcIsExtensionPresent (sndal_device, "ALC_SOFT_pause_device") == ALC_TRUE)
	{
		sndal_pause_device = (LPALCDEVICEPAUSESOFT)alcGetProcAddress (sndal_device, "alcDevicePauseSOFT");
		sndal_resume_device = (LPALCDEVICERESUMESOFT)alcGetProcAddress (sndal_device, "alcDeviceResumeSOFT");
		sndal_has_pause_device = (sndal_pause_device && sndal_resume_device) ? true : false;
	}

	sndal_hrtf_status = -1;
	if (has_hrtf == AL_TRUE)
	{
		status = -1;
		alcGetIntegerv (sndal_device, ALC_HRTF_STATUS_SOFT, 1, &status);
		sndal_hrtf_status = (int)status;
	}

	sndal_active = true;

	version = (const char *)alGetString (AL_VERSION);
	device = (const char *)alcGetString (sndal_device, ALC_DEVICE_SPECIFIER);
	Con_Printf ("OpenAL: %s, %s, %d sources\n", version ? version : "unknown", device ? device : "default device", sndal_numsources);
	Con_Printf ("OpenAL HRTF: %s\n", SNDAL_HrtfStatusName (sndal_hrtf_status));
	return true;

fail:
	Con_Printf ("OpenAL: initialization failed\n");
	SNDAL_Shutdown ();
	return false;
}

static void SNDAL_MusicShutdown (void)
{
	if (sndal_music_source)
	{
		alSourceStop (sndal_music_source);
		alSourcei (sndal_music_source, AL_BUFFER, 0);
		alDeleteSources (1, &sndal_music_source);
		sndal_music_source = 0;
	}
	if (sndal_music_buffers[0])
	{
		alDeleteBuffers (SNDAL_MUSIC_BUFFERS, sndal_music_buffers);
		memset (sndal_music_buffers, 0, sizeof (sndal_music_buffers));
	}
	sndal_music_numfree = 0;
	sndal_music_queued = 0;
}

void SNDAL_Shutdown (void)
{
	int i;

	if (sndal_context_current)
	{
		SNDAL_StopAll ();
		SNDAL_DeleteBuffers ();
		SNDAL_MusicShutdown ();
		sndal_context_current = false;
		alcMakeContextCurrent (NULL);
	}

	if (sndal_context)
	{
		alcDestroyContext (sndal_context);
		sndal_context = NULL;
	}
	if (sndal_device)
	{
		alcCloseDevice (sndal_device);
		sndal_device = NULL;
	}

	for (i = 0; i < sndal_numsources; i++)
	{
		sndal_sources[i].source = 0;
		sndal_sources[i].channel = NULL;
		sndal_sources[i].index = -1;
		sndal_sources[i].started = false;
		sndal_sources[i].looping = false;
	}
	sndal_numsources = 0;
	for (i = 0; i < MAX_CHANNELS; i++)
		sndal_binding[i] = -1;
	sndal_numbuffers = 0;
	sndal_music_source = 0;
	memset (sndal_music_buffers, 0, sizeof (sndal_music_buffers));
	memset (sndal_music_free, 0, sizeof (sndal_music_free));
	snd_output.ready = false;
	snd_output.speed = 0;
	sndal_pause_device = NULL;
	sndal_resume_device = NULL;
	sndal_active = false;
	sndal_blocked = false;
	sndal_has_direct_channels = false;
	sndal_has_pause_device = false;
	sndal_hrtf_status = -1;
	sndal_rawpos = 0;
	sndal_clockfrac = 0.0;
	sndal_lasttime = 0.0;
}
