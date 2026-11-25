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

#ifndef OBSERVER_H
#define OBSERVER_H

#include "quakedef.h"

void     Observer_Register (void);
void     Observer_Unregister (void);

qboolean Observer_Active (void);
qboolean Observer_Frozen (void);

void     Observer_Stop (void);
void     Observer_OnNewMap (void);

void     Observer_UpdateView (void);
void     Observer_MouseMove (float dmy);
void     Observer_Zoom (qboolean on);
void     Observer_Wheel (int y);
qboolean Observer_KeyEvent (int key, qboolean down);

#endif /* OBSERVER_H */
