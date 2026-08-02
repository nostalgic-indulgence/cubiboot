#include <math.h>
#include <stddef.h>

#include "picolibc.h"
#include "structs.h"
#include "attr.h"
#include "util.h"
#include "os.h"

#include "usbgecko.h"
#include "state.h"
#include "time.h"

#include "reloc.h"
#include "menu.h"

#include "dolphin_arq.h"
#include "flippy_sync.h"
#include "gc_dvd.h"
#include "games.h"
#include "upng.h"
#include "metaphrasis.h"

#include "video.h"
#include "dol.h"
#include "boot.h"
#include "gameid.h"

#define CUBE_TEX_WIDTH 84
#define CUBE_TEX_HEIGHT 84

#define GAMECUBE_LOGO_WIDTH 352
#define GAMECUBE_LOGO_HEIGHT 40

#define STATE_WAIT_LOAD  0x0f // delay after animation
#define STATE_START_GAME 0x10 // play full animation and start game
#define STATE_NO_DISC    0x12 // play full animation before menu
#define STATE_COVER_OPEN 0x13 // force direct to menu

// __attribute_data__ u32 prog_entrypoint;
// __attribute_data__ u32 prog_dst;
// __attribute_data__ u32 prog_src;
// __attribute_data__ u32 prog_len;

__attribute_data__ u32 cube_color = 0;
__attribute_data__ u32 start_passthrough_game = 0;

// Set by cubeboot.dol from the SRAM "System Boot Mode" bit (Swiss: Settings ->
// System Boot Mode). Non-zero = don't show the branded boot animation.
__attribute_data__ u32 skip_boot_animation = 0;

__attribute_data__ static u8 *cube_text_tex = NULL;
__attribute_data__ char cube_logo_path[MAX_FILE_NAME] = {0};
__attribute_data__ u32 force_progressive = 0;
__attribute_data__ u32 force_swiss_boot = 0;

// used if we are switching to 60Hz on a PAL IPL
__attribute_data__ static int fix_pal_ntsc = 0;

// used for optional delays
__attribute_data__ u32 preboot_delay_ms = 0;
__attribute_data__ u32 postboot_delay_ms = 0;
__attribute_data__ u64 completed_time = 0;

// used to start game
__attribute_reloc__ u32 (*PADSync)();
__attribute_reloc__ void (*__OSStopAudioSystem)();
// __attribute_reloc__ void (*run)(register void* entry_point, register u32 clear_start, register u32 clear_size);

// for setup
__attribute_reloc__ void (*orig_thread_init)();
__attribute_reloc__ void (*menu_init)(int unk);
__attribute_reloc__ void (*main)();

__attribute_reloc__ model *bg_outer_model;
__attribute_reloc__ model *bg_inner_model;
__attribute_reloc__ model *gc_text_model;
__attribute_reloc__ model *logo_model;
__attribute_reloc__ model *cube_model;

// locals
__attribute_data__ static GXColorS10 color_cube;
__attribute_data__ static GXColorS10 color_cube_low;
__attribute_data__ static GXColorS10 color_bg_inner;
__attribute_data__ static GXColorS10 color_bg_outer_0;
__attribute_data__ static GXColorS10 color_bg_outer_1;

// start
__attribute_data__ gm_file_entry_t boot_entry;
__attribute_data__ gm_file_entry_t second_boot_entry;

