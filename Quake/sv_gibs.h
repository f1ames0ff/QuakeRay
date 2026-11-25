// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#ifndef SV_GIBS_H
#define SV_GIBS_H

#include "quakedef.h"

void SV_Gibs_Init (void);
void SV_Gibs_OnCall (dfunction_t *callee, dfunction_t *caller);
void SV_Gibs_OnSpawn (edict_t *ed);
void SV_Gibs_Apply (edict_t *ent);
void SV_Gibs_OnNewMap (void);

#endif /* SV_GIBS_H */
