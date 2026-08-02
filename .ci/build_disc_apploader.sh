#!/usr/bin/env bash
# Rebuild the DISC apploader baked into cubiboot.iso's gbi.hdr (the "El Torito"
# apploader from cubeboot-tools that the console IPL runs to load cubeboot.dol).
#
# The apploader source changes themselves NO LONGER LIVE HERE. They are commits
# in the cubiboot fork of cubeboot-tools, which .ci/Dockerfile clones at a pinned
# revision -- so the diff against makeo/cubeboot-tools is reviewable in git
# instead of being a pile of sed rewrites applied at build time. What the fork
# changes, and why:
#
#   1. apploader Makefile: -O1 instead of -O2. GCC 13 miscompiles this 2006-era
#      gc-linux C at -O2 (-fstrict-aliasing vs. its type-punned DI buffer) and
#      green-screens the console before cubeboot.dol ever runs.
#   2. PATCH_IPL 3 + IGNORE_BOOT_MODE 1: suppress the IPL's stock boot animation,
#      including on a cold ODE power-on, so only cubiboot's branded animation
#      plays instead of the stock one followed by cubiboot's. Level 2 is not
#      enough -- it only calls skip_ipl_animation() at al_load step 8, after the
#      .dol/FST/bi2.bin reads, so the stock animation visibly plays first. Level 3
#      also runs hide_ipl_animation() at step 4. Holding A still reaches the stock
#      IPL menu.
#   3. #include <stdbool.h> (level 3 does not compile without it) and both
#      zero-initialised statics forced into .data (the flat `objcopy -O binary`
#      image has no .bss, so they would be garbage RAM at runtime).
#
# This script verifies the checkout actually carries all of that, then builds and
# asserts the result (entry point, empty .bss).
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

# These changes now live in the cubeboot-tools FORK the image clones (see
# .ci/Dockerfile), as reviewable commits rather than sed rewrites applied here.
# Verify the checkout really carries them: a wrong/stale tools revision must fail
# loudly, not silently produce a stock apploader that plays the IPL animation.
check() {
    grep -qE "$1" "$APPLOADER_DIR/apploader.c" || {
        echo "ERROR: cubeboot-tools checkout is missing '$2'." >&2
        echo "       Expected the cubiboot fork (see .ci/Dockerfile CUBEBOOT_TOOLS_*)." >&2
        exit 1
    }
}
check '^#define PATCH_IPL 3'        "PATCH_IPL 3"
check '^#define IGNORE_BOOT_MODE 1' "IGNORE_BOOT_MODE 1"
check '^#include <stdbool.h>'       "#include <stdbool.h>"
check 'static bool applied __attribute__\(\(section\(".data"\)\)\)'             "applied in .data"
check 'static uint32_t sound_level_val __attribute__\(\(section\(".data"\)\)\)' "sound_level_val in .data"
grep -qE '^CFLAGS := -O1' "$APPLOADER_DIR/Makefile" || { echo "ERROR: apploader Makefile is not -O1 (GCC 13 miscompiles it at -O2)" >&2; exit 1; }

# Build. -O1 comes from the fork's Makefile, so no CFLAGS override here.
make -C "$APPLOADER_DIR" clean >/dev/null
make -C "$APPLOADER_DIR"

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
