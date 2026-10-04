/*
 * arch_def.h
 * platform specific definitions
 * - standalone header
 * - doesn't and must not include any other headers
 * - shouldn't depend on compiler.h, q_stdinc.h, or
 *   any other headers
 *
 * Copyright (C) 2007-2016  O.Sezer <sezero@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef ARCHDEFS_H
#define ARCHDEFS_H

#if defined(_WIN32) || defined(__WIN32__) || defined(_WIN64) || defined(__NT__) || defined(_Windows)

#if !defined(PLATFORM_WINDOWS)
#define PLATFORM_WINDOWS 1
#endif

#endif /* PLATFORM_xxx */

#if defined(_WIN64)
#define PLATFORM_STRING "Win64"
#else
#define PLATFORM_STRING "Windows"
#endif /* PLATFORM_STRING */

#endif /* ARCHDEFS_H */
