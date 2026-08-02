# Booting from an RP2040 Pico

This guide describes how to install cubeboot as a built-in firmware for the pico.
This includes PicoBoot devices installed on the IPL.

## Which file do I want?

| File | Use it when |
| --- | --- |
| `cubiboot_picoboot_pico.uf2` | Fresh install on a **Pico / Pico W** (RP2040). PicoBoot firmware + cubiboot in one flash. |
| `cubiboot_picoboot_pico2.uf2` | Fresh install on a **Pico 2 / Pico 2 W** (RP2350). |
| `cubiboot_picoboot_payload.uf2` | You **already have PicoBoot installed** and only want to swap the payload to cubiboot. Works on both boards. Leaves the existing PicoBoot firmware alone. |

If you are unsure, use the full image for your board.

Note this is **PicoBoot** (the IPL-replacement modchip). If you have a
**PicoLoader** instead — which serves a disc image rather than replacing the IPL
— you want `cubiboot_picoloader.uf2` and the [SD boot guide](SD_Boot.md), not
this one.

## Install

Download the file you need from the GitHub releases page. Optionally desolder the
VCC wire from your pico to avoid over-voltage on your GameCube while flashing (if
you have a diode installed on VCC you can skip this step).

Hold `BOOTSEL` while plugging your pico into your computer. This causes a Drive
to appear on the computer. Copy the `.uf2` to the drive and wait until it
disappears. The firmware has been successfully updated.

If you desoldered VCC you should resolder it now before booting your GameCube again.

Make sure to download a copy of `cubeboot.ini` and copy it to your SD Card. This
settings file allows you to customize aspects of the boot process.

You no longer need an `IPL.dol` file on your SD Card after installing cubeboot as
firmware.

## Issues

TBD