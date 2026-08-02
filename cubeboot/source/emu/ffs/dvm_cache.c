/*-----------------------------------------------------------------------*/
/* LRU block cache between FatFs and the storage drivers                  */
/*-----------------------------------------------------------------------*/
/* Ported from libdvm's source/dvm_cache.c                                */
/* SPDX-License-Identifier: ZPL-2.1                                       */
/* SPDX-FileCopyrightText: Copyright fincs, devkitPro                     */
/* Upstream: https://github.com/extremscorner/libdvm                      */
/*                                                                        */
/* Why this exists: none of the storage drivers cache anything, and the SD */
/* paths issue one CMD17 per 512-byte sector (see tsd.c). FatFs re-reads   */
/* the same directory and FAT sectors constantly -- f_open() scans a       */
/* directory linearly from the start, so opening N files in one directory  */
/* re-walks the same sectors O(N^2) times. Caching those sectors is what   */
/* makes enumerating a large game directory tolerable.                     */
/*                                                                        */
/* Changes from upstream:                                                  */
/*  - Static storage instead of malloc/aligned_alloc: page count and page  */
/*    size are compile-time constants here, so the DvmDisc vtable, the      */
/*    flexible array member and dvmDiscCacheCreate() are all gone.          */
/*  - Write-through instead of write-back. Upstream tracks a dirty range   */
/*    per page and flushes on demand; that needs a flush hook wired to      */
/*    CTRL_SYNC and is a data-loss risk if any path misses it. Writes are   */
/*    near-nonexistent here (dvd_custom_write() is unimplemented and two of */
/*    the three drivers return failure on write), so passing writes         */
/*    straight through -- while keeping cached pages coherent -- costs      */
/*    nothing and removes the whole class of bug.                           */
/*  - No locking. Upstream takes a newlib _LOCK_T; FatFs here is built with */
/*    FF_FS_REENTRANT 0, meaning callers already serialise all access, and  */
/*    this sits strictly below FatFs.                                       */
/*  - Keyed on FatFs's pdrv, with a full invalidate on drive switch, in     */
/*    place of upstream's per-DvmDisc instances.                            */
/*  - No disc size is available (disk_ioctl reports GET_SECTOR_COUNT as     */
/*    0xFFFFFFFF), so upstream's bounds checks and end-of-disc page-fill    */
/*    clamp are replaced by a fallback: if filling a page fails, the        */
/*    request is retried uncached.                                          */
/*-----------------------------------------------------------------------*/

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "ff.h"
#include "diskio.h"
#include "dvm_cache.h"

#ifdef IPL_CODE
#include "../../attr.h"
/* .data_lowmem is NOLOAD: reserved RAM that costs nothing in the patch image.
   Nothing in it is zeroed at runtime, which is fine for the page data (every
   page is filled from disc before it is read) but is why the metadata below
   lives in .bss instead. */
#define DVM_CACHE_DATA_ATTR __attribute_aligned_data_lowmem__
#else
#define DVM_CACHE_DATA_ATTR __attribute__((aligned(32)))
#endif

/* disk_ioctl() reports GET_SECTOR_SIZE as 512 for every drive. */
#define DVM_SECTOR_SZ       512

/* 4 sectors (2 KiB) per page x 192 pages = 384 KiB.
 *
 * SIZING: this is not a gradual trade-off. The workload that matters is
 * f_open() re-scanning the game directory from the start, once per open, twice
 * per game -- a repeated linear sweep. If the whole directory fits, essentially
 * every scan after the first is free; if it does not, LRU is the worst possible
 * policy for a repeated sweep and the hit rate collapses to near zero. So the
 * cache must be at least as large as the directory:
 *
 *     directory bytes ~= files * 32 * (1 + ceil(name_len / 13))
 *
 * games.c caps a listing at 1920 entries, which at typical ISO name lengths is
 * ~300 KiB of directory -- hence 384 KiB. Modelled against that worst case,
 * physical sector traffic drops from ~1.36M sectors to ~11K.
 *
 * Given enough capacity, the remaining choice is page size against page count.
 * Bigger pages read ahead (good: both directory scans and FAT chain walks are
 * sequential) and shorten the O(DVM_CACHE_PAGES) search below, but amplify
 * every miss. 4 sectors measured better than 2 or 8 at this capacity. */
