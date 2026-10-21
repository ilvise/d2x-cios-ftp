/*
 * DIP plugin for Custom IOS.
 *
 * Network (FTP) disc image backend ("MODE_NET").
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
 * OVERVIEW
 * ========
 *
 * The d2x DIP plugin intercepts the commands that the game sends to the
 * DVD drive (/dev/di) and, in emulation modes, serves the disc reads from
 * another source. Upstream d2x supports WBFS partitions and "frag lists"
 * (lists of sectors of an .iso/.wbfs file on a USB/SD FAT/NTFS/ext drive).
 * This file adds a third source: an uncompressed .iso image stored on an
 * FTP server, read through the network stack of IOS itself.
 *
 * Where this code runs
 * --------------------
 * It runs on the Starlet (ARM926, big endian) inside the DI module of
 * IOS, in the DI thread, because the DIP plugin patches the DI command
 * handler (see patches.c / plugin.c). Every disc read requested by the
 * game ends up in Net_Read() and blocks the DI thread until the data is
 * there. Consequences:
 *   - no libc, no malloc: all the state is static;
 *   - code space is tiny: the plugin "exe" region is 0x3000 bytes (it was
 *     0x2000 upstream, see link.ld); this file is about 2.2 KB of Thumb
 *     code, so every feature is kept to the minimum;
 *   - all the network I/O is synchronous: os_ioctl()/os_ioctlv() on
 *     /dev/net/ip/top, i.e. the same IPC interface used by libogc on the
 *     PPC side (see libogc/network_wii.c, from which the ioctl numbers and
 *     the parameter layouts below are taken).
 *
 * Which network
 * -------------
 * The IOS network stack uses the connection configured in the Wii System
 * Settings: Wii LAN adapter (USB Ethernet) or internal Wi-Fi. Nothing here
 * is specific to one of them. With the LAN adapter, see the README about
 * the USB port to use (the d2x EHCI module claims USB port 0).
 *
 * Why FTP (and not SMB, NFS, HTTP...)
 * ------------------------------------
 * - SMB2/3 needs NTLMSSP authentication (MD4, HMAC-MD5), dialect
 *   negotiation, credits, etc.: tens of KB of code, it doesn't fit.
 * - FTP is a line based text protocol, supported by every NAS and PC
 *   (FileZilla Server, vsftpd, pure-ftpd, ProFTPD, pyftpdlib...), and
 *   "REST <offset>" + "RETR <file>" give random access to a file.
 * - HTTP Range requests would be even simpler, but the user asked for
 *   SMB or FTP, and FTP servers are more common on home NAS boxes.
 *
 * Read strategy
 * -------------
 * Opening an FTP data connection costs several round trips (PASV,
 * connect, REST, RETR), so the plugin keeps ONE transfer open and serves
 * reads from it while they are sequential:
 *   - read at the current stream position  -> just keep receiving;
 *   - read up to NET_SKIP_MAX bytes ahead   -> receive and discard the gap;
 *   - any other read (backward / far seek)  -> close the data connection
 *     (this aborts the transfer on the server, which answers 426 or 226),
 *     then PASV + REST <offset> + RETR <path> again.
 * Games read mostly sequentially (files are contiguous on the disc), so
 * most reads reuse the open stream. TCP flow control pauses the server
 * while the game is not reading.
 *
 * Error handling
 * --------------
 * Every socket wait has a timeout (NET_TIMEOUT). Errors are split in two
 * classes:
 *   - NET_EOF: a "clean" failure (read past the end of the image, file
 *     not found): the connection is still fine, the read just fails;
 *   - NET_ERR: the connection is broken or the server misbehaves: all the
 *     sockets are closed and the read is retried from scratch, up to
 *     NET_RETRIES times. This also covers games that restart the IOS
 *     network stack (e.g. online games), which kills our sockets.
 *
 * Testing
 * -------
 * The FTP logic (everything outside the NET_HOST_TEST block) is compiled
 * unchanged on a PC by tools/net-test/host_shim.c, which replaces the
 * IOS socket layer with POSIX sockets and checks random/sequential reads
 * against a local copy of the file, with optional fault injection.
 */

#include <string.h>

#include "net.h"
#include "syscalls.h"

/* Constants */
#define NET_TIMEOUT		5000		// ms, max wait for data on any socket
#define NET_RETRIES		3		// attempts per read on connection errors
#define NET_SKIP_MAX		(256 * 1024)	// forward seek served by skipping data
#define NET_BUF_SIZE		(32 * 1024)	// receive buffer (max bytes per recv)

