#include "../flippy_sync.h"

#include <string.h>
#include "ffs/ff.h"
#include "fdrv.h"
#include "tweaks.h"

#ifdef IPL_CODE
#include "../dvd_threaded.h"
#include "../reloc.h"
#include "../attr.h"
#include "../gc_dvd.h"
#include "../time.h"   // udelay, for the mount settle-retry below
#include "config.h"
#else
#include <stdio.h>
#include <di/di.h>
#include "../config.h"
#endif


static FATFS fs;

#ifdef IPL_CODE
__attribute_data__ int emu_sd_device;
#else
int emu_sd_device = -1;
#endif
// Storage devices tried in order; the first one that comes up wins.
//
// "fldrv" is special and must stay at index 0. It is a real FlippyDrive on the
// disc interface (DI) bus, which is the hardware cubeboot was originally
// written for: it serves files over a DI command protocol rather than as a
// block device, so it has no FatFs volume and never goes near f_mount. It gets
// probed first because when one is present it is unambiguously the device the
// user means -- and because everything downstream (the drive's internal flash,
// real file descriptors, booting a game through the drive instead of
// chainloading Swiss) is only available on that path.
//
// The rest are block devices behind FatFs: the GC Loader's SD card, also on the
// DI bus, then SD adapters on the EXI bus (SD2SP2 / SD Gecko), then IDE-EXI
// (ATA-over-EXI) drives. Their names must match both the FatFs volume strings
// (FF_VOLUME_STRS in ffconf.h) and Swiss's device names (used for the Swiss
// Autoload= handoff). Reorder those if a different device should take priority
// on your setup.
//
// "fldrv" is likewise Swiss's name for the FlippyDrive (deviceHandler-
// flippydrive.c), so the Autoload= handoff needs no special case.
#define DEVICE_FLIPPY 0
static const char* device_prio[] = { "fldrv", "gcldr", "sdc", "sdb", "sda", "ataa", "atab", "atac" };

static bool passthrough = false;

// Non-IPL (cubeboot.dol) gecko diagnostics. iprintf resolves to cubeboot's
// gecko printf (print.c). Compiled out in the IPL build, which has no iprintf.
#if !defined(IPL_CODE) && defined(GECKO_PRINT_ENABLE)
#define EMU_DBG(...) iprintf(__VA_ARGS__)
#else
#define EMU_DBG(...)
#endif

const char* emu_get_device() {
	return emu_sd_device < 0 ? NULL : device_prio[emu_sd_device];
}

// True once a real FlippyDrive has been found and adopted. Every dvd_custom_*
// entry point below forks on this: native DI file API, or FatFs emulation.
bool emu_is_native() {
	return emu_sd_device == DEVICE_FLIPPY;
}

// Bring device `i` up: identify a FlippyDrive, or mount a FAT volume.
static bool device_bring_up(int i) {
	if (i == DEVICE_FLIPPY) {
		// The inquiry is the whole test, as it is in Swiss: if a FlippyDrive
		// answers, it is the device the user means, and we adopt it without
		// checking that a filesystem is mounted on it. Deliberately -- a drive
		// with no SD card still has its internal flash, which on a FlippyDrive
		// install is where cubiboot's own files live. Falling through to an SD
		// Gecko while a FlippyDrive sits in the machine would be the wrong
		// answer far more often than the right one.
		return fdrv_probe();
	}

	static char mount_path[256];
	memcpy(mount_path, device_prio[i], strlen(device_prio[i]) + 1);
	strcat(mount_path, ":");

	return f_mount(&fs, mount_path, 1) == FR_OK;
}

