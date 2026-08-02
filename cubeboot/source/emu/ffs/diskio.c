/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2025        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "dvm_cache.h"
#include "../tsd.h"
#include "../gcode.h"
#include "../ata.h"

static bool disk_is_sd(BYTE pdrv) {
	return pdrv == DEV_SDA || pdrv == DEV_SDB || pdrv == DEV_SDC;
}

// IDE-EXI (ATA-over-EXI) drives, one per EXI channel (slot A/B and SP1).
static bool disk_is_ata(BYTE pdrv) {
	return pdrv == DEV_ATAA || pdrv == DEV_ATAB || pdrv == DEV_ATAC;
}

// The GCLoader exposes its SD card over the disc interface (DI) bus rather
// than EXI, so it is handled separately from the SD Gecko / SD2SP2 devices.
static bool disk_is_gcldr(BYTE pdrv) {
	return pdrv == DEV_GCLDR;
}

static const exi_port exi_port_map[FF_VOLUMES] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };

// IDE-EXI EXI mapping (matches Swiss): ataa = channel 0 dev 0 (memcard slot A),
// atab = channel 1 dev 0 (slot B), atac = channel 0 dev 2 (serial port 1).
static exi_port ata_port(BYTE pdrv) {
	if (pdrv == DEV_ATAC) {
		exi_port p = { 0, 2 };
		return p;
	}
	exi_port p = { (u8)(pdrv - DEV_ATAA), 0 };
	return p;
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(BYTE pdrv) {
	if (disk_is_sd(pdrv))
		return 0;

	if (disk_is_ata(pdrv))
		return 0;

	if (disk_is_gcldr(pdrv))
		return 0;

	return STA_NOINIT;
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(BYTE pdrv) {
	// Whatever is cached describes the medium that was here before this call,
	// which may not be the one that is here now.
	dvm_cache_invalidate();

	if (disk_is_sd(pdrv))
		return tsd_sd_init(exi_port_map[pdrv]) ? 0 : STA_NOINIT;

	if (disk_is_ata(pdrv))
		return ata_init(ata_port(pdrv)) ? 0 : STA_NOINIT;

	if (disk_is_gcldr(pdrv))
		return gcode_sd_init() ? 0 : STA_NOINIT;

	return STA_NOINIT;
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

// Uncached device access. Only dvm_cache.c should call this; everything else
// goes through disk_read() below.
DRESULT disk_read_raw(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	if (disk_is_sd(pdrv))
		return tsd_sd_read(exi_port_map[pdrv], sector, buff, count) ? RES_OK : RES_ERROR;

	if (disk_is_ata(pdrv))
		return ata_read(ata_port(pdrv), sector, buff, count) ? RES_OK : RES_ERROR;

	if (disk_is_gcldr(pdrv))
		return gcode_sd_read(sector, buff, count) ? RES_OK : RES_ERROR;

	return RES_PARERR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	if (!disk_is_sd(pdrv) && !disk_is_ata(pdrv) && !disk_is_gcldr(pdrv))
		return RES_PARERR;

	return dvm_cache_read(pdrv, buff, sector, count);
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

// Uncached device access; see disk_read_raw() above.
DRESULT disk_write_raw(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	if (disk_is_sd(pdrv))
		return tsd_sd_write(exi_port_map[pdrv], sector, buff, count) ? RES_OK : RES_WRPRT;

	if (disk_is_ata(pdrv))
		return ata_write(ata_port(pdrv), sector, buff, count) ? RES_OK : RES_WRPRT;

	if (disk_is_gcldr(pdrv))
		return gcode_sd_write(sector, buff, count) ? RES_OK : RES_WRPRT;

	return RES_PARERR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	if (!disk_is_sd(pdrv) && !disk_is_ata(pdrv) && !disk_is_gcldr(pdrv))
		return RES_PARERR;

	// Write-through: the cache keeps any affected page coherent and passes the
	// transfer straight down, so there is never anything buffered to flush.
	return dvm_cache_write(pdrv, buff, sector, count);
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
	if (disk_is_sd(pdrv) || disk_is_ata(pdrv) || disk_is_gcldr(pdrv)) {
		switch (cmd) {
			case CTRL_SYNC:
				// Nothing to do: the block cache is write-through.
				return RES_OK;
				
			case GET_SECTOR_COUNT:
				*(DWORD*)buff = 0xFFFFFFFF; // who cares
				return RES_OK;
				
			case GET_SECTOR_SIZE:
				*(WORD*)buff = 512;
				return RES_OK;
				
			case GET_BLOCK_SIZE:
				*(DWORD*)buff = 1;
				return RES_OK;
				
			default:
				return RES_PARERR;
		}
	}

	return RES_PARERR;
}


/*-----------------------------------------------------------------------*/
/* Shutdown Drive                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_shutdown(BYTE pdrv) {
	if (disk_is_sd(pdrv))
		return RES_OK;

	if (disk_is_ata(pdrv))
		return RES_OK;

	if (disk_is_gcldr(pdrv))
		return RES_OK;

	return RES_PARERR;
}