__attribute_used__ void mod_cube_colors() {
    if (cube_color == 0) {
        OSReport("Using default colors\n");
        return;
    }

    rgb_color target_color;
    target_color.color = (cube_color << 8) | 0xFF;

    // TODO: the HSL calculations do not render good results for darker inputs, I still need to tune SAT/LUM scaling
    // tough colors: 252850 A18594 763C28

    u32 target_hsl = GRRLIB_RGBToHSL(target_color.color);
    u32 target_hue = H(target_hsl);
    u32 target_sat = S(target_hsl);
    u32 target_lum = L(target_hsl);
    float sat_mult = (float)target_sat / 40.0; //* 1.5;
    if (sat_mult > 2.0) sat_mult = sat_mult * 0.5;
    if (sat_mult > 1.5) sat_mult = sat_mult * 0.5;
    // sat_mult = 0.35; // temp for light colors
    // sat_mult = 1.0;
    float lum_mult = (float)target_lum / 135.0; //* 0.75;
    if (lum_mult < 0.75) lum_mult = lum_mult * 1.5;
    OSReport("SAT MULT = %f\n", sat_mult);
    OSReport("LUM MULT = %f\n", lum_mult);
    OSReport("TARGET_HI: %02x%02x%02x = (%d, %d, %d)\n", target_color.parts.r, target_color.parts.g, target_color.parts.b, H(target_hsl), S(target_hsl), L(target_hsl));

    u8 low_hue = (u8)round((float)H(target_hsl) * 1.02857143);
    u8 low_sat = (u8)round((float)S(target_hsl) * 1.09482759);
    u8 low_lum = (u8)round((float)L(target_hsl) * 0.296296296);
    u32 low_hsl = HSLA(low_hue, low_sat, low_lum, A(target_hsl));
    rgb_color target_low;
    target_low.color = GRRLIB_HSLToRGB(low_hsl);
    OSReport("TARGET_LO: %02x%02x%02x = (%d, %d, %d)\n", target_low.parts.r, target_low.parts.g, target_low.parts.b, H(low_hsl), S(low_hsl), L(low_hsl));

    u8 bg_inner_hue = (u8)round((float)H(target_hsl) * 1.00574712);
    u8 bg_inner_sat = (u8)round((float)S(target_hsl) * 0.95867768);
    u8 bg_inner_lum = (u8)round((float)L(target_hsl) * 0.9);
    u32 bg_inner_hsl = HSLA(bg_inner_hue, bg_inner_sat, bg_inner_lum, bg_inner_model->data->mat[0].tev_color[0]->a);
    rgb_color bg_inner;
    bg_inner.color = GRRLIB_HSLToRGB(bg_inner_hsl);

    u8 bg_outer_hue_0 = (u8)round((float)H(target_hsl) * 1.02941176);
    u8 bg_outer_sat_0 = 0xFF;
    u8 bg_outer_lum_0 = (u8)round((float)L(target_hsl) * 1.31111111);
    u32 bg_outer_hsl_0 = HSLA(bg_outer_hue_0, bg_outer_sat_0, bg_outer_lum_0, bg_outer_model->data->mat[0].tev_color[0]->a);
    rgb_color bg_outer_0;
    bg_outer_0.color = GRRLIB_HSLToRGB(bg_outer_hsl_0);

    u8 bg_outer_hue_1 = (u8)round((float)H(target_hsl) * 1.07428571);
    u8 bg_outer_sat_1 = (u8)round((float)S(target_hsl) * 0.61206896);
    u8 bg_outer_lum_1 = (u8)round((float)L(target_hsl) * 0.92592592);
    u32 bg_outer_hsl_1 = HSLA(bg_outer_hue_1, bg_outer_sat_1, bg_outer_lum_1, bg_outer_model->data->mat[1].tev_color[0]->a);
    rgb_color bg_outer_1;
    bg_outer_1.color = GRRLIB_HSLToRGB(bg_outer_hsl_1);

    // logo

    DUMP_COLOR(logo_model->data->mat[1].tev_color[0]);
    DUMP_COLOR(logo_model->data->mat[1].tev_color[1]);
    DUMP_COLOR(logo_model->data->mat[2].tev_color[2]);

    tex_data *base = logo_model->data->tex->dat;
    for (int i = 0; i < 8; i++) {
        tex_data *p = base + i;
        if (p->width != 84) break; // early exit
        u16 wd = p->width;
        u16 ht = p->height;
        void *img_ptr = (void*)((u8*)base + p->offset + (i * 0x20));
        OSReport("FOUND TEX: %dx%d @ %p\n", wd, ht, img_ptr);

        // change hue of cube textures
        for (int y = 0; y < ht; y++) {
            for (int x = 0; x < wd; x++) {
                u32 color = GRRLIB_GetPixelFromtexImg(x, y, img_ptr, wd);

                // hsl
                {
                    u32 hsl = GRRLIB_RGBToHSL(color);
                    u32 sat = round((float)L(hsl) * sat_mult);
                    if (sat > 0xFF) sat = 0xFF;
                    u32 lum = round((float)L(hsl) * lum_mult);
                    if (lum > 0xFF) lum = 0xFF;
                    color = GRRLIB_HSLToRGB(HSLA(target_hue, (u8)sat, (u8)lum, A(hsl)));
                }

                GRRLIB_SetPixelTotexImg(x, y, img_ptr, wd, color);
            }
        }

        uint32_t buffer_size = (wd * ht) << 2;
        DCFlushRange(img_ptr, buffer_size);
    }

    // OSReport("Original Colors:\n");
    // color_cube
    // color_cube_low
    // color_bg_inner
    // color_bg_outer_0
    // color_bg_outer_1

    // DUMP_COLOR(cube_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(cube_model->data->mat[0].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[0].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[1].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[1].tev_color[1]);
    // DUMP_COLOR(logo_model->data->mat[2].tev_color[0]);
    // DUMP_COLOR(logo_model->data->mat[2].tev_color[1]);
    // DUMP_COLOR(bg_inner_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(bg_inner_model->data->mat[1].tev_color[0]);
    // DUMP_COLOR(bg_outer_model->data->mat[0].tev_color[0]);
    // DUMP_COLOR(bg_outer_model->data->mat[1].tev_color[0]);

    // while(1);


    // copy over colors
    copy_color(target_color, &cube_state->color);
    copy_color10(target_color, &color_cube);
    copy_color10(target_low, &color_cube_low);

    copy_color10(bg_inner, &color_bg_inner);
    copy_color10(bg_outer_0, &color_bg_outer_0);
    copy_color10(bg_outer_1, &color_bg_outer_1);

    cube_model->data->mat[0].tev_color[0] = &color_cube;
    cube_model->data->mat[0].tev_color[1] = &color_cube_low;

    logo_model->data->mat[0].tev_color[0] = &color_cube_low; // TODO: use different shades
    logo_model->data->mat[0].tev_color[1] = &color_cube_low; // TODO: <-
    logo_model->data->mat[1].tev_color[0] = &color_cube_low; // TODO: <-
    logo_model->data->mat[1].tev_color[1] = &color_cube_low; // TODO: <-
    logo_model->data->mat[2].tev_color[0] = &color_cube_low; // TODO: <-
    logo_model->data->mat[2].tev_color[1] = &color_cube_low; // TODO: <-

    bg_inner_model->data->mat[0].tev_color[0] = &color_bg_inner;
    bg_inner_model->data->mat[1].tev_color[0] = &color_bg_inner;

    bg_outer_model->data->mat[0].tev_color[0] = &color_bg_outer_0;
    bg_outer_model->data->mat[1].tev_color[0] = &color_bg_outer_1;

    return;
}

