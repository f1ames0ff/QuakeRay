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

#include "quakedef.h"
#include "qr_resources.h"

#include <SDL.h>

#ifdef _WIN32
#include <windows.h>

static char     qr_steam_root[MAX_OSPATH];
static qboolean qr_steam_scanned;

static qboolean QR_FileExists (const char *path)
{
	DWORD attrib = GetFileAttributesA (path);
	return attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

static qboolean QR_DirExists (const char *path)
{
	DWORD attrib = GetFileAttributesA (path);
	return attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}

static void QR_StripTrailingSlash (char *path)
{
	size_t len = strlen (path);

	while (len > 0 && (path[len - 1] == '\\' || path[len - 1] == '/'))
	{
		path[--len] = '\0';
	}
}

static qboolean QR_SteamRegistryPath (const wchar_t *subkey, char *out, size_t outsize)
{
	wchar_t buf[1024] = L"";
	DWORD   len = sizeof (buf);

	if (RegGetValueW (HKEY_LOCAL_MACHINE, subkey, L"InstallPath", RRF_RT_REG_SZ, NULL, buf, &len) != ERROR_SUCCESS)
	{
		return false;
	}

	if (WideCharToMultiByte (CP_ACP, 0, buf, -1, out, (int)outsize, NULL, NULL) <= 0)
	{
		out[0] = '\0';
		return false;
	}

	QR_StripTrailingSlash (out);
	return out[0] != '\0';
}

static void QR_TryLibrary (const char *library)
{
	char quake[MAX_OSPATH];

	if (!library[0])
	{
		return;
	}

	q_snprintf (quake, sizeof (quake), "%s/steamapps/common/Quake", library);
	if (QR_DirExists (quake))
	{
		q_strlcpy (qr_steam_root, quake, sizeof (qr_steam_root));
	}
}

static void QR_ScanLibraryFolders (const char *steam)
{
	char  vdf[MAX_OSPATH];
	FILE *f;
	char  line[1024];

	q_snprintf (vdf, sizeof (vdf), "%s/steamapps/libraryfolders.vdf", steam);
	f = fopen (vdf, "r");
	if (!f)
	{
		return;
	}

	while (qr_steam_root[0] == '\0' && fgets (line, sizeof (line), f))
	{
		char *token = strstr (line, "\"path\"");
		char *start;
		char *src;
		char *dst;
		char  library[MAX_OSPATH];

		if (!token)
		{
			continue;
		}

		start = strchr (token + 6, '"');
		if (!start)
		{
			continue;
		}
		start++;

		src = start;
		dst = library;
		while (*src && *src != '"' && (size_t)(dst - library) < sizeof (library) - 1)
		{
			if (src[0] == '\\' && src[1] == '\\')
			{
				src++;
			}
			*dst++ = *src++;
		}
		*dst = '\0';

		QR_TryLibrary (library);
	}

	fclose (f);
}

static void QR_ScanSteamRoot (void)
{
	char steam[MAX_OSPATH];

	if (!QR_SteamRegistryPath (L"SOFTWARE\\WOW6432Node\\Valve\\Steam", steam, sizeof (steam)) &&
	    !QR_SteamRegistryPath (L"SOFTWARE\\Valve\\Steam", steam, sizeof (steam)))
	{
		return;
	}

	QR_TryLibrary (steam);
	if (qr_steam_root[0] == '\0')
	{
		QR_ScanLibraryFolders (steam);
	}
}

void QR_Resources_Init (void)
{
	if (qr_steam_scanned)
	{
		return;
	}
	qr_steam_scanned = true;
	qr_steam_root[0] = '\0';
	QR_ScanSteamRoot ();

	if (qr_steam_root[0] != '\0')
	{
		Con_Printf ("QR: Steam Quake found at %s\n", qr_steam_root);
	}
}

qboolean QR_Resources_SteamDir (char *out, size_t outsize)
{
	QR_Resources_Init ();

	if (qr_steam_root[0] == '\0')
	{
		return false;
	}

	if (out && outsize > 0)
	{
		q_strlcpy (out, qr_steam_root, outsize);
	}
	return true;
}

qboolean QR_Resources_Resolve (const char *dir, char *out, size_t outsize)
{
	char path[MAX_OSPATH];

	QR_Resources_Init ();

	if (qr_steam_root[0] == '\0' || !dir || !dir[0])
	{
		return false;
	}

	q_snprintf (path, sizeof (path), "%s/%s", qr_steam_root, dir);
	if (QR_DirExists (path))
	{
		if (out && outsize > 0)
		{
			q_strlcpy (out, path, outsize);
		}
		return true;
	}

	q_snprintf (path, sizeof (path), "%s/rerelease/%s", qr_steam_root, dir);
	if (QR_DirExists (path))
	{
		if (out && outsize > 0)
		{
			q_strlcpy (out, path, outsize);
		}
		return true;
	}

	return false;
}

qboolean QR_Resources_HasGameData (void)
{
	char path[MAX_OSPATH];

	QR_Resources_Init ();

	q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", com_basedir);
	if (QR_FileExists (path))
	{
		return true;
	}

	if (qr_steam_root[0] != '\0')
	{
		q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", qr_steam_root);
		if (QR_FileExists (path))
		{
			return true;
		}
	}

	return false;
}

static qboolean QR_DirHasContent (const char *dir)
{
	char pattern[MAX_OSPATH];
	WIN32_FIND_DATAA fd;
	HANDLE h;

	q_snprintf (pattern, sizeof (pattern), "%s/*.pak", dir);
	h = FindFirstFileA (pattern, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		FindClose (h);
		return true;
	}

	q_snprintf (pattern, sizeof (pattern), "%s/progs.dat", dir);
	if (QR_FileExists (pattern))
	{
		return true;
	}

	return false;
}

static int QR_EnumModsIn (const char *root, void (*cb) (const char *base, const char *name, void *ctx), void *ctx)
{
	char pattern[MAX_OSPATH];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int count = 0;

	q_snprintf (pattern, sizeof (pattern), "%s/*", root);
	h = FindFirstFileA (pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	do
	{
		char dir[MAX_OSPATH];

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!strcmp (fd.cFileName, ".") || !strcmp (fd.cFileName, "..") ||
			    !q_strcasecmp (fd.cFileName, "id1") || !q_strcasecmp (fd.cFileName, "rerelease"))
			{
				continue;
			}

			q_snprintf (dir, sizeof (dir), "%s/%s", root, fd.cFileName);
			if (QR_DirHasContent (dir))
			{
				cb (root, fd.cFileName, ctx);
				count++;
			}
		}
	} while (FindNextFileA (h, &fd));

	FindClose (h);
	return count;
}

