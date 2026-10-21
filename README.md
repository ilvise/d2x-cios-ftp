<div align="center">

# d2x-cios-ftp

**Play Wii games from an FTP server: a d2x cIOS that reads the disc image over the network.**

Wii LAN adapter or Wi-Fi · uncompressed `.iso` images · any FTP server · USB Loader GX integration

[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
![Status: experimental](https://img.shields.io/badge/status-experimental-red.svg)
![Platform: Wii](https://img.shields.io/badge/platform-Wii-lightgrey.svg)
[![Based on d2x v11](https://img.shields.io/badge/based%20on-d2x%20cIOS%20v11-orange.svg)](https://github.com/wiidev/d2x-cios)

</div>

> [!CAUTION]
> **Experimental: do not install this on your Wii yet.**
> The code builds and the FTP logic passes the tests on a PC, but **it has never run on real
> hardware**. Installing a cIOS always carries some risk. Use it only if you know how to recover
> your console, and at your own risk.

## What it is

[d2x](https://github.com/wiidev/d2x-cios) is the custom IOS that USB loaders use to play games
from USB and SD drives. This fork adds a third source to its DIP plugin: an `.iso` stored on an
**FTP server**, which the cIOS reads through the console's own network stack while the game runs.

```
 Wii game ──► /dev/di ──► DIP plugin (cIOS 249) ──► IOS network stack ──► LAN / Wi-Fi ──► FTP server (.iso)
```

- **Loader side:** a loader turns the mode on with one new DI command, `IOCTL_DI_NET_SET` (`0xF8`).
  A patch for **USB Loader GX** lists the games on the server and boots them.
- **Why FTP:** the plugin has about 12 KB of code space. A full SMB client doesn't fit, while every
  NAS and PC can serve FTP.

## Resources

| | Resource | Description |
|---|---|---|
| 📖 | [docs/TECHNICAL.md](docs/TECHNICAL.md) | How it works: architecture, protocol, ABI, memory, limits |
| 📝 | [changelog.md](changelog.md) | Every change and design decision compared to upstream d2x |
| 🔌 | [source/dip-plugin/net.c](source/dip-plugin/net.c) | The FTP backend inside the cIOS (ARM) |
| 🎮 | [loader/usbloadergx/](loader/usbloadergx/) | USB Loader GX patch: FTP game list and boot |
| 🧰 | [tools/loader/](tools/loader/) | `WDVD_SetNetMode()` helper for other libogc loaders |
| 🧪 | [tools/net-test/](tools/net-test/) | Tests on a PC against a local FTP server |
| 📜 | [docs/d2x-upstream-README.md](docs/d2x-upstream-README.md) | Original d2x README |

## Quick start (for developers)

```sh
# cIOS modules (same toolchain as the upstream CI)
wget https://github.com/Leseratte10/compile-devkitARM-r32/releases/download/2020-02-20/devkitARM-r32-linux_debian-buster.tar.gz
sudo tar -xf devkitARM-r32-linux_debian-buster.tar.gz -C /opt
(cd stripios_src && g++ main.cpp -o stripios && cp stripios ..)
sudo apt-get install -y gawk libfl2
./maked2x.sh 11 ftp                 # output in build/

# FTP backend tests on the PC
pip install pyftpdlib
tools/net-test/run-tests.sh
```

Installation, the USB Loader GX settings and the server setup are covered in
[docs/TECHNICAL.md](docs/TECHNICAL.md#usage).

## Known limits

- Never tested on a Wii (see the warning above).
- The Wii's network is slow: about 0.6–1 MB/s with the LAN adapter, against 3–6 MB/s from the
  disc drive. Loading takes longer and video or streamed audio may stutter.
- Uncompressed `.iso` only (no `.wbfs`, `.ciso`, `.rvz`) and Wii games only (no GameCube).
- FTP sends passwords in clear text. Use it only on a trusted LAN, ideally with anonymous
  read-only access.

## Credits

This project is a modified version of other people's work:

- **[wiidev/d2x-cios](https://github.com/wiidev/d2x-cios)** by blackb0x and contributors: the d2x
  cIOS v11 source this fork is based on.
- **[davebaol/d2x-cios](https://github.com/davebaol/d2x-cios)** by davebaol: the original d2x cIOS.
- **Waninkoko** (cIOSX rev21), **WiiGator** (DIP plugin), **oggzee** (frag lists), **kwiirk** and
  **Hermes** (EHCI), **neimod** (Custom IOS Module Toolkit, `stripios`), **WiiPower**, **rodries**,
  **Crediar**, **ChaN** (FatFs) and everyone listed in the
  [upstream kudos](docs/d2x-upstream-README.md#kudos).
- **[devkitPro/libogc](https://github.com/devkitPro/libogc)**: `network_wii.c` was the reference for
  the IOS network ioctls and their parameter layouts.
- **[wiidev/usbloadergx](https://github.com/wiidev/usbloadergx)** by Dimok, blackb0x and
  contributors: the loader the patch applies to.
- **[Leseratte10/compile-devkitARM-r32](https://github.com/Leseratte10/compile-devkitARM-r32)**: the
  devkitARM r32 build used by the upstream CI, plus fixes to `stripios`.
- [pyftpdlib](https://github.com/giampaolo/pyftpdlib) and [vsftpd](https://security.appspot.com/vsftpd.html):
  the FTP servers used in the tests.

## License

[GNU GPL v3](LICENSE). The new code in this repository is released under the GPL v3 "or any later
version", the same license as the d2x DIP plugin it extends. Files inherited from upstream keep
their own license headers (GPL v2 or later, GPL v2 only, BSD-style, zlib-style). The license notes
in [docs/TECHNICAL.md](docs/TECHNICAL.md#license-notes) explain how they fit together.

This project is not affiliated with Nintendo, and it is meant only for playing backups of games
you own.
