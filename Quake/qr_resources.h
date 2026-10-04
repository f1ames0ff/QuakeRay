/*
Copyright (C) 2026 f1ames0ff

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

#ifndef QR_RESOURCES_H
#define QR_RESOURCES_H

#include "quakedef.h"

#define QR_FLAVOR_ORIGINAL   0
#define QR_FLAVOR_REMASTERED 1

void     QR_Resources_Init (void);
qboolean QR_Resources_SteamDir (char *out, size_t outsize);
qboolean QR_Resources_HasGameData (void);

qboolean QR_Resources_FlavorDir (const char *dir, int flavor);
qboolean QR_Resources_RemasteredDir (char *out, size_t outsize);
qboolean QR_Resources_NightdiveDir (char *out, size_t outsize);
int      QR_Resources_ChooseFlavor (void);

#endif /* QR_RESOURCES_H */
