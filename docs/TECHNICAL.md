# d2x-cios-ftp: technical notes

> [!CAUTION]
> Experimental and **never tested on real hardware**. See the warning in the [README](../README.md).

- [Architecture](#architecture)
- [The DI command `IOCTL_DI_NET_SET`](#the-di-command-ioctl_di_net_set)
- [FTP backend (cIOS side)](#ftp-backend-cios-side)
- [Memory layout](#memory-layout)
- [IOS reload](#ios-reload)
- [USB Loader GX integration](#usb-loader-gx-integration)
- [Usage](#usage)
- [Testing](#testing)
- [Limits and open questions](#limits-and-open-questions)
- [License notes](#license-notes)

## Architecture

A Wii game reads its disc through the IOS DI driver (`/dev/di`). The d2x **DIP plugin**
(`source/dip-plugin`) patches the DI command handler. In its emulation modes it answers the
read commands itself, from a WBFS partition or from a *frag list* (the sectors of an image file
on a USB/SD drive).

This fork adds **`MODE_NET`**: reads are served from an `.iso` on an FTP server.

```
PPC (game)                      ARM / Starlet (IOS, cIOS 249)
──────────                      ─────────────────────────────────────────────
DVD read ──IPC──► /dev/di ──► DI_EmulateCmd()            plugin.c
                                └─► __DI_ReadUnencrypted()
                                     └─► Net_Read()          net.c
                                          └─► FTP client
                                               └─► /dev/net/ip/top   (IOS network stack)
                                                    └─► Wii LAN adapter / Wi-Fi ──► FTP server
```

The network stack is the one built into IOS, configured from the Wii System Settings, so the
cIOS needs no network driver of its own.

## The DI command `IOCTL_DI_NET_SET`

New custom DI command `0xF8`, modeled on `IOCTL_DI_FRAG_SET` (`0xF9`):

| Word | Value |
|---|---|
| `inbuf[0]` | `0xF8 << 24` |
| `inbuf[1]` | pointer to a `struct netConfig` (PPC virtual address) |
| `inbuf[2]` | `sizeof(struct netConfig)` = **264** (any other size disables the mode) |
| `outbuf[0]` | plugin result, `0` = ok |

The IOS ioctl itself returns `1` when the command ran. A NULL pointer or a wrong size turns the
mode off. Like the other d2x custom commands, it is hidden from non-homebrew titles
(`Stealth_CheckRunningTitle`).

```c
struct netConfig {          /* big endian, no padding, 264 bytes */
    u32  ip;                /* IPv4, 192.168.1.10 = 0xC0A8010A */
    u16  port;              /* control port, 0 = 21 */
    u16  flags;             /* reserved, 0 */
    char user[32];          /* "" = anonymous */
    char pass[32];
    char path[192];         /* absolute path of the .iso */
};
```

Setting `MODE_NET` closes frag mode and the other way round: the two modes share a buffer. The
mode is part of `MODE_EMUL`, so cover, motor and status commands are emulated as in frag mode.

## FTP backend (cIOS side)

File: [`source/dip-plugin/net.c`](../source/dip-plugin/net.c), extensively commented.

- **Lazy start.** Nothing is contacted when the mode is set. The first disc read brings up the
  network stack (NWC24 startup, SO startup, then waits up to 20 s for an IP from DHCP), connects
  and logs in (`USER`/`PASS`, `TYPE I`), and reads the image size with `SIZE`.
- **One open transfer.** A read at the current stream position keeps receiving. A read up to
  256 KB ahead receives and discards the gap. Any other read closes the data connection (which
  aborts the transfer; the server answers 426/226) and starts again with
  `PASV` + `REST <offset>` + `RETR <path>`.
- **Errors.**
  - Every socket wait has a 5 s timeout.
  - A broken connection resets the session, and the read is retried up to 3 times. This also
    covers games that restart the network stack.
  - "Clean" failures fail the read without retrying: a read past the end of the image (this is
    how d2x tells single-layer from dual-layer discs) or a 5xx reply such as file not found.
  - `Net_Read()` returns `0` or a negative value, like `Frag_Read()`.
- **NAT friendly.** The address in the `PASV` reply is ignored: data connections always go to
  the configured server IP.
- **No 64-bit division.** `__aeabi_uldivmod` would pull in the libgcc unwinder, which doesn't
  link. The `REST` offset is printed in base 10⁹ with 32-bit math instead.
- **Cache handling.** Ioctl inputs are flushed before each call, and outputs are invalidated
  *before* the call, not after (the reason is in the comment on `__So_Ioctl`). The data is
  received into a plugin buffer, then copied to the game buffer, which is flushed for the PPC.

## Memory layout

| Region | Upstream | d2x-cios-ftp |
|---|---|---|
| DIPP code (`exe`) | `0x13800000`, 0x2000 bytes | `0x13800000`, **0x3000** bytes (0x24F0 used) |
| DIPP data (`ram`) | `0x13802000`, 0x40000 bytes | `0x13803000`, **0x3F000** bytes (~0x3C900 used) |
| End of DIPP memory | `0x13842000` | `0x13842000` (unchanged) |

The new code doesn't fit in the upstream 8 KB code region, so the region was grown. The end of
the plugin memory is unchanged, so no other module moves. The 32 KB receive buffer reuses the
240 KB frag list storage (the modes are mutually exclusive), which keeps RAM use almost
unchanged (`net.c` adds about 0.9 KB of variables).

## IOS reload

When a game boots it reloads its IOS. With d2x "IOS reload block", the cIOS is reloaded instead,
and the DIP state is saved in `/sys/dip.cfg` and restored afterwards. In net mode the file holds
`struct dipConfigState` (with `mode = MODE_NET` and `frag_size = 264`) followed by
`struct netConfig` (`config.c`). After the reload, `Net_Init()` restores the configuration, and
the connection is opened again on the first read.

## USB Loader GX integration

Patch: [`loader/usbloadergx/`](../loader/usbloadergx/), against
[wiidev/usbloadergx](https://github.com/wiidev/usbloadergx) commit `e25c4f35`.

- **Settings** (*Settings → Loader Settings*, after *Autoboot Discs Delay*): **FTP Games** on/off,
  **FTP Server** (`ip` or `ip:port`), **FTP Path** (default `/wbfs`). `FtpUser`/`FtpPass` exist
  only in `GXGlobal.cfg`, because anonymous login is the default.
- **Game list.**
  - `GameList::ReadGameList()` scans the server after the local partitions.
  - It finds `*.iso` files in the folder and one level below
    (`/wbfs/Name [ID6]/ID6.iso`) using `CWD` + `NLST`, then reads the first 0x60 bytes of each
    (game ID, magic, title). Non-Wii images are skipped.
  - The games get partition number `FTP_PARTITION` (1000). The `WBFS_*` functions reject that
    number, so nothing touches a drive for these games.
  - A game that is also on a local drive is skipped: the local copy wins.
- **Boot.** `GameBooter::SetupDisc()` calls `FtpGames_SetDisc()` (`IOCTL_DI_NET_SET`, then reads
  the disc ID to check the game) instead of building a frag list. Everything else (patches,
  IOS reload block, cheats) is unchanged.

## Usage

1. **Server.** Put the `.iso` files on an FTP server reachable from the Wii, e.g.
   `/wbfs/Mario Kart Wii [RMCP01]/RMCP01.iso`. Anonymous, read-only access is recommended. The
   server must support `PASV`, `REST` and `RETR`; `SIZE` is recommended. Every common server does.
2. **Wii network.** Set up and test the connection in *Wii Settings → Internet*. With the LAN
   adapter, use **USB port 1**: the d2x EHCI module takes port 0 for high-speed devices, so the
   Nintendo network driver wouldn't see the adapter there. Keep the HDD on port 0.
3. **cIOS.** Build the modules (see the [README](../README.md#quick-start-for-developers)) and
   install them like any d2x beta, with ModMii or the d2x cIOS Installer (`build/ciosmaps.xml`
   plus `build/d2x-v11-ftp/*.app`), slot 249, base 56 or 57.
4. **Loader.** Build USB Loader GX with the patch, set the FTP options, and set the game IOS to
   the d2x-cios-ftp slot.

Other loaders can use [`tools/loader/netdisc.c`](../tools/loader/netdisc.c):
`WDVD_SetNetMode()` replaces `WDVD_SetFragList()`.

## Testing

- **cIOS FTP backend:** [`tools/net-test`](../tools/net-test). The FTP client of `net.c` is
  compiled unchanged on a PC, with POSIX sockets in place of the IOS ones, and read through
  `Net_Read()` against a local FTP server. `run-tests.sh` covers:
  - sequential reads, small skips and random seeks on a 64 MB image;
  - offsets above 4 GB on a 5 GB sparse image;
  - fault injection (random recv failures, followed by reconnects);
  - a missing file, which must fail cleanly;
  - reads past the end of the image.

  All reads are compared byte by byte with the local file. It passes with pyftpdlib (user and
  password) and vsftpd (anonymous).
- **USB Loader GX:** the patched loader builds with devkitPPC r47. `FtpGames.cpp` was also run
  on a PC with socket stubs against pyftpdlib and vsftpd: it listed fake Wii images in the root
  and in subfolders, skipped GameCube images and other files, and built the `netConfig`
  correctly.
- **Not tested:** everything on a real Wii.

## Limits and open questions

- **To verify on hardware:**
  - that the Nintendo NET module accepts ioctls from the DI thread (the plugin already calls FS
    and EHCI this way);
  - the ARM cache handling of the socket buffers;
  - that NWC24/SO startup works so early after the IOS reload.
- **Speed:** the LAN adapter runs at USB 1.1 speed, about 0.6–1 MB/s.
- **Online games:** if a game restarts the network stack, the plugin reconnects, but the game may
  stall for a few seconds.
- **Formats:** uncompressed Wii `.iso` only. Paths are limited to 191 characters, IPv4 only, and
  there is no DNS (IP addresses only).
- **Security:** FTP is clear text, so use it only on a trusted LAN.
- **USB Loader GX:**
  - it still needs a USB or SD device for its own data;
  - the scan blocks for up to 5 s per server reply when the server is unreachable;
  - with the title cache on, refresh the list after adding games to the server.

## License notes

*Not legal advice. This is a summary of the license headers in the source.*

- **The repository** is released under the [GNU GPL v3](../LICENSE). The new files are "GPL v3
  or later": `net.c`/`net.h`, `tools/loader/*`, `tools/net-test/*` and the USB Loader GX
  `FtpGames.*`.
- **The DIP plugin**, the only cIOS module changed, is GPL v3 upstream
  (`source/dip-plugin/LICENSE.txt`). It is linked with cios-lib objects that are GPL v2 *or
  later* (`ios`, `isfs`, `libc`, `stealth`, `swi_mload`, `syscalls`, `tools`, `usbstorage`),
  which are compatible with the GPL v3.
- **Upstream files that are GPL v2 *only*:** `cios-lib/libwbfs/libwbfs.c`,
  `cios-lib/libwbfs/wiidisc.c` and `cios-lib/hollywood.h`. They are linked only into the EHCI,
  USBS and MLOAD modules (GPL v2 modules, unchanged by this fork), never into the DIP plugin.
  Each IOS module is a separate program, so the GPL v3 code here is not combined with GPL v2-only
  code. These files are distributed unchanged, under their own license.
- **Other upstream files** have permissive licenses: FatFs (ChaN, BSD-style), `sdio.c` (BSD-style)
  and `usbstorage.c` (zlib-style). Files without a header fall under the repository license, as
  upstream.
- **USB Loader GX files** changed by the patch keep their own headers. Several are zlib-style (by
  Dimok), which allows modified versions as long as they are marked as modified. The patch marks
  every change with `d2x-cios-ftp` comments.
