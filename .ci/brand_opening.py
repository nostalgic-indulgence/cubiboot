#!/usr/bin/env python3
"""
brand_opening.py — brand patches/data/default_opening.bin, the cubeboot default
banner (a BNR1) compiled into the loader as default_opening_bin.h and pointed at by
banner_pointer (menu.c). It is what the menu cube shows before any entry is selected,
and what B resets the cube back to.

Two independent operations, because they were needed at different times:

  --text          rewrite the BNRDesc text fields to the cubiboot branding
                  (stock upstream reads "Cubeboot Loader" / "Team OffBroadway")
  --banner SRC    bake banner pixels in from SRC

Upstream ships this file with an ALL-ZERO pixelData, so the menu cube's default
banner renders as nothing at all. --banner fills it in. SRC is a raw 32x32 RGB5A3
texture (patches/data/dol_tex.bin) or a full BNR.

BOTH operations delegate to .ci/brand_gbi.py -- build_banner() for the pixels,
TITLE/SUBTITLE/patch_desc() for the text -- because that is the script that brands
the disc banner inside cubiboot.iso. Sharing the code rather than keeping a parallel
copy of the art and the strings is the point: the menu cube and the disc banner are
the same product shown in two places, and they used to drift (this file said
"cubiboot loader" with an empty company while the disc said "Cubiboot" / "Games
Loader"). Change the branding in brand_gbi.py and re-run both scripts.

Note this file is a tracked binary, patched in place and committed -- it is an input
to the patches build, not a build product, so the result must be checked in.

With no flags, defaults to --text (the original behaviour).

BNR1 layout: 0x20 header + 6144 px @ 0x20 + one BNRDesc @ 0x1820:
  gameName     0x1820 (0x20)   <- title line   (short)
  company      0x1840 (0x20)   <- maker line   (short)
  fullGameName 0x1860 (0x40)   <- title line   (full)
  fullCompany  0x18A0 (0x40)   <- maker line   (full)
  description  0x18E0 (0x80)   <- info text

Usage: brand_opening.py <default_opening.bin> [--text] [--banner SRC]
       (patched in place)
"""
import importlib.util
import os
import sys

PIXELDATA = 0x20
PIXELDATA_LEN = 96 * 32 * 2
DESC = 0x1820

_gbi_mod = None


def gbi():
    """The disc-banner brander, loaded as a module: single source of truth for
    both the banner art and the BNRDesc strings."""
    global _gbi_mod
    if _gbi_mod is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brand_gbi.py")
        spec = importlib.util.spec_from_file_location("brand_gbi", path)
        _gbi_mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_gbi_mod)
    return _gbi_mod


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args.pop(0)

    do_text = False
    banner_src = None
    while args:
        arg = args.pop(0)
        if arg == "--text":
            do_text = True
        elif arg == "--banner":
            if not args:
                raise SystemExit("--banner needs a source file")
            banner_src = args.pop(0)
        else:
            raise SystemExit(f"unknown argument: {arg}")
    if not do_text and banner_src is None:
        do_text = True                       # original no-flag behaviour

    buf = bytearray(open(path, "rb").read())
    if buf[0:4] != b"BNR1":
        raise SystemExit("not a BNR1 file — aborting")

    done = []
    if banner_src is not None:
        px = gbi().build_banner(open(banner_src, "rb").read())
        buf[PIXELDATA:PIXELDATA + PIXELDATA_LEN] = px
        done.append(f"banner from {os.path.basename(banner_src)} "
                    f"({sum(1 for b in px if b)}/{PIXELDATA_LEN} bytes non-zero)")

    if do_text:
        # Same writer and same strings the disc banner gets, so the two cannot drift.
        gbi().patch_desc(buf, DESC)
        done.append(f"text '{gbi().TITLE.decode()}' / '{gbi().SUBTITLE.decode()}'")

    open(path, "wb").write(buf)
    print(f">> {path}: " + ", ".join(done))


if __name__ == "__main__":
    main()
