#include "fdrv.h"

#include <string.h>

#ifdef IPL_CODE
#include "../reloc.h"   // OSReport
#include "../dolphin_os.h" // OSYieldThread
extern void DCInvalidateRange(void *addr, u32 nBytes);
extern void DCFlushRange(void *addr, u32 nBytes);
extern void udelay(unsigned us);
#define fdrv_settle_ms(ms) udelay((ms) * 1000u)
#define FDRV_LOG(...) OSReport(__VA_ARGS__)
#else
#include <stdio.h>
#include <ogc/cache.h>
#include <unistd.h>
#include "../config.h"
#define fdrv_settle_ms(ms) usleep((ms) * 1000u)
#ifdef GECKO_PRINT_ENABLE
#define FDRV_LOG(...) iprintf(__VA_ARGS__)
#else
#define FDRV_LOG(...)
#endif
#endif

// ---------------------------------------------------------------------------
// DI registers (YAGCD), matching the layout used in flippy_sync.c / gcode.c
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

// DI command opcodes.
#define DVD_OEM_INQUIRY              0x12000000
#define DVD_OEM_READ                 0xA8000000
#define DVD_FLIPPY_FILEAPI_BASE      0xB5000000
#define DVD_FLIPPY_BYPASS            0xDC000000

// Bootloader "boot into firmware" door-knocks. These ride on the inquiry
// opcode with magic arguments; see dvd_bootloader_* in flippy_sync.c. They are
// reimplemented here rather than called because only the cubeboot.dol copy of
// flippy_sync.c actually defines them -- the IPL copy declares them and stops
// there, so calling them would not link.
#define FLIPPY_BOOT_MAGIC0           0xabadbeef
#define FLIPPY_BOOT_MAGIC1           0xcafe6969 // FLIPPY_MODE_BOOT
#define FLIPPY_NOUPDATE_MAGIC1       0xdecaf420 // FLIPPY_MODE_NOUPDATE

// Inquiry release-date signatures, from Swiss's deviceHandler_Flippy_test().
// The FlippyDrive answers with one of two dates depending on what is running:
#define FLIPPY_REL_DATE_BOOTLOADER   0x20220420
#define FLIPPY_REL_DATE_FIRMWARE     0x20220426

// ...and these are the stock GameCube/Panasonic drive dates. A FlippyDrive in
// bypass (serving the physical disc, or emulating one) answers with the drive
// it is impersonating, so seeing one of these is NOT proof there is no
// FlippyDrive -- it is the cue to exit bypass and ask again.
static const u32 stock_rel_dates[] = {
	0x20010608, 0x20010831, 0x20020402, 0x20020823,
};

// The GC Loader (and compatible ODEs) answer the same inquiry with this fixed
// date. Recognised only so we can stop early instead of throwing bypass-exit
// commands at a drive we already know is something else. See gcode.c.
#define GCODE_REL_DATE               0x20196c64

static int has_fdrv = -1; // -1 = not probed yet, 0 = absent, 1 = present
static flippy_version_parts_t fdrv_fw_ver = { 0 };

static void fdrv_di_prepare(void) {
	// Mask/ack interrupts and clear any pending cover interrupt so the transfer
	// can be polled to completion, exactly as flippy_sync.c does.
	_di_regs[DI_SR] = (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINT | DI_SR_DEINTMASK);
	_di_regs[DI_CVR] = 0;
}

// Spin until the command finishes. Unbounded, like the original FlippyDrive
// code: by the time anything but the probe runs we have already identified the
// drive, and a bounded wait would risk cutting short a legitimately long
// transfer (a multi-megabyte DOL section, a game's FST).
static int fdrv_wait(void) {
	while (_di_regs[DI_CR] & DI_CR_TSTART)
		;
	return (_di_regs[DI_SR] & DI_SR_DEINT) ? 1 : 0;
}

// Bounded variant, for the probe only. Nothing has identified the hardware yet
// at that point: with no ODE at all on the DI bus (PicoBoot into a bare
// console) the transfer can simply never complete, and hanging here would take
// the whole boot down before any other storage device got a chance. Returns
// non-zero on error OR timeout, so a silent bus reads as "not a FlippyDrive".
static int fdrv_wait_bounded(void) {
	u32 timeout = 1000000;
	while ((_di_regs[DI_CR] & DI_CR_TSTART) && --timeout)
		;
	if (timeout == 0)
		return 1;
	return (_di_regs[DI_SR] & DI_SR_DEINT) ? 1 : 0;
}

