#include <stdbool.h>
#include <ogc/system.h>

// Validates the SRAM block checksum. Not declared in any libogc header, but it
// is defined in libogc's system.c (Swiss externs it the same way). The checksum
// covers bytes 12..19, which includes ntd -- the byte holding the System Boot
// Mode bit -- so this is a real guard on that bit and not just on SRAM generally.
extern u32 __SYS_CheckSram(void);

void set_sram_swiss(bool syncSram);
void create_swiss_config();
