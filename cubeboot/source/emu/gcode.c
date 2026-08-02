#include "gcode.h"

#include <string.h>

#ifdef IPL_CODE
// Provided by the IPL build (os.c / time.c). Declared here to avoid pulling in
// build-specific headers that are not on the shared emu include path.
extern void DCInvalidateRange(void *addr, u32 len);
extern void udelay(unsigned us);
#define gcode_settle_ms(ms) udelay((ms) * 1000u)
#else
#include <ogc/cache.h>
#include <unistd.h>
#define gcode_settle_ms(ms) usleep((ms) * 1000u)
#endif

// ---------------------------------------------------------------------------
// DI registers (YAGCD), matching the layout used in flippy_sync.c
// ---------------------------------------------------------------------------
#define DI_SR            0  // 0xCC006000 - Status Register
#define DI_SR_BRKINTMASK (1 << 5)
#define DI_SR_TCINT      (1 << 4)
#define DI_SR_TCINTMASK  (1 << 3)
#define DI_SR_DEINT      (1 << 2) // Device Error Interrupt Status
#define DI_SR_DEINTMASK  (1 << 1)

#define DI_CVR           1  // 0xCC006004 - Cover Register
#define DI_CMDBUF0       2  // 0xCC006008 - Command Buffer 0
#define DI_CMDBUF1       3  // 0xCC00600c - Command Buffer 1
#define DI_CMDBUF2       4  // 0xCC006010 - Command Buffer 2
#define DI_MAR           5  // 0xCC006014 - DMA Memory Address Register
#define DI_LENGTH        6  // 0xCC006018 - DMA Transfer Length Register
#define DI_CR            7  // 0xCC00601c - Control Register
#define DI_CR_RW         (1 << 2)
#define DI_CR_DMA        (1 << 1)
#define DI_CR_TSTART     (1 << 0)

static vu32 * const _di_regs = (vu32 *)0xCC006000;

// DI / GCLoader command opcodes.
#define DVD_OEM_INQUIRY  0x12000000
#define DVD_GCODE_READ   0xB2000000

// GCLoader and compatible ODEs report this fixed release date in the
// inquiry response. The same signature is used by libogc2's __io_gcode
// driver and by cubeboot's original (disabled) GCLoader detection.
#define GCODE_REL_DATE   0x20196c64

#define GCODE_SECTOR_SIZE 512u
// Keep a single DMA transfer comfortably below the hardware limit (0x500000).
#define GCODE_MAX_XFER    (64u * 1024u)

// Inquiry response. Only the first 8 bytes (rev_level, dev_code, rel_date)
// are needed; the structure is padded and 32-byte aligned for DI DMA.
typedef struct __attribute__((packed, aligned(32))) {
	u16 rev_level;
	u16 dev_code;
	u32 rel_date;
	u8  pad[24];
} gcode_drvinfo;

static int has_gcode = -1; // -1 = unknown, 0 = absent, 1 = present

static void gcode_di_prepare(void) {
	// Mask/ack interrupts and clear any pending cover interrupt so we can
	// poll the transfer to completion, exactly as flippy_sync.c does.
	_di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
	_di_regs[DI_CVR] = 0;
}

// Number of inquiry attempts and the settle delay between them. On a COLD ODE
// boot the GC Loader's DI interface is not ready the instant we first probe it:
// BS2 / the disc apploader just finished using the DI to read the disc image
// (cubiboot.iso), and the GC Loader needs a moment before it answers the SD
// inquiry again. A single too-early inquiry would mark the drive permanently
// absent (has_gcode cached), so nothing mounts and the screen goes black after
// the boot animation. Retry with a short settle delay so the cold path
// recovers; only the FIRST call pays this, and a warm boot (e.g. launched from
// Swiss) succeeds on the first attempt and never waits. (This is also why
// enabling gecko prints "fixed" it: the prints happened to add the delay.)
#define GCODE_INIT_ATTEMPTS   40
#define GCODE_INIT_SETTLE_MS  10   // up to ~400ms total

