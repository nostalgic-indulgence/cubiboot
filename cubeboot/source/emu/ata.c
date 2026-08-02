#include "ata.h"

#include <string.h>

// EXI plumbing. In the IPL build we borrow the retail IPL's EXI driver through
// the relocated function pointers (same ones tsd.c uses, defined in mcp.c); in
// the cubeboot.dol build we use libogc's EXI directly. Either way the body below
// is identical — only the names differ, so alias them.
#ifdef IPL_CODE
#include "../usbgecko.h"   // EXICallback, EXI_READ/EXI_WRITE, EXI_SPEED_*, EXI_DEVICE_0
extern s32 (*EXILock)(s32 nChn, s32 nDev, EXICallback unlockCB);
extern s32 (*EXIUnlock)(s32 nChn);
extern s32 (*EXISelect)(s32 nChn, s32 nDev, s32 nFrq);
extern s32 (*EXIDeselect)(s32 nChn);
extern s32 (*EXIImmEx)(s32 nChn, void *pData, u32 nLen, u32 nMode);
// Memory-mapped EXI registers (0xCC006800): [1]=DMA addr, [2]=DMA len, [3]=ctrl.
// Used to run the sector DMA directly, since the IPL has no relocatable EXIDma.
extern volatile u32 EXI[3][5];
extern void DCInvalidateRange(void *addr, u32 len);
#else
#include <ogc/exi.h>
#include "../config.h"   // GECKO_PRINT_ENABLE (defined here in the .dol build)
#define EXILock     EXI_Lock
#define EXIUnlock   EXI_Unlock
#define EXISelect   EXI_Select
#define EXIDeselect EXI_Deselect
#define EXIImmEx    EXI_ImmEx
#endif

// ---------------------------------------------------------------------------
// ATA task-file registers (IDE-EXI address encoding) and command/status bits.
// Values match Swiss/libogc's ata.h.
// ---------------------------------------------------------------------------
#define ATA_REG_DATA      0x10
#define ATA_REG_FEATURES  0x11
#define ATA_REG_ERROR     0x11
#define ATA_REG_SECCOUNT  0x12
#define ATA_REG_LBALO     0x13
#define ATA_REG_LBAMID    0x14
#define ATA_REG_LBAHI     0x15
#define ATA_REG_DEVICE    0x16
#define ATA_REG_COMMAND   0x17
#define ATA_REG_STATUS    0x17
#define ATA_REG_DEVCTRL   0x0E   // device control (alt status on read)

#define ATA_DCR_SRST      0x04   // soft reset
#define ATA_DCR_NIEN      0x02   // disable device interrupt

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READSECT      0x21   // LBA28 PIO
#define ATA_CMD_READSECTEXT   0x24   // LBA48 PIO

#define ATA_SR_BSY        0x80
#define ATA_SR_DRDY       0x40
#define ATA_SR_DRQ        0x08
#define ATA_SR_ERR        0x01

#define ATA_HEAD_USE_LBA  0x40

// EXI transfer speed for the IDE-EXI adapter, as the EXI frequency index
// (0=1MHz,1=2,2=4,3=8,4=16,5=32). 32 MHz matches Swiss on this setup. Drop to 4
// (16 MHz) if a particular adapter/cable shows read errors. Literal avoids the
// EXI_SPEED_16MHZ (IPL) vs EXI_SPEED16MHZ (libogc) naming split.
#define ATA_EXI_FREQ      5

// Attempts per sector. A failed attempt soft-resets the drive before the next,
// to recover from the intermittent stuck-BSY seen at 32 MHz.
#define ATA_READ_RETRIES  4

#define ATA_NUM_CHN       3   // EXI channels 0..2 (slot A, slot B, SP1)

#if defined(GECKO_PRINT_ENABLE)
  #ifdef IPL_CODE
    #define ATA_DEBUG(...) custom_OSReport(__VA_ARGS__)
  #else
    extern int iprintf(const char *fmt, ...);
    #define ATA_DEBUG(...) iprintf(__VA_ARGS__)
  #endif
