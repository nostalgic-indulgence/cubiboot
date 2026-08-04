#ifndef TWEAKS_H
#define TWEAKS_H

#include <stdint.h>
#include <stdbool.h>
#ifdef IPL_CODE
#include "../games.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

const char* emu_get_device();

// True when the adopted device is a real FlippyDrive, i.e. the dvd_custom_*
// calls are going to the drive's own file API rather than being emulated over
// FatFs. Callers use it to decide whether the native-only capabilities (the
// drive's flash, dvd_set_default_fd disc emulation) are available.
bool emu_is_native();

#ifdef IPL_CODE
void emu_update_boot();
bool emu_can_boot(gm_file_type_t type);
void emu_draw_boot_error(gm_file_type_t type, u8 ui_alpha);
bool emu_has_dvd();

// Keyed per FILE (see gm_bnr_cache_key() in games.c), not per game ID.
bool bnr_cache_get(u32 key, BNR* bnr);
void bnr_cache_put(u32 key, BNR* bnr);

#else
void ensure_ipl_loaded(uint8_t* bios_buffer);
#endif

#ifdef __cplusplus
}
#endif

#endif // TWEAKS_H
