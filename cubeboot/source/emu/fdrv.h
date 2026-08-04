#pragma once

// Native FlippyDrive backend.
//
// The FlippyDrive is not a block device: it exposes a file API over the disc
// interface (DI) bus, which is what cubeboot was originally written against.
// Everything in emu/ffs (FatFs over SD/IDE-EXI/GC Loader) exists to emulate
// that API for hardware that does not have it. When a real FlippyDrive is
// present we talk to it directly instead, which is both faster and gives us
// the things the emulation cannot do at all: the drive's internal flash, real
// file descriptors, and disc emulation (dvd_set_default_fd) so a game boots
// through the drive without chainloading Swiss.
//
// flippy_emu.c owns the dispatch: it probes for a FlippyDrive first and routes
// every dvd_custom_* call here, falling back to the FatFs device chain when
// there isn't one. Nothing outside flippy_emu.c should call fdrv_* directly.
//
// Shared by both builds (cubeboot.dol and the IPL patches); entry/Makefile
// copies cubeboot/source/emu into patches/source/emu at build time.

#include "../flippy_sync.h"

// Probe the DI bus for a FlippyDrive and bring it into a state where the file
// API answers. Cached after the first call; safe to call on any hardware.
//
// This does what Swiss's deviceHandler_Flippy_test() does: a drive that is
// currently emulating a disc reports the *emulated* drive's inquiry data, so a
// stock-looking answer gets a bypass-exit and a re-inquiry before we believe
// it, and a drive sitting in its bootloader gets booted into firmware.
bool fdrv_probe(void);

// True once fdrv_probe() has positively identified a FlippyDrive. Does not
// probe -- call fdrv_probe() first.
bool fdrv_present(void);

// Firmware version reported by the inquiry, or NULL if no drive. Valid after a
// successful fdrv_probe().
const flippy_version_parts_t *fdrv_version(void);

// File API. Return 0 on success, non-zero on failure, matching dvd_custom_*.
// `dst` buffers are DMA targets and must be 32-byte aligned.
int  fdrv_open(const char *path, uint8_t type, uint8_t flags);
int  fdrv_open_flash(const char *path, uint8_t type, uint8_t flags);
int  fdrv_status(file_status_t *dst);
void fdrv_close(uint32_t fd);
void fdrv_set_default_fd(uint32_t current_fd, uint32_t second_fd);
int  fdrv_read(void *dst, unsigned int len, uint64_t offset, unsigned int fd);
int  fdrv_threaded_read(void *dst, unsigned int len, uint64_t offset, unsigned int fd);
int  fdrv_readdir(file_entry_t *dst, uint32_t fd);
int  fdrv_mkdir(const char *path);
int  fdrv_unlink(const char *path);
int  fdrv_unlink_flash(const char *path);
int  fdrv_write(const char *buf, uint32_t offset, uint32_t length, uint32_t fd);
int  fdrv_fs_info(fs_info_t *dst);
int  fdrv_presence(bool playing, const char *status, const char *sub_status);

// Hand the DI bus to the physical disc (bypass) and take it back again.
void fdrv_bypass_enter(void);
void fdrv_bypass_exit(void);
