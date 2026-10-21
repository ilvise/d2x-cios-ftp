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
 * See FtpGames.h for the integration in the loader.
 *
 * Two different FTP clients are involved:
 *   - this file (PPC, libogc sockets) only LISTS the games: it finds the
 *     .iso files and reads the first 0x60 bytes of each one (the disc
 *     header with game ID and title);
 *   - the cIOS (ARM, source/dip-plugin/net.c of d2x-cios-ftp) READS the
 *     game while it runs, after the loader has passed it the server and
 *     the path of the image.
 *
 * Folder layout: the .iso files are searched in the configured folder
 * (Settings.FtpPath, default "/wbfs") and in its direct subfolders, so
 * both "/wbfs/RMCP01.iso" and the usual USB Loader GX layout
 * "/wbfs/Mario Kart Wii [RMCP01]/RMCP01.iso" work. Deeper folders are not
 * scanned, to keep the number of FTP commands (and the scan time) low.
 *
 * Login: anonymous by default (FtpUser empty). FtpUser/FtpPass can be set
 * in GXGlobal.cfg; there is no GUI field for them on purpose (anonymous
 * read only access is the recommended setup for a LAN game share).
 ***************************************************************************/
#include <string>
#include <vector>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>

#include "FtpGames.h"
#include "network/networkops.h"
#include "settings/CSettings.h"
#include "settings/SettingsEnums.h"
#include "usbloader/wdvd.h"
#include "gecko.h"

#define FTP_TIMEOUT		5000		// ms, max wait for any server reply or data
#define FTP_MAX_GAMES	500			// safety limit for the game list
#define WII_MAGIC		0x5D1C9EA3	// Wii disc magic at offset 0x18 of the header

/*
 * Must match struct netConfig in the cIOS (source/dip-plugin/net.h of
 * d2x-cios-ftp): same order, same sizes, 264 bytes, no padding. The cIOS
 * rejects other sizes. Both sides are big endian.
 */
struct netConfig
{
	u32 ip;				// server IPv4 address, native (= network) byte order
	u16 port;			// control port, 0 = 21
	u16 flags;			// reserved, 0
	char user[32];		// "" = anonymous
	char pass[32];
	char path[192];		// absolute path of the .iso on the server
} ATTRIBUTE_PACKED;

/* Passed to the cIOS by pointer: static and 32 byte aligned for IOS */
static struct netConfig netcfg ATTRIBUTE_ALIGN(32);

/*
 * Minimal FTP client, just enough to list folders and read file headers.
 * One control connection; each LIST/RETR opens a passive data connection.
 */
class FtpClient
{
	public:
		FtpClient() : ctrl(-1), rlen(0) { line[0] = 0; }
		~FtpClient() { Close(); }

		//! Connect and log in (empty user = anonymous), binary mode
		bool Connect(u32 ip, u16 port, const char *user, const char *pass);
		//! QUIT and close the control connection
		void Close();
		//! Send "cmd[ arg]" and return the code of the first reply, < 0 on error
		int Cmd(const char *cmd, const char *arg = NULL);
		//! Names (not paths) of the entries of folder "dir"
		bool List(const std::string &dir, std::vector<std::string> &names);
		//! Read the disc header of the Wii image "path" into "header"
		bool ReadHeader(const std::string &path, struct discHdr *header);
		//! Last reply line, for error messages
		const char *LastLine() const { return line; }

	private:
		int Reply();
		int OpenData();
		bool WaitData(s32 s);

		s32 ctrl;			// control connection socket, -1 = closed
		u32 ip;				// server address, also used for data connections
		char rbuf[512];		// control connection receive buffer
		u32 rlen;			// bytes pending in rbuf
		char line[512];		// final line of the last reply
};

/* TCP connection to ip:port (ip in host = big endian order), < 0 on error */
static s32 ConnectTo(u32 ip, u16 port)
{
	s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s < 0)
		return s;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(ip);

	if (net_connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		net_close(s);
		return -1;
	}

	return s;
}

