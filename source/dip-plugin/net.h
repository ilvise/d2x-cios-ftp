/*
 * DIP plugin for Custom IOS.
 *
 * Network (FTP) disc image backend ("MODE_NET"), see net.c.
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

#ifndef _NET_H_
#define _NET_H_

#include "types.h"

/*
 * Network config, passed by the loader with IOCTL_DI_NET_SET (0xF8):
 *   inbuf[0] = 0xF8 << 24
 *   inbuf[1] = pointer to this struct (PPC virtual address)
 *   inbuf[2] = sizeof(struct netConfig) = 264
 * The plugin result (0 = ok) is returned in outbuf[0].
 *
 * ABI: this layout is shared with the loaders (tools/loader/netdisc.h,
 * USB Loader GX FtpGames.cpp). Both sides are big endian and the struct
 * has no padding (4 + 2 + 2 + 32 + 32 + 192 = 264 bytes). Don't change it
 * without changing the loaders: the size is checked by the plugin.
 *
 * The struct is also appended to /sys/dip.cfg (see config.c) to survive
 * the IOS reload done when the game boots.
 */
struct netConfig {
	u32  ip;		// FTP server IPv4 address (e.g. 192.168.1.10 = 0xC0A8010A)
	u16  port;		// FTP control port (0 = 21)
	u16  flags;		// reserved, must be 0
	char user[32];		// "" = anonymous
	char pass[32];
	char path[192];		// absolute path of the .iso on the server
};

/* Prototypes */
s32  Net_Init(struct netConfig *cfg);
void Net_Close(void);
s32  Net_Read(void *outbuf, u32 len, u32 woffset);

/* Current config (also read/written by config.c for the IOS reload) */
extern struct netConfig netcfg_data;

#endif
