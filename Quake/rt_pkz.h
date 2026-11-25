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

#ifndef RT_PKZ_H
#define RT_PKZ_H

#include "quakedef.h"

void RT_PKZ_MountDir(const char *dir, unsigned int path_id);
void RT_PKZ_Unmount(void *archive);
void RT_PKZ_Shutdown(void);

int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx);
int RT_PKZ_ListArchiveFiles(const void *archive, const char *dir, const char *ext,
                            int (*cb)(const char *name, void *ctx), void *ctx);

int RT_PKZ_FindFile(const void *archive, const char *name, int *outSize);

int RT_PKZ_OpenFile(const void *archive, const char *name, int *outSize);
byte *RT_PKZ_LoadFile(const void *archive, const char *name, int *outSize);

FILE *RT_PKZ_OpenFileAsFILE(const void *archive, const char *name);

qboolean RT_PKZ_IsHandle(int handle);
int RT_PKZ_Read(int handle, void *dest, int count);
void RT_PKZ_Seek(int handle, int position);
void RT_PKZ_Close(int handle);

#endif
