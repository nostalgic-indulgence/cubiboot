#!/usr/bin/env bash
# Rebuild the DISC apploader baked into cubiboot.iso's gbi.hdr (the "El Torito"
# apploader from cubeboot-tools that the console IPL runs to load cubeboot.dol),
# with two changes vs the stock prebuilt blob:
#
#   1. Compile at -O1, NOT the Makefile's default -O2. The apploader is 2006-era
#      gc-linux C that type-puns a char[] DI buffer into packed structs; modern
#      devkitPPC GCC (13.x) miscompiles that at -O2 (-fstrict-aliasing, on at -O2
#      and off at -O1, plus aggressive opts) and produces an apploader that
#      green-screens the console before cubeboot.dol ever runs. -O1 matches the
#      codegen shape of the known-good shipped blob (frame-based al_start).
#
#   2. PATCH_IPL=3 + IGNORE_BOOT_MODE=1 so the apploader suppresses the IPL's
#      stock boot animation (even on a cold ODE power-on, not just on reset).
#      Without this the GC Loader plays the stock animation AND then cubeboot's
#      branded one -> two animations. With it, only cubeboot's branded one plays.
#
#      PATCH_IPL=2 alone is NOT enough, and that is what produced the "snippet of
#      the stock animation, then cubeboot's animation" behaviour: level 2 only
#      calls skip_ipl_animation(), which runs at al_load step 8 -- i.e. AFTER the
#      whole cubeboot.dol, the FST and bi2.bin have been read off the disc. The
#      animation is happily drawing for that entire read, so you see the first
#      second or so of it before it is cut off. Level 3 additionally enables
#      hide_ipl_animation(), which runs at step 4 (before the first .dol section
#      is read) and NOPs the IPL's three cube-drawing calls + zeroes its sound
#      level for all 11 IPL revisions, so nothing is drawn or heard from the very
#      start. skip_ipl_animation() then still ends the splash at step 8 as before.
#      Holding A (DISABLE_A_SKIP=0) still suppresses the hide, so the stock IPL
#      menu remains reachable.
#
#   3. Two source fixes needed to make level 3 usable, applied below:
#        a. hide_ipl_animation() uses bool/true/false but the vendored
#           apploader.c never includes <stdbool.h> (it compiles at levels 1 and 2
#           because nothing else uses bool), so level 3 fails to build outright.
#        b. its two zero-initialised statics (`applied`, `sound_level_val`) land
#           in .bss, and the apploader is shipped as a flat `objcopy -O binary`
#           image -- .bss is NOBITS, so it is past the end of apploader.bin and
#           the IPL never zeroes it. `applied` would come up as garbage RAM
#           (hide silently does nothing) and the garbage `sound_level_val` would
#           be swapped into the IPL's sound level. Forcing both into .data makes
#           them part of the loaded image; the build asserts .bss is empty.
#
# This regenerates /opt/src/cubeboot-tools/mkgbi/gbi.hdr and then copies it to
# <repo>/gbi.hdr, which is what build_iso.sh consumes. The copy matters: that path
# is inside the image, not the bind-mounted repo, so when the two scripts run in
# separate `docker run --rm` containers (as CI does) the regenerated header would
# otherwise be discarded with the first container and build_iso.sh would silently
# fall back to the stock gbi.hdr baked into the image -- shipping an ISO with the
# unpatched upstream apploader and no animation suppression at all. Run this
# BEFORE build_iso.sh.
#
# The apploader/mkgbi sources live in the cubiboot-dev image at
# /opt/src/cubeboot-tools (not in this repo), so override the location with
# $CUBEBOOT_TOOLS if yours differs.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="${CUBEBOOT_TOOLS:-/opt/src/cubeboot-tools}"
APPLOADER_DIR="$TOOLS/ppc/apploader"
MKGBI_DIR="$TOOLS/mkgbi"