/* Internal read results */
#define NET_OK			0
#define NET_EOF			1		// clean failure (out of range, file error)
#define NET_ERR			-1		// connection broken, reset and retry

/* Values returned by Net_Read() on failure (always negative, like Frag_Read) */
#define NET_READ_ERR		-1		// network/server error after all retries
#define NET_READ_EOF		-2		// out of range or file error

#ifndef NET_HOST_TEST
/*
 * ===================================================================
 * IOS socket layer
 *
 * Thin wrappers around the /dev/net/ip/top ioctls. On the PC test build
 * (NET_HOST_TEST) these functions are provided by host_shim.c instead.
 * ===================================================================
 */
#include "frag.h"

/* IOS network ioctls (same numbering as libogc/network_wii.c) */
#define IOCTL_NWC24_STARTUP	0x06		// on /dev/net/kd/request
#define IOCTL_SO_CLOSE		0x03
#define IOCTL_SO_CONNECT	0x04
#define IOCTL_SO_POLL		0x0B
#define IOCTLV_SO_RECVFROM	0x0C
#define IOCTLV_SO_SENDTO	0x0D
#define IOCTL_SO_SOCKET		0x0F
#define IOCTL_SO_GETHOSTID	0x10
#define IOCTL_SO_STARTUP	0x1F

#define NWC24_EBUSY		-29		// NWC24 startup still in progress
#define SO_POLLIN		0x0003		// POLLRDNORM | POLLRDBAND

/*
 * Receive buffer.
 *
 * MODE_NET and MODE_FRAG are mutually exclusive (setting one closes the
 * other, see plugin.c), so the 240 KB fraglist storage (frag.c) is unused
 * in network mode and its first NET_BUF_SIZE bytes are reused here. This
 * keeps the RAM footprint of the plugin unchanged.
 *
 * Data is received here and then copied to the game buffer, instead of
 * receiving directly into the game buffer, because:
 *   - it's not known whether the NET module may write to PPC memory
 *     (MEM1/MEM2) at arbitrary, unaligned addresses;
 *   - this buffer is 32 byte aligned, as IOS IPC buffers usually are.
 * The copy costs little compared to the network speed.
 */
#define NET_BUF			((u8 *)fraglist_data.frag)

/* Device names, aligned because they are passed to IOS by pointer */
static char kdFs[]  ATTRIBUTE_ALIGN(32) = "/dev/net/kd/request";
static char topFs[] ATTRIBUTE_ALIGN(32) = "/dev/net/ip/top";

/* /dev/net/ip/top descriptor, -1 when the stack is not started */
static s32    topFd = -1;

/*
 * Shared ioctl parameter block and vectors. The DI thread is the only
 * user of this code, so a single static buffer is safe and saves stack.
 * 10 words = 40 bytes = the biggest parameter block (sendto).
 */
static u32    soParams[10] ATTRIBUTE_ALIGN(32);
static ioctlv soVector[3]  ATTRIBUTE_ALIGN(32);

/* Message queue storage used by __Net_Sleep() */
static u32    soQueue[4]   ATTRIBUTE_ALIGN(32);


/*
 * Sleep for "ms" milliseconds.
 *
 * cios-lib has Timer_Sleep(), but it needs Timer_Init(), which allocates
 * from the cios-lib heap, not set up in the DIP plugin. Here a one-shot
 * IOS timer posts a message to a temporary queue and we wait for it.
 */
static void __Net_Sleep(u32 ms)
{
	u32 msg;
	s32 qid, tid;

	qid = os_message_queue_create(soQueue, 4);
	if (qid < 0)
		return;

	/* One shot timer (repeat = 0) that sends message 1 to the queue */
	tid = os_create_timer(ms * 1000, 0, qid, 1);
	if (tid >= 0) {
		os_message_queue_receive(qid, &msg, 0);
		os_destroy_timer(tid);
	}

	os_message_queue_destroy(qid);
}

/*
 * Issue an ioctl on /dev/net/ip/top with soParams as input.
 *
 * Cache handling: the input block is flushed (clean) so the NET module
 * sees it even if it reads RAM directly; the output buffer is invalidated
 * BEFORE the call, not after. If the NET module writes the result through
 * the (shared) ARM data cache we read it from the cache; if it writes RAM
 * we fetch it from RAM because the lines were invalidated. Invalidating
 * after the call could instead discard results still dirty in the cache.
 */
