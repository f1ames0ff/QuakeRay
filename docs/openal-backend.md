# OpenAL Soft sound backend

Step 1 of the audio workstream: the engine's sound effects are rendered by OpenAL Soft as
positioned sources, with its built-in HRTF turning the mix binaural on headphones, while the SDL
mixer stays as the fallback and the A/B reference. Step 2 (HRTF datasets, EFX reverb, occlusion)
is not implemented yet and is listed at the end.

Set `s_openal 0` to get the SDL path back; `s_openal 1` (the default) uses OpenAL Soft when it can
be loaded and silently falls back to SDL otherwise.

## Recon: the engine surface and its OpenAL mapping

| Engine | Value / meaning | OpenAL counterpart |
|---|---|---|
| `channel_t` (`Quake/q_sound.h`) | one active sound: `sfx`, `origin`, `master_vol` (0-255), `dist_mult`, `end`, `pos`, `entnum`, `entchannel` | one AL source from the pool, bound while the channel is active |
| `sfxcache_t` (`sfx->cache`) | decoded PCM: `length`, `loopstart`, `speed`, `width` (1 or 2 bytes), mono (stereo samples are rejected by `S_LoadSound`) | one AL buffer per `(sfx, cache)` pair, uploaded lazily on first start |
| `sfxcache_t.width` | 8-bit caches store signed bytes the mixer reads through `snd_scaletable`; 16-bit caches are signed native shorts | always `AL_FORMAT_MONO16`; 8-bit is expanded to 16-bit on upload |
| `sfxcache_t.loopstart` / `length` | loop region `[loopstart, length)`, `-1` = one-shot | `AL_LOOPING` plus `AL_LOOP_POINTS_SOFT` = `{loopstart, length}` (whole buffer kept, so the pre-loop attack still plays); without the extension the buffer is the loop region alone |
| `channel_t.pos` | start offset into the cache | `AL_SAMPLE_OFFSET` when the source is (re)started |
| `channel_t.origin` | world position of the emitter | `AL_POSITION` (absolute world space) |
| `channel_t.master_vol`, `dist_mult` | `master_vol = fvol * 255`, `dist_mult = attenuation / sound_nominal_clip_dist` (`1000`), statics use `(attenuation / 64) / sound_nominal_clip_dist` | `AL_GAIN`, reproducing the engine law: `gain = master_vol / 255 * (1 - min(dist, 1)) * sfxvolume`, `dist = |origin - listener_origin| * dist_mult`; `AL_ROLLOFF_FACTOR = 0` keeps AL's own distance model out of it |
| `SND_Spatialize` | the SDL pan law (`leftvol`/`rightvol` from `listener_right`) | not used for gain; direction comes from `AL_POSITION` relative to the listener |
| `listener_origin` / `listener_forward` / `listener_right` / `listener_up` | listener basis, refreshed by `S_Update` | `AL_POSITION` + `AL_ORIENTATION = {forward, up}`; Quake's `right = forward x up` matches OpenAL's convention, so no axis swap is needed |
| `entnum == cl.viewentity`, ambient channels `0..NUM_AMBIENTS-1` | always full volume, "inside the head" | `AL_SOURCE_RELATIVE` at `(0, 0, 0)` |
| `MAX_CHANNELS` / `MAX_DYNAMIC_CHANNELS` / `NUM_AMBIENTS` | `1024` / `128` / `4` (statics up to `MAX_CHANNELS`) | source pool, `s_openal_max_sources` (default `256`, clamped to `MAX_CHANNELS`); a channel without a free source stays silent, the engine's own channel stealing still decides who plays |
| `shm->speed` / `snd_mixspeed` | mixer rate, default `44100` | `ALC_FREQUENCY` on the context; caches and music are resampled to the device rate `ALC_FREQUENCY` reports |
| `shm->channels` | `2` | stereo output; HRTF convolution happens inside OpenAL Soft |
| `S_RawSamples` / `s_rawsamples` / `s_rawend` / `paintedtime` (`MAX_RAW_SAMPLES` = `8192`) | streamed stereo music and ambience, already scaled by `bgmvolume` | a separate `AL_SOURCE_RELATIVE` stereo streaming source (`AL_SOFT_direct_channels` when available) fed from the same ring by `SNDAL_Update`; `paintedtime` advances by the samples queued to it, so `BGM_UpdateStream`'s flow control keeps working unchanged |
| `SNDDMA_Init` / `SNDDMA_Shutdown` / `SNDDMA_BlockSound` / `SNDDMA_UnblockSound` (`snd_sdl.c`) | SDL2 device, callback pulls the mixer | `SNDAL_Init` / `SNDAL_Shutdown` / `SNDAL_BlockSound` (device pause) / `SNDAL_UnblockSound`; `S_Startup` and `S_Shutdown` pick the backend, `s_openal 0` keeps `SNDDMA_*` exactly as before |
| `nosound`, `sfxvolume`, `bgmvolume`, `loadas8bit`, `precache`, `_snd_mixahead`, `snd_show`, `snd_filterquality` | existing knobs | `nosound`, `sfxvolume`, `bgmvolume`, `loadas8bit`, `precache`, `snd_show` apply as before; `_snd_mixahead`, `snd_filterquality` and the `sndspeed` lowpass only shape the SDL mixer |