#ifndef DVM_PAGE_SHIFT
#define DVM_PAGE_SHIFT      2
#endif
#ifndef DVM_CACHE_PAGES
#ifdef IPL_CODE
#define DVM_CACHE_PAGES     192		/* 384 KiB */
#else
/* cubeboot.dol only probes for a device and reads a couple of small files
   (settings ini, boot logo) -- there is no directory enumeration here, so the
   sizing argument above does not apply and 64 KiB is ample. */
#define DVM_CACHE_PAGES     32
#endif
#endif

#define DVM_PAGE_SECTORS    (1U << DVM_PAGE_SHIFT)
#define DVM_PAGE_SZ         (DVM_PAGE_SECTORS * DVM_SECTOR_SZ)

/* Matches the CPU cache line, and the alignment the DI DMA path in gcode.c
   needs to avoid its slow one-sector-at-a-time bounce. */
#define DVM_BUFFER_ALIGN    32

#define DVM_EMPTY_PAGE      (~(LBA_t)0)

typedef struct dvm_cache_entry dvm_cache_entry;

struct dvm_cache_entry {
	dvm_cache_entry *next;
	dvm_cache_entry *prev;
	LBA_t base_sector;
};

/* Sentinel deliberately placed in the SAME region as the pages, so that
   anything which destroys them destroys this too.
 *
 * The pages live in RAM that the rest of the firmware treats as scratch --
 * bs2start() memsets all of .data_lowmem before every boot, and load_dol() will
 * clear a DOL's BSS over it -- while dvm_entries[] is .bss in the patch region
 * and survives. Without this the metadata would go on reporting hits for pages
 * that had been zeroed underneath it, and every read would return zeros.
 *
 * Both of those callers call dvm_cache_disable() first, and that remains the
 * primary defence. This is a backstop for the case that actually shipped
 * broken: a wipe of the whole region takes the sentinel with the pages, so the
 * cache notices and rebuilds. It also covers the first ever access, where
 * .data_lowmem is NOLOAD and holds garbage.
 *
 * It is NOT a general guarantee: a partial overwrite that lands on the pages
 * but misses these four bytes -- load_dol() clearing a DOL's BSS is exactly
 * that shape -- is undetectable here. Such a caller MUST disable the cache
 * itself. Both cases are covered by the host tests in the scratchpad. */
#define DVM_MAGIC 0x44564D31u	/* 'DVM1' */
static DVM_CACHE_DATA_ATTR uint32_t dvm_magic;

/* Page data. See DVM_CACHE_DATA_ATTR above for why this is split from the
   metadata. */
static DVM_CACHE_DATA_ATTR BYTE dvm_cache_data[DVM_CACHE_PAGES * DVM_PAGE_SZ];

/* Metadata. Plain .bss, which is zero-filled and loaded in both builds, so
   dvm_ready is trustworthy on entry. base_sector still needs explicit
   initialisation because 0 is a valid sector number. */
static dvm_cache_entry dvm_entries[DVM_CACHE_PAGES];
static dvm_cache_entry dvm_list;	/* .next = most recently used, .prev = least */
static BYTE dvm_owner_pdrv;
static bool dvm_ready;
static bool dvm_disabled;

static void dvm_cache_reset(void)
{
	dvm_list.next = &dvm_entries[0];
	dvm_list.prev = &dvm_entries[DVM_CACHE_PAGES - 1];

	for (unsigned i = 0; i < DVM_CACHE_PAGES; i++) {
		dvm_cache_entry *p = &dvm_entries[i];

		p->next = (i + 1) < DVM_CACHE_PAGES ? &p[1] : NULL;
		p->prev = i ? &p[-1] : NULL;
		p->base_sector = DVM_EMPTY_PAGE;
	}

	dvm_magic = DVM_MAGIC;
	dvm_ready = true;
}