// Immediate (no DMA) command with two argument words.
static int fdrv_cmd(u32 cmd, u32 arg0, u32 arg1, bool bounded) {
	fdrv_di_prepare();

	_di_regs[DI_CMDBUF0] = cmd;
	_di_regs[DI_CMDBUF1] = arg0;
	_di_regs[DI_CMDBUF2] = arg1;

	_di_regs[DI_MAR] = 0;
	_di_regs[DI_LENGTH] = 0;
	_di_regs[DI_CR] = DI_CR_TSTART;

	return bounded ? fdrv_wait_bounded() : fdrv_wait();
}

// DMA command. `dst` must be 32-byte aligned and `len` a multiple of 32.
static int fdrv_dma(u32 cmd, u32 arg0, u32 arg1, void *buf, u32 len, bool to_drive) {
	fdrv_di_prepare();

	_di_regs[DI_CMDBUF0] = cmd;
	_di_regs[DI_CMDBUF1] = arg0;
	_di_regs[DI_CMDBUF2] = arg1;

	if (to_drive) {
		// Push our copy out of dcache so the DMA engine reads what we wrote.
		DCFlushRange(buf, len);
	} else {
		// DI DMA writes straight to RAM behind the CPU's back. Drop any cache
		// lines covering the destination BEFORE the transfer, so a later
		// eviction of a dirty line cannot clobber the DMA'd data. Same defect
		// (and same fix) as the GC Loader read path -- see gcode.c.
		DCInvalidateRange(buf, len);
	}

	_di_regs[DI_MAR] = (u32)buf & 0x1FFFFFFF; // cached -> physical
	_di_regs[DI_LENGTH] = len;
	_di_regs[DI_CR] = (to_drive ? (DI_CR_RW | DI_CR_DMA | DI_CR_TSTART)
	                            : (DI_CR_DMA | DI_CR_TSTART));

	int ret = fdrv_wait();

	if (!to_drive)
		DCInvalidateRange(buf, len);

	return ret;
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

static GCN_ALIGNED(dvd_info_t) probe_info;

static int fdrv_inquiry(void) {
	// Flush, not just invalidate, so the zeroes actually reach RAM: the DMA
	// destination is read by the drive-side engine, not by us. Invalidating
	// alone would discard the memset's dirty lines and leave RAM holding the
	// *previous* inquiry -- which on a re-probe is the answer we are trying to
	// replace. (DCFlushRange also invalidates, so this covers the pre-transfer
	// invalidate the read path needs.)
	memset(&probe_info, 0, sizeof(probe_info));
	DCFlushRange(&probe_info, sizeof(probe_info));

	fdrv_di_prepare();

	_di_regs[DI_CMDBUF0] = DVD_OEM_INQUIRY;
	_di_regs[DI_CMDBUF1] = 0;
	_di_regs[DI_CMDBUF2] = 0;

	_di_regs[DI_MAR] = (u32)&probe_info & 0x1FFFFFFF;
	_di_regs[DI_LENGTH] = sizeof(probe_info);
	_di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

	int ret = fdrv_wait_bounded();

	DCInvalidateRange(&probe_info, sizeof(probe_info));

	return ret;
}

static bool is_stock_rel_date(u32 rel_date) {
	for (unsigned i = 0; i < sizeof(stock_rel_dates) / sizeof(stock_rel_dates[0]); i++) {
		if (rel_date == stock_rel_dates[i])
			return true;
	}
	return false;
}

// Number of inquiry attempts while waiting for the bootloader to hand over to
// the firmware, and the settle delay between them.
#define FDRV_BOOT_ATTEMPTS   60
#define FDRV_BOOT_SETTLE_MS  10 // up to ~600ms total

bool fdrv_probe(void) {
	if (has_fdrv >= 0)
		return has_fdrv == 1; // already determined

	if (fdrv_inquiry() != 0) {
		// No answer at all. Two very different situations look identical here:
		// there is no ODE on the bus (PicoBoot into a bare console), or there
		// is one that has not finished waking up -- on a cold boot the drive
		// has just been read by BS2 and needs a moment before it answers again.
		//
		// So this is the one outcome we deliberately do NOT cache. Caching it
		// is what made the GC Loader boot to a black screen: one too-early
		// probe marked the drive permanently absent. Leaving has_fdrv at -1
		// means the caller's own retry loop (the settle loop in main.c, or the
		// IPL's mount retry) re-probes for free, and nothing else pays for it
		// -- once any device mounts, flippy_emu_mount() stops probing entirely.
		FDRV_LOG("fdrv: no inquiry response\n");
		return false;
	}

	FDRV_LOG("fdrv: inquiry rev=%04x dev=%04x date=%08x\n",
	         probe_info.rev_level, probe_info.dev_code, probe_info.rel_date);

	if (probe_info.rel_date == GCODE_REL_DATE) {
		// A GC Loader. Leave it alone -- gcode.c handles it as a block device.
		// A definite answer, so cache it and never probe again.
		has_fdrv = 0;
		return false;
	}

	if (is_stock_rel_date(probe_info.rel_date)) {
		// Either a genuine optical drive, or a FlippyDrive currently pretending
		// to be one. Ask it to leave bypass and inquire again: a real drive
		// ignores the command and keeps reporting its stock date, a FlippyDrive
		// comes back as itself. This is the case that matters most for us --
		// cubiboot is routinely launched as a disc image, which is exactly when
		// the drive is in bypass.
		FDRV_LOG("fdrv: stock drive date, trying bypass exit\n");
		fdrv_cmd(DVD_FLIPPY_BYPASS, FD_BYPASS_EXIT_MAGIC0, FD_BYPASS_EXIT_MAGIC1, true);

		if (fdrv_inquiry() != 0)
			return false; // uncached, as above

		FDRV_LOG("fdrv: post-bypass date=%08x\n", probe_info.rel_date);
	}

	if (probe_info.rel_date == FLIPPY_REL_DATE_BOOTLOADER) {
		// Sitting in its bootloader. Tell it to boot the firmware and to skip
		// the update prompt, then wait for the firmware's inquiry date.
		FDRV_LOG("fdrv: bootloader, booting firmware\n");
		fdrv_cmd(DVD_OEM_INQUIRY, FLIPPY_BOOT_MAGIC0, FLIPPY_BOOT_MAGIC1, true);
		fdrv_cmd(DVD_OEM_INQUIRY, FLIPPY_BOOT_MAGIC0, FLIPPY_NOUPDATE_MAGIC1, true);

		int attempt;
		for (attempt = 0; attempt < FDRV_BOOT_ATTEMPTS; attempt++) {
			if (fdrv_inquiry() == 0 && probe_info.rel_date == FLIPPY_REL_DATE_FIRMWARE)
				break;
			fdrv_settle_ms(FDRV_BOOT_SETTLE_MS);
		}

		if (attempt == FDRV_BOOT_ATTEMPTS) {
			FDRV_LOG("fdrv: firmware never came up\n");
			return false;
		}
	}

	if (probe_info.rel_date == FLIPPY_REL_DATE_FIRMWARE) {
		fdrv_fw_ver = probe_info.fw_ver;
		has_fdrv = 1;

		FDRV_LOG("fdrv: FlippyDrive firmware %d.%d.%d\n",
		         fdrv_fw_ver.major, fdrv_fw_ver.minor, fdrv_fw_ver.build);

		return true;
	}

	if (is_stock_rel_date(probe_info.rel_date)) {
		// Still reporting a stock drive after being asked to leave bypass, so
		// it really is an ordinary optical drive. Cache it.
		has_fdrv = 0;
		return false;
	}

	// It answered, but with an identity we don't recognise. Treat that the same
	// as no answer and leave it uncached: a drive that is still coming up is far
	// more likely than a brand-new ODE, and getting this wrong costs a boot
	// (see the no-response case above) while getting it "wrong" the other way
	// costs one more inquiry.
	FDRV_LOG("fdrv: unrecognised drive, will re-probe\n");
	return false;
}

bool fdrv_present(void) {
	return has_fdrv == 1;
}

const flippy_version_parts_t *fdrv_version(void) {
	return has_fdrv == 1 ? &fdrv_fw_ver : NULL;
}

// ---------------------------------------------------------------------------
// File API
// ---------------------------------------------------------------------------

static int fdrv_open_common(u32 ipc_cmd, const char *path, uint8_t type, uint8_t flags) {
	GCN_ALIGNED(file_entry_t) entry;

	memset(&entry, 0, sizeof(entry));
	strncpy(entry.name, path, sizeof(entry.name) - 1);
	entry.name[sizeof(entry.name) - 1] = 0;
	entry.type = type;
	entry.flags = flags;

	return fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | ipc_cmd, 0, 0,
	                &entry, sizeof(entry), true);
}

