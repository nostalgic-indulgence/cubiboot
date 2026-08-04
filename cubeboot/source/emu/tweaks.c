#include "tweaks.h"

#include <ogc/gx.h>
#include <string.h>
#include "../flippy_sync.h"

#ifndef IPL_CODE
#include <gccore.h>
#include "../descrambler.h"
#include "../ipl.h"
#include "../boot/ssaram.h"
#include "../crc32.h"
#include "../sd.h"
#else
#include "../gc_dvd.h"
#include "../time.h"
#include "../attr.h"
#include "../dolphin_arq.h"
#include "../usbgecko.h"
#endif


#ifdef IPL_CODE
static bool found_swiss = false;

void emu_update_boot() {
    dvd_custom_open_flash("/swiss-gc.dol", FILE_ENTRY_TYPE_FILE, 0);
    file_status_t* status = dvd_custom_status();
    found_swiss = (status != NULL && status->result == 0);
    // Guard the close: dvd_custom_status() can now genuinely return NULL, which
    // it never did while every device went through the FatFs emulation.
    if (status != NULL)
        dvd_custom_close(status->fd);
}

bool emu_can_boot(gm_file_type_t type) {
    switch (type) {
        case GM_FILE_TYPE_GAME: {
            // A real FlippyDrive serves the ISO as a disc and boots it itself
            // (chainload_boot_game), so Swiss is only in the picture when the
            // user has forced that route with force_swiss_boot.
            extern u32 force_swiss_boot;
            if (emu_is_native() && !force_swiss_boot)
                return true;

            return found_swiss;
        }
        case GM_FILE_TYPE_PROGRAM:
            // DOLs always go through Swiss -- see bs2start(), which notes that
            // loading one directly is only partially working.
            return found_swiss;
        default:
            return true;
    }
}

extern void draw_info_box(u16 width, u16 height, u16 center_x, u16 center_y, u8 alpha, GXColor* top_color, GXColor* bottom_color);
extern void draw_text(char* s, s16 size, u16 x, u16 y, GXColor* color);

void emu_draw_boot_error(gm_file_type_t type, u8 ui_alpha) {
    GXColor white = {0xFF, 0xFF, 0xFF, ui_alpha};
    GXColor error_top = {0xd4, 0x2c, 0x2c, 0xc8};
    GXColor error_bottom = {0x8b, 0x00, 0x50, 0xb4}; 

    draw_info_box(0x1A00, 0x7E0, 0x1230, 0x6F0, ui_alpha, &error_top, &error_bottom);
    draw_text("File not found: swiss-gc.dol", 24, 170, 100 - 20, &white);
    draw_text("Download swiss, rename to swiss-gc.dol", 18, 170, 155 - 20, &white);
    draw_text("and place it on the SD card", 18, 170, 190 - 20, &white);
}

bool emu_has_dvd() {
    if (emu_is_native()) {
        // A FlippyDrive is not showing us the physical disc right now -- it is
        // busy being our filesystem. Hand the bus over, look, and hand it back
        // either way: the menu we return to still needs to read from the drive,
        // and bs2start() enters bypass again for real if the user boots the
        // disc. Skipping the restore leaves the menu with no storage at all.
        dvd_custom_bypass_enter();
        udelay(10 * 1000);

        bool found = (dvd_read_id() == 0 && dvd_get_error() == 0);
        if (!found)
            dvd_stop_motor();

        dvd_custom_bypass_exit();
        udelay(10 * 1000);

        return found;
    }

    dvd_reset();
    udelay(10 * 1000);

    if (dvd_read_id() != 0 || dvd_get_error() != 0) {
        dvd_stop_motor();
        return false;
    }

    return true;
}


static volatile bool bnr_store_bsy = false;
static void bnr_cache_store_cb(u32 arq_request_ptr) {
    bnr_store_bsy = false;
}