__attribute_aligned_data_lowmem__ static u8 color_image_buffer[GAMECUBE_LOGO_WIDTH * GAMECUBE_LOGO_HEIGHT * 4];
static void load_cube_logo(const char *path) {
    if (path == NULL || strlen(path) == 0) {
        OSReport("No cube logo path\n");
        return;
    }

    OSReport("Loading cube logo: %s\n", path);

    // No retry loop here on purpose: what fails on an EXI SD is the underlying
    // mount, and flippy_emu_mount() now does the settle-retry itself. Retrying
    // the open on top of that would just multiply the delays.
    dvd_custom_open(path, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK);
    file_status_t *status = dvd_custom_status();
    if (status == NULL || status->result != 0) {
        OSReport("ERROR: could not open icon file: %s\n", path);
        return;
    }

    // allocate file buffer
    u32 file_size = (u32)__builtin_bswap64(*(u64*)(&status->fsize));
    file_size += 31;
    file_size &= 0xffffffe0;
    void *file_buf = gm_memalign(file_size, 32);
    if (file_buf == NULL) {
        OSReport("ERROR: could not allocate cube logo buffer (%u bytes)\n", file_size);
        dvd_custom_close(status->fd);
        return;
    }

    // read
    dvd_read(file_buf, file_size, 0, status->fd);
    dvd_custom_close(status->fd);

    upng_t *img = upng_new_from_bytes(file_buf, file_size);
    if (img == NULL) {
        OSReport("ERROR: could not allocate png\n");
        gm_freealign(file_buf);
        return;
    }

    upng_error png_err = upng_decode(img);
    if (png_err != UPNG_EOK) {
        OSReport("ERROR: could not decode cube logo (%d)\n", png_err);
        goto cleanup;
    }

    // upng_get_format
    upng_format png_format = upng_get_format(img);
    if (png_format != UPNG_RGBA8) {
        // upng only accepts true-color 8-bit; indexed/palette PNGs land here.
        // Re-export the logo as 32-bit RGBA (with alpha) to fix this.
        OSReport("ERROR: cube logo must be 32-bit RGBA PNG (format=%d)\n", png_format);
        goto cleanup;
    }

    u32 png_width = upng_get_width(img);
    u32 png_height = upng_get_height(img);

    OSReport("PNG: %d x %d\n", png_width, png_height);

    if (png_width != GAMECUBE_LOGO_WIDTH || png_height != GAMECUBE_LOGO_HEIGHT) {
        OSReport("ERROR: invalid png size\n");
        goto cleanup;
    }

    Metaphrasis_convertBufferToRGBA8((uint32_t*)upng_get_buffer(img), (uint32_t*)color_image_buffer, png_width, png_height);
    DCFlushRange(color_image_buffer, sizeof(color_image_buffer));

    OSReport("Cube logo loaded (%dx%d)\n", png_width, png_height);
    cube_text_tex = color_image_buffer;
cleanup:
    // img is non-NULL here (the alloc-failure path returns early above);
    // upng_free() releases the decoded buffer and the struct itself.
    upng_free(img);
    gm_freealign(file_buf);
}

