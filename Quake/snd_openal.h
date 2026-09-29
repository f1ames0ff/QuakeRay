/*
 * snd_openal.h -- OpenAL Soft output backend
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

#ifndef __SND_OPENAL__
#define __SND_OPENAL__

#include "q_sound.h"

qboolean SNDAL_Init (dma_t *dma);
void     SNDAL_Shutdown (void);
qboolean SNDAL_IsActive (void);

void SNDAL_Update (void);
void SNDAL_StartChannel (channel_t *ch);
void SNDAL_StopChannel (channel_t *ch);
void SNDAL_StopAll (void);
void SNDAL_ClearBuffer (void);
void SNDAL_ClearAll (void);
void SNDAL_BlockSound (void);
void SNDAL_UnblockSound (void);

#endif /* __SND_OPENAL__ */
