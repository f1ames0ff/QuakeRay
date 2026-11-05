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

#if defined(_WIN32) && defined(_MSC_VER)
#pragma comment(linker, \
	"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

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

static qboolean QR_RegString (HKEY root, const wchar_t *subkey, const wchar_t *value, char *out, size_t outsize)
{
	wchar_t buf[1024] = L"";
	DWORD   len = sizeof (buf);

	if (RegGetValueW (root, subkey, value, RRF_RT_REG_SZ, NULL, buf, &len) != ERROR_SUCCESS)
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
	char  manifest[MAX_OSPATH];
	char  quake[MAX_OSPATH];
	char  installdir[128] = "";
	FILE *f;
	char  line[1024];

	if (!library[0] || qr_steam_root[0] != '\0')
	{
		return;
	}

	q_snprintf (manifest, sizeof (manifest), "%s/steamapps/appmanifest_2310.acf", library);
	f = fopen (manifest, "r");
	if (f)
	{
		while (fgets (line, sizeof (line), f))
		{
			char *token = strstr (line, "\"installdir\"");

			if (token)
			{
				char *start = strchr (token + 12, '"');
				char *end;

				if (!start)
				{
					continue;
				}
				start++;
				end = strchr (start, '"');
				if (!end)
				{
					continue;
				}
				*end = '\0';
				q_strlcpy (installdir, start, sizeof (installdir));
				break;
			}
		}
		fclose (f);
	}

	if (!installdir[0])
	{
		q_strlcpy (installdir, "Quake", sizeof (installdir));
	}

	q_snprintf (quake, sizeof (quake), "%s/steamapps/common/%s", library, installdir);
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

	q_snprintf (vdf, sizeof (vdf), "%s/config/libraryfolders.vdf", steam);
	f = fopen (vdf, "r");
	if (!f)
	{
		q_snprintf (vdf, sizeof (vdf), "%s/steamapps/libraryfolders.vdf", steam);
		f = fopen (vdf, "r");
	}
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

	if (QR_RegString (HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam, sizeof (steam)))
	{
		QR_TryLibrary (steam);
		if (qr_steam_root[0] == '\0')
		{
			QR_ScanLibraryFolders (steam);
		}
		if (qr_steam_root[0] != '\0')
		{
			return;
		}
	}

	if (QR_RegString (HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", steam, sizeof (steam)) ||
	    QR_RegString (HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", steam, sizeof (steam)))
	{
		QR_TryLibrary (steam);
		if (qr_steam_root[0] == '\0')
		{
			QR_ScanLibraryFolders (steam);
		}
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

	if (COM_CheckParm ("-nosteam"))
	{
		return;
	}

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

qboolean QR_Resources_HasGameData (void)
{
	char path[MAX_OSPATH];
	int  i;

	for (i = 0; i < com_numbasedirs; i++)
	{
		q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", com_basedirs[i]);
		if (QR_FileExists (path))
		{
			return true;
		}
	}

	return false;
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
	const char *profile;

	if (COM_CheckParm ("-nonightdive"))
	{
		return false;
	}

	{
		PWSTR savedgames = NULL;

		if (SHGetKnownFolderPath (&FOLDERID_SavedGames, 0, NULL, &savedgames) == S_OK)
		{
			char saved[MAX_OSPATH];
			char wide[MAX_OSPATH] = "";

			WideCharToMultiByte (CP_ACP, 0, savedgames, -1, wide, sizeof (wide), NULL, NULL);
			CoTaskMemFree (savedgames);

			q_snprintf (saved, sizeof (saved), "%s/Nightdive Studios/Quake", wide);
			if (QR_DirExists (saved))
			{
				if (out && outsize > 0)
				{
					q_strlcpy (out, saved, outsize);
				}
				return true;
			}
		}
	}

	profile = getenv ("USERPROFILE");
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

qboolean QR_Resources_HasGameData (void)
{
	char path[MAX_OSPATH];
	int  i;

	for (i = 0; i < com_numbasedirs; i++)
	{
		q_snprintf (path, sizeof (path), "%s/id1/pak0.pak", com_basedirs[i]);
		if (Sys_FileTime (path) != -1)
		{
			return true;
		}
	}

	return false;
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

	if (SDL_ShowMessageBox (&box, &choice) < 0)
	{
		Con_Printf ("QR: flavor dialog failed: %s\n", SDL_GetError ());
		return QR_FLAVOR_REMASTERED;
	}

	if (choice < 0)
	{
		SDL_Quit ();
		exit (0);
	}

	return choice;
}