/* Wait until socket "s" is readable (data, EOF or error), with timeout */
bool FtpClient::WaitData(s32 s)
{
	struct pollsd sd;
	sd.socket = s;
	sd.events = POLLIN;
	sd.revents = 0;

	return net_poll(&sd, 1, FTP_TIMEOUT) > 0 && sd.revents != 0;
}

/*
 * Read the next complete reply and return its code (100..599), -1 on
 * timeout/error. Only a line starting with "ddd " ends a reply (multi-line
 * replies use "ddd-" on the other lines, RFC 959); its text is kept in
 * line[]. Data received after the reply stays in rbuf for the next call.
 * Same logic as __Ftp_Reply() in the cIOS.
 */
int FtpClient::Reply()
{
	while (true)
	{
		for (u32 i = 0; i < rlen; ++i)
		{
			if (rbuf[i] != '\n')
				continue;

			int code = -1;
			if (i >= 3 && rbuf[3] == ' ' && rbuf[0] >= '1' && rbuf[0] <= '5')
				code = atoi(rbuf);

			memcpy(line, rbuf, i);
			line[i] = 0;

			/* Consume the line */
			rlen -= i + 1;
			memmove(rbuf, rbuf + i + 1, rlen);

			if (code > 0)
				return code;

			/* Not a final line: rescan from the start (i++ makes it 0) */
			i = (u32)-1;
		}

		/* Line longer than the buffer: drop it */
		if (rlen >= sizeof(rbuf))
			rlen = 0;

		if (!WaitData(ctrl))
			return -1;

		int n = net_recv(ctrl, rbuf + rlen, sizeof(rbuf) - rlen, 0);
		if (n <= 0)
			return -1;

		rlen += n;
	}
}

int FtpClient::Cmd(const char *cmd, const char *arg)
{
	char buf[300];
	int len = snprintf(buf, sizeof(buf), arg ? "%s %s\r\n" : "%s\r\n", cmd, arg);
	if (len <= 0 || len >= (int)sizeof(buf))
		return -1;

	if (net_send(ctrl, buf, len, 0) != len)
		return -1;

	return Reply();
}

bool FtpClient::Connect(u32 server, u16 port, const char *user, const char *pass)
{
	ip = server;
	ctrl = ConnectTo(ip, port);
	if (ctrl < 0)
		return false;

	/* Welcome message: skip preliminary 1xx replies, expect 220 */
	int ret;
	do
	{
		ret = Reply();
	} while (ret > 0 && ret < 200);

	if (ret != 220)
		return false;

	/* 331 = password required; 230 = logged in; 202 = login not needed */
	ret = Cmd("USER", (user && *user) ? user : "anonymous");
	if (ret == 331)
		ret = Cmd("PASS", (pass && *pass) ? pass : "wii@");
	if (ret != 230 && ret != 202)
		return false;

	/* Binary mode: ASCII mode could alter the data */
	return Cmd("TYPE", "I") == 200;
}

void FtpClient::Close()
{
	if (ctrl >= 0)
	{
		Cmd("QUIT");
		net_close(ctrl);
	}
	ctrl = -1;
	rlen = 0;
}

/*
 * PASV, then connect the data connection. The address in the 227 reply
 * is ignored and the configured server address is used, because servers
 * behind NAT often announce an address that is not reachable from the
 * LAN (the cIOS does the same).
 */
int FtpClient::OpenData()
{
	if (Cmd("PASV") != 227)
		return -1;

	/* "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" */
	const char *p = strchr(line, '(');
	if (!p)
		return -1;

	u32 v[6];
	for (int i = 0; i < 6; ++i)
	{
		v[i] = strtoul(p + 1, (char **)&p, 10);
		if (!p)
			return -1;
	}

	//! Data connection always to the configured server (NAT friendly)
	return ConnectTo(ip, (v[4] << 8) | v[5]);
}

/*
 * List folder "dir" with CWD + NLST (names only, one per line). NLST is
 * used instead of LIST because the LIST format is not standardized.
 * NLST does not tell files from folders: the caller just tries to list
 * every entry that is not an .iso as a folder, and CWD fails on files.
 */