bool flippy_emu_mount() {
	#ifdef IPL_CODE

	static bool mounted = false;
	if (mounted)
		return true;

	int num_devices = sizeof(device_prio) / sizeof(device_prio[0]);

	// Retry the device cubeboot.dol already found -- and ONLY that device.
	//
	// Do NOT probe the rest of the chain from here. device_prio contains the
	// IDE-EXI entries, and "ataa" is EXI channel 0 device 0 -- the very same port
	// an SD card in memory-card slot A sits on. Probing ATA there pushes ATA
	// register writes at the SD card. A gecko trace of a PicoBoot + SD-slot-A
	// boot showed the logo's mount attempt doing exactly that:
	//     ATA init chn=0: status=ff / ATA chn=0: BSY never cleared (no drive)
	// three times over, before giving up -- slow, and poking the card we are
	// actually trying to read.
	//
	// cubeboot.dol has already probed and told us the answer via emu_sd_device,
	// so the IPL side only needs to wait for that one device to come up. This is
	// the first storage access on the IPL side (its own FATFS, its own driver
	// state), an EXI SD can need a moment, and load_cube_logo() gets a single
	// shot before the enumeration thread takes over the DVD interface.
	if (emu_sd_device >= 0 && emu_sd_device < num_devices) {
		for (int attempt = 0; attempt < 30; attempt++) {
			if (device_bring_up(emu_sd_device)) {
				OSReport("emu: IPL mounted %s after %d retries\n", device_prio[emu_sd_device], attempt);
				mounted = true;
				emu_update_boot();
				return true;
			}
			udelay(10 * 1000); // 10ms; up to ~300ms total
		}

		OSReport("emu: IPL could not mount %s\n", device_prio[emu_sd_device]);
		return false;
	}

	// Only reached if cubeboot.dol never identified a device. Probe the chain as
	// a last resort; the ATA caveat above applies, but a boot with no storage at
	// all is worse.
	for (int i = 0; i < num_devices; i++) {
		if (!device_bring_up(i))
			continue;

		OSReport("emu: IPL mounted %s (probed; cubeboot.dol had none)\n", device_prio[i]);
		emu_sd_device = i;
		mounted = true;
		emu_update_boot();
		return true;
	}

	OSReport("emu: IPL failed to mount any device\n");
	return false;

	#else

	if (emu_sd_device < 0) {
		int num_devices = sizeof(device_prio) / sizeof(device_prio[0]);
		for (int i = 0; i < num_devices; i++) {
			EMU_DBG("mount try %s ...\n", device_prio[i]);
			if (device_bring_up(i)) {
				EMU_DBG("mount %s OK\n", device_prio[i]);
				emu_sd_device = i;
				return true;
			}
			EMU_DBG("mount %s fail\n", device_prio[i]);
		}
		return false;
	}
	return true;

	#endif
}

static FIL file;
static FFDIR dir;

int dvd_custom_open(const char* path, uint8_t type, uint8_t flags) {
	if (!flippy_emu_mount())
		return 1;

	if (emu_is_native()) {
		// No implicit close here, unlike the emulation below: the drive hands
		// out real file descriptors and callers rely on holding more than one
		// at a time (chainload_boot_game() opens a game and its second disc,
		// then makes them the default fds).
		return fdrv_open(path, type, flags);
	}

	dvd_custom_close(1);

	char dev_path[256];
	memcpy(dev_path, emu_get_device(), strlen(emu_get_device()) + 1);
	strcat(dev_path, ":");
	strcat(dev_path, path);

	if (type == FILE_ENTRY_TYPE_DIR) {
		return f_opendir(&dir, dev_path) == FR_OK ? 0 : 1;
	}

	if (type == FILE_ENTRY_TYPE_FILE) {
		int ffs_flags = FA_READ;
		if (flags & IPC_FILE_FLAG_WRITE)
			ffs_flags |= FA_WRITE | FA_OPEN_ALWAYS;

		return f_open(&file, dev_path, ffs_flags) == FR_OK ? 0 : 1;
	}

	return 1;
}

int dvd_custom_open_flash(const char *path, uint8_t type, uint8_t flags) {
	if (type != FILE_ENTRY_TYPE_FILE)
		return 1;

	if (flippy_emu_mount() && emu_is_native()) {
		// The real thing: a small FAT12 volume inside the drive itself, which
		// is where cubeboot's own assets (stub.bin, swiss-gc.dol, apploader.img)
		// live on a FlippyDrive install. Fall back to the SD card if the file
		// isn't in flash, so a half-populated flash still boots.
		//
		// The open command's own return only tells us the transfer completed;
		// whether the file was actually found is in the status blob, which is
		// what every caller goes on to read anyway.
		if (fdrv_open_flash(path, type, flags) == 0) {
			GCN_ALIGNED(file_status_t) st;
			if (fdrv_status(&st) == 0 && st.result == 0)
				return 0;
		}

		return fdrv_open(path, type, flags);
	}

	char flash_path[256];
	strcpy(flash_path, "/cubiboot");
	strcat(flash_path, path);
	if (dvd_custom_open(flash_path, type, flags) == 0)
		return 0;

	return dvd_custom_open(path, type, flags);
}

