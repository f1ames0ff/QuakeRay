# Hand-off: an OpenAL Soft backend with HRTF

This document hands the audio workstream to the next agent. The owner wants a better sound backend with
binaural/HRTF spatialization. OpenAL Soft is the chosen direction (Steam Audio was rejected as too heavy);
nothing of this work exists in the tree yet.

## Goal

Each engine sound becomes an OpenAL Soft source, positioned relative to the listener, so headphones get a
binaural (HRTF) mix: direction, distance and elevation, with the built-in HRTF and optionally a custom SOFA
dataset. The codec layer and the SDL window/input stay; the SDL audio backend stays as a fallback and as the
reference for A/B listening.

## Why OpenAL Soft (decision record)

- LGPL-2.1-or-later: dynamic linking plus a notice fits the project's GPLv2-or-later licensing.
- Built-in HRTF; custom datasets through SOFA (libmysofa).
- No scene simulation, no MKL/Embree baggage - unlike the rejected Steam Audio.
- The de-facto standard backend for Quake-family ports.

## Current audio architecture (anchors in this tree)

- `Quake/snd_dma.c` - the channel manager: `channel_t snd_channels[MAX_CHANNELS]` with
  `leftvol`/`rightvol`/`master_vol`/`end`/`sfx`/`entnum`/`entchannel`, the listener basis
  (`listener_origin`/`listener_forward`/`listener_right`/`listener_up`), `paintedtime`/`soundtime`
  (sample pairs), and the cvars `nosound`, `sfxvolume`, `precache`, `loadas8bit`, `bgmvolume`,
  `ambient_level`, `ambient_fade`, `snd_noextraupdate`, `snd_show`, `_snd_mixahead`.
- `Quake/snd_mix.c` - the software mixer producing the stereo int16 stream; the spatialization today is the
  classic volume+pan into `leftvol`/`rightvol`.
- `Quake/snd_sdl.c` - the only output backend (SDL2 audio device; the callback pulls the mix).
- `Quake/snd_mem.c`, `Quake/snd_codec.c` and `snd_{wav,vorbis,opus,flac,mp3,modplug,mikmod,umx,xmp}.c` -
  sample loading/decoding; keep them exactly as they are.
- Music/`bgmvolume` - a stereo stream; it must bypass HRTF.

## Design

### Step 1 - the device and per-source spatialization (the first increment)

- New `Quake/snd_openal.c` + `snd_openal.h`, with a runtime switch (e.g. `s_openal 1`) and the SDL path as
  the fallback; keep the mixer intact for that fallback.
- Device/context: `alcOpenDevice`/`alcCreateContext` with `ALC_HRTF_SOFT`
  (`ALC_HRTF_ON`/`ALC_HRTF_DISABLED`, auto by default), the output frequency from the engine's settings,
  stereo output.
- Buffers: upload each loaded sample's decoded PCM as an OpenAL buffer (mono; the engine's 8/16-bit and
  mono/stereo cases map onto `AL_FORMAT_*`; looped sounds use `AL_LOOPING`).
- Sources: a pool sized like the engine's channel count; for each active `channel_t` pick a free source and
  set `AL_POSITION` (channel origin relative to the listener), `AL_GAIN` (from `master_vol` and `sfxvolume`)
  and the distance/rolloff parameters calibrated against the engine's own attenuation law.
- Listener: update `AL_POSITION`/`AL_ORIENTATION` from the listener basis each frame.
- Stereo music/ambient streams: `AL_SOURCE_RELATIVE` plus stereo (or `AL_SOFT_direct_channels`) so HRTF does
  not smear them.

### Step 2 - optional polish

- Custom HRTF datasets (SOFA via `libmysofa`), a dataset cvar and a picker.
- EFX reverb (`ALC_EFX_EXT`) keyed to the map's `ambient_level`/rooms.
- Occlusion (EFX or `AL_SOFT_source_occlusion`), per-material filtering.

## Build and shipping

- Add OpenAL Soft (`al.h`/`alc.h`/`alext.h`; `OpenAL32.dll` at runtime - dynamic loading is the easy path,
  an import lib is fine too). CMake: `find_package(OpenAL)` or a vendored `third_party/openal-soft`.
- `bundle_release.ps1`: ship `OpenAL32.dll` and its LGPL notice; update the readme's runtime DLL list.
- New cvars: `s_openal` (on/off), `s_openal_hrtf` (auto/on/off), `s_openal_hrtf_dataset` (SOFA path),
  `s_openal_max_sources`.

## Licensing

- OpenAL Soft: LGPL-2.1-or-later; dynamic linking and the license text in the bundle.
- `libmysofa` (only if SOFA loading is added): permissive, but verify and ship its notice.
- No change to the existing codec licenses.

## Gate and test matrix (the owner runs these)

1. Build, then a map with known emitters (wind/water ambient, a door loop, a growling enemy): with HRTF on a
   headset the front/behind/left/right directions must be distinct and artifact-free; with HRTF off the mix
   must match the SDL path as closely as the pan law allows.
2. Loop sounds: no clicks; distance attenuation comparable to the SDL path.
3. Music: stereo, un-warped by HRTF.
4. Latency and underruns: tune `_snd_mixahead`/buffer; `snd_show` sanity.
5. Fallback: `s_openal 0` reproduces today's behavior exactly.
6. Performance: frame time and audio-thread load unchanged within noise.

## Risks

- Latency and buffer sizing against the engine's small `_snd_mixahead`.
- Rate conversion: OpenAL Soft resamples per source - pick a sane output frequency.
- Source-count limits and a voice-stealing policy that matches the engine's channel stealing.
- Attenuation-law mismatch: calibrate `AL_ROLLOFF_FACTOR`/`AL_REFERENCE_DISTANCE`/`AL_MAX_DISTANCE` against
  the engine's volume law (`sound_nominal_clip_dist` is part of it).
- HRTF defaults on speakers: auto/off, never a surprise.

## First steps for the implementing agent

1. Recon the exact names and limits in this tree (`MAX_CHANNELS`, `MAX_RAW_SAMPLES`, the SDL device init
   block, the music path, the `S_*`/`SNDDMA_*` surface) and write the source-to-AL mapping table.
2. Prototype: device + one fixed test sound; then the channel pool; then loops; then the music bypass; then
   the cvar and the fallback.
3. One commit per step; every step gated by the owner's build and listen (as the renderer increments were).
