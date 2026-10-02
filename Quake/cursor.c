// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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

#include "quakedef.h"
#include "cursor.h"

#include <SDL.h>

static SDL_Cursor *cursor_sdl;
static QrMaterial  cursor_material = QR_NO_MATERIAL;
static int         cursor_size;
static int         cursor_hot_x;
static int         cursor_hot_y;

static void Cursor_DisplaySize (int *width, int *height)
{
	SDL_Window     *window = (SDL_Window *)VID_GetWindow ();
	SDL_DisplayMode mode = {0};
	int             display = window != NULL ? SDL_GetWindowDisplayIndex (window) : 0;

	if (display < 0)
		display = 0;

	if (SDL_GetDesktopDisplayMode (display, &mode) == 0 && mode.w > 0 && mode.h > 0)
	{
		*width = mode.w;
		*height = mode.h;
		return;
	}

	*width = vid.width;
	*height = vid.height;
}

static int Cursor_ChooseSize (int displayWidth, int displayHeight)
{
	if (displayWidth < 1920 || displayHeight < 1080)
		return 16;

	if (displayWidth >= 3840)
		return 64;

	return 32;
}

static void Cursor_FindHotspot (const byte *pixels, int width, int height, int *hotX, int *hotY)
{
	int x, y;

	for (y = 0; y < height; y++)
	{
		int first = -1;
		int last = -1;

		for (x = 0; x < width; x++)
		{
			if (pixels[(y * width + x) * 4 + 3] > 128)
			{
				if (first < 0)
					first = x;
				last = x;
			}
		}

		if (first >= 0)
		{
			*hotX = (first + last) / 2;
			*hotY = y;
			return;
		}
	}

	*hotX = 0;
	*hotY = 0;
}

static void Cursor_CreateMaterial (const byte *pixels, int width, int height)
{
	QrMaterialCreateInfo info = {0};
	QrResult             result;

	if (vulkan_globals.instance == QR_NULL_HANDLE)
		return;

	info.flags = 0;
	info.size.width = (uint32_t)width;
	info.size.height = (uint32_t)height;
	info.textures.pDataAlbedoAlpha = pixels;
	info.pRelativePath = NULL;
	info.filter = QR_SAMPLER_FILTER_LINEAR;
	info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

	result = qrCreateMaterial (vulkan_globals.instance, &info, &cursor_material);
	if (result != QR_SUCCESS)
		cursor_material = QR_NO_MATERIAL;
}

void Cursor_Init (void)
{
	int          displayWidth, displayHeight;
	int          width = 0, height = 0;
	int          size;
	char         path[MAX_OSPATH];
	byte        *pixels;
	SDL_Surface *surface;

	Cursor_DisplaySize (&displayWidth, &displayHeight);
	size = Cursor_ChooseSize (displayWidth, displayHeight);

	if (size == cursor_size && cursor_sdl != NULL && cursor_material != QR_NO_MATERIAL)
	{
		SDL_SetCursor (cursor_sdl);
		return;
	}

	if (cursor_sdl != NULL)
	{
		SDL_FreeCursor (cursor_sdl);
		cursor_sdl = NULL;
	}

	if (cursor_material != QR_NO_MATERIAL && vulkan_globals.instance != QR_NULL_HANDLE)
	{
		qrDestroyMaterial (vulkan_globals.instance, cursor_material);
		cursor_material = QR_NO_MATERIAL;
	}

	q_snprintf (path, sizeof (path), "%s/gfx/quake_axe_%ix%i.png", host_parms->basedir, size, size);

	pixels = Image_LoadImageOSPath (path, &width, &height);
	if (pixels == NULL)
	{
		Con_DPrintf ("cursor: couldn't load %s\n", path);
		return;
	}

	Cursor_FindHotspot (pixels, width, height, &cursor_hot_x, &cursor_hot_y);

	surface = SDL_CreateRGBSurfaceWithFormatFrom (pixels, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
	if (surface != NULL)
	{
		cursor_sdl = SDL_CreateColorCursor (surface, cursor_hot_x, cursor_hot_y);
		SDL_FreeSurface (surface);
	}

	if (cursor_sdl != NULL)
		SDL_SetCursor (cursor_sdl);

	Cursor_CreateMaterial (pixels, width, height);

	Mem_Free (pixels);

	cursor_size = size;
}

void Cursor_Shutdown (void)
{
	if (cursor_sdl != NULL)
	{
		SDL_FreeCursor (cursor_sdl);
		cursor_sdl = NULL;
	}

	if (cursor_material != QR_NO_MATERIAL && vulkan_globals.instance != QR_NULL_HANDLE)
	{
		qrDestroyMaterial (vulkan_globals.instance, cursor_material);
		cursor_material = QR_NO_MATERIAL;
	}

	cursor_size = 0;
}

int Cursor_GetGuiCursor (int64_t *texture, int *size, int *hotX, int *hotY)
{
	if (cursor_material == QR_NO_MATERIAL)
		return 0;

	*texture = (int64_t)cursor_material;
	*size = cursor_size;
	*hotX = cursor_hot_x;
	*hotY = cursor_hot_y;
	return 1;
}