#ifdef IPL_CODE
static GCN_ALIGNED(file_status_t) _status;
file_status_t* dvd_custom_status() {
	file_status_t* status = &_status;
#else
int dvd_custom_status(file_status_t* status) {
#endif
	if (emu_is_native()) {
		// The drive fills in the whole blob itself: result, real fd, and a
		// byte-swapped size (which is what every caller here already expects --
		// the emulation below byte-swaps to match it).
		int ret = fdrv_status(status);
		#ifdef IPL_CODE
		return ret == 0 ? status : NULL;
		#else
		return ret;
		#endif
	}

	memset(status, 0, sizeof(file_status_t));
	status->fd = 1;

	if (file.obj.fs == NULL && dir.obj.fs == NULL) {
		status->result = 1;
		status->fsize = 0;
		#ifdef IPL_CODE
		return status;
		#else
		return 0;
		#endif
	}
	
	status->result = 0;
	status->fsize = __builtin_bswap64(f_size(&file));
	#ifdef IPL_CODE
	return status;
	#else
	return 0;
	#endif
}

int dvd_read(void* dst, unsigned int len, uint64_t offset, unsigned int fd) {
	if (passthrough) {
		extern int normal_dvd_read(void* dst, unsigned int len, uint64_t offset, unsigned int fd);
		return normal_dvd_read(dst, len, offset, fd);
	}

	// Native reads go over the same DI read command as a disc read, with the fd
	// selecting the open file (or the default fd, when 0). No `passthrough`
	// special case is needed on this path: in bypass the drive itself decides
	// what fd 0 means.
	if (emu_is_native())
		return fdrv_read(dst, len, offset, fd);

	FRESULT res;
	UINT bytes_read;
	
	res = f_lseek(&file, offset);
	if (res != FR_OK) {
		return 1;
	}
	
	res = f_read(&file, dst, len, &bytes_read);
	if (res != FR_OK) {
		return 1;
	}
	
	return 0;
}

int dvd_threaded_read(void* dst, unsigned int len, uint64_t offset, unsigned int fd) {
	if (!passthrough && emu_is_native())
		return fdrv_threaded_read(dst, len, offset, fd);

	return dvd_read(dst, len, offset, fd);
}

int dvd_custom_readdir(file_entry_t* dst, unsigned int fd) {
	if (emu_is_native())
		return fdrv_readdir(dst, fd);

	FILINFO fno;
	FRESULT res;

	fno.fname[0] = 0;
	res = f_readdir(&dir, &fno);
	if (res != FR_OK)
		return 1;
	
	if (fno.fname[0] == 0) {
		dst->name[0] = 0;
		return 0;
	}

	strcpy(dst->name, fno.fname);
	dst->type = (fno.fattrib & AM_DIR) ? FILE_ENTRY_TYPE_DIR : FILE_ENTRY_TYPE_FILE;
	dst->size = fno.fsize;
	dst->attrib = fno.fattrib;
	
	return 0;
}

int dvd_custom_mkdir(char* path) {
	if (!flippy_emu_mount())
		return 1;

	if (emu_is_native())
		return fdrv_mkdir(path);

	return f_mkdir(path) == FR_OK ? 0 : 1;
}

void dvd_custom_close(uint32_t fd) {
	if (emu_is_native()) {
		fdrv_close(fd);
		return;
	}

	f_close(&file);
	f_closedir(&dir);
}

void dvd_custom_bypass_enter() {
	passthrough = true;

	// On a FlippyDrive, bypass is a command to the drive: it stops emulating
	// and puts the physical disc on the bus. Everywhere else there is nothing
	// to hand over -- the DI bus already belongs to whatever drive is there --
	// so all we can do is reset it and stop intercepting reads.
	if (emu_is_native()) {
		fdrv_bypass_enter();
		return;
	}

	#ifdef IPL_CODE
	dvd_reset();
	#else
	DI_Reset();
	#endif
}

void dvd_custom_bypass() {
	dvd_custom_bypass_enter();
}

void dvd_custom_bypass_exit() {
	if (emu_is_native()) {
		fdrv_bypass_exit();
		passthrough = false;
		return;
	}

	#ifdef IPL_CODE
	dvd_stop_motor();
	#else
	DI_StopMotor();
	#endif
	passthrough = false;
}


// Everything below is native-only: the FatFs emulation has no equivalent (or,
// for writes, deliberately doesn't offer one), so it keeps returning failure.
int dvd_custom_write(char *buf, uint32_t offset, uint32_t length, uint32_t fd) {
	if (emu_is_native())
		return fdrv_write(buf, offset, length, fd);

	return 1;
}

void dvd_set_default_fd(uint32_t current_fd, uint32_t second_fd) {
	// This is what makes a game boot through the drive rather than through
	// Swiss: the opened ISO becomes what fd 0 reads, so the console's own disc
	// code works unmodified. There is no way to emulate it over FatFs.
	if (emu_is_native())
		fdrv_set_default_fd(current_fd, second_fd);
}

int dvd_custom_unlink(char *path) {
	if (emu_is_native())
		return fdrv_unlink(path);

	return 1;
}

int dvd_custom_unlink_flash(char *path) {
	if (emu_is_native())
		return fdrv_unlink_flash(path);

	return 1;
}

int dvd_custom_presence(bool playing, const char *status, const char *sub_status) {
	if (emu_is_native())
		return fdrv_presence(playing, status, sub_status);

	return 1;
}

int dvd_custom_fs_info(fs_info_t* status) {
	if (emu_is_native())
		return fdrv_fs_info(status);

	return 1;
}
