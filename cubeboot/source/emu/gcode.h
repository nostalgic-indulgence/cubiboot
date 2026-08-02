#ifndef GCODE_H
#define GCODE_H

#include <stdint.h>
#include <stdbool.h>
#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// GCLoader (and compatible ODEs) expose their SD card as raw 512-byte
// sectors over the disc interface (DI) bus using the "Gcode" command set.
// This is the DI-bus equivalent of the EXI-based SD Gecko access in tsd.c,
// letting cubiboot browse the GCLoader SD card in the same manner as an
// SD Gecko / SD2SP2.

// Detect and initialise a GCLoader-class ODE on the disc interface.
// Returns true if a compatible device responded (cached after first call).
bool gcode_sd_init(void);

// Read `count` 512-byte sectors starting at LBA `sector` into `data`.
bool gcode_sd_read(uint32_t sector, uint8_t *data, uint32_t count);

// Write support is not provided by this interface (read-only).
bool gcode_sd_write(uint32_t sector, const uint8_t *data, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // GCODE_H