#else
  #define ATA_DEBUG(...)
#endif

// Per-channel LBA48 capability, learned at init.
static bool ata_lba48[ATA_NUM_CHN];

// ---------------------------------------------------------------------------
// Low-level IDE-EXI register access. Each access is a self-contained EXI
// transaction (select -> command -> [data] -> deselect), exactly as the Swiss
// driver does it. Command byte encoding:
//   read  reg R (8-bit)  : write {R, 0x00},                 then read 1 byte
//   write reg R = V      : write {0x80|R, V, 0x00}
//   read  512-byte data  : write {0x70, 0x80, 0x00},        then read 512 bytes
// ---------------------------------------------------------------------------
// Just before a game boots, cubeboot zeroes 0x80100000-0x81600000 (which holds
// the retail IPL's EXI driver) and calls tsd_set_native(true). From then on EXI
// must be driven by raw registers, not the now-zeroed IPL functions. ata goes
// native alongside it so loading swiss-gc.dol off IDE-EXI after the wipe works.
#ifdef IPL_CODE
static bool ata_native = false;
void ata_set_native(bool native) { ata_native = native; }
#endif

static bool ata_select(exi_port port) {
#ifdef IPL_CODE
    if (ata_native) {
        EXI[port.chn][0] = (EXI[port.chn][0] & 0x405) | ((1 << port.dev) << 7) | (ATA_EXI_FREQ << 4);
        return true;
    }
#endif
    if (!EXILock(port.chn, port.dev, NULL))
        return false;
    if (!EXISelect(port.chn, port.dev, ATA_EXI_FREQ)) {
        EXIUnlock(port.chn);
        return false;
    }
    return true;
}

static void ata_deselect(exi_port port) {
#ifdef IPL_CODE
    if (ata_native) {
        EXI[port.chn][0] &= 0x405;
        return;
    }
#endif
    EXIDeselect(port.chn);
    EXIUnlock(port.chn);
}

// One immediate EXI transfer of up to 4 bytes, MSB-first. For a write, `val`
// carries the bytes in its top; a read returns them in the top.
static u32 ata_imm(exi_port port, u32 val, int len, int mode) {
#ifdef IPL_CODE
    if (ata_native) {
        EXI[port.chn][4] = val;
        EXI[port.chn][3] = ((len - 1) << 4) | (mode << 2) | 1;
        while (EXI[port.chn][3] & 1)
            ;
        return EXI[port.chn][4];
    }
#endif
    EXIImmEx(port.chn, &val, len, mode);
    return val;
}

static u8 ata_rd8(exi_port port, u8 reg) {
    if (!ata_select(port))
        return 0xFF;
    ata_imm(port, (u32)reg << 24, 2, EXI_WRITE);   // write {reg, 0x00}
    u32 r = ata_imm(port, 0, 1, EXI_READ);          // read 1 byte (MSB)
    ata_deselect(port);
    return (u8)(r >> 24);
}

static void ata_wr8(exi_port port, u8 reg, u8 val) {
    if (!ata_select(port))
        return;
    ata_imm(port, 0x80000000u | ((u32)reg << 24) | ((u32)val << 16), 3, EXI_WRITE); // {0x80|reg, val, 0}
    ata_deselect(port);
}