int fdrv_open(const char *path, uint8_t type, uint8_t flags) {
	return fdrv_open_common(IPC_FILE_OPEN, path, type, flags);
}

int fdrv_open_flash(const char *path, uint8_t type, uint8_t flags) {
	return fdrv_open_common(IPC_FILE_OPEN_FLASH, path, type, flags);
}

int fdrv_status(file_status_t *dst) {
	int ret = fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | IPC_READ_STATUS, 0, 0,
	                   dst, sizeof(file_status_t), false);

	// Mark the failure in the blob as well as in the return value. Plenty of
	// callers only look at ->result, and a transfer that never happened leaves
	// whatever the buffer held before -- which for a static status buffer is
	// the *previous* file's successful result. Note this has to happen after
	// the transfer, not before: fdrv_dma() invalidates the destination's cache
	// lines on the way in, so anything written beforehand is simply discarded.
	if (ret != 0)
		dst->result = 1;

	return ret;
}

void fdrv_close(uint32_t fd) {
	fdrv_cmd(DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_CLOSE | ((fd & 0xFF) << 16), 0, 0, false);
}

void fdrv_set_default_fd(uint32_t current_fd, uint32_t second_fd) {
	fdrv_cmd(DVD_FLIPPY_FILEAPI_BASE | IPC_SET_DEFAULT_FD
	             | ((current_fd & 0xFF) << 16) | ((second_fd & 0xFF) << 8),
	         0, 0, false);
}

