# USB Loader GX: FTP games patch

> [!CAUTION]
> Experimental and never tested on a Wii. See the main [README](../../README.md).

This patch adds FTP games to [USB Loader GX](https://github.com/wiidev/usbloadergx): Wii `.iso`
images on an FTP server appear in the game list next to the local ones and are played through
the **d2x-cios-ftp** cIOS. They don't work with a standard d2x cIOS.

| File | Content |
|---|---|
| [`0001-Add-FTP-games.patch`](0001-Add-FTP-games.patch) | The full patch, against wiidev/usbloadergx commit `e25c4f35` ("Increased version to 1283") |
| [`FtpGames.cpp`](FtpGames.cpp), [`FtpGames.h`](FtpGames.h) | The new files of the patch, here for easy reading |

## Build

```sh
git clone https://github.com/wiidev/usbloadergx.git
cd usbloadergx
git checkout e25c4f35
git am /path/to/d2x-cios-ftp/loader/usbloadergx/0001-Add-FTP-games.patch
make            # needs devkitPPC + libogc (docker image devkitpro/devkitppc:20250527)
```

## Settings

*Settings → Loader Settings*, after *Autoboot Discs Delay*:

| Option | Value |
|---|---|
| **FTP Games** | ON / OFF (default OFF) |
| **FTP Server** | `192.168.1.10` or `192.168.1.10:2121` (IPv4 only, the cIOS has no DNS) |
| **FTP Path** | folder with the games, default `/wbfs` |

Login is **anonymous**. For a server that needs credentials, set `FtpUser = ...` and
`FtpPass = ...` in `GXGlobal.cfg`; there is no GUI field for them.

The `.iso` files are searched in *FTP Path* and in its direct subfolders, for example:

```
/wbfs/RMCP01.iso
/wbfs/Mario Kart Wii [RMCP01]/RMCP01.iso
```

Also set the game IOS (*Games IOS* / *Custom Games IOS*) to the slot where d2x-cios-ftp is
installed.

## What the patch changes

| File | Change |
|---|---|
| `usbloader/FtpGames.cpp/.h` | **New.** FTP client (libogc sockets) that lists the games and reads their headers, plus `FtpGames_SetDisc()` |
| `usbloader/GameList.cpp/.h` | Appends FTP games after the local partitions, with partition `FTP_PARTITION` (local copies win) |
| `usbloader/GameBooter.cpp` | `SetupDisc()`: FTP games call `FtpGames_SetDisc()` instead of the frag list |
| `usbloader/wdvd.c/.h` | `WDVD_SetNetMode()`: the `IOCTL_DI_NET_SET` (0xF8) DI command |
| `settings/CSettings.cpp/.h` | `FtpGames`, `FtpServer`, `FtpPath`, `FtpUser`, `FtpPass` settings |
| `settings/menus/LoaderSettings.cpp/.hpp` | The three GUI options; the list is rescanned when they change |

Every change is marked with a `d2x-cios-ftp` comment. The details are in
[docs/TECHNICAL.md](../../docs/TECHNICAL.md#usb-loader-gx-integration).

## Limits

- The loader still needs a USB or SD device for its own data.
- If the server is unreachable, the scan blocks for up to 5 s per server reply.
- With the title cache on, refresh the game list after adding games to the server.

## License

`FtpGames.cpp/.h` are GPL v3 or later. The modified USB Loader GX files keep their original
license headers (zlib-style or GPL), with a note that marks them as modified.
