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

/*
 * For loader developers (libogc, PPC side).
 *
 * Copy netdisc.c/.h into a loader (WiiFlow, CFG, your own...) and call
 * WDVD_SetNetMode() where the loader would call WDVD_SetFragList() for a
 * game on a USB/SD drive. Then go on as usual: reset the drive, read the
 * disc ID, open the game partition, run the apploader. From then on every
 * disc read is served by the cIOS from the FTP server.
 *
 * Only works with d2x-cios-ftp. On other cIOSes the ioctl is unknown and
 * WDVD_SetNetMode() returns an error.
 *
 * USB Loader GX has a complete integration with game list scanning, see
 * loader/usbloadergx/ in this repository.
 */

#ifndef _NETDISC_H_
#define _NETDISC_H_

#include <gccore.h>

/* Custom DI ioctl of d2x-cios-ftp (source/dip-plugin/ioctl.h) */
#define IOCTL_DI_NET_SET	0xF8

/* DIP mode bit reported by IOCTL_DI_MODE_GET (0xFA) when net mode is on */
#define DIP_MODE_NET		0x20

/*
 * Must match struct netConfig in source/dip-plugin/net.h: same order,
 * same sizes, 264 bytes, no padding. The plugin rejects other sizes.
 */
struct netConfig {
	u32  ip;		// FTP server IPv4 address, e.g. 192.168.1.10 = 0xC0A8010A
	u16  port;		// FTP control port (0 = 21)
	u16  flags;		// reserved, must be 0
	char user[32];		// "" = anonymous
	char pass[32];
	char path[192];		// absolute path of the .iso on the server
} ATTRIBUTE_PACKED;

/*
 * Set the network disc mode in the cIOS.
 *
 * di_fd:  descriptor of an open "/dev/di"
 * server: dotted IPv4 address, e.g. "192.168.1.10" (no host names: the
 *         cIOS has no DNS resolver, and IPs are what home users have)
 * port:   FTP control port, 0 = 21
 * user:   NULL or "" = anonymous login
 * pass:   NULL or "" = default anonymous password
 * path:   absolute path of the .iso on the server, max 191 characters
 *
 * Returns 0 on success, < 0 on IOS errors, > 0 if the plugin rejected the
 * config. The server is not contacted here: read the disc ID afterwards
 * to check that the game is reachable.
 */
s32 WDVD_SetNetMode(s32 di_fd, const char *server, u16 port,
		    const char *user, const char *pass, const char *path);

#endif