__attribute_used__ void mod_cube_text() {
        tex_data *gc_text_tex = gc_text_model->data->tex->dat;

        u16 wd = gc_text_tex->width;
        u16 ht = gc_text_tex->height;
        void *img_ptr = (void*)((u8*)gc_text_tex + gc_text_tex->offset);
        u32 img_size = wd * ht;

#ifndef DEBUG
        (void)img_ptr;
        (void)img_size;
#endif

        OSReport("CUBE TEXT TEX: %dx%d[%d] (type=%d) @ %p\n", wd, ht, img_size, gc_text_tex->format, img_ptr);
        OSReport("PTR = %08x\n", (u32)cube_text_tex);
        OSReport("ORIG_PTR_PARTS = %08x, %08x\n", (u32)gc_text_tex, gc_text_tex->offset);

        if (cube_text_tex != NULL) {
            s32 desired_offset = gc_text_tex->offset;
            if ((u32)gc_text_tex > (u32)cube_text_tex) {
                desired_offset = -1 * (s32)((u32)gc_text_tex - (u32)cube_text_tex);
            } else {
                desired_offset = (s32)((u32)cube_text_tex - (u32)gc_text_tex);
            }

            OSReport("DESIRED = %d\n", desired_offset);

            // change the texture format
            gc_text_tex->format = GX_TF_RGBA8;
            gc_text_tex->offset = desired_offset;
        }
}


