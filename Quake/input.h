/*
Copyright (C) 1996-2001 Id Software, Inc.
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

#ifndef _QUAKE_INPUT_H
#define _QUAKE_INPUT_H

// input.h -- external (non-keyboard) input devices

void IN_Init (void);

void IN_Shutdown (void);

void IN_Commands (void);
// oportunity for devices to stick commands on the script buffer

// mouse moved by dx and dy pixels
void IN_MouseMotion (int dx, int dy);

// delivering mouse motion events (the ImGui panel reads them for its cursor).
void IN_FreeCursorForGui (void);

void IN_SendKeyEvents (void);
// used as a callback for Sys_SendKeyEvents() by some drivers

void IN_UpdateInputMode (void);
// do stuff if input mode (text/non-text) changes matter to the keyboard driver

void IN_Move (usercmd_t *cmd);
// add additional movement on top of the keyboard move cmd

void IN_ClearStates (void);
// restores all button and position states to defaults

// called when the app becomes active
void IN_Activate ();

// called when the app becomes inactive
void IN_Deactivate (qboolean free_cursor);

// called when a menu is opened; keeps the fullscreen cursor hidden when ui_mouse is 0
void IN_DeactivateForMenu (void);

// returns the current mouse position in display pixels (vid.width/vid.height space)
void IN_GetMousePos (int *outx, int *outy);

// converts window coordinates to display pixels (vid.width/vid.height space)
void IN_ScaleMouseCoords (float x, float y, int *outx, int *outy);

#endif /* _QUAKE_INPUT_H */
