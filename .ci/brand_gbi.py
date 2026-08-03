#!/usr/bin/env python3
"""
brand_gbi.py — rebrand the opening.bnr embedded in a GameCube "generic boot image"
header (gbi.hdr) so cubiboot.iso shows cubeboot branding in the console BIOS intro
instead of the stock gc-linux "Game Play" banner.

The console IPL/BIOS reads the disc's opening.bnr (baked into gbi.hdr) to draw the
"press start / press A" intro cube. mkgbi (cubeboot-tools) embeds the gc-linux banner
there. We overwrite, in place:

  * the 96x32 RGB5A3 banner image  -> the cubeboot loader banner (the same one the
                                      menu shows on the cube, default_opening.bin)
  * the BNRDesc text fields        -> "cubiboot" / "Homebrew Program"

Offsets are derived from the BNR1 magic, located dynamically (its absolute offset
shifts with the apploader size — e.g. rebuilding the apploader at a different
optimization level moves it):
  pixelData    @ BNR1 + 0x20  (96*32*2 = 6144 bytes)
  desc[0]      @ BNR1 + 0x20 + 6144  (BNRDesc, BNR1 = single language)

The banner source may be either:
  * a full opening.bnr (BNR1/BNR2) — e.g. patches/data/default_opening.bin — whose
    96x32 RGB5A3 pixelData (at file offset 0x20) is copied across verbatim, or
  * a raw 32x32 RGB5A3 texture (e.g. dol_tex.bin) — centred into the 96x32 banner.
Both are GX-tiled RGB5A3 (4x4-pixel tiles, 32 bytes each, left-to-right then top-to-
bottom); for the 32x32 case the logo is exactly 8x8 tiles and drops in at tile column
8 with whole-tile copies, so no per-pixel rescaling is needed.

Usage: brand_gbi.py <in_gbi.hdr> <banner_src.bnr|dol_tex.bin> <out_gbi.hdr>
"""
import sys

PIXELDATA_LEN = 96 * 32 * 2          # 6144
BNR_PIXEL_OFF = 0x20                 # pixelData offset inside an opening.bnr / after BNR1 magic

# BNRDesc field (offset, length) within a desc block, and the text to write.
TITLE = b"Cubiboot"
SUBTITLE = b"Games Loader"
FIELDS = [
    (0x00, 0x20, TITLE),       # gameName      (short)
    (0x20, 0x20, SUBTITLE),    # company       (short)
    (0x40, 0x40, TITLE),       # fullGameName
    (0x80, 0x40, SUBTITLE),    # fullCompany
    (0xC0, 0x80, SUBTITLE),    # description
]

BANNER_TILES_W = 96 // 4             # 24
LOGO_TILES_W   = 32 // 4             # 8
TILES_H        = 32 // 4             # 8
LOGO_TILE_COL  = (BANNER_TILES_W - LOGO_TILES_W) // 2   # 8 -> centred
TILE_BYTES     = 32


def build_banner(src: bytes) -> bytes:
    # Full opening.bnr (BNR1/BNR2): copy its 96x32 pixelData straight across.
    if src[0:3] == b"BNR" and len(src) >= BNR_PIXEL_OFF + PIXELDATA_LEN:
        px = src[BNR_PIXEL_OFF:BNR_PIXEL_OFF + PIXELDATA_LEN]
        # patches/data/default_opening.bin is a blank template -- the loader paints
        # its pixels at runtime. Baked onto a disc it is an all-black banner, so
        # refuse it rather than shipping an ISO with no visible banner.
        if not any(px):
            raise SystemExit(
                "banner source has empty pixelData (blank BNR template) -- "
                "pass a real banner, e.g. patches/data/dol_tex.bin")
        return px

    # Otherwise treat it as a raw 32x32 RGB5A3 texture and centre it in the banner.
    if len(src) < LOGO_TILES_W * TILES_H * TILE_BYTES:
        raise SystemExit("banner source is neither a BNR nor a 32x32 RGB5A3 texture")

    # Background = the logo's top-left pixel (first tile, first texel) so the banner
    # reads as a horizontal extension of the icon rather than an abrupt border.
    bg = src[0:2]
    banner = bytearray(bg * (PIXELDATA_LEN // 2))

    for ty in range(TILES_H):
        for sx in range(LOGO_TILES_W):
            soff = (ty * LOGO_TILES_W + sx) * TILE_BYTES
            dst = (ty * BANNER_TILES_W + (LOGO_TILE_COL + sx)) * TILE_BYTES
            banner[dst:dst + TILE_BYTES] = src[soff:soff + TILE_BYTES]

    assert len(banner) == PIXELDATA_LEN
    return bytes(banner)


def patch_desc(buf: bytearray, desc_off: int):
    for off, length, text in FIELDS:
        base = desc_off + off
        buf[base:base + length] = b"\x00" * length          # clear field
        n = min(len(text), length - 1)                       # keep NUL terminator
        buf[base:base + n] = text[:n]


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    in_gbi, banner_src_path, out_gbi = sys.argv[1:4]

    buf = bytearray(open(in_gbi, "rb").read())
    # Locate the embedded opening.bnr by its BNR1 magic; its offset depends on the
    # apploader size, so don't assume the stock 0x43C0.
    bnr1_off = buf.find(b"BNR1")
    if bnr1_off < 0:
        raise SystemExit("BNR1 magic not found in gbi.hdr — unexpected layout, aborting")
    pixeldata_off = bnr1_off + BNR_PIXEL_OFF
    desc_off = pixeldata_off + PIXELDATA_LEN

    banner_src = open(banner_src_path, "rb").read()
    buf[pixeldata_off:pixeldata_off + PIXELDATA_LEN] = build_banner(banner_src)
    patch_desc(buf, desc_off)

    open(out_gbi, "wb").write(buf)
    print(f">> wrote {out_gbi} ({len(buf)} bytes): banner + '{TITLE.decode()}' / "
          f"'{SUBTITLE.decode()}' branded")


if __name__ == "__main__":
    main()
