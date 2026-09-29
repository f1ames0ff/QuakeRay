# OpenAL Soft sound system

The engine's sound effects are rendered by OpenAL Soft as positioned sources, with its built-in
HRTF turning the mix binaural on headphones. There is no second backend: the SDL audio device and
the software mixer are removed, `snd_dma.c` drives OpenAL directly, and a missing OpenAL library
makes the game start silently with a console message instead of falling back to another device.
Step 2 (HRTF datasets, EFX reverb, occlusion) is not implemented yet and is listed at the end.

## Recon: the engine surface and its OpenAL mapping

| Engine | Value / meaning | OpenAL counterpart |
|---|---|---|
| `channel_t` (`Quake/q_sound.h`) | one active sound: `sfx`, `origin`, `master_vol` (0-255), `dist_mult`, `end`, `pos`, `entnum`, `entchannel` | one AL source from the pool, bound while the channel is active |
| `sfxcache_t` (`sfx->cache`) | decoded PCM: `length`, `loopstart`, `speed`, `width` (1 or 2 bytes), mono (stereo samples are rejected by `S_LoadSound`) | one AL buffer per `(sfx, cache)` pair, uploaded lazily on first start |
| `sfxcache_t.width` | 8-bit caches are signed bytes, 16-bit caches are signed native shorts | always `AL_FORMAT_MONO16`; 8-bit is expanded to 16-bit on upload |
| `sfxcache_t.loopstart` / `length` | loop region `[loopstart, length)`, `-1` = one-shot | `AL_LOOPING` plus `AL_LOOP_POINTS_SOFT` = `{loopstart, length}` (whole buffer kept, so the pre-loop attack still plays); `AL_SOFT_loop_points` is required, and without it the backend declines to start and the game is silent |
| `channel_t.pos` | start offset into the cache | `AL_SAMPLE_OFFSET` when the source is (re)started, and written back from it while playing |
| `channel_t.origin` | world position of the emitter | `AL_POSITION` (absolute world space) |
| `channel_t.master_vol`, `dist_mult` | `master_vol = fvol * 255`, `dist_mult = attenuation / sound_nominal_clip_dist` (`1000`), statics use `(attenuation / 64) / sound_nominal_clip_dist` | `AL_GAIN`, reproducing the engine law: `gain = master_vol / 510 * (1 - min(dist, 1)) * sfxvolume`, `dist = |origin - listener_origin| * dist_mult`; the `510` carries the mixer's deliberate 6 dB headroom, and `AL_ROLLOFF_FACTOR = 0` keeps AL's own distance model out of it |
| `SND_Spatialize` | respatializes a channel each frame (it also produces the engine's `leftvol`/`rightvol`) | kept for culling and `snd_show`; direction and gain come from `AL_POSITION`/`AL_GAIN` |
| `listener_origin` / `listener_forward` / `listener_right` / `listener_up` | listener basis, refreshed by `S_Update` | `AL_POSITION` + `AL_ORIENTATION = {forward, up}`; Quake's `right = forward x up` matches OpenAL's convention, so no axis swap is needed |
| `entnum == cl.viewentity`, ambient channels `0..NUM_AMBIENTS-1` | always full volume, "inside the head" | `AL_SOURCE_RELATIVE` at `(0, 0, 0)` |
| `MAX_CHANNELS` / `MAX_DYNAMIC_CHANNELS` / `NUM_AMBIENTS` | `1024` / `128` / `4` (statics up to `MAX_CHANNELS`) | source pool, `s_openal_max_sources` (default `256`, clamped to `MAX_CHANNELS`; the music source is reserved first and OpenAL Soft's own limit caps the count); an inaudible static's slot is reused when the pool is exhausted, the engine's own channel stealing still decides who plays |
| `snd_output.speed` / `snd_mixspeed` | output rate, default `44100` | `ALC_FREQUENCY` on the context; caches and music are resampled to the device rate `ALC_FREQUENCY` reports |
| `snd_output.channels` | `2` | stereo output; HRTF convolution happens inside OpenAL Soft |
| `S_RawSamples` / `s_rawsamples` / `s_rawend` (`MAX_RAW_SAMPLES` = `8192`) | streamed stereo music and ambience, already scaled by `bgmvolume` | a separate `AL_SOURCE_RELATIVE` stereo streaming source (`AL_SOFT_direct_channels` when available) fed from the same ring by `SNDAL_Update`; the ring has its own read cursor (`SNDAL_RawPosition`, exposed to `bgmusic.c` through `S_RawSamplesCursor`) |
| `paintedtime` | playback clock in sample pairs, used by `SND_PickChannel` for "closest to finishing" and by start/end bookkeeping | a wall-clock sample counter advanced by `SNDAL_AdvanceClock` from `Sys_DoubleTime`, independent of the music ring |
| `snd_sdl.c` / `snd_mix.c` | the removed SDL device and software mixer | `SNDAL_Init` / `SNDAL_Shutdown` / `SNDAL_BlockSound` (device pause) / `SNDAL_UnblockSound`; `S_Startup` and `S_Shutdown` call them directly |
| `nosound`, `sfxvolume`, `bgmvolume`, `loadas8bit`, `precache`, `snd_show` | existing knobs | all apply as before |

### Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `s_openal_hrtf` | `2` | HRTF mode: `0` off, `1` on, `2` auto (the device decides, headphones suggested); requires a context restart, so changing it restarts the backend |
| `s_openal_max_sources` | `256` | size of the source pool, clamped to `1..MAX_CHANNELS`; takes effect on the next backend restart |
| `snd_mixspeed` | `44100` | output rate the OpenAL device is asked for (`-mixspeed` sets it at startup) |

The startup line reports the device, its rate, the source count and the HRTF status OpenAL Soft
granted (`enabled`, `disabled`, `denied`, `headphones detected`, `unsupported format`).

## Loops

A looping cache is uploaded whole; `AL_SOFT_loop_points` marks `[loopstart, length)`, and
`AL_LOOPING` runs it until the game stops the channel (`S_StopSound`, `S_StopAllSounds`) or reuses
it (`S_StartSound` overrides, where the source is stopped, rebound and restarted at
`channel_t.pos`). The extension is required, which keeps every loop seam and every pre-loop attack
exact; without it `SNDAL_Init` declines and the game runs silently.

A one-shot source is retired when AL reports it stopped, or when the wall-clock sample counter
passes `channel_t.end` while the source is not playing - the latter matches what the mixer used to do
to a channel that was culled (its position is frozen while it is inaudible, but its absolute end time
still passes). A culled channel that becomes audible again is started at the offset that makes it end
at its absolute end time, exactly as the old mixer painted the remaining samples. While a source
plays, its `AL_SAMPLE_OFFSET` is written back into `channel_t.pos`, so the engine's duplicate-start
de-phasing, restart after `S_ClearAll` and the saved position of a paused source all see a real
playback position.

## Music

`S_RawSamples` and the `MAX_RAW_SAMPLES` ring keep their shape. The backend consumes the ring in 8
buffers of 1024 sample pairs (~186 ms of queue) on the music source: the source is
`AL_SOURCE_RELATIVE`, gets `AL_DIRECT_CHANNELS_SOFT` when the extension is present so HRTF does not
fold the stereo image to mono, and is fed by `SNDAL_Update` after the SFX sources. `bgmusic.c` asks
`S_RawSamplesCursor()` how far the ring has been consumed; `S_ExtraUpdate` pumps the music queue too,
so the long GPU waits the renderer feeds it do not drain the stream.

## Life cycle

- `S_Startup` calls `SNDAL_Init`; a failure leaves `snd_output.ready` false and the game silent, with
  the reason on the console.
- `S_StartSound` arms a pool source at the same moment, so the first sample is not delayed to the
  next `S_Update`; channels that are assigned outside `S_StartSound` - the four ambient beds through
  `S_UpdateAmbientSounds` and the statics through `S_StaticSound` - are bound by `SNDAL_Update` on
  their first audible frame.
- `S_Update` advances the clock, updates the listener, the music queue and every bound source.
- `S_StopSound` / `S_StopAllSounds` / `S_ClearBuffer` / `S_ClearAll` release the matching sources
  and AL buffers; focus loss pauses the device (`ALC_SOFT_pause_device`). Changing `s_openal_hrtf`
  or `s_openal_max_sources` restarts the backend; a failed restart stops the music stream and leaves
  the backend retryable once the sound system was fully up at least once.
- A channel is paused only while it is out of the engine's earshot (distance attenuation reaches
  zero); `sfxvolume 0` keeps the sources running at `AL_GAIN 0` so one-shots still expire.
- `S_Shutdown` closes the OpenAL device and unloads the library; no SDL audio subsystem is opened at
  any point, so SDL teardown on exit cannot touch the audio device.

## Build and shipping

- Headers, 32/64-bit `OpenAL32.dll` (the official `soft_oal.dll` renamed so the bundled
  implementation is the one that loads), the LGPL text (`COPYING`, the Library GPL v2 text the
  upstream release ships) and the pffft licence live in `Windows/openal/`. Version: OpenAL Soft
  1.25.2.
- CMake adds the include directory on every platform (non-Windows builds configure with
  `find_path(AL/alext.h)` and get a message naming the OpenAL Soft dev package) and copies the
  matching DLL next to `quakeray.exe`; `bundle_release.ps1` ships the DLL and the licence texts
  under `licenses/`.
- The library is loaded at runtime with `SDL_LoadObject`; a missing or broken OpenAL starts the game
  without sound instead of failing it.

## Deferred to step 2

- Custom HRTF datasets: `ALC_NUM_HRTF_SPECIFIERS_SOFT` / `ALC_HRTF_SPECIFIER_SOFT` plus
  `ALC_HRTF_ID_SOFT` for the built-in and configured datasets, a `s_openal_hrtf_dataset` cvar and a
  picker; SOFA files reach OpenAL Soft through its configuration (`hrtf = <file.sofa>`).
- EFX reverb keyed to `ambient_level` / rooms, occlusion and per-material filtering.
