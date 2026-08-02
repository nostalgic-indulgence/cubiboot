#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>
#include <gctypes.h>

#include "tsd.h"   // exi_port { chn, dev }

#ifdef __cplusplus
extern "C" {
#endif

// IDE-EXI (ATA-over-EXI) storage, as used by Swiss. An ATA/IDE drive bridged to
// the EXI bus through an IDE-EXI adapter in a memory-card slot or serial port.
// Same shape as the SD (tsd) and GCLoader (gcode) backends so it drops straight
// into the FatFs diskio glue.
bool ata_init(exi_port port);
bool ata_read(exi_port port, uint32_t sector, uint8_t* data, uint32_t count);
bool ata_write(exi_port port, uint32_t sector, const uint8_t* data, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // ATA_H
