#!/usr/bin/env python3
"""
make_vlw.py
=====================================================================
Rasterize a TTF into TFT_eSPI ".vlw" smooth (anti-aliased) fonts and emit
them as flash-resident C arrays for the ESP32 firmware's reading view.

The firmware loads each array with  tft.loadFont(array)  — no SD access, so
scrolling stays smooth (see BibleInterface reading view).

Charset: printable ASCII (0x21..0x7E) plus the German umlauts the firmware
uses (Ä Ö Ü ä ö ü ß).  Space (0x20) is intentionally omitted — TFT_eSPI draws
it from its own spaceWidth guess, so a space glyph would just be ignored.

Usage:
  pip install freetype-py
  python make_vlw.py "C:/Windows/Fonts/DejaVuSans.ttf" \
      --sizes 10,12,14,18,22,28 --out ../bible_firmware/fonts_vlw.h

Produces one array per size (e.g. vlw_14[], vlw_18[], …) in a single header,
plus a VLW_FONTS[] table {ptr, px} the firmware can index.

VLW format (big-endian, reverse-engineered in TFT_eSPI Smooth_font.cpp):
  header  = 6 x uint32 : gCount, version(11), sizePt, 0, ascent, descent
  per glyph = 7 x int32 : unicode, height, width, xAdvance, dY, dX, 0
  bitmaps  = gCount blocks of width*height bytes (8-bit alpha, row-major)
  trailer  = nameLen, name, psLen, psname, aaFlag(1)
"""

import argparse
import struct
import sys

try:
    import freetype
except ImportError:
    sys.exit("ERROR: freetype-py is required.  Install with:  pip install freetype-py")

# Printable ASCII (space excluded — TFT_eSPI handles it via spaceWidth) + umlauts.
# The umlauts and eszett are the point of using VLW at all: the TFT_eSPI bitmap
# fonts have no glyphs for them.
BASE_CHARSET = [chr(c) for c in range(0x21, 0x7F)] + list("ÄÖÜäöüß")
# --extended also bakes in the German typographic marks: „ “ ‚ ‘ ’ – — …
# (The reader firmware's Fraktur charset is deliberately NOT carried over —
# this project has no Fraktur family.)
EXTRA_CHARSET = [chr(c) for c in (0x201E, 0x201C, 0x201A, 0x2018, 0x2019,
                                  0x2013, 0x2014, 0x2026)]


def be32(v):
    return struct.pack(">i", v)


def build_vlw(ttf_path, px, charset):
    face = freetype.Face(ttf_path)
    face.set_pixel_sizes(0, px)
    ascent  = face.size.ascender >> 6          # 26.6 fixed point → pixels
    descent = -(face.size.descender >> 6)      # descender is negative
    if ascent <= 0:
        ascent = px
    if descent < 0:
        descent = 0

    glyphs = []   # (unicode, w, h, advance, dY, dX, bitmap_bytes)
    for ch in charset:
        if face.get_char_index(ord(ch)) == 0:
            continue                            # font lacks this glyph — skip it
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        g   = face.glyph
        bmp = g.bitmap
        w, h, pitch = bmp.width, bmp.rows, bmp.pitch
        advance = g.advance.x >> 6
        dX = g.bitmap_left
        dY = g.bitmap_top
        # Row-major 8-bit alpha, honoring pitch (>= width, may be padded).
        buf = bmp.buffer
        data = bytearray()
        for row in range(h):
            start = row * pitch
            data += bytes(buf[start:start + w])
        # VLW stores w/h/advance as uint8 and dX as int8 — clamp/validate.
        if w > 255 or h > 255 or advance > 255:
            sys.exit(f"ERROR: glyph {ch!r} at {px}px exceeds 255px — pick a smaller size.")
        if not (-128 <= dX <= 127):
            dX = max(-128, min(127, dX))
        glyphs.append((ord(ch), w, h, advance, dY, dX, bytes(data)))

    out = bytearray()
    out += be32(len(glyphs))     # gCount
    out += be32(11)              # version
    out += be32(px)              # size (informational)
    out += be32(0)               # deprecated mboxY
    out += be32(ascent)
    out += be32(descent)
    for (u, w, h, adv, dY, dX, _) in glyphs:
        out += be32(u) + be32(h) + be32(w) + be32(adv) + be32(dY) + be32(dX) + be32(0)
    for (_, _, _, _, _, _, data) in glyphs:
        out += data
    # Trailer (unused by the array loader, included for format completeness).
    name = b"BibleVLW"
    out += bytes([len(name)]) + name
    out += bytes([len(name)]) + name
    out += bytes([1])            # anti-aliased flag
    line_h = ascent + descent
    return bytes(out), ascent, descent, line_h


def emit_array(fh, name, blob):
    fh.write(f"static const uint8_t {name}[] PROGMEM = {{\n")
    for i in range(0, len(blob), 16):
        row = ", ".join(f"0x{b:02X}" for b in blob[i:i + 16])
        fh.write(f"  {row},\n")
    fh.write("};\n\n")


def main():
    ap = argparse.ArgumentParser(description="TTF → TFT_eSPI .vlw flash arrays.")
    ap.add_argument("ttf", help="source TrueType font (needs German umlaut glyphs)")
    ap.add_argument("--sizes", default="10,12,14,18,22,28",
                    help="comma-separated pixel sizes (default 10,12,14,18,22,28)")
    ap.add_argument("--out", default="fonts_vlw.h", help="output C header path")
    ap.add_argument("--prefix", default="vlw",
                    help="array/table name prefix (e.g. 'frak' → frak_10[], FRAK_FONTS[])")
    ap.add_argument("--extended", action="store_true",
                    help="also include German typographic marks (curly quotes, dashes, …)")
    ap.add_argument("--no-struct", action="store_true",
                    help="do not emit the shared 'struct VlwFont' (for a 2nd font header)")
    args = ap.parse_args()

    charset = BASE_CHARSET + (EXTRA_CHARSET if args.extended else [])
    tbl     = args.prefix.upper()
    sizes   = [int(s) for s in args.sizes.split(",") if s.strip()]
    entries = []
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// Auto-generated by sd_prep/make_vlw.py — do not edit by hand.\n")
        fh.write(f"// Smooth (VLW) fonts from {args.ttf}\n#pragma once\n#include <stdint.h>\n\n")
        for px in sizes:
            blob, asc, desc, lh = build_vlw(args.ttf, px, charset)
            name = f"{args.prefix}_{px}"
            emit_array(fh, name, blob)
            entries.append((name, px, lh, len(blob)))
            print(f"{name}: {len(blob):>6} bytes  ascent={asc} descent={desc} lineH={lh}")
        if not args.no_struct:
            fh.write("struct VlwFont { const uint8_t* data; uint8_t px; uint8_t lineH; };\n")
        fh.write(f"static const VlwFont {tbl}_FONTS[] = {{\n")
        for (name, px, lh, _) in entries:
            fh.write(f"  {{ {name}, {px}, {lh} }},\n")
        fh.write("};\n")
        fh.write(f"static const uint8_t {tbl}_FONT_COUNT = {len(entries)};\n")
    total = sum(e[3] for e in entries)
    print(f"Wrote {args.out}  ({len(entries)} sizes, {total:,} bytes of flash)")


if __name__ == "__main__":
    main()