static bool ata_rd_buffer(exi_port port, void* dst) {
    // 32-byte aligned bounce: the V2 path DMAs into it, and it lets callers pass
    // an unaligned destination (FatFs window buffers aren't 32-byte aligned).
    static u8 buf[512] __attribute__((aligned(32)));

    if (!ata_select(port))
        return false;

    const u16 dwords = 128; // 128 * 4 = 512 bytes
    u32 cmd = 0x70000000u | ((u32)(dwords & 0xff) << 16) | ((u32)((dwords >> 8) & 0xff) << 8);
    ata_imm(port, cmd, 4, EXI_WRITE); // 4-byte burst-read command

#ifdef IPL_CODE
    // V2+ DMA via raw EXI registers (no relocatable EXIDma in the IPL). The
    // channel is already selected by ata_select; program the DMA address/length
    // and kick it. EXI DMA writes straight to physical RAM, so invalidate the
    // dcache around it (before, so a dirty eviction can't clobber the data;
    // after, so the CPU reads the fresh bytes).
    DCInvalidateRange(buf, 512);
    EXI[port.chn][1] = (u32)buf & 0x1FFFFFE0;   // DMA memory address (physical, 32B aligned)
    EXI[port.chn][2] = 512;                      // DMA length
    EXI[port.chn][3] = (1 << 1) | 1;             // control: DMA | read | start
    u32 timeout = 1000000;
    while ((EXI[port.chn][3] & 1) && --timeout)
        ;
    DCInvalidateRange(buf, 512);
    ata_deselect(port);
    if (timeout == 0)
        return false;
#else
    // V2+: single DMA transfer of the whole 512-byte sector.
    EXI_DmaEx(port.chn, buf, 512, EXI_READ);
    ata_deselect(port);
#endif

    memcpy(dst, buf, 512);
    return true;
}

// Poll the status register until (status & mask) == want, or give up. Each read
// is an EXI round-trip (~tens of us), so the iteration cap is a generous timeout
// and also bounds us if the slot holds something that isn't an IDE-EXI drive
// (status reads back 0xFF) so device probing can't hang.
static bool ata_wait(exi_port port, u8 mask, u8 want) {
    for (int i = 0; i < 100000; i++) {
        u8 st = ata_rd8(port, ATA_REG_STATUS);
        if (st == 0xFF)
            return false;            // no drive responding
        if ((st & mask) == want)
            return true;
    }
    return false;
}

// Pulse the device-control SRST bit to un-wedge a drive that's stuck BSY after a
// read that didn't complete (seen intermittently at 32 MHz). Recovers the drive
// so the next attempt starts clean, the way a real ATA driver does.
static void ata_soft_reset(exi_port port) {
    ata_wr8(port, ATA_REG_DEVCTRL, ATA_DCR_SRST | ATA_DCR_NIEN);
    for (volatile int i = 0; i < 20000; i++) ; // brief assert (>5us)
    ata_wr8(port, ATA_REG_DEVCTRL, ATA_DCR_NIEN);
    ata_wait(port, ATA_SR_BSY, 0);             // wait for the reset to settle
}