static s32 __So_Ioctl(u32 cmd, u32 inlen, void *out, u32 outlen)
{
	/* Flush params, invalidate output */
	os_sync_after_write(soParams, sizeof(soParams));
	if (out)
		os_sync_before_read(out, outlen);

	return os_ioctl(topFd, cmd, soParams, inlen, out, outlen);
}

/* socket(AF_INET, SOCK_STREAM, 0), returns the socket or < 0 */
static s32 __So_Socket(void)
{
	soParams[0] = 2;	// AF_INET
	soParams[1] = 1;	// SOCK_STREAM
	soParams[2] = 0;	// protocol: default (TCP)

	return __So_Ioctl(IOCTL_SO_SOCKET, 12, NULL, 0);
}

/* close(s), errors are ignored (nothing useful can be done about them) */
static void __So_Close(s32 s)
{
	soParams[0] = s;
	__So_Ioctl(IOCTL_SO_CLOSE, 4, NULL, 0);
}

/*
 * connect(s, ip:port). "ip" is the IPv4 address as a native (big endian)
 * u32, e.g. 192.168.1.10 = 0xC0A8010A, which is also network byte order.
 */
static s32 __So_Connect(s32 s, u32 ip, u16 port)
{
	memset(soParams, 0, sizeof(soParams));

	/*
	 * connect_params { u32 socket; u32 has_addr; u8 addr[28]; } where
	 * addr is the Wii sockaddr_in { u8 len; u8 family; u16 port; u32 addr; }
	 * (no sin_zero). Packed by hand: len = 8, family = AF_INET (2).
	 */
	soParams[0] = s;
	soParams[1] = 1;
	soParams[2] = (8 << 24) | (2 << 16) | port;
	soParams[3] = ip;

	return __So_Ioctl(IOCTL_SO_CONNECT, 36, NULL, 0);
}

/*
 * Wait until socket "s" is readable (data, EOF or error), up to "ms".
 * Returns the poll revents (> 0) if something happened, 0 on timeout,
 * < 0 on error.
 */
static s32 __So_Poll(s32 s, u32 ms)
{
	s32 ret;

	/* Input: timeout as u64 milliseconds (high word first, big endian) */
	soParams[0] = 0;
	soParams[1] = ms;

	/* Output: one pollsd { s32 socket; u32 events; u32 revents; } */
	soParams[4] = s;
	soParams[5] = SO_POLLIN;
	soParams[6] = 0;

	ret = __So_Ioctl(IOCTL_SO_POLL, 8, &soParams[4], 12);

	return (ret > 0) ? soParams[6] : ret;
}

/* send(s, buf, len), returns the number of bytes sent or < 0 */
static s32 __So_Send(s32 s, const void *buf, u32 len)
{
	/* sendto params { socket, flags, has_destaddr, destaddr[28] } = 40 bytes */
	memset(soParams, 0, sizeof(soParams));
	soParams[0] = s;

	/* Two input vectors: the data, then the parameters */
	soVector[0].data = (void *)buf;
	soVector[0].len  = len;
	soVector[1].data = soParams;
	soVector[1].len  = 40;

	os_sync_after_write((void *)buf, len);
	os_sync_after_write(soParams, sizeof(soParams));
	os_sync_after_write(soVector, sizeof(soVector));

	return os_ioctlv(topFd, IOCTLV_SO_SENDTO, 2, 0, soVector);
}

/*
 * recv(s, buf, len), returns the number of bytes received (it may be less
 * than len), 0 when the peer closed the connection, < 0 on error.
 */
static s32 __So_Recv(s32 s, void *buf, u32 len)
{
	/* recvfrom params { socket, flags } */
	soParams[0] = s;
	soParams[1] = 0;

	/* One input vector (params), two output vectors (data, source address) */
	soVector[0].data = soParams;
	soVector[0].len  = 8;
	soVector[1].data = buf;
	soVector[1].len  = len;
	soVector[2].data = NULL;	// source address not needed
	soVector[2].len  = 0;

	os_sync_after_write(soParams, sizeof(soParams));
	os_sync_after_write(soVector, sizeof(soVector));
	os_sync_before_read(buf, len);

	return os_ioctlv(topFd, IOCTLV_SO_RECVFROM, 1, 2, soVector);
}