void bnr_cache_store(BNR* bnr, u32 aram_offset) {
    custom_OSReport("Store banner at: 0x%x\n", aram_offset);
    static ARQRequest req;
    u32 owner = make_type('I', 'X', 'X', 'S');
    u32 type = ARAM_DIR_MRAM_TO_ARAM;
    u32 priority = ARQ_PRIORITY_LOW;
    u32 source = (u32)bnr;
    u32 dest = aram_offset;
    u32 length = sizeof(BNR);

    bnr_store_bsy = true;
    DCFlushRange(bnr, sizeof(BNR));
    dolphin_ARQPostRequest(&req, owner, type, priority, source, dest, length, &bnr_cache_store_cb);
    while (bnr_store_bsy)
        OSYieldThread();
}

static volatile bool bnr_load_bsy = false;
static void bnr_cache_load_cb(u32 arq_request_ptr) {
    bnr_load_bsy = false;
}

void bnr_cache_load(BNR* bnr, u32 aram_offset) {
    custom_OSReport("Load banner from: 0x%x\n", aram_offset);
    static ARQRequest req;
    u32 owner = make_type('I', 'X', 'X', 'L');
    u32 type = ARAM_DIR_ARAM_TO_MRAM;
    u32 priority = ARQ_PRIORITY_LOW;
    u32 source = aram_offset;
    u32 dest = (u32)bnr;
    u32 length = sizeof(BNR);

    bnr_load_bsy = true;
    dolphin_ARQPostRequest(&req, owner, type, priority, source, dest, length, &bnr_cache_load_cb);
    while (bnr_load_bsy)
        OSYieldThread();
    DCFlushRange(bnr, sizeof(BNR));
}

#define BNR_CACHE_SIZE 1024

// Keyed on `key`, a hash of the file's full path (gm_bnr_cache_key() in games.c).
//
// This used to be keyed on the 6-byte game ID instead, which is NOT unique per
// file: every build of cubiboot.iso is "GBLPGL", and homebrew ISOs in general
// reuse a handful of generic IDs. gm_warm_files() walks the whole card populating
// this cache first-seen-wins, and gm_load_banner() checks it before reading the
// file -- so one stale copy of a game anywhere on the card handed its banner to
// every other file sharing that ID. The tell was a banner whose desc text looked
// right while the image was wrong or blank: the text is memcpy'd from RAM but the
// pixels come back through ARAM, so both came from the cached stranger.
//
// The path hash means two copies of the same game now take two slots rather than
// sharing one. That is the intended trade: 1024 slots wrap, and a duplicate
// costing ARAM is much cheaper than a duplicate showing the wrong art.
typedef struct {
    u32 key;
    u32 aram_offset;
    bool valid;
} bnr_cache_entry_t;

static bnr_cache_entry_t bnr_cache[BNR_CACHE_SIZE] = {0};
static u32 bnr_cache_next_index = 0;

bool bnr_cache_get(u32 key, BNR* bnr) {
    for (int i = 0; i < BNR_CACHE_SIZE; i++) {
        if (bnr_cache[i].valid && bnr_cache[i].key == key) {
            bnr_cache_load(bnr, bnr_cache[i].aram_offset);
            return true;
        }
    }

    return false;
}

void bnr_cache_put(u32 key, BNR* bnr) {
    for (int i = 0; i < BNR_CACHE_SIZE; i++) {
        if (bnr_cache[i].valid && bnr_cache[i].key == key) {
            return;
        }
    }

    bnr_cache_entry_t* entry = &bnr_cache[bnr_cache_next_index];
    entry->valid = false;
    entry->key = key;
    entry->aram_offset = (16 * 1024 * 1024) - (sizeof(BNR) * (bnr_cache_next_index + 1));
    bnr_cache_store(bnr, entry->aram_offset);
    entry->valid = true;

    bnr_cache_next_index = (bnr_cache_next_index + 1) % BNR_CACHE_SIZE;
}

#else

#define IPL_ROM_FONT_SJIS	0x1AFF00
#define DECRYPT_START		0x100

#define IPL_SIZE 0x200000
#define BS2_START_OFFSET 0x800
#define BS2_CODE_OFFSET (BS2_START_OFFSET + 0x20)
#define BS2_BASE_ADDR 0x81300000

