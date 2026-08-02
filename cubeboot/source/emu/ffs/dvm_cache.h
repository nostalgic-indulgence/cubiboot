/*-----------------------------------------------------------------------*/
/* LRU block cache between FatFs and the storage drivers                  */
/*-----------------------------------------------------------------------*/
/* Ported from libdvm's source/dvm_cache.c (Copyright fincs, devkitPro;   */
/* SPDX-License-Identifier: ZPL-2.1), by way of                           */
/* https://github.com/extremscorner/libdvm                                */
/*                                                                        */
/* See dvm_cache.c for what was changed relative to upstream.             */
/*-----------------------------------------------------------------------*/

#ifndef DVM_CACHE_H
#define DVM_CACHE_H

#include "ff.h"
#include "diskio.h"

/* Cached entry points. disk_read()/disk_write() in diskio.c route through
   these; everything else in FatFs is unaware the cache exists. */
DRESULT dvm_cache_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT dvm_cache_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);

/* Drop every cached page. Called automatically when a request arrives for a
   different drive than the one currently cached (which is what happens while
   flippy_emu_mount() probes the device list). */
void dvm_cache_invalidate(void);

/* Take the cache out of the path entirely; reads and writes go straight to the
   device until dvm_cache_enable().
 *
 * MUST be called before anything that writes over RAM the cache is holding.
 * The pages and the bookkeeping that indexes them do not live together: in the
 * IPL build the 384 KiB page buffer is in .data_lowmem (0x80456160 in the
 * current link) while dvm_entries[] is .bss up in the patch region. So an
 * operation that clears low memory wipes the pages and leaves the metadata
 * insisting they are still valid, and every subsequent read is served zeros.
 *
 * Concretely: bs2start() in main.c memsets 0x80100000-0x81600000 before every
 * single boot, which is exactly this hazard -- that is what "no game or DOL
 * boots on any device" looks like. load_dol() clearing a DOL's BSS over the
 * page buffer is the same problem by another route. Both call this first. */
void dvm_cache_disable(void);

/* Re-arm the cache, dropping whatever it was holding. */
void dvm_cache_enable(void);

/* Uncached device access, implemented by diskio.c. */
DRESULT disk_read_raw(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_write_raw(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);

#endif /* DVM_CACHE_H */