void dvm_cache_invalidate(void)
{
	dvm_cache_reset();
}

void dvm_cache_disable(void)
{
	/* Mark the pages untrusted as well as bypassing them, so that a later
	   dvm_cache_enable() cannot resurrect stale content. */
	dvm_disabled = true;
	dvm_ready = false;
}

void dvm_cache_enable(void)
{
	dvm_disabled = false;
	dvm_ready = false;	/* forces a reset on the next access */
}

/* Called on every request; resets the cache the first time through, whenever
   FatFs switches drives (i.e. while probing the device list), and whenever the
   pages have been destroyed underneath us (see dvm_magic above). */
static void dvm_cache_claim(BYTE pdrv)
{
	if (dvm_magic != DVM_MAGIC || !dvm_ready || pdrv != dvm_owner_pdrv) {
		dvm_cache_reset();
		dvm_owner_pdrv = pdrv;
	}
}

static BYTE *dvm_entry_data(dvm_cache_entry *p)
{
	return dvm_cache_data + (unsigned)(p - dvm_entries) * DVM_PAGE_SZ;
}

static bool dvm_is_aligned(const void *ptr)
{
	return ((uintptr_t)ptr & (DVM_BUFFER_ALIGN - 1)) == 0;
}

/* Returns the entry holding page_sector on a hit, otherwise the cached entry
   with the lowest base_sector greater than page_sector (so the caller knows
   how far it may read directly before running into cached data), or NULL. */
static dvm_cache_entry *dvm_cache_search(LBA_t page_sector)
{
	LBA_t min_sec = DVM_EMPTY_PAGE;
	dvm_cache_entry *ret = NULL;

	for (dvm_cache_entry *p = dvm_list.next; p; p = p->next) {
		/* Unused entries are only ever taken from the tail, so the first
		   one seen means there is nothing further to find. */
		if (p->base_sector == DVM_EMPTY_PAGE) {
			break;
		}

		if (p->base_sector == page_sector) {
			return p;
		}

		if (p->base_sector > page_sector && p->base_sector < min_sec) {
			min_sec = p->base_sector;
			ret = p;
		}
	}

	return ret;
}

static void dvm_cache_promote(dvm_cache_entry *p)
{
	if (p == dvm_list.next) {
		return;
	}

	p->prev->next = p->next;
	if (p->next) {
		p->next->prev = p->prev;
	} else {
		dvm_list.prev = p->prev;
	}

	p->next = dvm_list.next;
	p->next->prev = p;
	p->prev = NULL;
	dvm_list.next = p;
}

/* Takes the least recently used entry, preferring an unused one. */
static dvm_cache_entry *dvm_cache_evict(void)
{
	dvm_cache_entry *p = dvm_list.prev;

	while (p->base_sector == DVM_EMPTY_PAGE && p->prev &&
	       p->prev->base_sector == DVM_EMPTY_PAGE) {
		p = p->prev;
	}

	return p;
}