bool gcode_sd_init(void) {
	if (has_gcode >= 0)
		return has_gcode == 1; // already determined (cached, success or absent)

	static gcode_drvinfo info __attribute__((aligned(32)));

	for (int attempt = 0; attempt < GCODE_INIT_ATTEMPTS; attempt++) {
		memset(&info, 0, sizeof(info));

		gcode_di_prepare();

		_di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
		_di_regs[DI_CMDBUF1] = 0;
		_di_regs[DI_CMDBUF2] = 0;

		_di_regs[DI_MAR] = (u32)&info & 0x1FFFFFFF; // cached -> physical
		_di_regs[DI_LENGTH] = sizeof(info);
		_di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

		// Bounded wait: if there is no GC Loader on the DI bus (e.g. booted via
		// PicoBoot/gekkoboot with no ODE, or a plain optical drive), the inquiry
		// DMA may never complete. Don't spin forever -> treat as absent so the
		// device probe can move on instead of hanging the whole boot.
		u32 timeout = 1000000;
		while ((_di_regs[DI_CR] & DI_CR_TSTART) && --timeout)
			;
		if (timeout == 0) {
			has_gcode = 0;
			return false;
		}

		DCInvalidateRange(&info, sizeof(info));

		// A valid GC Loader answers with no device error and the fixed
		// release-date signature. Cache success and stop retrying.
		if (!(_di_regs[DI_SR] & DI_SR_DEINT) && info.rel_date == GCODE_REL_DATE) {
			has_gcode = 1;
			return true;
		}

		gcode_settle_ms(GCODE_INIT_SETTLE_MS);
	}

	// Genuinely not a GC Loader (or never became ready). Cache the result so we
	// don't pay the retry window again on subsequent reads.
	has_gcode = 0;
	return false;
}

// Read `bytes` (a multiple of 32) into a 32-byte aligned buffer at `sector`.
static bool gcode_read_aligned(uint32_t sector, void *buf, uint32_t bytes) {
	gcode_di_prepare();

	_di_regs[DI_CMDBUF0] = DVD_GCODE_READ;
	_di_regs[DI_CMDBUF1] = sector; // LBA in 512-byte blocks
	_di_regs[DI_CMDBUF2] = bytes;  // length in bytes
	_di_regs[DI_MAR] = (u32)buf & 0x1FFFFFFF;
	_di_regs[DI_LENGTH] = bytes;

	// DI DMA writes straight to RAM, bypassing the CPU cache. Drop any cache
	// lines covering the destination BEFORE the transfer so a later eviction
	// of a dirty line cannot clobber the DMA'd data (buf is 32-byte aligned
	// and bytes is a multiple of 32, so the range is cache-line aligned).
	// Without this, browsing (small reads) worked but loading a full DOL/ISO
	// over DI returned silently-corrupt data -> crash/black-screen on boot.
	DCInvalidateRange(buf, bytes);

	_di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

	u32 timeout = 1000000;
	while ((_di_regs[DI_CR] & DI_CR_TSTART) && --timeout)
		; // wait for transfer to complete (bounded)
	if (timeout == 0)
		return false;

	DCInvalidateRange(buf, bytes);

	// ERR asserted -> read failed.
	return (_di_regs[DI_SR] & DI_SR_DEINT) == 0;
}

bool gcode_sd_read(uint32_t sector, uint8_t *data, uint32_t count) {
	if (has_gcode != 1 && !gcode_sd_init())
		return false;

	// DI DMA requires a 32-byte aligned destination. 512-byte sectors are
	// already a multiple of 32, so only the buffer address can be a problem.
	if (((u32)data & 0x1F) == 0) {
		while (count > 0) {
			uint32_t chunk = count;
			if (chunk * GCODE_SECTOR_SIZE > GCODE_MAX_XFER)
				chunk = GCODE_MAX_XFER / GCODE_SECTOR_SIZE;

			if (!gcode_read_aligned(sector, data, chunk * GCODE_SECTOR_SIZE))
				return false;

			sector += chunk;
			data += chunk * GCODE_SECTOR_SIZE;
			count -= chunk;
		}
		return true;
	}

	// Unaligned destination: bounce one sector at a time.
	static u8 bounce[GCODE_SECTOR_SIZE] __attribute__((aligned(32)));
	for (uint32_t i = 0; i < count; i++) {
		if (!gcode_read_aligned(sector + i, bounce, GCODE_SECTOR_SIZE))
			return false;
		memcpy(data + (i * GCODE_SECTOR_SIZE), bounce, GCODE_SECTOR_SIZE);
	}
	return true;
}

bool gcode_sd_write(uint32_t sector, const uint8_t *data, uint32_t count) {
	(void)sector;
	(void)data;
	(void)count;
	return false; // read-only interface
}