__attribute_used__ void mod_cube_anim() {
    if (fix_pal_ntsc) {
        cube_state->cube_side_frames = 10;
        cube_state->cube_corner_frames = 16;
        cube_state->fall_anim_frames = 5;
        cube_state->fall_delay_frames = 16;
        cube_state->up_anim_frames = 18;
        cube_state->up_delay_frames = 7;
        cube_state->move_anim_frames = 10;
        cube_state->move_delay_frames = 20;
        cube_state->done_delay_frames = 40;
        cube_state->logo_hold_frames = 60;
        cube_state->logo_delay_frames = 5;
        cube_state->audio_cue_frames_b = 7;
        cube_state->audio_cue_frames_c = 6;
    }
}

__attribute_used__ void pre_thread_init() {
    dolphin_ARAMInit();
    orig_thread_init();

    gm_init_heap();
    gm_init_thread();

    // The boot-logo load and the game-enumeration thread both moved to
    // pre_menu_init(). This function replaces the IPL's *thread init* call, so
    // it runs before the IPL has brought EXI up -- see the comment there.
}

__attribute_used__ void pre_menu_init(int unk) {
    menu_init(unk);

    // change default menu
    *prev_menu_id = MENU_GAMESELECT_TRANSITION_ID;
    *cur_menu_id = MENU_GAMESELECT_ID;

    custom_gameselect_init();

    mod_cube_colors();

    // Load the optional custom boot logo (cube_logo = *.png) HERE, not in
    // pre_thread_init().
    //
    // pre_thread_init() replaces the IPL's thread-init call, which runs before
    // the IPL has initialised EXI. An SD adapter in a memory-card slot is an EXI
    // device, so the mount cannot succeed that early no matter how long we wait.
    // A gecko trace of a PicoBoot + SD-slot-A boot made this unambiguous: 30
    // mount retries over 300ms all failed there, and then the very next mount --
    // once the IPL had moved on -- succeeded on its first attempt ("after 0
    // retries"). The GCLoader lives on the DI bus, already up by then, which is
    // why this only ever broke on the SD paths.
    //
    // By this point EXI is up. Loading here also keeps the emu layer's single
    // shared FIL race-free, because the enumeration thread is not started until
    // after the logo has been decoded -- that ordering was the original reason
    // this lived in pre_thread_init().
    load_cube_logo(cube_logo_path);

    mod_cube_text();
    mod_cube_anim();

    // Start enumeration only now, so it cannot race the logo load above.
    if (!start_passthrough_game) {
        gm_start_thread("/");
    }

    // delay before boot animation (to wait for GCVideo)
    const int fps = rmode->viTVMode >> 2 == VI_NTSC ? 60 : 50;
    const int total_frames = preboot_delay_ms / fps;
    for (int i = 0; i < total_frames; i++) {
        VIWaitForRetrace();
    }
}

__attribute_used__ void pre_main() {
    OSReport("RUNNING BEFORE MAIN\n");

    OSReport("efbHeight = %u\n", rmode->efbHeight);
    OSReport("xfbHeight = %u\n", rmode->xfbHeight);

    if (force_progressive) {
        OSReport("Patching video mode to Progressive Scan\n");
        fix_pal_ntsc = rmode->viTVMode >> 2 != VI_NTSC;
        if (fix_pal_ntsc) {
            rmode->fbWidth = 592;
            rmode->efbHeight = 226;
            rmode->xfbHeight = 448;
            rmode->viXOrigin = 40;
            rmode->viYOrigin = 16;
            rmode->viWidth = 640;
            rmode->viHeight = 448;
        }

        // sample points arranged in increasing Y order
        u8  sample_pattern[12][2] = {
            {6,6},{6,6},{6,6},  // pix 0, 3 sample points, 1/12 units, 4 bits each
            {6,6},{6,6},{6,6},  // pix 1
            {6,6},{6,6},{6,6},  // pix 2
            {6,6},{6,6},{6,6}   // pix 3
        };
        memcpy(&rmode->sample_pattern[0][0], &sample_pattern[0][0], (12*2));

        rmode->viTVMode = VI_TVMODE_NTSC_PROG;
        rmode->xfbMode = VI_XFBMODE_SF;
        // rmode->aa = FALSE; // breaks the IPL??
        // rmode->field_rendering = TRUE;

        rmode->vfilter[0] = 0;
        rmode->vfilter[1] = 0;
        rmode->vfilter[2] = 21;
        rmode->vfilter[3] = 22;
        rmode->vfilter[4] = 21;
        rmode->vfilter[5] = 0;
        rmode->vfilter[6] = 0;
    }

    main();

    __builtin_unreachable();
}