[ -f "$APPLOADER_DIR/apploader.c" ] || { echo "ERROR: apploader.c not found in $APPLOADER_DIR" >&2; exit 1; }

# Enable the IPL animation hide+skip. (Source ships PATCH_IPL 1 / IGNORE_BOOT_MODE 0.)
sed -i \
    -e 's/#define PATCH_IPL 1/#define PATCH_IPL 3/' \
    -e 's/#define IGNORE_BOOT_MODE 0/#define IGNORE_BOOT_MODE 1/' \
    -e 's|^#include <stddef.h>|#include <stdbool.h>\n#include <stddef.h>|' \
    -e 's/^\(\s*\)static bool applied = false;/\1static bool applied __attribute__((section(".data"))) = false;/' \
    -e 's/^\(\s*\)static uint32_t sound_level_val = 0;/\1static uint32_t sound_level_val __attribute__((section(".data"))) = 0;/' \
    "$APPLOADER_DIR/apploader.c"
grep -qE '^#define PATCH_IPL 3'        "$APPLOADER_DIR/apploader.c" || { echo "ERROR: failed to set PATCH_IPL=3" >&2; exit 1; }
grep -qE '^#define IGNORE_BOOT_MODE 1' "$APPLOADER_DIR/apploader.c" || { echo "ERROR: failed to set IGNORE_BOOT_MODE=1" >&2; exit 1; }
grep -qE '^#include <stdbool.h>'       "$APPLOADER_DIR/apploader.c" || { echo "ERROR: failed to add stdbool.h include" >&2; exit 1; }
grep -qE 'static bool applied __attribute__\(\(section\(".data"\)\)\)'       "$APPLOADER_DIR/apploader.c" || { echo "ERROR: failed to move 'applied' to .data" >&2; exit 1; }
grep -qE 'static uint32_t sound_level_val __attribute__\(\(section\(".data"\)\)\)' "$APPLOADER_DIR/apploader.c" || { echo "ERROR: failed to move 'sound_level_val' to .data" >&2; exit 1; }

# Build the apploader at -O1 (command-line CFLAGS overrides the Makefile's -O2).
make -C "$APPLOADER_DIR" clean >/dev/null
make -C "$APPLOADER_DIR" CFLAGS=-O1

OBJDUMP="${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin/powerpc-eabi-objdump"

# al_start must stay at the load address the IPL jumps to. (The Makefile has an
# entry_point_check target but it never actually runs, so check it here.)
ENTRY="$("$OBJDUMP" -f "$APPLOADER_DIR/apploader.elf" | awk '/start address/ { print $NF }')"
[ "$ENTRY" = "0x81200000" ] || { echo "ERROR: apploader entry point is $ENTRY, must be 0x81200000" >&2; exit 1; }

# The flat apploader.bin carries no .bss, so anything left there is uninitialised
# RAM at runtime. Fail loudly rather than shipping a silently-broken hide.
BSS_SIZE="$("$OBJDUMP" -h "$APPLOADER_DIR/apploader.elf" | awk '$2 == ".bss" { print $3 }')"
[ "$BSS_SIZE" = "00000000" ] || { echo "ERROR: apploader has a non-empty .bss ($BSS_SIZE bytes) which is not loaded at runtime" >&2; exit 1; }

echo ">> apploader.bin: $(stat -c%s "$APPLOADER_DIR/apploader.bin") bytes (-O1, hide+skip-animation, entry $ENTRY, .bss empty)"

# Regenerate gbi.hdr from the freshly built apploader.
make -C "$MKGBI_DIR" clean >/dev/null
make -C "$MKGBI_DIR"
echo ">> regenerated $MKGBI_DIR/gbi.hdr ($(stat -c%s "$MKGBI_DIR/gbi.hdr") bytes)"

# Hand it to build_iso.sh through the bind-mounted repo so it survives the
# container this script ran in.
cp "$MKGBI_DIR/gbi.hdr" "$REPO/gbi.hdr"
echo ">> wrote $REPO/gbi.hdr"
