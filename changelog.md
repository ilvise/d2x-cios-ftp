# Changelog

All the changes of d2x-cios-ftp compared to the upstream d2x cIOS, with the decisions behind
them. The file is written so that a person or an AI agent can resume the work from here.

- **Upstream base:** [wiidev/d2x-cios](https://github.com/wiidev/d2x-cios), branch `master`, commit
  `33ad1ee` ("Fixed IOS38 version identification for Wii"), i.e. d2x v11.
- **USB Loader GX base:** [wiidev/usbloadergx](https://github.com/wiidev/usbloadergx), commit `e25c4f35`
  ("Increased version to 1283").

## [Unreleased] – first version (experimental)

### Status

- ✅ cIOS modules build (devkitARM r32), DIPP fits in memory (code 0x24F0 / 0x3000).
- ✅ The FTP backend passes the host tests (`tools/net-test/run-tests.sh`) with pyftpdlib and vsftpd.
- ✅ The patched USB Loader GX builds (devkitPPC r47); its FTP scan was tested on a PC with stubs.
- ❌ **Never run on a Wii.** Hardware testing is the next step.

### Added – cIOS (DIP plugin)

- **`MODE_NET` (0x20)**: disc reads are served from an `.iso` on an FTP server
  (`source/dip-plugin/net.c`, `net.h`). Part of `MODE_EMUL`.
- **`IOCTL_DI_NET_SET` (0xF8)**: new DI command to set the mode. `inbuf[1]` is a pointer to
  `struct netConfig` (264 bytes: ip, port, flags, user[32], pass[32], path[192]) and
  `inbuf[2]` is its size. The result goes to `outbuf[0]`. Modeled on `IOCTL_DI_FRAG_SET`.
- **IOS reload support**: `struct netConfig` is appended to `/sys/dip.cfg` (`config.c`) and restored
  in `__DI_InitDriveEmulation()` (`plugin.c`).
- **`tools/loader/netdisc.c/.h`**: `WDVD_SetNetMode()` helper for libogc loaders.
- **`tools/net-test/`**: host test harness (`host_shim.c` compiles `net.c` unchanged with POSIX
  sockets), `run-tests.sh` (pyftpdlib, 64 MB and 5 GB sparse images, fault injection, missing file).
- **CI**: the workflow also runs the host tests, installs `gawk` and builds with version `11 ftp`.

### Added – USB Loader GX (patch in `loader/usbloadergx/`)

- FTP game list: scans `*.iso` files in *FTP Path* and in its direct subfolders (`CWD` + `NLST`),
  reads the first 0x60 bytes of each (ID, Wii magic, title), skips non-Wii images. Games use
  partition `FTP_PARTITION` (1000); local copies of the same game take precedence.
- Boot: `GameBooter::SetupDisc()` calls `FtpGames_SetDisc()` (`IOCTL_DI_NET_SET`, then reads the
  disc ID to check the game) instead of the frag list.
- Settings: *FTP Games*, *FTP Server* (`ip[:port]`), *FTP Path* in *Loader Settings*;
  `FtpUser`/`FtpPass` only in `GXGlobal.cfg`.

### Changed

- **`source/dip-plugin/link.ld`**: code region from 0x2000 to **0x3000** bytes, data region now
  starts at `0x13803000` (was `0x13802000`), length 0x3F000 (was 0x40000). The end of the plugin
  memory (`0x13842000`) is unchanged, so no other module moves.
- **`source/dip-plugin/plugin.c`**: `IOCTL_DI_FRAG_SET` now also closes net mode, and
  `IOCTL_DI_NET_SET` also closes frag mode: the two modes share a buffer.
- **`README.md`**: new README; the upstream one is in `docs/d2x-upstream-README.md`.

### Design decisions

| Decision | Why |
|---|---|
| **FTP**, not SMB | SMB2 needs NTLMSSP (MD4, HMAC-MD5), negotiation and credits: tens of KB, while DIPP had about 1.5 KB of free code space. FTP is line based, `REST`+`RETR` give random access, and every NAS has it. |
| FTP code in the **DIP plugin**, not in a new module | The plugin is where the reads are; a separate module would need a new IPC device and more memory map changes. |
| **IOS network stack** (`/dev/net/ip/top`) | It is already in the base IOS (56/57/58) with the LAN adapter and Wi-Fi drivers and the Wii network settings; the ioctls are documented by libogc `network_wii.c`. |
| **One open `RETR` stream**, skip ≤ 256 KB, otherwise `REST` + `RETR` | A new data connection costs ~4–5 round trips; games read mostly sequentially. |
| **Abort by closing the data socket**, not `ABOR` | Works on all common servers, which then send one final 426/226 reply that is consumed. |
| **Ignore the `PASV` address** | Data connections always go to the configured IP: servers behind NAT often announce an unreachable address. |
| **`SIZE` for range checks** | d2x probes a read at 4.7 GB to tell single from dual layer discs; with the size known it fails without network traffic. |
| **Lazy connection** on the first read | After the IOS reload the DIP state is restored very early; the NET module may not be ready and DHCP takes seconds. |
| **Retry ×3 with a full reconnect**, 5 s timeouts | Survives transient errors and games that restart the network stack; never blocks forever. |
| **Clean failures return without retrying** | Out of range reads and 5xx errors would otherwise cause reconnect storms. |
| **All errors are negative** from `Net_Read()` | `__DI_ReadDiscId()` only checks `ret < 0`. A positive code would be taken as success; this bug was found and fixed during review. |
| Receive into a **plugin buffer** + `memcpy` | Unknown whether the NET module may write unaligned PPC memory; the copy is cheap compared to the network speed. |
| Buffer **shared with the fraglist** | DIPP RAM was almost full (240 KB fraglist); the modes are mutually exclusive. |
| Invalidate outputs **before** the ioctl | Invalidating after could discard results still dirty in the shared ARM cache. |
| **No 64-bit division** | `__aeabi_uldivmod` pulls in the libgcc unwinder, which doesn't link; the `REST` offset is printed in base 10⁹. |
| `struct netConfig` passed **by pointer** | DI commands have only 32 bytes of input; same approach as the frag list. |
| **IPv4 addresses only** | No DNS resolver in the cIOS; home users have IPs. |
| USB Loader GX: **`FTP_PARTITION`** with normal `TYPE_GAME_WII_IMG` headers | Covers, settings and categories work unchanged; `WBFS_*` functions reject the partition number safely. |
| USB Loader GX: **anonymous login** by default, credentials only in the config file | As requested; anonymous read-only access is the recommended LAN setup and keeps the GUI small. |

### Known limits / TODO

- [ ] Test on a Wii: NET ioctls from the DI thread, cache handling, startup after the IOS reload, LAN adapter on USB port 1.
- [ ] Measure the real speed; tune `NET_SKIP_MAX` / `NET_BUF_SIZE`.
- [ ] USB Loader GX: run the scan in the background, cache the FTP list, add GUI fields for credentials.
- [ ] Optional: support `.wbfs` images on the server (needs a WBFS sector map in the cIOS).
- [ ] Optional: GameCube games (handled by Nintendont, not by the cIOS: out of scope).

### How to resume the work

1. **Toolchain:**
   - cIOS: devkitARM r32 from
     `https://github.com/Leseratte10/compile-devkitARM-r32/releases/download/2020-02-20/devkitARM-r32-linux_debian-buster.tar.gz`
     in `/opt/devkitARM`, `stripios` built from `stripios_src/main.cpp`, then `gawk` and `libfl2`.
   - USB Loader GX: devkitPPC from the docker image `devkitpro/devkitppc:20250527` (its layers can
     be extracted to `/opt/devkitpro` without docker), with `DEVKITPRO=/opt/devkitpro` and
     `DEVKITPPC=/opt/devkitpro/devkitPPC`.
2. **Build:** `./maked2x.sh 11 ftp` (output in `build/`). Check the DIPP size in
   `source/dip-plugin/dip-plugin.map`: `.text` must stay below 0x3000.
3. **Test:** `pip install pyftpdlib && tools/net-test/run-tests.sh`.
4. **Where the code is:**
   - `source/dip-plugin/net.c` (backend, with a long overview comment);
   - `plugin.c` / `config.c` (look for `MODE_NET` and `d2x-cios-ftp`);
   - `loader/usbloadergx/` (loader patch).
