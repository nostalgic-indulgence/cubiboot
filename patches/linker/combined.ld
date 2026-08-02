/* Boot-animation draw calls + sound level, from Extrems' apploader
 * hide_ipl_animation() (cubeboot-tools ppc/apploader/apploader.c). NOPping the
 * three draw calls is what makes the animation genuinely not appear; nothing
 * inside the animation's own state machine can do it. anim_draw_inner is always
 * anim_draw_cubes + 0x3C on every revision, which cross-checks the table. */
ntsc10_anim_draw_cubes = 0x8131055c;
ntsc10_anim_draw_outer = 0x8130d224;
ntsc10_anim_draw_inner = 0x81310598;
ntsc10_anim_sound_level = 0x8145d4d0;

ntsc11_anim_draw_cubes = 0x81310754;
ntsc11_anim_draw_outer = 0x8130d428;
ntsc11_anim_draw_inner = 0x81310790;
ntsc11_anim_sound_level = 0x81481278;

ntsc12_001_anim_draw_cubes = 0x81310aec;
ntsc12_001_anim_draw_outer = 0x8130d79c;
ntsc12_001_anim_draw_inner = 0x81310b28;
ntsc12_001_anim_sound_level = 0x81483340;

ntsc12_101_anim_draw_cubes = 0x81310b04;
ntsc12_101_anim_draw_outer = 0x8130d7b4;
ntsc12_101_anim_draw_inner = 0x81310b40;
ntsc12_101_anim_sound_level = 0x814837c0;

pal10_anim_draw_cubes = 0x81310e94;
pal10_anim_draw_outer = 0x8130d868;
pal10_anim_draw_inner = 0x81310ed0;
pal10_anim_sound_level = 0x814ad118;

/* pal11 is MPAL (apploader IPL_MPAL_11) */
pal11_anim_draw_cubes = 0x81310680;
pal11_anim_draw_outer = 0x8130d354;
pal11_anim_draw_inner = 0x813106bc;
pal11_anim_sound_level = 0x8147bf38;

pal12_anim_draw_cubes = 0x81310fd4;
pal12_anim_draw_outer = 0x8130d9a8;
pal12_anim_draw_inner = 0x81311010;
pal12_anim_sound_level = 0x814af400;

ntsc10_OSSetCurrentContext = 0x81334300;
ntsc10_OSClearContext = 0x813344c8;
ntsc10___OSSetInterruptHandler = 0x813361fc;
ntsc10___OSUnmaskInterrupts = 0x813365d8;

pal10_OSSetCurrentContext = 0x8135d308;
pal10_OSClearContext = 0x8135d4d0;
pal10___OSSetInterruptHandler = 0x8135f29c;
pal10___OSUnmaskInterrupts = 0x8135f6a0;

pal11_OSSetCurrentContext = 0x81359c48;
pal11_OSClearContext = 0x81359e10;
pal11___OSSetInterruptHandler = 0x8135bbdc;
pal11___OSUnmaskInterrupts = 0x8135bfe0;

ntsc11_OSSetCurrentContext = 0x81359d28;
ntsc11_OSClearContext = 0x81359ef0;
ntsc11___OSSetInterruptHandler = 0x8135bcbc;
ntsc11___OSUnmaskInterrupts = 0x8135c0c0;

pal12_OSSetCurrentContext = 0x8136276c;
pal12_OSClearContext = 0x81362934;
pal12___OSSetInterruptHandler = 0x8136332c;
pal12___OSUnmaskInterrupts = 0x81363730;

ntsc12_001_OSSetCurrentContext = 0x8135f3f4;
ntsc12_001_OSClearContext = 0x8135f5bc;
ntsc12_001___OSSetInterruptHandler = 0x8135ffb4;
ntsc12_001___OSUnmaskInterrupts = 0x813603b8;

ntsc12_101_OSSetCurrentContext = 0x8135f3f4;
ntsc12_101_OSClearContext = 0x8135f5bc;
ntsc12_101___OSSetInterruptHandler = 0x8135ffb4;
ntsc12_101___OSUnmaskInterrupts = 0x813603b8;