extern void __SYS_ReadROM(void* buf, u32 len, u32 offset);

static u32 bs2_size = IPL_SIZE - BS2_CODE_OFFSET;
static u8 *bs2 = (u8*)(BS2_BASE_ADDR);

void ensure_ipl_loaded(uint8_t* bios_buffer) {
    ipl_metadata_t* metadata = (void*)0x81500000 - sizeof(ipl_metadata_t);
    if (metadata->magic == 0xC0DE)
        return;

    memset(metadata, 0, sizeof(ipl_metadata_t));

    const char* bios_path = "/ipl.bin";
    if (get_file_size(bios_path) != IPL_SIZE || load_file_buffer(bios_path, bios_buffer)) {
        __SYS_ReadROM(bios_buffer, IPL_SIZE, 0);
    }
    Descrambler(bios_buffer + DECRYPT_START, IPL_ROM_FONT_SJIS - DECRYPT_START);
    memcpy(bs2, bios_buffer + BS2_CODE_OFFSET, bs2_size);

    metadata->magic = 0xC0DE;
    
    u32 code_end_addr = 0;
    if (*(u32*)0x81300310 == 0x81300000)
        code_end_addr = *(u32*)0x81300324;
    else
        code_end_addr = *(u32*)0x8130039c;
    
    u32 code_size = code_end_addr - 0x81300000;
    if (code_size > IPL_SIZE)
        code_size = 0;
    metadata->code_size = code_end_addr - 0x81300000;
}

void apply_additional_patches() {
    extern s8 bios_index;
    if (bios_index < 0)
        return;

    // Disable the IPL's EXI probe for whichever slot our SD card is in.
    //
    // This matters more than it looks: an SD adapter in memory-card slot A is on
    // EXI channel 0 device 0, exactly where the IPL expects a memory card. If the
    // IPL probes that slot it drives the same EXI device we are trying to read,
    // and our SD access fails for as long as the probe is running -- which is
    // early in the IPL, right when pre_thread_init() loads the boot logo.
    //
    // These writes patch IPL *code* from the data side, so they MUST be paired
    // with a flush + icache invalidate. Without it the store can sit dirty in
    // dcache while the CPU fetches the stale instruction from RAM, and the probe
    // is never actually disabled. (It appeared to work only because the ~100KB
    // patch-copy loop that runs afterwards tends to evict the line -- luck, not
    // correctness.) Same defect and same fix as the IPL patching elsewhere.
    const char* dev = emu_get_device();
    if (dev == NULL) {
        iprintf("apply_additional_patches: no device, skipping EXI probe patch\n");
        return;
    }

    u32 addr = 0;
    u32 value = 0x38600000; // li r3, 0
    if (strcmp(dev, "sda") == 0) {
        u32 probe_card_0[] = { 0x8131b1c4, 0x8131b8f0, 0x8131bc88, 0x8131bca0, 0x8131c29c, 0x8131b81c, 0x8131c3dc };
        addr = probe_card_0[bios_index];
    } else if (strcmp(dev, "sdb") == 0) {
        u32 probe_card_1[] = { 0x8131b274, 0x8131b9a0, 0x8131bd38, 0x8131bd50, 0x8131c34c, 0x8131b8cc, 0x8131c48c };
        addr = probe_card_1[bios_index];
    } else if (strcmp(dev, "sdc") == 0) {
        u32 init_ad16_addr[] = { 0x81335f54, 0x8135b9b4, 0x8136572c, 0x81365890, 0x8135ef94, 0x8135b8d4, 0x81368c08 };
        addr = init_ad16_addr[bios_index];
        value = 0x4e800020; // blr
    }

    if (addr != 0) {
        iprintf("Disabling IPL EXI probe for %s at %08x (was %08x)\n", dev, addr, *(u32*)addr);
        *(u32*)addr = value;
        DCFlushRange((void*)addr, 4);
        ICInvalidateRange((void*)addr, 4);
    }
}

#endif
