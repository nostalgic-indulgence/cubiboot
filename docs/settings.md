# Settings

These are all of the values supported by the `cubeboot.ini` file.

```
cube_color = 00ffff     # hex color code
cube_logo = path.png    # path to a 352x40px PNG image
force_progressive = 1   # enables progressive scan
```

## Boot animation

Not a `config.ini` key — cubiboot's branded boot animation is controlled entirely
by the console's SRAM **System Boot Mode** bit (`sram->ntd` bit 7), the same bit
the GameCube IPL itself uses to decide whether to show the logo screen. Swiss
exposes it directly: **Settings → System Boot Mode**.

| Swiss setting | SRAM value | cubiboot |
| --- | --- | --- |
| `Default` | `SYS_BOOT_DEVELOPMENT` (0x00) | straight to the game menu |
| `Production` | `SYS_BOOT_PRODUCTION` (0x80) | plays the branded animation |

The animation plays unless SRAM is readable *and* positively says `Default`. A
dead RTC battery or cleared SRAM therefore falls back to the animation rather
than silently skipping it — see below for why that needs an explicit check.

Because it lives in SRAM it survives power-off, so setting it once in Swiss
changes every subsequent cubiboot boot. This is the same bit the disc apploader
tests, which keeps the two consistent — note though that the apploader suppresses
the console's *stock* animation unconditionally, so this only ever governs
cubiboot's own branded one. `Production` does not bring the stock GameCube
animation back.

When skipping, cubiboot suppresses the animation's *drawing*, using the same
mechanism the disc apploader uses on the console's stock animation: the IPL's
three animation draw calls are swapped for `nop` and its sound level for 0, and
`cube_state->cube_anim_done` is forced so the splash ends immediately. All four
are swapped back the moment the splash is over — the same draw calls render the
menu's background cubes, so leaving them patched would break the menu.

Suppressing the drawing is necessary because nothing inside the animation's own
state machine can stop it having visibly started. Three other approaches were
tried on hardware and all failed: the IPL's "cover open" disc state leaves the
finished logo held on screen, shortening the animation's frame counters merely
fast-forwards it so you catch the cube mid-jump, and forcing the IPL's "is A
held?" check had no effect at all.

### Why a dead battery needs an explicit check

There is no third "invalid" value to detect. `SYS_GetBootMode()` masks `ntd` with
`0x80`, so it can only ever return `0x00` or `0x80` — a dead battery, a cleared
SRAM and a garbage read all surface as `0x00`, which is byte-for-byte
indistinguishable from a genuine `Default`. Simply testing "not Production" would
treat every one of those as "skip the animation".

So cubiboot validates the SRAM block checksum with `__SYS_CheckSram()` first and
then tests positively for `SYS_BOOT_DEVELOPMENT`. The checksum covers bytes
12..19, which includes `ntd`, so it genuinely guards the bit in question rather
than SRAM in general.

Two caveats:

- The game grid is filled in by a background thread that the animation normally
  gives a few seconds of cover. Skipping it means entries may keep appearing for
  a moment after the menu shows up on a large library — the same behaviour you
  already see when navigating into a directory. `preboot_delay_ms` buys that time
  back if it bothers you.
- This only affects the boot-to-menu path. Booting straight into a game
  (`default_program` or a `button_*` binding) still plays the animation.

## `cube_logo`

Replaces the "GAMECUBE" wordmark shown during the boot animation with your own
image. The file must be:

- exactly **352 x 40 pixels**
- a **32-bit RGBA PNG** (true color with an alpha channel)

The alpha channel is honoured, so transparent areas let the background show
through around your artwork. Indexed/palette PNGs are **not** supported — if the
image was exported as indexed color, re-export it as 32-bit RGBA. The path is
relative to the SD card root (same place as `config.ini`). Decode progress and
any errors are printed over USB Gecko (slot B) when running a debug build.
