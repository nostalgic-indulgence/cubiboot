#include "structs.h"
#include "config.h"

extern BOOL is_running_dolphin;

#ifdef DEBUG
extern void (*OSReport)(const char* text, ...);
#else
#define OSReport(...)
#endif

extern void custom_OSReport(const char *fmt, ...);

extern bios_pad *pad_status;
extern u32 *prev_menu_id;
extern u32 *cur_menu_id;
extern u32 *main_menu_id;
extern GXRModeObj *rmode;

// from main
extern state *cube_state;

// boot-animation suppression (see bs2tick in main.c)
extern u32 *anim_draw_cubes;
extern u32 *anim_draw_outer;
extern u32 *anim_draw_inner;
extern u32 *anim_sound_level;

// from menu
extern u32 (*OSDisableInterrupts)();
extern BOOL (*OSRestoreInterrupts)(BOOL);
extern void (*VIWaitForRetrace)();
extern void (*GXSetCurrentMtx)(u32);
