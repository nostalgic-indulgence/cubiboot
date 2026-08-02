# cubiboot

This is a fork of [cubeboot](https://github.com/OffBroadway/cubeboot) by [TeamOffBroadway](https://github.com/OffBroadway) with support for SD2SP2, SD Gecko or similar SD adapters.

If you have questions regarding this fork you can join the [Discord server](https://discord.gg/YtA9aU3BKZ)!

> [!IMPORTANT]
> Format your SD card using exFat (Not FAT32).\
> Currently loading of files is very slow when using FAT32 formatted SD cards.

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

## Other ODEs (e.g. GC Loader)
Download the [```cubiboot.iso```](https://github.com/makeo/cubiboot/releases/latest/download/cubiboot.iso) and use it as appropriate for your ODE.\
Files and the config have to be stored on a separate SD2SP2, SD Gecko or similar SD adapter.\
ODEs besides PicoLoader are not supported, and issues specific to these devices might not be fixed.

## Known Bugs
- loading of files is very slow when using FAT32
- button_* options to not work (use gekkoboot for this functionality instead)
- no PicoBoot uf2

## Special Thanks
- [TeamOffBroadway](https://github.com/OffBroadway) for creating cubeboot
- [Extrems](https://github.com/Extrems), [emukidid](https://github.com/emukidid) and everyone involved in creating Swiss

## Acknowledgements
- [cubeboot](https://github.com/OffBroadway/cubeboot) (GPL-2.0)
- [apploader](https://github.com/makeo/cubeboot-tools) (GPL-2.0)
- [packer](https://github.com/emukidid/swiss-gc/tree/master/cube/packer) for apploader.img (GPL-2.0)
- For more, see [CREDIT.md](https://github.com/makeo/cubiboot/blob/main/CREDIT.md)