/*
 * Bring up the IOS network stack using the Wii network settings.
 *
 * Same sequence as libogc net_init(): NWC24 startup on /dev/net/kd/request
 * (it loads the network configuration and brings the interface up; it
 * returns NWC24_EBUSY while still working), then SO startup on
 * /dev/net/ip/top, then wait until DHCP (or the static configuration)
 * gives us an IP address.
 *
 * This is called lazily on the first read, not when the mode is set:
 * after an IOS reload the DIP state is restored very early (when the DI
 * driver receives its first request), the NET module may not be ready
 * yet and DHCP can take seconds. The first disc read of the game waits
 * for it instead, as it would wait for a real drive to spin up.
 *
 * If another client (the loader, the game) already started the stack,
 * the startup calls just succeed or fail harmlessly and GETHOSTID returns
 * the current address immediately.
 */
static s32 __Net_Up(void)
{
	s32 fd, i;
	u32 ip = 0;

	if (topFd >= 0)
		return 0;

	/* NWC24 startup, retried for up to ~5 seconds while busy */
	fd = os_open(kdFs, 0);
	if (fd >= 0) {
		for (i = 0; i < 50; i++) {
			soParams[0] = 0;
			__So_Ioctl(IOCTL_NWC24_STARTUP, 0, soParams, 0x20);
			if ((s32)soParams[0] != NWC24_EBUSY)
				break;
			__Net_Sleep(100);
		}
		os_close(fd);
	}

	/* Socket startup (its result is ignored: "already started" is fine) */
	topFd = os_open(topFs, 0);
	if (topFd < 0)
		return topFd;

	os_ioctl(topFd, IOCTL_SO_STARTUP, NULL, 0, NULL, 0);

	/* Wait for an IP address (DHCP), up to 20 seconds */
	for (i = 0; i < 200; i++) {
		ip = os_ioctl(topFd, IOCTL_SO_GETHOSTID, NULL, 0, NULL, 0);
		if (ip)
			break;
		__Net_Sleep(100);
	}

	if (!ip) {
		os_close(topFd);
		topFd = -1;
		return NET_ERR;
	}

	return 0;
}

/* Forget the stack, so that the next __Net_Up() starts it again */
static void __Net_Down(void)
{
	if (topFd >= 0)
		os_close(topFd);
	topFd = -1;
}
#endif /* NET_HOST_TEST */

/*
 * ===================================================================
 * FTP client
 *
 * Protocol reminders (RFC 959 / RFC 3659):
 *   - commands are "VERB [argument]\r\n" on the control connection;
 *   - every reply starts with a 3 digit code; multi-line replies are
 *     "ddd-text" ... "ddd text", only the last line has a space after the
 *     code; 1xx = preliminary (more replies follow), 2xx = done,
 *     3xx = send the next command, 4xx/5xx = errors;
 *   - PASV -> "227 ... (h1,h2,h3,h4,p1,p2)": connect to port p1*256+p2;
 *   - REST <n> -> 350, the next RETR starts at byte n;
 *   - RETR -> 150/125, data flows on the data connection, then 226 when
 *     complete, or 426/451 when the client closed the data connection.
 * ===================================================================
 */

/* Network config (set by IOCTL_DI_NET_SET, saved in /sys/dip.cfg) */
struct netConfig netcfg_data = { 0 };

static s32  ctrlSock = -1;		// control connection, -1 = not connected
static s32  dataSock = -1;		// data connection of the open RETR, -1 = none
static u64  dataPos  = 0;		// image offset of the next byte on dataSock
static u64  fileSize = 0;		// image size from SIZE, 0 = unknown
static u32  rlen     = 0;		// bytes pending in rbuf
static char rbuf[256]    ATTRIBUTE_ALIGN(32);	// control connection receive buffer
static char cmdbuf[256]  ATTRIBUTE_ALIGN(32);	// command, then last reply line


/*
 * Read the next complete reply from the control connection and return
 * its code (100..599), or NET_ERR on timeout / connection error.
 *
 * Lines are accumulated in rbuf; data received after the reply stays in
 * rbuf for the next call. The final line of the reply is copied to cmdbuf
 * (NUL terminated, with the trailing '\r'), so callers can parse it (PASV
 * address, SIZE value). cmdbuf can be reused because the command that
 * produced the reply has already been sent.
 *
 * Only lines starting with "ddd " end a reply; the other lines (multi-line
 * replies, "ddd-" continuations) are skipped. A line longer than rbuf is
 * dropped.
 */