__attribute_used__ u32 get_tvmode() {
    return rmode->viTVMode;
}

// Boot-animation suppression, ported from the disc apploader's
// hide_ipl_animation() -- the mechanism already proven on this hardware for the
// console's stock animation.
//
// This has to suppress the *drawing*, because nothing inside the animation's own
// state machine can stop it having visibly started: returning STATE_COVER_OPEN
// leaves the finished logo held on screen, shortening cube_state's frame
// counters just fast-forwards it so you catch the cube mid-jump, and forcing the
// IPL's "is A held?" check had no effect at all.
//
// The three draw calls are swapped for `nop` and the sound level for 0, then all
// four are swapped BACK once the splash is over. The restore is not optional:
// draw_outer/draw_inner also render the menu's background cubes, so leaving them
// NOPped would break the menu we are trying to reach.
__attribute_data__ static bool anim_hidden = false;
__attribute_data__ static u32 saved_draw_cubes = 0x60000000; // nop
__attribute_data__ static u32 saved_draw_outer = 0x60000000; // nop
__attribute_data__ static u32 saved_draw_inner = 0x60000000; // nop
__attribute_data__ static u32 saved_sound_level = 0;

static void swap_word(u32 *addr, u32 *slot) {
    u32 tmp = *addr;
    *addr = *slot;
    *slot = tmp;
    DCFlushRange(addr, 4);
    ICInvalidateRange(addr, 4);
}

static void set_anim_hidden(bool hide) {
    if (hide == anim_hidden) return;

    swap_word(anim_sound_level, &saved_sound_level);
    swap_word(anim_draw_cubes, &saved_draw_cubes);
    swap_word(anim_draw_outer, &saved_draw_outer);
    swap_word(anim_draw_inner, &saved_draw_inner);

    anim_hidden = hide;
}

__attribute_data__ int frame_count = 0;
__attribute_used__ u32 bs2tick() {
    frame_count++;

    if (skip_boot_animation) {
        // *main_menu_id is what the apploader calls splash_state: 1 while the
        // boot splash is running. Hide the animation for exactly that window,
        // and put the IPL back the way we found it the moment it ends.
        if (*main_menu_id == 1) {
            set_anim_hidden(true);
            cube_state->cube_anim_done = 1; // end the splash immediately
        } else {
            set_anim_hidden(false);
        }
    }

    if (!completed_time && cube_state->cube_anim_done) {
        OSReport("FINISHED (%d frames)\n", frame_count);
        completed_time = gettime();
    }

    if (start_passthrough_game) {
        if (postboot_delay_ms) {
            u64 elapsed = diff_msec(completed_time, gettime());
            if (completed_time > 0 && elapsed > postboot_delay_ms) {
                return STATE_START_GAME;
            } else {
                return STATE_WAIT_LOAD;
            }
        }
        return STATE_START_GAME;
    }

    // this helps the start menu show correctly
    if (*main_menu_id >= 3) {
        return STATE_START_GAME;
    }

    return STATE_NO_DISC;
}

