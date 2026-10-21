/*
 * Host (PC) test harness for source/dip-plugin/net.c.
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
 * HOW IT WORKS
 *
 * net.c has two parts: an IOS socket layer (the __So_* / __Net_* static
 * functions, compiled only when NET_HOST_TEST is NOT defined) and the FTP
 * client on top of it. This file defines NET_HOST_TEST, provides POSIX
 * implementations of the socket layer with the same names and signatures,
 * and then #includes net.c, so the FTP client is compiled UNCHANGED and
 * tested against a real FTP server running on the same PC (127.0.0.1).
 *
 * The test reads the image through Net_Read() with a pattern similar to a
 * game (mostly sequential reads, some small forward skips, some random
 * seeks), compares every read with the local copy of the same file, and
 * finally checks that a read past the end of the image fails, as needed
 * by the dual layer detection of the DIP plugin.
 *
 * To look like IOS, recv() randomly returns short chunks; with the
 * FAULT_RATE=N environment variable, 1 recv call out of N fails, to test
 * the reconnect/retry logic.
 *
 * Usage:
 *   net-test <port> <ftp path> <local copy of the file> <reads> [user pass]
 * Exit status 0 = all reads correct. See run-tests.sh for a full run.
 *
 * Limits: this tests the FTP logic only. The IOS ioctl layer, the cache
 * handling and the integration with the DI module can only be tested on
 * a real Wii.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "types.h"	// stub types.h of this folder (u8, u32, ATTRIBUTE_ALIGN...)

/* On the Wii, NET_BUF aliases the fraglist storage; here a plain buffer */
static u8 host_buf[32 * 1024];
#define NET_BUF host_buf

/* Number of sockets created: shows how often the transfer was restarted */
static int sockets_opened;

/* 1 recv call out of fault_rate fails, 0 = no fault injection */
static int fault_rate;

/* The network is always "up" on the PC */
static s32 __Net_Up(void) { return 0; }
static void __Net_Down(void) { }

static s32 __So_Socket(void)
{
	sockets_opened++;
	return socket(AF_INET, SOCK_STREAM, 0);
}

static void __So_Close(s32 s)
{
	close(s);
}

/* "ip" is a host order u32, like the native big endian value on the Wii */
static s32 __So_Connect(s32 s, u32 ip, u16 port)
{
	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(ip);
	return connect(s, (struct sockaddr *)&a, sizeof(a));
}

/* Same contract as the IOS version: revents (> 0), 0 on timeout, < 0 on error */
static s32 __So_Poll(s32 s, u32 ms)
{
	struct pollfd p = { s, POLLIN, 0 };
	int r = poll(&p, 1, ms);
	return r > 0 ? p.revents : r;
}

static s32 __So_Send(s32 s, const void *b, u32 l)
{
	return send(s, b, l, 0);
}

static s32 __So_Recv(s32 s, void *b, u32 l)
{
	/* Emulate the connection being reset (e.g. by the game) */
	if (fault_rate && rand() % fault_rate == 0)
		return -1;

	/* Emulate IOS returning short chunks */
	if (l > 4000 && (rand() & 1))
		l = 1 + rand() % 4000;

	return recv(s, b, l, 0);
}

/* Compile the FTP client of the plugin, unchanged, on top of the above */
#define NET_HOST_TEST
#include "../../source/dip-plugin/net.c"

int main(int argc, char **argv)
{
	struct netConfig cfg = { 0 };
	static u8 a[1 << 20], b[1 << 20];	// Net_Read() result, expected data
	int i, n, bad = 0;
	u64 size, off = 0;
	s32 eof;
	FILE *f;

	if (argc != 5 && argc != 7) {
		printf("usage: %s <port> <ftp path> <local file> <reads> [user pass]\n", argv[0]);
		return 2;
	}

	f = fopen(argv[3], "rb");
	if (!f) {
		printf("can't open %s\n", argv[3]);
		return 2;
	}

	fseeko(f, 0, SEEK_END);
	size = ftello(f);
	n = atoi(argv[4]);

	/* Same config that a loader sends with IOCTL_DI_NET_SET */
	cfg.ip = 0x7F000001;	// 127.0.0.1
	cfg.port = atoi(argv[1]);
	if (argc == 7) {
		snprintf(cfg.user, sizeof(cfg.user), "%s", argv[5]);
		snprintf(cfg.pass, sizeof(cfg.pass), "%s", argv[6]);
	}
	snprintf(cfg.path, sizeof(cfg.path), "%s", argv[2]);

	if (Net_Init(&cfg)) {
		printf("init failed\n");
		return 1;
	}

	if (getenv("FAULT_RATE"))
		fault_rate = atoi(getenv("FAULT_RATE"));

	/* Fixed seed: runs are reproducible */
	srand(1234);

	for (i = 0; i < n; i++) {
		/* 2 KB .. 128 KB, multiples of the DVD sector size */
		u32 len = (1 + rand() % 64) * 2048;
		int r = rand() % 10;
		s32 ret;

		if (r < 6)		/* 60%: sequential */
			;
		else if (r < 8)		/* 20%: small forward skip (< 256 KB) */
			off += (rand() % 64) * 4096;
		else			/* 20%: random seek anywhere */
			off = ((u64)rand() * 2048) % size;

		/* DI offsets are in 32 bit words: keep them 4 byte aligned */
		off &= ~3ULL;
		if (off + len > size)
			off = size - len - ((size - len) & 3);

		ret = Net_Read(a, len, (u32)(off >> 2));

		fseeko(f, off, SEEK_SET);
		if (fread(b, 1, len, f) != len) {
			printf("local read failed at %llu\n", (unsigned long long)off);
			return 2;
		}

		if (ret || memcmp(a, b, len)) {
			printf("MISMATCH read %d off=%llu len=%u ret=%d\n", i, (unsigned long long)off, len, ret);
			bad++;
		}
		off += len;
	}

	/* A read past the end must fail (dual layer detection) */
	eof = Net_Read(a, 2048, (u32)((size + 4096) >> 2));
	printf("beyond EOF: ret=%d (%s)\n", eof, eof < 0 ? "ok" : "WRONG, must be < 0");
	if (eof >= 0)
		bad++;

	printf("reads=%d bad=%d sockets=%d\n", n, bad, sockets_opened);
	Net_Close();
	fclose(f);

	return bad != 0;
}