static s32 __Ftp_Reply(void)
{
	u32 i, j;
	s32 n;

	while (1) {
		/* Look for complete lines */
		for (i = 0; i < rlen; i++) {
			if (rbuf[i] != '\n')
				continue;

			/* Final reply line is "ddd text" */
			n = -1;
			if (i >= 3 && rbuf[3] == ' ' &&
			    rbuf[0] >= '1' && rbuf[0] <= '5')
				n = (rbuf[0] - '0') * 100 + (rbuf[1] - '0') * 10 + (rbuf[2] - '0');

			/* Save line (i < sizeof(rbuf) == sizeof(cmdbuf)) */
			memcpy(cmdbuf, rbuf, i);
			cmdbuf[i] = 0;

			/* Consume line (libc of cios-lib has no memmove) */
			i++;
			rlen -= i;
			for (j = 0; j < rlen; j++)
				rbuf[j] = rbuf[j + i];

			if (n > 0)
				return n;

			/* Restart the scan from the beginning (i++ makes it 0) */
			i = -1;
		}

		/* Line too long, drop it */
		if (rlen >= sizeof(rbuf))
			rlen = 0;

		/* Receive more */
		if (__So_Poll(ctrlSock, NET_TIMEOUT) <= 0)
			return NET_ERR;

		n = __So_Recv(ctrlSock, rbuf + rlen, sizeof(rbuf) - rlen);
		if (n <= 0)
			return NET_ERR;

		rlen += n;
	}
}

/*
 * Send "cmd[ arg]\r\n" and return the code of the first reply.
 *
 * The longest command is "RETR " + path: the path is limited to 191
 * characters by struct netConfig, so it always fits in cmdbuf.
 */
static s32 __Ftp_Cmd(const char *cmd, const char *arg)
{
	u32 len;

	/* Build command line */
	strcpy(cmdbuf, cmd);
	if (arg) {
		strcat(cmdbuf, " ");
		strcat(cmdbuf, arg);
	}
	strcat(cmdbuf, "\r\n");

	len = strlen(cmdbuf);
	if (__So_Send(ctrlSock, cmdbuf, len) != len)
		return NET_ERR;

	return __Ftp_Reply();
}

/* Drop the FTP session (both connections), keep the network stack up */
static void __Ftp_Reset(void)
{
	if (dataSock >= 0)
		__So_Close(dataSock);
	if (ctrlSock >= 0)
		__So_Close(ctrlSock);

	dataSock = -1;
	ctrlSock = -1;
	rlen     = 0;
}

/*
 * Open the FTP session: network up, connect, login, binary mode, and
 * read the image size.
 *
 * An empty user name means anonymous login ("anonymous" / "wii@").
 * Servers that log in without a password answer 230 to USER directly.
 *
 * SIZE is optional (RFC 3659): when the server supports it, reads past
 * the end of the image fail immediately in Net_Read() without talking to
 * the server. That matters because the DIP plugin probes the image size
 * on every disc ID read (__DI_CheckDisc() reads at 4.7 GB to tell single
 * from dual layer discs).
 */
static s32 __Ftp_Connect(void)
{
	const char *user = netcfg_data.user[0] ? netcfg_data.user : "anonymous";
	const char *pass = netcfg_data.pass[0] ? netcfg_data.pass : "wii@";
	s32 ret;
	char *p;

	/* Network up */
	ret = __Net_Up();
	if (ret < 0)
		return ret;

	/* Control connection */
	ctrlSock = __So_Socket();
	if (ctrlSock < 0) {
		/* Stack gone (e.g. reset by the game), restart it next time */
		__Net_Down();
		return NET_ERR;
	}

	if (__So_Connect(ctrlSock, netcfg_data.ip, netcfg_data.port ? netcfg_data.port : 21) < 0)
		return NET_ERR;

	/* Welcome: skip "120 service ready in nnn minutes" style replies */
	do {
		ret = __Ftp_Reply();
	} while (ret > 0 && ret < 200);
	if (ret != 220)
		return NET_ERR;

	/* Login (202 = "command not needed") */
	ret = __Ftp_Cmd("USER", user);
	if (ret == 331)
		ret = __Ftp_Cmd("PASS", pass);
	if (ret != 230 && ret != 202)
		return NET_ERR;

	/* Binary mode, otherwise ASCII mode could alter the data */
	if (__Ftp_Cmd("TYPE", "I") != 200)
		return NET_ERR;

	/* File size (optional): "213 <decimal size>" */
	fileSize = 0;
	ret = __Ftp_Cmd("SIZE", netcfg_data.path);
	if (ret < 0)
		return NET_ERR;
	if (ret == 213)
		for (p = cmdbuf + 4; *p >= '0' && *p <= '9'; p++)
			fileSize = fileSize * 10 + (*p - '0');

	return 0;
}