bool FtpClient::List(const std::string &dir, std::vector<std::string> &names)
{
	if (Cmd("CWD", dir.c_str()) != 250)
		return false;

	s32 data = OpenData();
	if (data < 0)
		return false;

	int ret = Cmd("NLST");
	if (ret != 125 && ret != 150)
	{
		net_close(data);
		return false;
	}

	/* Receive the whole listing until the server closes the connection */
	std::string listing;
	char buf[1024];
	while (WaitData(data))
	{
		int n = net_recv(data, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		listing.append(buf, n);
	}
	net_close(data);

	//! Transfer complete (226)
	if (Reply() < 0)
		return false;

	/* Split lines, strip "\r", keep the last path component */
	size_t pos = 0;
	while (pos < listing.size())
	{
		size_t end = listing.find('\n', pos);
		if (end == std::string::npos)
			end = listing.size();

		std::string name = listing.substr(pos, end - pos);
		if (!name.empty() && name[name.size() - 1] == '\r')
			name.erase(name.size() - 1);

		//! Some servers return full paths
		size_t slash = name.rfind('/');
		if (slash != std::string::npos)
			name.erase(0, slash + 1);

		if (!name.empty() && name != "." && name != "..")
			names.push_back(name);

		pos = end + 1;
	}

	return true;
}

/*
 * Read the first 0x60 bytes of "path" (RETR, then abort the transfer by
 * closing the data connection: the server answers 426 or 226, which is
 * consumed). Fails for files that are not Wii images (no Wii magic at
 * 0x18), e.g. GameCube images, which the cIOS can't serve in this mode.
 */
bool FtpClient::ReadHeader(const std::string &path, struct discHdr *header)
{
	s32 data = OpenData();
	if (data < 0)
		return false;

	int ret = Cmd("RETR", path.c_str());
	if (ret != 125 && ret != 150)
	{
		net_close(data);
		return false;
	}

	u8 buf[0x60];
	u32 len = 0;
	while (len < sizeof(buf) && WaitData(data))
	{
		int n = net_recv(data, buf + len, sizeof(buf) - len, 0);
		if (n <= 0)
			break;
		len += n;
	}

	//! Abort the transfer, the server answers 426 or 226
	net_close(data);
	if (Reply() < 0 || len < sizeof(buf))
		return false;

	/* Byte by byte: independent of the host endianness (host tests) */
	u32 magic = (buf[0x18] << 24) | (buf[0x19] << 16) | (buf[0x1A] << 8) | buf[0x1B];
	if (magic != WII_MAGIC)
		return false;

	/*
	 * The first 0x60 bytes of struct discHdr follow the disc header
	 * layout (ID at 0x00, magic at 0x18, title at 0x20), as for the
	 * headers read from WBFS/FAT; the loader-internal fields that
	 * overlap the disc header (is_ciso, tid, type, unused) are reset.
	 * path[] holds the FTP path of the image, used by FtpGames_SetDisc().
	 */
	memset(header, 0, sizeof(struct discHdr));
	memcpy(header, buf, sizeof(buf));
	header->is_ciso = 0;
	header->tid = 0;
	header->type = TYPE_GAME_WII_IMG;
	memset(header->unused, 0, sizeof(header->unused));
	header->title[sizeof(header->title) - 1] = 0;
	snprintf(header->path, sizeof(header->path), "%s", path.c_str());

	return true;
}

/* Case insensitive ".iso" extension check */
static bool IsIso(const std::string &name)
{
	return name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".iso") == 0;
}

/*
 * Settings.FtpServer is "192.168.1.10" or "192.168.1.10:2121". Only IPv4
 * addresses: the cIOS has no DNS resolver. Returns false if invalid.
 */
static bool ParseServer(const char *server, u32 *ip, u16 *port)
{
	char host[64];
	snprintf(host, sizeof(host), "%s", server);

	*port = 21;
	char *colon = strchr(host, ':');
	if (colon)
	{
		*colon = 0;
		*port = atoi(colon + 1);
	}

	struct in_addr addr;
	if (!inet_aton(host, &addr) || *port == 0)
		return false;

	*ip = ntohl(addr.s_addr);
	return true;
}