int QR_Resources_EnumMods (void (*cb) (const char *base, const char *name, void *ctx), void *ctx)
{
	char rerelease[MAX_OSPATH];
	int  count;

	QR_Resources_Init ();

	if (!cb || qr_steam_root[0] == '\0')
	{
		return 0;
	}

	count = QR_EnumModsIn (qr_steam_root, cb, ctx);

	q_snprintf (rerelease, sizeof (rerelease), "%s/rerelease", qr_steam_root);
	count += QR_EnumModsIn (rerelease, cb, ctx);
	return count;
}

qboolean QR_Resources_RemasteredDir (char *out, size_t outsize)
{
	char path[MAX_OSPATH];

	QR_Resources_Init ();

	if (qr_steam_root[0] == '\0')
	{
		return false;
	}

	q_snprintf (path, sizeof (path), "%s/rerelease", qr_steam_root);
	if (!QR_DirExists (path) || !QR_Resources_FlavorDir (path, QR_FLAVOR_REMASTERED))
	{
		return false;
	}

	if (out && outsize > 0)
	{
		q_strlcpy (out, path, outsize);
	}
	return true;
}

qboolean QR_Resources_NightdiveDir (char *out, size_t outsize)
{
	char        path[MAX_OSPATH];
	const char *profile = getenv ("USERPROFILE");

	if (!profile || !profile[0])
	{
		return false;
	}

	q_snprintf (path, sizeof (path), "%s/Saved Games/Nightdive Studios/Quake", profile);
	if (!QR_DirExists (path))
	{
		return false;
	}

	if (out && outsize > 0)
	{
		q_strlcpy (out, path, outsize);
	}
	return true;
}

#else /* !_WIN32 */

void QR_Resources_Init (void)
{
}

qboolean QR_Resources_SteamDir (char *out, size_t outsize)
{
	(void)out;
	(void)outsize;
	return false;
}

qboolean QR_Resources_Resolve (const char *dir, char *out, size_t outsize)
{
	(void)dir;
	(void)out;
	(void)outsize;
	return false;
}

qboolean QR_Resources_HasGameData (void)
{
	char path[MAX_OSPATH];

	q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", com_basedir);
	return Sys_FileTime (path) != -1;
}

int QR_Resources_EnumMods (void (*cb) (const char *base, const char *name, void *ctx), void *ctx)
{
	(void)cb;
	(void)ctx;
	return 0;
}

qboolean QR_Resources_RemasteredDir (char *out, size_t outsize)
{
	(void)out;
	(void)outsize;
	return false;
}

qboolean QR_Resources_NightdiveDir (char *out, size_t outsize)
{
	(void)out;
	(void)outsize;
	return false;
}

#endif /* _WIN32 */

qboolean QR_Resources_FlavorDir (const char *dir, int flavor)
{
	char path[MAX_OSPATH];

	if (!dir || !dir[0])
	{
		return false;
	}

	if (flavor != QR_FLAVOR_REMASTERED)
	{
		q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", dir);
		if (Sys_FileTime (path) != -1)
		{
			return true;
		}
	}

	if (flavor != QR_FLAVOR_ORIGINAL)
	{
		q_snprintf (path, sizeof (path), "%s/QuakeEX.kpf", dir);
		if (Sys_FileTime (path) != -1)
		{
			return true;
		}
	}

	return false;
}

int QR_Resources_ChooseFlavor (void)
{
	static const SDL_MessageBoxButtonData buttons[] = {
		{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, QR_FLAVOR_REMASTERED, "Remastered"},
		{0, QR_FLAVOR_ORIGINAL, "Original"},
	};
	SDL_MessageBoxData box;
	int                 choice = -1;

	memset (&box, 0, sizeof (box));
	box.buttons = buttons;
	box.numbuttons = countof (buttons);
	box.flags = SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
	box.title = "QuakeRay";
	box.message = "Which Quake version would you like to play?";

	if (SDL_ShowMessageBox (&box, &choice) < 0 || choice < 0)
	{
		SDL_Quit ();
		exit (0);
	}

	return choice;
}
