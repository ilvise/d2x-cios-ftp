/****************************************************************************
 * FTP game list for USB Loader GX (d2x-cios-ftp integration)
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
 *
 * Wii games stored as uncompressed .iso files on an FTP server, played
 * through the d2x-cios-ftp cIOS, whose DIP plugin reads the disc image
 * from the server (custom DI ioctl IOCTL_DI_NET_SET, 0xF8).
 *
 * Integration in the loader, in short:
 *   - GameList::ReadGameList() calls FtpGames_ReadList() after reading
 *     the local partitions, and stores the FTP games with the partition
 *     number FTP_PARTITION;
 *   - GameBooter::SetupDisc() calls FtpGames_SetDisc() for those games
 *     instead of building and sending a frag list.
 * FTP games are normal TYPE_GAME_WII_IMG headers, so covers, game
 * settings, categories, etc. work as for local games.
 ***************************************************************************/
#ifndef _FTPGAMES_H_
#define _FTPGAMES_H_

#include <vector>
#include "usbloader/disc.h"

/*
 * Partition number used in GameList::GamePartitionList for games on the
 * FTP server. It is far above any real partition index, so the WBFS_*
 * functions (guarded by VALID(part) in wbfs.cpp) just fail for these
 * games instead of touching a drive. It is also stored in the game
 * header cache, so cached FTP games can still be booted.
 */
#define FTP_PARTITION	1000

/*
 * Scan the FTP server set in the loader settings and append the Wii games
 * found to "list". Returns the number of games added, 0 if the feature is
 * off, < 0 on error (network down, server unreachable, folder missing).
 * Blocking: it starts the network if needed and waits up to 5 s per
 * server reply.
 */
int FtpGames_ReadList(std::vector<struct discHdr> &list);

/*
 * Set the game in the cIOS (IOCTL_DI_NET_SET), then read the disc ID
 * through it, which makes the cIOS connect to the server, and check that
 * it matches the game. Returns 0 on success, < 0 on error.
 * Must be called after the reload into the game cIOS.
 */
int FtpGames_SetDisc(const struct discHdr *header);

#endif