// ---------------------------------------------------------------------------
// Public backend interface (mirrors tsd_sd_* / gcode_sd_*).
// ---------------------------------------------------------------------------
bool ata_init(exi_port port) {
    if (port.chn >= ATA_NUM_CHN)
        return false;

    ATA_DEBUG("ATA init chn=%d: status=%02x\n", port.chn, ata_rd8(port, ATA_REG_STATUS));

    // Select the master device (IDE-EXI exposes a single drive as master).
    ata_wr8(port, ATA_REG_DEVICE, ATA_HEAD_USE_LBA);
    if (!ata_wait(port, ATA_SR_BSY, 0)) {
        ATA_DEBUG("ATA chn=%d: BSY never cleared (no drive)\n", port.chn);
        return false;
    }

    // IDENTIFY DEVICE; a present drive raises DRQ with its 256-word data block.
    ata_wr8(port, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    if (!ata_wait(port, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ)) {
        ATA_DEBUG("ATA chn=%d: IDENTIFY no DRQ (status=%02x)\n", port.chn, ata_rd8(port, ATA_REG_STATUS));
        return false;
    }
    if (ata_rd8(port, ATA_REG_STATUS) & ATA_SR_ERR) {
        ATA_DEBUG("ATA chn=%d: IDENTIFY error\n", port.chn);
        return false;
    }

    static u16 id[256] __attribute__((aligned(32)));
    if (!ata_rd_buffer(port, id))
        return false;

    // IDENTIFY word 83 bit 10 = "48-bit Address feature set supported". Some
    // (emulated) IDE-EXI adapters read IDENTIFY back as zeros yet only implement
    // the LBA48 read command, so LBA28 reads of high sectors fail. When IDENTIFY
    // is inconclusive (id[0]==0, never true for a real drive) default to LBA48 —
    // safe for anything >8GB, which is every drive that matters here.
    ata_lba48[port.chn] = (id[83] & (1 << 10)) != 0 || id[0] == 0;
    ATA_DEBUG("ATA chn=%d: drive found, lba48=%d (id[0]=%04x id[83]=%04x)\n",
        port.chn, ata_lba48[port.chn], id[0], id[83]);
    return true;
}

static bool ata_read_one(exi_port port, uint32_t lba, uint8_t* dst) {
    if (!ata_wait(port, ATA_SR_BSY, 0)) {
        ATA_DEBUG("ATA rd lba=%u: BSY stuck (st=%02x)\n", lba, ata_rd8(port, ATA_REG_STATUS));
        return false;
    }

    if (ata_lba48[port.chn]) {
        // LBA48: high bytes first, then low bytes (the task-file FIFO).
        ata_wr8(port, ATA_REG_DEVICE,   ATA_HEAD_USE_LBA);
        ata_wr8(port, ATA_REG_SECCOUNT, 0);                  // count[15:8]
        ata_wr8(port, ATA_REG_LBALO,    (lba >> 24) & 0xFF); // LBA[31:24]
        ata_wr8(port, ATA_REG_LBAMID,   0);                  // LBA[39:32]
        ata_wr8(port, ATA_REG_LBAHI,    0);                  // LBA[47:40]
        ata_wr8(port, ATA_REG_SECCOUNT, 1);                  // count[7:0]
        ata_wr8(port, ATA_REG_LBALO,    lba & 0xFF);         // LBA[7:0]
        ata_wr8(port, ATA_REG_LBAMID,   (lba >> 8) & 0xFF);  // LBA[15:8]
        ata_wr8(port, ATA_REG_LBAHI,    (lba >> 16) & 0xFF); // LBA[23:16]
        ata_wr8(port, ATA_REG_COMMAND,  ATA_CMD_READSECTEXT);
    } else {
        ata_wr8(port, ATA_REG_DEVICE,   ATA_HEAD_USE_LBA | ((lba >> 24) & 0x0F));
        ata_wr8(port, ATA_REG_SECCOUNT, 1);
        ata_wr8(port, ATA_REG_LBALO,    lba & 0xFF);
        ata_wr8(port, ATA_REG_LBAMID,   (lba >> 8) & 0xFF);
        ata_wr8(port, ATA_REG_LBAHI,    (lba >> 16) & 0xFF);
        ata_wr8(port, ATA_REG_COMMAND,  ATA_CMD_READSECT);
    }

    if (!ata_wait(port, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ)) {
        ATA_DEBUG("ATA rd lba=%u: no DRQ (st=%02x err=%02x)\n", lba,
            ata_rd8(port, ATA_REG_STATUS), ata_rd8(port, ATA_REG_ERROR));
        return false;
    }
    if (ata_rd8(port, ATA_REG_STATUS) & ATA_SR_ERR) {
        ATA_DEBUG("ATA rd lba=%u: ERR (err=%02x)\n", lba, ata_rd8(port, ATA_REG_ERROR));
        return false;
    }

    return ata_rd_buffer(port, dst);
}

bool ata_read(exi_port port, uint32_t sector, uint8_t* data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        bool ok = false;
        for (int t = 0; t < ATA_READ_RETRIES && !ok; t++) {
            if (t > 0)
                ata_soft_reset(port); // un-wedge a stuck-BSY drive before retrying
            ok = ata_read_one(port, sector + i, data + (i * 512));
        }
        if (!ok)
            return false;
    }
    return true;
}

bool ata_write(exi_port port, uint32_t sector, const uint8_t* data, uint32_t count) {
    (void)port;
    (void)sector;
    (void)data;
    (void)count;
    return false; // read-only interface (cubiboot never writes game storage)
}
