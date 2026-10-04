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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>

#include "quakedef.h"
#include "snd_openal.h"

static const char *Sys_ArchString (void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
	return "arm64";
#elif defined(_WIN64) || defined(__x86_64__) || defined(__amd64__)
	return "x64";
#else
	return "x86";
#endif
}

static void Sys_TrimString (char *str)
{
	size_t start = 0;
	size_t end;

	while (str[start] == ' ' || str[start] == '\t')
		start++;

	if (start > 0)
		memmove (str, str + start, strlen (str + start) + 1);

	end = strlen (str);
	while (end > 0 && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\n' || str[end - 1] == '\r'))
		str[--end] = 0;
}

typedef LONG (WINAPI *Sys_RtlGetVersionFn) (OSVERSIONINFOW *);

static void Sys_OSVersionString (char *out, size_t outSize)
{
	Sys_RtlGetVersionFn fn = NULL;
	HMODULE            ntdll = GetModuleHandleW (L"ntdll.dll");
	OSVERSIONINFOW     version;

	if (ntdll != NULL)
		fn = (Sys_RtlGetVersionFn)GetProcAddress (ntdll, "RtlGetVersion");

	memset (&version, 0, sizeof (version));
	version.dwOSVersionInfoSize = sizeof (version);

	if (fn != NULL && fn (&version) == 0)
	{
		const char *name = "Windows";

		if (version.dwMajorVersion == 10)
			name = version.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
		else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 1)
			name = "Windows 7";
		else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 2)
			name = "Windows 8";
		else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 3)
			name = "Windows 8.1";

		q_snprintf (out, outSize, "%s %lu.%lu.%lu %s", name, version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, Sys_ArchString ());
	}
	else
		q_snprintf (out, outSize, "Windows (unknown version) %s", Sys_ArchString ());
}

static void Sys_CPUNameString (char *out, size_t outSize)
{
	int    regs[4] = {0, 0, 0, 0};
	char   brand[49] = "";
	size_t i;

	__cpuid (regs, (int)0x80000000);
	if ((unsigned)regs[0] < 0x80000004u)
	{
		q_snprintf (out, outSize, "unknown");
		return;
	}

	for (i = 0; i < 3; i++)
	{
		__cpuid (regs, (int)(0x80000002u + i));
		memcpy (brand + i * 16, regs, 16);
	}

	Sys_TrimString (brand);
	q_snprintf (out, outSize, "%s", brand);
}

static void Sys_DriverVersionString (char *out, size_t outSize, uint32_t vendorId, uint32_t driverVersion)
{
	if (vendorId == 0x10DEu)
		q_snprintf (out, outSize, "%u.%u.%u.%u",
		            (driverVersion >> 22) & 0x3FFu, (driverVersion >> 14) & 0xFFu,
		            (driverVersion >> 6) & 0xFFu, driverVersion & 0x3Fu);
	else
		q_snprintf (out, outSize, "%u.%u.%u",
		            (driverVersion >> 22) & 0x3FFu, (driverVersion >> 12) & 0x3FFu,
		            driverVersion & 0xFFFu);
}

static void Sys_APIVersionString (char *out, size_t outSize, uint32_t apiVersion)
{
	q_snprintf (out, outSize, "%u.%u.%u",
	            (apiVersion >> 22) & 0x3FFu, (apiVersion >> 12) & 0x3FFu, apiVersion & 0xFFFu);
}

void Sys_PrintSystemInfo (void)
{
	QrAdapterInfo adapter;
	char          os[160];
	char          cpu[160];
	char          driverVersion[64];
	char          apiVersion[32];
	SDL_version   sdlVersion;
	const char   *audioDevice;

	Sys_OSVersionString (os, sizeof (os));
	Sys_CPUNameString (cpu, sizeof (cpu));
	SDL_GetVersion (&sdlVersion);

	Con_Printf ("\nSystem information\n");
	Con_Printf ("OS      : %s, SDL %u.%u.%u\n", os, sdlVersion.major, sdlVersion.minor, sdlVersion.patch);
	Con_Printf ("CPU     : %s, %d CPUs, %.1f GB RAM\n", cpu, SDL_GetCPUCount (), SDL_GetSystemRAM () / 1024.0);

	if (vulkan_globals.instance != NULL && qrGetAdapterInfo (vulkan_globals.instance, &adapter) == QR_SUCCESS)
	{
		Sys_DriverVersionString (driverVersion, sizeof (driverVersion), adapter.vendorId, adapter.driverVersion);
		Sys_APIVersionString (apiVersion, sizeof (apiVersion), adapter.apiVersion);
		Con_Printf ("GPU     : %s [%04x:%04x]\n", adapter.name, (unsigned)adapter.vendorId, (unsigned)adapter.deviceId);
		Con_Printf ("Video   : %s %s, driver version %s, Vulkan %s\n", adapter.driverName, adapter.driverInfo, driverVersion, apiVersion);
	}
	else
		Con_Printf ("GPU     : unavailable\n");

	audioDevice = SNDAL_DeviceName ();
	Con_Printf ("Audio   : %s%s%s\n",
	            snd_output.ready ? "OpenAL Soft" : "(none)",
	            audioDevice != NULL ? " - " : "",
	            audioDevice != NULL ? audioDevice : "");
}