int fdrv_read(void *dst, unsigned int len, uint64_t offset, unsigned int fd) {
	if (offset >> 2 > 0xFFFFFFFF)
		return -1;

	// The read command takes the offset in 32-bit words, and fd 0 means "the
	// default fd" -- which is how a game boots straight off the drive: open the
	// ISO, make it the default, and every fd-0 read behaves like a disc read.
	return fdrv_dma(DVD_OEM_READ | ((fd & 0xFF) << 16),
	                (u32)(offset >> 2), len, dst, len, false);
}

int fdrv_threaded_read(void *dst, unsigned int len, uint64_t offset, unsigned int fd) {
#ifdef IPL_CODE
	if (offset >> 2 > 0xFFFFFFFF)
		return -1;

	fdrv_di_prepare();

	_di_regs[DI_CMDBUF0] = DVD_OEM_READ | ((fd & 0xFF) << 16);
	_di_regs[DI_CMDBUF1] = (u32)(offset >> 2);
	_di_regs[DI_CMDBUF2] = len;

	DCInvalidateRange(dst, len);

	_di_regs[DI_MAR] = (u32)dst & 0x1FFFFFFF;
	_di_regs[DI_LENGTH] = len;
	_di_regs[DI_CR] = (DI_CR_DMA | DI_CR_TSTART);

	// Same transfer as fdrv_read(), but yield instead of busy-waiting: this is
	// the path the menu's file-enumeration thread uses, and spinning here would
	// starve the boot animation running alongside it.
	while (_di_regs[DI_CR] & DI_CR_TSTART)
		OSYieldThread();

	DCInvalidateRange(dst, len);

	return (_di_regs[DI_SR] & DI_SR_DEINT) ? 1 : 0;
#else
	return fdrv_read(dst, len, offset, fd);
#endif
}

int fdrv_readdir(file_entry_t *dst, uint32_t fd) {
	return fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_READDIR | ((fd & 0xFF) << 16),
	                0, 0, dst, sizeof(file_entry_t), false);
}

static int fdrv_path_cmd(u32 ipc_cmd, const char *path) {
	GCN_ALIGNED(file_entry_t) entry;

	memset(&entry, 0, sizeof(entry));
	strncpy(entry.name, path, sizeof(entry.name) - 1);
	entry.name[sizeof(entry.name) - 1] = 0;

	return fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | ipc_cmd, 0, 0,
	                &entry, sizeof(entry), true);
}

int fdrv_mkdir(const char *path) {
	return fdrv_path_cmd(IPC_FILE_MKDIR, path);
}

int fdrv_unlink(const char *path) {
	return fdrv_path_cmd(IPC_FILE_UNLINK, path);
}

int fdrv_unlink_flash(const char *path) {
	return fdrv_path_cmd(IPC_FILE_UNLINK_FLASH, path);
}

int fdrv_write(const char *buf, uint32_t offset, uint32_t length, uint32_t fd) {
	return fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | IPC_FILE_WRITE | ((fd & 0xFF) << 16),
	                offset, length, (void *)buf, (length + 31) & 0xFFFFFFE0, true);
}

int fdrv_fs_info(fs_info_t *dst) {
	int ret = fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | IPC_FS_INFO, 0, 0,
	                   dst, sizeof(fs_info_t), false);

	if (ret != 0) // see fdrv_status()
		dst->result = 1;

	return ret;
}

int fdrv_presence(bool playing, const char *status, const char *sub_status) {
	GCN_ALIGNED(flippydrive_net_presence_t) presence;

	memset(&presence, 0, sizeof(presence));
	strncpy(presence.status, status, sizeof(presence.status) - 1);
	strncpy(presence.sub_status, sub_status, sizeof(presence.sub_status) - 1);
	presence.presence = playing ? 0x01 : 0x00;

	return fdrv_dma(DVD_FLIPPY_FILEAPI_BASE | IPC_NET_PRESENCE, 0, 0,
	                &presence, sizeof(presence), true);
}

void fdrv_bypass_enter(void) {
	fdrv_cmd(DVD_FLIPPY_BYPASS, 0, 0, false);
}

void fdrv_bypass_exit(void) {
	fdrv_cmd(DVD_FLIPPY_BYPASS, FD_BYPASS_EXIT_MAGIC0, FD_BYPASS_EXIT_MAGIC1, false);
}
