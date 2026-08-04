# cubiboot

This is a fork of [cubeboot](https://github.com/OffBroadway/cubeboot) by [TeamOffBroadway](https://github.com/OffBroadway) with support for SD2SP2, SD Gecko or similar SD adapters, the **GC Loader's own SD card**, and **IDE-EXI** hard drives / adapters — on top of the **FlippyDrive** that upstream cubeboot was written for, which still works natively.

If you have questions regarding this fork you can join the [Discord server](https://discord.gg/YtA9aU3BKZ)!

> [!IMPORTANT]
> Format your SD card using exFat (Not FAT32).\
> Currently loading of files is very slow when using FAT32 formatted SD cards.

## Storage devices
Your programs and `config.ini` can live on any of the devices below. They are
probed in this order and the first one that mounts is used, so if you have more
than one attached, the one nearest the top wins.

| Probe order | Device  | Where it is                            | Notes |
|-------------|---------|----------------------------------------|-------|
| 1 | `fldrv` | FlippyDrive SD card **and** its internal flash | Native. Not emulated — see below. |
| 2 | `gcldr` | GC Loader SD card                          | **New.** Read over the disc (DI) bus, so no second SD adapter is needed. |
| 3 | `sdc`   | SD2SP2 (serial port 2)                     | |
| 4 | `sdb`   | SD Gecko in memory card slot B             | |
| 5 | `sda`   | SD Gecko in memory card slot A             | |
| 6 | `ataa`  | IDE-EXI in memory card slot A              | **New.** EXI channel 0, device 0. |
| 7 | `atab`  | IDE-EXI in memory card slot B              | **New.** EXI channel 1, device 0. |
| 8 | `atac`  | IDE-EXI in serial port 1 (SP1)             | **New.** EXI channel 0, device 2. |

### FlippyDrive
A FlippyDrive is checked for before anything else, and when one answers it is
used — no other device is probed. It is the only entry in the table that is not
a block device behind FatFs: cubeboot was originally written against the
FlippyDrive's own file protocol on the disc (DI) bus, and everything else in the
list exists to emulate that protocol for hardware which does not speak it. On a
real FlippyDrive cubiboot talks to it directly again, which restores the things
the emulation cannot do at all:

- **The drive's internal flash.** `swiss-gc.dol`, `stub.bin` and
  `cubeboot.dol` are read from flash, exactly as on a stock FlippyDrive
  install.
- **Booting games through the drive.** The drive serves the selected ISO as if
  it were a disc, so games start without chainloading Swiss and without
  `swiss-gc.dol` being present at all. Set `force_swiss_boot` if you would
  rather keep the Swiss route.
- **Booting the physical disc.** Z in the menu hands the bus to the real disc
  and takes it back if there isn't one.

Detection follows Swiss: cubiboot inquires on the DI bus, asks a drive that is
busy emulating a disc to step aside so it can inquire again (which is the normal
state of affairs when cubiboot itself was launched as a disc image), and boots
the firmware if the drive is sitting in its bootloader.

> [!NOTE]
> The FlippyDrive path is restored upstream code driven by a new detect-and-
> dispatch layer. It has not been re-validated on FlippyDrive hardware by this
> fork. If a drive is not detected, everything falls through to the devices
> below it in the table.

### GC Loader SD card
Booting `cubiboot.iso` renamed to boot.iso from a GC Loader now reads your 
programs straight off the GC Loader's own SD card. Previously this needed 
a separate SD Gecko or SD2SP2 in a memory card slot; that still works and 
takes priority if present.

### IDE-EXI/USB Dolphin
IDE-EXI/USB Dolphin (ATA-over-EXI) adapters are supported in both memory card slots A,B and in
SP1, with the same EXI mapping Swiss uses.

> [!NOTE]
> The IDE-EXI/USB Dolphin and GC Loader read paths have been validated on hardware, but not
> in every adapter/drive combination.

### Block cache
An LRU block cache sits between FatFs and the storage drivers, so repeated reads
of the same areas — directory entries and FAT chains, which get walked over and
over while the game list is built — are served from RAM instead of the device. It
is ported from [libdvm](https://github.com/extremscorner/libdvm) and applies to
every FatFs device in the table above. A FlippyDrive bypasses it entirely — its
own firmware does the caching, on the far side of the DI bus.

It is sized differently on each side of the boot:

| Build | Cache | Why |
|-------|-------|-----|
| Menu (IPL) | 384 KiB | Enumerates games and reads banners/icons — the work that benefits. |
| `cubeboot.dol` | 64 KiB | Only probes for a device and reads `config.ini` and the boot logo. |

Pages are 2 KiB (4 sectors), which reads ahead usefully because directory scans
and FAT chain walks are sequential. This does not change the exFAT-vs-FAT32
advice above — FAT32 is slow for reasons the cache cannot fix.

## Installation - [PicoLoader](https://github.com/makeo/PicoLoader)
1. Download the [```cubiboot_picoloader.uf2```](https://github.com/makeo/cubiboot/releases/latest/download/cubiboot_picoloader.uf2) file
2. Hold down the button on the RP Pico whilst plugging it into your PC
3. Copy the .uf2 file to the USB drive
4. Download the [latest Swiss](https://github.com/emukidid/swiss-gc/releases/latest) dol
5. Rename the Swiss dol to ```swiss-gc.dol``` and place it on your SD card

## Installation - [PicoLoader](https://github.com/makeo/PicoLoader)/[PicoBoot](https://github.com/webhdx/PicoBoot) with gekkoboot payload
1. Download the [```cubiboot.dol```](https://github.com/makeo/cubiboot/releases/latest/download/cubiboot.dol)
2. Rename it to ```ipl.dol```
3. Copy the ```ipl.dol``` onto your SD card
4. Download the [latest Swiss](https://github.com/emukidid/swiss-gc/releases/latest) dol
5. Rename the Swiss dol to ```swiss-gc.dol``` and place it on your SD card

## Using In-Game Reset
1. Download [```EXTRACT_TO_ROOT.zip```](https://github.com/makeo/cubiboot/releases/latest/download/EXTRACT_TO_ROOT.zip)
2. Extract the contents to the root of the SD card
3. Pressing Z + A + START whilst in a game brings you back to the cubiboot menu

## Other ODEs (e.g. GC Loader/CubeODE)
Download the [```cubiboot.iso```](https://github.com/makeo/cubiboot/releases/latest/download/cubiboot.iso) and use it as appropriate for your ODE.\
On a FlippyDrive, cubiboot uses the drive natively — use the ipl.dol 
renamed to cubeboot.dol and replace the one on its internal FlippyDrive Flash,
`config.ini` goes on its SD card, and its internal flash should contain
`swiss-gc.dol`, `cubeboot.dol`. On a GC Loader, your programs 
and `config.ini` can go on the GC Loader's own SD
card — a separate SD2SP2, SD Gecko or similar adapter is no longer required, but
still works and takes priority if one is attached.\
ODEs besides FlippyDrive, CubeODE and GC Loader are not supported, and issues
specific to these devices might not be fixed.

## Release files — which one do I need?
Every build produces the files below. You only need the ones matching how
cubiboot is started on your console; they are alternatives, not a set to install
together.

**Pick one, based on your setup:**

| File | For | What it is |
|------|-----|------------|
| `cubiboot_picoloader.uf2` | [PicoLoader](https://github.com/makeo/PicoLoader) | PicoLoader firmware with `cubiboot.iso` embedded. Flash to the Pico; it serves the image to the disc interface. |
| `cubiboot_picoboot_pico.uf2` | [PicoBoot](https://github.com/webhdx/PicoBoot) on a **Pico / RP2040** | Complete image: PicoBoot firmware + cubiboot as its payload. Use for a fresh install. |
| `cubiboot_picoboot_pico2.uf2` | PicoBoot on a **Pico 2 / RP2350** | Same, for the RP2350 board. |
| `cubiboot_picoboot_payload.uf2` | A Pico **already running** PicoBoot | Payload only — swaps in cubiboot without touching the firmware. Works on both board families **Pico/Pico 2**. |
| `cubiboot.iso` | GC Loader and other ODEs | Bootable GameCube disc image. |
| `ipl.dol` | gekkoboot, or PicoBoot/PicoLoader chainloading a payload from SD | The cubiboot loader itself, already renamed for you — copy it to the SD card root as-is. |

**Supporting files, used alongside the above:**

| File | What it is |
|------|------------|
| `config.ini` | Settings (custom boot logo/colors, boot delays). Goes on the SD card root. See [docs/settings.md](docs/settings.md) and [Custom boot logo](#custom-boot-logo). |
| `apploader.img` | Enables In-Game Reset — Z + A + START returns you to the cubiboot menu. Goes in `swiss/patches/` on the SD card. |
| `EXTRACT_TO_ROOT.zip` | Convenience bundle of `ipl.dol`, `config.ini` and `swiss/patches/apploader.img`. Extract to the SD card root to place all three at once. |

> [!NOTE]
> PicoBoot and PicoLoader are different products and are not interchangeable.
> PicoLoader serves `cubiboot.iso` to the disc drive interface; PicoBoot replaces
> the console's IPL and injects a `.dol` over EXI.

## Custom boot logo
`cube_logo` replaces the "GAMECUBE" wordmark in the boot animation with your own
image. Make one with the **[Boot Logo Generator](https://silverstee1.github.io/cubiboot-logo-generator/)**
([source](https://github.com/silverstee1/cubiboot-logo-generator)) — it runs
entirely in your browser, nothing is uploaded, and it always outputs the only
format the decoder accepts: a **32-bit RGBA PNG, exactly 352×40**.

Drop the result on your SD card root as `logo.png`; the shipped `config.ini`
already points at it:
```ini
cube_logo = logo.png
```
The PNG is **not** included in the release files — it is yours to supply, and
cubiboot falls back to the stock wordmark when it is missing.

> [!TIP]
> `cube_color` tints the light pixels of the logo. If you set a colour, use
> light-on-black art (the generator's Text tab and its greyscale image option
> both produce this) so the tint applies cleanly.

## Building
Pushes to `main` are built automatically by GitHub Actions, which produces every
file listed above. Grab them from the run's artifacts, or from the Releases page
for tagged versions (push a `v*` tag to publish one).

To build locally you only need Docker:
```bash
docker build -t cubiboot-dev - < .ci/Dockerfile
docker run --rm -v "$PWD":/work cubiboot-dev bash -lc 'cd entry && make clean && make'
docker run --rm -v "$PWD":/work cubiboot-dev bash -lc 'bash /work/.ci/build_apploader.sh'
docker run --rm -v "$PWD":/work cubiboot-dev bash -lc 'bash /work/.ci/build_disc_apploader.sh'
docker run --rm -v "$PWD":/work cubiboot-dev bash -lc 'bash /work/.ci/build_iso.sh'
docker run --rm -v "$PWD":/work cubiboot-dev bash -lc 'bash /work/.ci/build_picoboot_uf2.sh'
```
`build_disc_apploader.sh` must run before `build_iso.sh` — it regenerates the
`gbi.hdr` that the ISO embeds. The toolchain is pinned (devkitPPC 20231110,
libogc2/libfat `--before=2026-01-20`); newer libogc2 needs a newer devkitPPC than
that base image, so don't bump it without testing.

## Known Bugs
- loading of files is very slow when using FAT32
- button_* options to not work (use gekkoboot for this functionality instead)

## Special Thanks
- [TeamOffBroadway](https://github.com/OffBroadway) for creating cubeboot
- [Extrems](https://github.com/Extrems), [emukidid](https://github.com/emukidid) and everyone involved in creating Swiss

## Acknowledgements
- [cubeboot](https://github.com/OffBroadway/cubeboot) (GPL-2.0)
- [apploader](https://github.com/makeo/cubeboot-tools) (GPL-2.0)
- [packer](https://github.com/emukidid/swiss-gc/tree/master/cube/packer) for apploader.img (GPL-2.0)
- [libdvm](https://github.com/extremscorner/libdvm) for the block cache (ZPL-2.1, Copyright fincs/devkitPro)
- For more, see [CREDIT.md](https://github.com/makeo/cubiboot/blob/main/CREDIT.md)