/*
 * Stop the current transfer, if any.
 *
 * Closing the data connection is enough to abort a RETR on every common
 * server; the server then sends exactly one final reply (426/451 when the
 * transfer was aborted, 226 if it was already complete), which must be
 * consumed so that it is not taken as the reply to the next command.
 * ABOR is not used: it needs Telnet "urgent" signalling to be reliable
 * and adds a reply to track.
 */
static s32 __Ftp_StopData(void)
{
	s32 ret;

	if (dataSock < 0)
		return 0;

	/* Abort transfer by closing the data connection */
	__So_Close(dataSock);
	dataSock = -1;

	/* Transfer complete / aborted reply */
	ret = __Ftp_Reply();

	return (ret < 0) ? NET_ERR : 0;
}

/*
 * Start a transfer of the image from byte "offset":
 * PASV, connect the data connection, REST <offset> (if not 0), RETR <path>.
 *
 * Returns NET_OK, NET_EOF (permanent 5xx error, e.g. file not found) or
 * NET_ERR.
 */
static s32 __Ftp_StartData(u64 offset)
{
	char  num[24];
	char *p;
	u32   v[6], i;
	s32   ret;

	/* Passive mode */
	if (__Ftp_Cmd("PASV", NULL) != 227)
		return NET_ERR;

	/* Parse "(h1,h2,h3,h4,p1,p2)" */
	p = strchr(cmdbuf, '(');
	if (!p)
		return NET_ERR;

	for (i = 0; i < 6; i++) {
		v[i] = 0;
		for (p++; *p >= '0' && *p <= '9'; p++)
			v[i] = v[i] * 10 + (*p - '0');
	}

	/*
	 * Data connection. The address in the PASV reply (h1..h4) is ignored
	 * on purpose and the configured server address is used instead: a
	 * server behind NAT, or badly configured, often announces an address
	 * that is not reachable from the LAN.
	 */
	dataSock = __So_Socket();
	if (dataSock < 0)
		return NET_ERR;

	if (__So_Connect(dataSock, netcfg_data.ip, (v[4] << 8) | v[5]) < 0)
		return NET_ERR;

	/* Restart offset */
	if (offset) {
		u32 hi = 0, lo;

		/*
		 * Convert the 64 bit offset to decimal without a 64 bit division:
		 * __aeabi_uldivmod from libgcc pulls in the C++ unwinder, which
		 * does not link (no abort(), no exception tables) and would not
		 * fit anyway. Offsets are < 10 GB, so the value is split in
		 * hi * 10^9 + lo with a few subtractions, then the 9 low digits
		 * (zero padded) and the high digits are printed with 32 bit math.
		 */
		while (offset >= 1000000000ULL) {
			offset -= 1000000000ULL;
			hi++;
		}
		lo = offset;

		/* Digits are written backwards from the end of num[] */
		p = num + sizeof(num) - 1;
		*p = 0;
		for (i = 0; i < 9 || lo; i++) {
			*--p = '0' + (lo % 10);
			lo /= 10;
			if (i == 8 && hi) {
				/* Low 9 digits done, continue with the high part */
				lo = hi;
				hi = 0;
			}
		}

		/* Strip the padding zeros (offsets < 10^8), keep at least one digit */
		while (*p == '0' && p[1])
			p++;

		if (__Ftp_Cmd("REST", p) != 350)
			return NET_ERR;
	}

	/* Start transfer */
	ret = __Ftp_Cmd("RETR", netcfg_data.path);
	if (ret == 125 || ret == 150)
		return NET_OK;

	/* Permanent error (file not found...), retrying would not help */
	if (ret >= 500) {
		__So_Close(dataSock);
		dataSock = -1;
		return NET_EOF;
	}

	return NET_ERR;
}

/*
 * One read attempt: "len" bytes at byte "offset" of the image to "outbuf".
 * Connects if needed, reuses or restarts the transfer (see OVERVIEW).
 * Returns NET_OK, NET_EOF or NET_ERR.
 */