static DRESULT dvm_cache_access(BYTE pdrv, BYTE *buffer, LBA_t first_sector,
                                UINT num_sectors, bool is_write)
{
	BYTE * const orig_buffer = buffer;
	const LBA_t orig_first = first_sector;
	const UINT orig_count = num_sectors;

	const bool is_aligned = dvm_is_aligned(buffer);
	/* Requests smaller than a page can never be served by a straight
	   pass-through, so they are the ones worth caching. Bulk transfers (game
	   headers, banners, DOLs) go direct and leave the cache alone. */
	const bool is_partial = num_sectors < DVM_PAGE_SECTORS;

	dvm_cache_entry *p = NULL;
	LBA_t search_base = 0;

	while (num_sectors) {
		LBA_t cur_page_sector = first_sector & ~(LBA_t)(DVM_PAGE_SECTORS - 1);
		unsigned cur_page_offset = (unsigned)(first_sector & (DVM_PAGE_SECTORS - 1));

		UINT max_cur_sectors = DVM_PAGE_SECTORS - cur_page_offset;
		UINT cur_sectors = num_sectors < max_cur_sectors ? num_sectors : max_cur_sectors;

		bool is_whole = cur_page_offset == 0 && cur_sectors == DVM_PAGE_SECTORS;

		if (cur_page_sector >= search_base) {
			p = dvm_cache_search(cur_page_sector);
			search_base = p ? p->base_sector + 1 : DVM_EMPTY_PAGE;
		}

		if (p && p->base_sector == cur_page_sector) {
			/* Cache hit. */
			BYTE *data = dvm_entry_data(p) + cur_page_offset * DVM_SECTOR_SZ;

			if (is_write) {
				/* Write-through: keep the page coherent, then let the
				   transfer fall through to the device below. */
				memcpy(data, buffer, cur_sectors * DVM_SECTOR_SZ);
				if (disk_write_raw(pdrv, buffer, first_sector, cur_sectors) != RES_OK) {
					return RES_ERROR;
				}
			} else {
				memcpy(buffer, data, cur_sectors * DVM_SECTOR_SZ);
			}

			/* Whole-page accesses do not promote, so streaming a large file
			   cannot evict the directory and FAT pages we care about. */
			if (!is_whole) {
				dvm_cache_promote(p);
			}
		} else if (!is_write && (is_partial || !is_aligned)) {
			/* Miss on a read worth caching. Fill a page and retry the copy
			   through the hit path above by restarting this iteration. */
			p = dvm_cache_evict();
			p->base_sector = cur_page_sector;

			if (disk_read_raw(pdrv, dvm_entry_data(p), cur_page_sector,
			                  DVM_PAGE_SECTORS) != RES_OK) {
				/* Most likely a page that runs past the end of the volume.
				   Abandon the cache for this request and serve the original
				   transfer directly. */
				p->base_sector = DVM_EMPTY_PAGE;
				return disk_read_raw(pdrv, orig_buffer, orig_first, orig_count);
			}

			dvm_cache_promote(p);

			/* Not upstream: search_base still refers to whatever the search
			   found before this page was allocated, and p no longer does.
			   Leaving it be makes the next iteration reuse a stale p, whose
			   base_sector is now behind first_sector -- the direct branch then
			   underflows max_cur_sectors and reads straight over any cached
			   page in between. Harmless with write-through, but it throws the
			   rest of the request out of the cache. Force a re-search instead;
			   this iteration is unaffected, since cur_page_sector has not
			   advanced yet. */
			search_base = cur_page_sector + 1;
			continue;
		} else {
			/* Straight through to the device. Stop short of the next cached
			   page so the loop picks it up on the following iteration. */
			max_cur_sectors = p ? (UINT)(p->base_sector - first_sector) : num_sectors;
			cur_sectors = num_sectors < max_cur_sectors ? num_sectors : max_cur_sectors;

			DRESULT res = is_write
				? disk_write_raw(pdrv, buffer, first_sector, cur_sectors)
				: disk_read_raw(pdrv, buffer, first_sector, cur_sectors);

			if (res != RES_OK) {
				return res;
			}
		}

		buffer += cur_sectors * DVM_SECTOR_SZ;
		first_sector += cur_sectors;
		num_sectors -= cur_sectors;
	}

	return RES_OK;
}

DRESULT dvm_cache_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (count == 0) {
		return RES_OK;
	}

	if (dvm_disabled) {
		return disk_read_raw(pdrv, buff, sector, count);
	}

	dvm_cache_claim(pdrv);
	return dvm_cache_access(pdrv, buff, sector, count, false);
}

DRESULT dvm_cache_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (count == 0) {
		return RES_OK;
	}

	if (dvm_disabled) {
		return disk_write_raw(pdrv, buff, sector, count);
	}

	dvm_cache_claim(pdrv);
	return dvm_cache_access(pdrv, (BYTE *)buff, sector, count, true);
}
