/*
 * Loader-side helper for the d2x-cios-ftp network (FTP) disc mode.
 *
 * Copyright (C) 2026 d2x-cios-ftp contributors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <gccore.h>

#include "netdisc.h"

/*
 * IPC buffers. They are static and 32 byte aligned because IOS reads
 * them from RAM: the config is flushed from the PPC cache before the
 * call, and the cIOS keeps its own copy, so this buffer can be reused.
 */
static struct netConfig netcfg ATTRIBUTE_ALIGN(32);
static u32 inbuf[8]  ATTRIBUTE_ALIGN(32);
static u32 outbuf[8] ATTRIBUTE_ALIGN(32);

/*
 * "a.b.c.d" -> 0xAABBCCDD (native big endian = network order on the Wii).
 * Returns 0 if the string is not made of exactly 4 numbers.
 */
static u32 __ParseIp(const char *s)
{
	u32 ip = 0, part = 0, n = 0;

	for (;; s++) {
		if (*s >= '0' && *s <= '9')
			part = part * 10 + (*s - '0');
		else {
			ip = (ip << 8) | (part & 0xFF);
			part = 0;
			n++;
			if (*s != '.')
				break;
		}
	}

	return (n == 4) ? ip : 0;
}

s32 WDVD_SetNetMode(s32 di_fd, const char *server, u16 port,
		    const char *user, const char *pass, const char *path)
{
	s32 ret;

	/* Build the config (strings always NUL terminated) */
	memset(&netcfg, 0, sizeof(netcfg));
	netcfg.ip   = __ParseIp(server);
	netcfg.port = port;
	if (user)
		strncpy(netcfg.user, user, sizeof(netcfg.user) - 1);
	if (pass)
		strncpy(netcfg.pass, pass, sizeof(netcfg.pass) - 1);
	strncpy(netcfg.path, path, sizeof(netcfg.path) - 1);

	if (!netcfg.ip || !netcfg.path[0])
		return -1;

	/* The cIOS reads the struct from RAM */
	DCFlushRange(&netcfg, sizeof(netcfg));

	memset(inbuf, 0, sizeof(inbuf));
	memset(outbuf, 0, sizeof(outbuf));

	/* DI command block: command << 24, then the arguments */
	inbuf[0] = IOCTL_DI_NET_SET << 24;
	inbuf[1] = (u32)&netcfg;
	inbuf[2] = sizeof(netcfg);

	/* The DI driver answers 1 when the command succeeded */
	ret = IOS_Ioctl(di_fd, IOCTL_DI_NET_SET, inbuf, sizeof(inbuf), outbuf, sizeof(outbuf));
	if (ret != 1)
		return (ret < 0) ? ret : -ret;

	/* outbuf[0] is the plugin result (0 = ok) */
	return outbuf[0];
}