static s32 __Net_ReadData(u8 *outbuf, u32 len, u64 offset)
{
	u32 skip = 0;
	s32 ret, n;

	/* Connect */
	if (ctrlSock < 0) {
		ret = __Ftp_Connect();
		if (ret)
			return ret;
	}

	/* Reuse the current transfer if possible */
	if (dataSock >= 0 && offset >= dataPos && (offset - dataPos) <= NET_SKIP_MAX)
		skip = offset - dataPos;
	else {
		ret = __Ftp_StopData();
		if (ret)
			return ret;

		ret = __Ftp_StartData(offset);
		if (ret)
			return ret;

		dataPos = offset;
	}

	/* First discard "skip" bytes, then receive "len" bytes */
	while (skip || len) {
		u32 chunk = skip ? skip : len;

		if (chunk > NET_BUF_SIZE)
			chunk = NET_BUF_SIZE;

		/* Wait for data */
		if (__So_Poll(dataSock, NET_TIMEOUT) <= 0)
			return NET_ERR;

		n = __So_Recv(dataSock, NET_BUF, chunk);

		/* End of file before the requested data: read past the end */
		if (n == 0) {
			__So_Close(dataSock);
			dataSock = -1;
			return (__Ftp_Reply() < 0) ? NET_ERR : NET_EOF;
		}

		if (n < 0 || n > chunk)
			return NET_ERR;

		if (skip)
			skip -= n;
		else {
			memcpy(outbuf, NET_BUF, n);
			outbuf += n;
			len    -= n;
		}

		dataPos += n;
	}

	return NET_OK;
}

/*
 * Set the network config. Called by IOCTL_DI_NET_SET (cfg points to the
 * loader's buffer, already converted to a physical address) and after an
 * IOS reload (cfg == &netcfg_data, loaded from /sys/dip.cfg).
 *
 * No connection is made here: see __Net_Up() for why it is lazy. The
 * loader can check the config by reading the disc ID right after setting
 * the mode, which connects.
 *
 * Returns 0, or -1 if the config is not usable.
 */
s32 Net_Init(struct netConfig *cfg)
{
	/* Close previous session */
	Net_Close();

	/* Check config */
	if (!cfg->ip || !cfg->path[0])
		return -1;

	/* Copy config (invalidate first: the loader wrote it from the PPC) */
	if (cfg != &netcfg_data) {
		os_sync_before_read(cfg, sizeof(*cfg));
		memcpy(&netcfg_data, cfg, sizeof(netcfg_data));
	}

	/* Size is unknown until connected (the image may have changed) */
	fileSize = 0;

	/* Ensure strings are terminated, the loader may send garbage */
	netcfg_data.user[sizeof(netcfg_data.user) - 1] = 0;
	netcfg_data.pass[sizeof(netcfg_data.pass) - 1] = 0;
	netcfg_data.path[sizeof(netcfg_data.path) - 1] = 0;

	/* Connection is opened on first read */
	return 0;
}

/* Close the FTP session (the IOS network stack is left running) */
void Net_Close(void)
{
	__Ftp_Reset();
}

/*
 * Disc read entry point, called by __DI_ReadUnencrypted() in plugin.c.
 *
 * woffset is the disc offset in 32 bit words (the DI convention, it can
 * address 16 GB with a u32), len is in bytes.
 *
 * Returns 0 on success, or a negative value on failure (like Frag_Read),
 * so that every caller in plugin.c sees the error: __DI_ReadDiscId() for
 * example only checks "ret < 0".
 */
s32 Net_Read(void *outbuf, u32 len, u32 woffset)
{
	u64 offset = (u64)woffset << 2;
	s32 ret = NET_ERR, cnt;

	/* Out of range (used by the disc type check) */
	if (fileSize && offset + len > fileSize)
		return NET_READ_EOF;

	for (cnt = 0; cnt < NET_RETRIES; cnt++) {
		ret = __Net_ReadData(outbuf, len, offset);
		if (ret == NET_OK) {
			/* Flush the data to RAM so that the PPC sees it */
			os_sync_after_write(outbuf, len);
			return 0;
		}

		/* Clean failure: the session is fine, don't retry */
		if (ret == NET_EOF)
			break;

		/* Connection broken, reconnect and retry */
		__Ftp_Reset();
	}

	return (ret == NET_EOF) ? NET_READ_EOF : NET_READ_ERR;
}
