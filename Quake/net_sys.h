/*
 * net_sys.h -- common network system header.
 * - depends on arch_def.h
 * - may depend on q_stdinc.h
 *
 * Copyright (C) 2007-2012  O.Sezer <sezero@users.sourceforge.net>
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

#ifndef __NET_SYS_H__
#define __NET_SYS_H__

#include <sys/types.h>
#include <errno.h>
#include <stddef.h>
#include <limits.h>

#define SA_FAM_OFFSET 0

/* windows includes and compatibility macros */
#if defined(PLATFORM_WINDOWS)

/* NOTE: winsock2.h already includes windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>

/* there is no in_addr_t on windows: define it as
   the type of the S_addr of in_addr structure */
typedef u_long in_addr_t; /* uint32_t */

/* on windows, socklen_t is to be a winsock2 thing */
#if !defined(IP_MSFILTER_SIZE)
typedef int socklen_t;
#endif /* socklen_t type */

typedef SOCKET sys_socket_t;

#define selectsocket  select
#define IOCTLARG_P(x) /* (u_long *) */ x

#define SOCKETERRNO      WSAGetLastError ()
#define NET_EWOULDBLOCK  WSAEWOULDBLOCK
#define NET_ECONNREFUSED WSAECONNREFUSED
/* must #include "wsaerror.h" for this : */
#define socketerror(x)   __WSAE_StrError ((x))

COMPILE_TIME_ASSERT (sockaddr, offsetof (struct sockaddr, sa_family) == SA_FAM_OFFSET);

#endif /* end of windows stuff */

/* macros which may still be missing */

#if !defined(INADDR_NONE)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#endif /* INADDR_NONE */

#if !defined(INADDR_LOOPBACK)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001) /* 127.0.0.1	*/
#endif                                          /* INADDR_LOOPBACK */

#if !defined(MAXHOSTNAMELEN)
/* SUSv2 guarantees that `Host names are limited to 255 bytes'.
   POSIX 1003.1-2001 guarantees that `Host names (not including
   the terminating NUL) are limited to HOST_NAME_MAX bytes'. */
#define MAXHOSTNAMELEN 256
#endif /* MAXHOSTNAMELEN */

#endif /* __NET_SYS_H__ */