### New cvars

| Cvar | Default | Meaning |
|---|---|---|
| `s_openal` | `1` | `1` uses OpenAL Soft when available, `0` pins the SDL backend; changing it restarts the audio backend |
| `s_openal_hrtf` | `2` | HRTF mode: `0` off, `1` on, `2` auto (the device decides, headphones suggested); requires a context restart, so changing it restarts the backend |
| `s_openal_max_sources` | `256` | size of the source pool, clamped to `1..MAX_CHANNELS`; takes effect on the next backend restart |

The startup line reports the device, its rate, the source count and the HRTF status OpenAL Soft
granted (`enabled`, `disabled`, `denied`, `headphones detected`, `unsupported format`).

## Loops

A looping cache is uploaded whole; `AL_SOFT_loop_points` (present in OpenAL Soft) marks
`[loopstart, length)`, and `AL_LOOPING` runs it until the game stops the channel (`S_StopSound`,
`S_StopAllSounds`) or reuses it (`S_StartSound` overrides, where the source is stopped, rebound and
restarted at `channel_t.pos`). A looping source is never retired by the backend itself; a one-shot
source that AL reports as stopped clears `channel_t.sfx`, mirroring the mixer's end-of-sample.

## Music

`S_RawSamples` and the `MAX_RAW_SAMPLES` ring stay exactly as they are. On the OpenAL path the
backend consumes the ring in 4 buffers of 1024 sample pairs (~93 ms of queue) on the music source:
the source is `AL_SOURCE_RELATIVE`, gets `AL_DIRECT_CHANNELS_SOFT` when the extension is present so
HRTF does not fold the stereo image to mono, and is fed by `SNDAL_Update` after the SFX sources.
`paintedtime` moves with the samples queued, which is what `BGM_UpdateStream` reads to decide how
much to stream next; nothing in `bgmusic.c` or in the codec layer changes.

## Life cycle

- `S_Startup` tries `SNDAL_Init` when `s_openal` is set, then `SNDDMA_Init` if that fails; the SDL
  path prints the reason for the fallback.
- `S_StartSound` marks the channel and the backend arms a pool source at the same moment, so the
  first sample is not delayed to the next `S_Update`.
- `S_Update` updates the listener and every bound source; `S_ExtraUpdate` is a no-op on this path
  because OpenAL mixes on its own thread.
- `S_StopSound` / `S_StopAllSounds` / `S_ClearBuffer` / `S_ClearAll` release the matching sources
  and AL buffers; focus loss pauses the device (`ALC_SOFT_pause_device`).

## Build and shipping

- Headers, 32/64-bit `OpenAL32.dll` (the official `soft_oal.dll` renamed so the bundled
  implementation is the one that loads), the LGPL-2.1 text and the pffft licence live in
  `Windows/openal/`. Version: OpenAL Soft 1.25.2.
- CMake adds the include directory on every platform (`find_path(AL/al.h)` when not on Windows) and
  copies the matching DLL next to `quakeray.exe`; `bundle_release.ps1` ships the DLL and the licence
  texts under `licenses/`.
- The library is loaded at runtime with `SDL_LoadObject`, so a missing or broken OpenAL never
  prevents the SDL fallback from starting.

## Deferred to step 2

- Custom HRTF datasets: `ALC_NUM_HRTF_SPECIFIERS_SOFT` / `ALC_HRTF_SPECIFIER_SOFT` plus
  `ALC_HRTF_ID_SOFT` for the built-in and configured datasets, a `s_openal_hrtf_dataset` cvar and a
  picker; SOFA files reach OpenAL Soft through its configuration (`hrtf = <file.sofa>`).
- EFX reverb keyed to `ambient_level` / rooms, occlusion and per-material filtering.