int FtpGames_ReadList(std::vector<struct discHdr> &list)
{
	if (!Settings.FtpGames || !Settings.FtpServer[0])
		return 0;

	u32 ip;
	u16 port;
	if (!ParseServer(Settings.FtpServer, &ip, &port))
	{
		gprintf("FTP: invalid server %s\n", Settings.FtpServer);
		return -1;
	}

	/* The loader may not have started the network yet */
	if (!IsNetworkInit())
		Initialize_Network(3);

	if (!IsNetworkInit())
	{
		gprintf("FTP: network not available\n");
		return -1;
	}

	FtpClient ftp;
	if (!ftp.Connect(ip, port, Settings.FtpUser, Settings.FtpPass))
	{
		gprintf("FTP: connection to %s failed: %s\n", Settings.FtpServer, ftp.LastLine());
		return -1;
	}

	/* Normalize the folder: leading '/', no trailing '/' */
	std::string root(Settings.FtpPath[0] ? Settings.FtpPath : "/");
	if (root[0] != '/')
		root.insert(0, "/");
	if (root.size() > 1 && root[root.size() - 1] == '/')
		root.erase(root.size() - 1);
	std::string prefix = (root == "/") ? "" : root;

	std::vector<std::string> names;
	if (!ftp.List(root, names))
	{
		gprintf("FTP: can't list %s\n", root.c_str());
		return -1;
	}

	//! .iso in the root folder, or one folder level below
	std::vector<std::string> isos;
	for (u32 i = 0; i < names.size(); ++i)
	{
		if (IsIso(names[i]))
		{
			isos.push_back(prefix + "/" + names[i]);
			continue;
		}

		/* Not an .iso: maybe a folder (List fails on files) */
		std::vector<std::string> sub;
		std::string dir = prefix + "/" + names[i];
		if (!ftp.List(dir, sub))
			continue;

		for (u32 j = 0; j < sub.size(); ++j)
			if (IsIso(sub[j]))
				isos.push_back(dir + "/" + sub[j]);
	}

	int count = 0;
	for (u32 i = 0; i < isos.size() && count < FTP_MAX_GAMES; ++i)
	{
		//! The cIOS path is limited to 191 characters
		if (isos[i].size() >= sizeof(netcfg.path))
			continue;

		struct discHdr header;
		if (!ftp.ReadHeader(isos[i], &header))
			continue;

		gprintf("FTP: %.6s %s\n", header.id, isos[i].c_str());
		list.push_back(header);
		count++;
	}

	return count;
}

int FtpGames_SetDisc(const struct discHdr *header)
{
	u32 ip;
	u16 port;
	if (!ParseServer(Settings.FtpServer, &ip, &port) || !header->path[0] || strlen(header->path) >= sizeof(netcfg.path))
		return -1;

	memset(&netcfg, 0, sizeof(netcfg));
	netcfg.ip = ip;
	netcfg.port = port;
	strlcpy(netcfg.user, Settings.FtpUser, sizeof(netcfg.user));
	strlcpy(netcfg.pass, Settings.FtpPass, sizeof(netcfg.pass));
	strlcpy(netcfg.path, header->path, sizeof(netcfg.path));

	gprintf("Calling WDVD_SetNetMode: ftp://%s%s\n", Settings.FtpServer, netcfg.path);
	int ret = WDVD_SetNetMode(&netcfg, sizeof(netcfg));
	if (ret)
		return -1;

	/*
	 * First read: the cIOS starts the IOS network stack (DHCP can take a
	 * few seconds) and connects to the server. Reading the disc ID also
	 * checks that the path still points to the right game, like
	 * set_frag_list() does for local games.
	 */
	char discid[32] ATTRIBUTE_ALIGN(32);
	memset(discid, 0, sizeof(discid));
	ret = WDVD_UnencryptedRead(discid, 32, 0);
	gprintf("Reading ID from FTP: %.6s (expected: %.6s) ret = %d\n", discid, header->id, ret);
	if (ret < 0)
		return ret;

	return (strncasecmp((const char *)header->id, discid, 6) != 0) ? -3 : 0;
}
