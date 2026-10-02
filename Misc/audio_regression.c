#define QUAKEDEFS_H
#undef NDEBUG

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define CVAR_ARCHIVE 1
#define MAX_QPATH 64
#define NUM_AMBIENTS 4
#define q_max(a, b) ((a) > (b) ? (a) : (b))

typedef unsigned char byte;
typedef int qboolean;
typedef float vec_t;
typedef float vec3_t[3];
enum { false, true };

typedef struct cvar_s
{
	const char *name;
	const char *string;
	int flags;
	float value;
	void (*callback)(struct cvar_s *);
} cvar_t;

typedef int SDL_mutex;
static SDL_mutex test_mutex;
SDL_mutex *snd_mutex = &test_mutex;
static double test_time = 1.0;
static int com_filesize;
static struct { int viewentity; } cl;

static void SDL_LockMutex (SDL_mutex *mutex) { (void)mutex; }
static void SDL_UnlockMutex (SDL_mutex *mutex) { (void)mutex; }
static void *Mem_Alloc (size_t size) { return calloc (1, size); }
static void Mem_Free (void *ptr) { free (ptr); }
static void Con_Printf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	vprintf (fmt, args);
	va_end (args);
}
static void Con_DPrintf2 (const char *fmt, ...) { (void)fmt; }
static void Sys_Error (const char *fmt, ...) { (void)fmt; abort (); }
static double Sys_DoubleTime (void) { return test_time; }
static short LittleShort (short value) { return value; }
static void VectorSubtract (const vec3_t a, const vec3_t b, vec3_t out)
{
	int i;
	for (i = 0; i < 3; i++) out[i] = a[i] - b[i];
}
static float VectorLength (const vec3_t v) { return sqrtf (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
static void q_strlcpy (char *dst, const char *src, size_t size) { snprintf (dst, size, "%s", src); }
static void q_strlcat (char *dst, const char *src, size_t size) { size_t n = strlen (dst); q_strlcpy (dst+n, src, size-n); }
static byte *COM_LoadFile (const char *name, void *unused) { (void)name; (void)unused; return NULL; }
static void Cvar_RegisterVariable (cvar_t *var) { var->value = (float)atof (var->string); }
static void Cvar_SetCallback (cvar_t *var, void (*callback)(cvar_t *)) { var->callback = callback; }
static void Cvar_SetQuick (cvar_t *var, const char *value)
{
	var->value = (float)atof (value);
	if (var->callback) var->callback (var);
}
void SNDEQ_GuiInit (void) {}

#include "../Quake/snd_eq.c"
#include "../Quake/snd_mem.c"
#include "../Quake/snd_openal.c"

channel_t snd_channels[MAX_CHANNELS];
int total_channels;
int paintedtime;
int s_rawend;
portable_samplepair_t s_rawsamples[MAX_RAW_SAMPLES];
vec3_t listener_origin;
vec3_t listener_forward = {1, 0, 0};
vec3_t listener_right = {0, -1, 0};
vec3_t listener_up = {0, 0, 1};
cvar_t snd_mixspeed = {"snd_mixspeed", "48000", 0, 48000, NULL};
cvar_t sfxvolume = {"volume", "0.7", 0, 0.7f, NULL};
cvar_t loadas8bit;
cvar_t bgmvolume;
cvar_t s_openal_hrtf = {"s_openal_hrtf", "1", 0, 1, NULL};
cvar_t s_openal_max_sources = {"s_openal_max_sources", "8", 0, 8, NULL};

static void SetGains (float gain)
{
	int i;
	for (i = 0; i < SNDEQ_BANDS; i++) sndeq_cvars[i]->value = gain;
}

static void TestEqualizer (void)
{
	const int rates[] = {8000, 11025, 22050, 44100, 48000, 96000, 192000};
	short mono[4096], stereo[8192], whole[4096], split[4096];
	sndeq_state_t a, b;
	double input_power = 0, output_power = 0;
	int r, i, band, boost;

	SNDEQ_Init ();
	for (r = 0; r < (int)(sizeof (rates) / sizeof (rates[0])); r++)
	{
		for (boost = -12; boost <= 12; boost += 24)
		{
			SetGains ((float)boost);
			memset (&a, 0, sizeof (a));
			memset (mono, 0, sizeof (mono));
			mono[0] = 500;
			SNDEQ_Process (&a, mono, 4096, 1, rates[r]);
			for (band = 0; band < SNDEQ_BANDS; band++)
			{
				sndeq_band_t *f = &a.band[band];
				assert (isfinite (f->z1[0]) && isfinite (f->z2[0]));
				assert (fabs (f->a2) < 1.0);
				assert (1.0 + f->a1 + f->a2 > 0.0);
				assert (1.0 - f->a1 + f->a2 > 0.0);
			}
			assert (abs (mono[4095]) <= 2);
		}
		for (band = 0; band < SNDEQ_BANDS; band++)
		{
			SetGains (0);
			sndeq_cvars[band]->value = 6;
			if (sndeq_freqs[band] >= rates[r] * 0.5)
			{
				memset (&a, 0, sizeof (a));
				for (i = 0; i < 4096; i++) mono[i] = (short)(i % 100);
				memcpy (whole, mono, sizeof (mono));
				SNDEQ_Process (&a, mono, 4096, 1, rates[r]);
				assert (!memcmp (mono, whole, sizeof (mono)));
			}
			else
				assert (fabsf (SNDEQ_ResponseDb ((float)sndeq_freqs[band], rates[r]) - 6) < 0.01f);
		}
	}
	SetGains (0);
	s_eq_910.value = 6;
	memset (&a, 0, sizeof (a));
	memset (&b, 0, sizeof (b));
	memset (stereo, 0, sizeof (stereo));
	for (i = 0; i < 4096; i++)
	{
		whole[i] = (short)(1000 * sin (2 * M_PI * 910 * i / 48000));
		split[i] = whole[i];
		stereo[i * 2] = whole[i];
	}
	SNDEQ_Process (&a, whole, 4096, 1, 48000);
	SNDEQ_Process (&b, split, 1000, 1, 48000);
	SNDEQ_Process (&b, split+1000, 3096, 1, 48000);
	assert (!memcmp (whole, split, sizeof (whole)));
	for (i = 2048; i < 4096; i++)
	{
		input_power += (double)stereo[i * 2] * stereo[i * 2];
		output_power += (double)whole[i] * whole[i];
	}
	assert (fabs (10.0 * log10 (output_power / input_power) - 6.0) < 0.05);
	memset (&a, 0, sizeof (a));
	SNDEQ_Process (&a, stereo, 4096, 2, 48000);
	for (i = 0; i < 4096; i++) assert (stereo[i * 2 + 1] == 0);
	SetGains (0);
	SNDEQ_Process (&a, stereo, 4096, 2, 48000);
	assert (a.band[2].z1[0] == 0);
	Cvar_SetQuick (&s_eq_60, "nan");
	assert (s_eq_60.value == 0);
	Cvar_SetQuick (&s_eq_60, "100");
	assert (s_eq_60.value == 12);
	SetGains (0);
	puts ("EQ: seven rates, stable poles, Nyquist bypass, gain, chunk continuity, stereo isolation, reset and finite values passed");
}

static void TestResampling (void)
{
	const int input_rate = 11025, output_rate = 48000, input_samples = 11025;
	byte *input = Mem_Alloc ((size_t)input_samples);
	sfx_t sfx = {0};
	signed char *output;
	int i;

	for (i = 0; i < input_samples; i++) input[i] = (byte)(i % 256);
	sfx.cache = Mem_Alloc (sizeof (sfxcache_t) + output_rate * sizeof (short));
	sfx.cache->length = input_samples;
	sfx.cache->loopstart = input_rate / 2;
	snd_output.speed = output_rate;
	ResampleSfx (&sfx, input_rate, 1, input);
	assert (sfx.cache->length == output_rate);
	assert (sfx.cache->loopstart == (int)((int64_t)(input_rate/2)*output_rate/input_rate));
	output = (signed char *)sfx.cache->data;
	for (i = 0; i < output_rate; i++)
		assert (output[i] == (int)input[(int64_t)i*input_rate/output_rate]-128);
	Mem_Free (sfx.cache);
	Mem_Free (input);
	puts ("SFX resampling: exact 11025-to-48000 sample positions and loop point passed");
}

static void TestBackend (void)
{
	sfx_t sfx = {0};
	channel_t *channel = &snd_channels[NUM_AMBIENTS];
	ALint offset, state;
	int slot, before;

	assert (SNDAL_Init ());
	assert (SNDAL_HrtfEnabled ());
	assert (!strcmp (alcGetString (sndal_device, ALC_HRTF_SPECIFIER_SOFT), "Built-In HRTF"));
	sfx.cache = Mem_Alloc (sizeof (sfxcache_t) + 48000 * sizeof (short));
	sfx.cache->length = 48000;
	sfx.cache->speed = 48000;
	sfx.cache->width = 2;
	sfx.cache->loopstart = -1;
	channel->sfx = &sfx;
	channel->end = 48000;
	channel->master_vol = 200;
	paintedtime = 12000;
	SNDAL_StartChannel (channel);
	slot = sndal_binding[NUM_AMBIENTS];
	assert (slot >= 0);
	alGetSourcei (sndal_sources[slot].source, AL_SAMPLE_OFFSET, &offset);
	assert (offset >= 12000);
	SNDAL_BlockSound ();
	before = paintedtime;
	test_time += 10;
	SNDAL_AdvanceClock ();
	assert (paintedtime == before);
	SNDAL_UnblockSound ();
	test_time += 0.01;
	SNDAL_AdvanceClock ();
	assert (paintedtime - before >= 479 && paintedtime - before <= 481);
	sndal_has_pause_device = false;
	SNDAL_BlockSound ();
	alGetSourcei (sndal_sources[slot].source, AL_SOURCE_STATE, &state);
	assert (state == AL_PAUSED);
	before = paintedtime;
	test_time += 10;
	SNDAL_UnblockSound ();
	SNDAL_AdvanceClock ();
	assert (paintedtime == before);
	s_rawend = SNDAL_MUSIC_SAMPLES * 2;
	SNDAL_UpdateMusic ();
	assert (sndal_music_queued == 2);
	SNDAL_PauseMusic (true);
	SNDAL_UpdateMusic ();
	alGetSourcei (sndal_music_source, AL_SOURCE_STATE, &state);
	assert (state == AL_PAUSED);
	SNDAL_PauseMusic (false);
	SNDAL_UpdateMusic ();
	alGetSourcei (sndal_music_source, AL_SOURCE_STATE, &state);
	assert (state == AL_PLAYING);
	SNDAL_ClearMusic ();
	alGetSourcei (sndal_music_source, AL_BUFFERS_QUEUED, &state);
	assert (state == 0 && sndal_rawpos == 0);
	SNDAL_StopChannel (channel);
	sfx.cache->loopstart = 12000;
	SNDAL_ClearAll ();
	channel->pos = 0;
	SNDAL_StartChannel (channel);
	slot = sndal_binding[NUM_AMBIENTS];
	assert (slot >= 0);
	{
		ALint points[2];
		ALfloat orientation[6];
		ALuint buffer = sndal_buffers[0].buffer;

		alGetBufferiv (buffer, AL_LOOP_POINTS_SOFT, points);
		assert (points[0] == 12000 && points[1] == 48000);
		SNDAL_UpdateListener ();
		alGetListenerfv (AL_ORIENTATION, orientation);
		assert (orientation[0] == 1 && orientation[2] == 0 && orientation[5] == 1);
	}
	assert (alGetError () == AL_NO_ERROR);
	SNDAL_Shutdown ();
	Mem_Free (sfx.cache);
	memset (snd_channels, 0, sizeof (snd_channels));
	s_openal_hrtf.value = 0;
	assert (SNDAL_Init ());
	assert (!SNDAL_HrtfEnabled ());
	SNDAL_Shutdown ();
	puts ("OpenAL: MIT KEMAR selection, HRTF on/off, delayed SFX, paused clock, music pause/resume/flush and restart passed");
}

static void TestWaveHeaders (void)
{
	byte wav[48] = {
		'R','I','F','F',40,0,0,0,'W','A','V','E',
		'f','m','t',' ',16,0,0,0,1,0,1,0,
		0x11,0x2b,0,0,0x11,0x2b,0,0,1,0,8,0,
		'd','a','t','a',4,0,0,0,0,128,255,128
	};
	wavinfo_t info;
	int size;

	info = GetWavinfo ("test", wav, sizeof (wav));
	assert (info.samples == 4 && info.rate == 11025 && info.loopstart == -1 && info.dataofs == 44);
	for (size = 0; size < (int)sizeof (wav); size++)
	{
		info = GetWavinfo ("truncated", wav, size);
		assert (info.samples == 0);
	}
	wav[16] = 2;
	info = GetWavinfo ("short fmt", wav, sizeof (wav));
	assert (info.samples == 0);
	puts ("WAV: valid PCM, every truncation boundary and short format chunk passed");
}

int main (void)
{
	TestEqualizer ();
	TestResampling ();
	TestWaveHeaders ();
	TestBackend ();
	return 0;
}