__attribute_used__ void bs2start() {
    OSReport("DONE\n");

    // read boot info into lowmem
    struct dolphin_lowmem *lowmem = (struct dolphin_lowmem*)0x80000000;

    if (!start_passthrough_game || force_swiss_boot) {
        gm_deinit_thread();
    } else {
        dvd_custom_bypass_enter();
        udelay(10 * 1000);

        int ret = dvd_read_id();
        int err = dvd_get_error();
        if (ret != 0 || err != 0) {
            custom_OSReport("Failed to read disc ID\n");
            dvd_custom_bypass_exit();
            udelay(10 * 1000);

            load_stub(); // exit to loader again
            u32 *sig = (u32*)0x80001804;
            if ((*sig++ == 0x53545542 || *sig++ == 0x53545542) && *sig == 0x48415858) {
                static void (*reload)(void) = (void(*)(void))0x80001800;
                run(reload);
            }
        }

        custom_OSReport("Game ID: %c%c%c%c\n", lowmem->b_disk_info.game_code[0], lowmem->b_disk_info.game_code[1], lowmem->b_disk_info.game_code[2], lowmem->b_disk_info.game_code[3]);
        dvd_audio_config(lowmem->b_disk_info.audio_streaming, lowmem->b_disk_info.stream_buffer_size);

        char diskName[64] = "DISC GAME\0";
        setup_gameid_commands(&lowmem->b_disk_info, diskName);
    }

    // no IPL code should be running after this point

    extern void tsd_set_native(bool native);
    tsd_set_native(true);

    // IDE-EXI (ata) must also switch to raw-register EXI before the IPL (and its
    // EXI driver) is wiped below, so launching a game off an IDE-EXI drive works.
    extern void ata_set_native(bool native);
    ata_set_native(true);

    while (!PADSync());
    OSDisableInterrupts();
    __OSStopAudioSystem();

    u32 start_addr = 0x80100000;
    u32 end_addr = 0x81600000;
    u32 len = end_addr - start_addr;

    // This wipe covers all of .data_lowmem, which is where the storage block
    // cache keeps its 384 KiB of pages -- but NOT its bookkeeping, which is a
    // .bss object up in the patch region and survives. Leave it enabled and
    // every read the loader makes below is served out of a page the memset just
    // zeroed, while the cache happily reports a hit: nothing boots, on any
    // device. (FatFs' own window buffer is fine here only because it lives in
    // the same surviving region as the FATFS struct that indexes it.)
    //
    // Turn the cache off before the wipe, not after -- there must be no window
    // in which the metadata and the pages disagree. It exists to speed up
    // browsing, and browsing is over.
    dvm_cache_disable();

    memset((void*)start_addr, 0, len); // cleanup
    DCFlushRange((void*)start_addr, len);
    ICInvalidateRange((void*)start_addr, len);

    // Passthrough mode
    if (start_passthrough_game) {
        chainload_boot_game(NULL, true);
    }

    char *boot_path = boot_entry.path;
    if (boot_entry.type == GM_FILE_TYPE_PROGRAM) {
        custom_OSReport("Booting DOL\n");
        load_stub();

        chainload_swiss_game(boot_path, false); // use swiss as normal dol load is partially broken
        //dol_info_t info = load_dol_file(boot_path, false);
        //run(info.entrypoint);
    } else {
        custom_OSReport("Booting ISO\n");

        if (!force_swiss_boot) {
            custom_OSReport("Booting ISO (custom apploader)\n");
            chainload_boot_game(&boot_entry, false);
        } else {
            custom_OSReport("Booting ISO (swiss chainload)\n");
            chainload_swiss_game(boot_path, false);
        }
    }

    __builtin_unreachable();
}

void mega_trap(u32 r3, u32 r4, u32 r5, u32 r6) {
    u32 caller = (u32)__builtin_return_address(0);
    OSReport("[%08x] (r3=%08x r4=%08x r5=%08x, r6=%08x) You hit the mega trap dog\n", caller, r3, r4, r5, r6);
    while(1);
}

// unused
void _start() {}
