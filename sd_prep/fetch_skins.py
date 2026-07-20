#!/usr/bin/env python3
"""Sideload Mastermine skins onto the SD card.

The firmware can browse and download skins over WiFi by itself; this script is
the same thing done from a PC, for when there is no network on the device (or
you just want a card pre-loaded). It writes the IDENTICAL on-card layout, so
the firmware has exactly one code path for reading skins:

    <out>/mastermine/skins/<id>/meta.json
    <out>/mastermine/skins/<id>/<key>.tex

Skins are user-created content from https://mastermine.app/skins. They are
downloaded to your own card and are NOT redistributed with this firmware.

The store, as of 2026-07 (measured against the live API, not guessed):
    GET https://mastermine.app/api/topSkins?pageNo=N   -- no auth
    -> {"success": true, "skins": [...20 entries...], "eof": false}
    Pages are ZERO-BASED: pageNo=0 is the most-downloaded page. "eof" goes true
    on the last page, which is the signal to stop paginating.
    entry = {id, skinName, downloadCount, score, textures: {key: url}}
    every texture is a 512x512 JPEG on store.mastermine.app
Not every skin defines every key, so the firmware falls back to its built-in
default for whatever is missing.

Examples:
    python fetch_skins.py --list
    python fetch_skins.py --top 5  --out E:/
    python fetch_skins.py --id 5 --id 86 --out E:/
    python fetch_skins.py --id 5 --out ./card --preview

Needs Pillow (pip install pillow); HTTP is stdlib urllib.
"""

import argparse
import io
import json
import os
import sys
import urllib.request

from PIL import Image

API = "https://mastermine.app/api/topSkins?pageNo=%d"
UA = "ESP32_Mastermine-sd_prep/1.0 (+https://github.com/H4W9)"

# The 12 keys the firmware actually renders. menu1..menu4 are skipped on
# purpose: this hardware draws its own menus with the H4W9 shell, so pulling
# them would just be four more 512x512 JPEGs over a slow link for nothing.
TEX_KEYS = ["tile1", "tile2", "tile3", "tile4", "tile5", "tile6", "tile7", "tile8",
            "tileUnrevealed", "tileRevealed", "tileFlagged", "redSpot"]

# 128, matching TEX_SIZE in skin.h. 64 was too soft: a tile covers 60-100 px
# when the cube is zoomed in, and the thin strokes many published skins use for
# their digits turned to mush. Version 1 files (64 px) still load in the
# firmware, just less sharply.
TEX_SIZE = 128
TEX_MAGIC = b"MMTX"
TEX_VERSION = 2


def http_get(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def fetch_page(page):
    """Return (entries, eof) for a zero-based page number."""
    d = json.loads(http_get(API % page).decode("utf-8"))
    return d.get("skins", []), bool(d.get("eof"))


def to_tex(jpeg_bytes):
    """512x512 JPEG -> .tex blob: 16-byte header + 64x64 RGB565 little-endian.

    Little-endian because that is the byte order of a uint16_t on the ESP32, so
    the firmware can read the payload straight into a buffer and hand it to
    TFT_eSprite::pushImage with no per-pixel swap.
    """
    im = Image.open(io.BytesIO(jpeg_bytes))
    im = im.convert("RGB")
    if im.size != (TEX_SIZE, TEX_SIZE):
        # BOX matches the firmware, which decodes the JPEG in full and averages
        # each source region into the texture. Keeping them the same means a
        # sideloaded skin and a device-downloaded one look identical.
        im = im.resize((TEX_SIZE, TEX_SIZE), Image.BOX)

    out = bytearray(TEX_MAGIC)
    out += bytes([TEX_VERSION, TEX_SIZE, TEX_SIZE, 0])
    out += bytes(8)                                   # reserved
    px = im.load()
    for y in range(TEX_SIZE):
        for x in range(TEX_SIZE):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += bytes([v & 0xFF, (v >> 8) & 0xFF])  # little-endian
    return bytes(out)


def tex_to_image(blob):
    """Inverse of to_tex, for --preview / verification."""
    assert blob[:4] == TEX_MAGIC, "not a .tex file"
    w, h = blob[5], blob[6]
    im = Image.new("RGB", (w, h))
    px = im.load()
    off = 16
    for y in range(h):
        for x in range(w):
            v = blob[off] | (blob[off + 1] << 8)
            off += 2
            px[x, y] = (((v >> 11) & 0x1F) * 255 // 31,
                        ((v >> 5) & 0x3F) * 255 // 63,
                        (v & 0x1F) * 255 // 31)
    return im


def save_skin(entry, root, preview=False):
    sid = entry["id"]
    name = entry.get("skinName", "skin %d" % sid)
    textures = entry.get("textures", {})
    d = os.path.join(root, "mastermine", "skins", str(sid))
    os.makedirs(d, exist_ok=True)

    got = []
    for key in TEX_KEYS:
        url = textures.get(key)
        if not url:
            continue                                   # firmware falls back
        try:
            blob = to_tex(http_get(url))
        except Exception as e:
            print("    %-15s FAILED (%s)" % (key, e))
            continue
        with open(os.path.join(d, key + ".tex"), "wb") as f:
            f.write(blob)
        got.append(key)
        print("    %-15s ok" % key)
        if preview:
            tex_to_image(blob).save(os.path.join(d, key + ".png"))

    meta = {"id": sid, "name": name, "keys": got,
            "downloadCount": entry.get("downloadCount", 0)}
    with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)

    missing = [k for k in TEX_KEYS if k not in got]
    print("  saved %d/%d textures to %s%s"
          % (len(got), len(TEX_KEYS), d,
             ("  (missing: %s)" % ", ".join(missing)) if missing else ""))
    return len(got)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".", help="SD card root (e.g. E:/)")
    ap.add_argument("--id", type=int, action="append", default=[],
                    help="skin id to fetch; repeatable")
    ap.add_argument("--top", type=int, default=0,
                    help="fetch the N most-downloaded skins")
    ap.add_argument("--pages", type=int, default=4,
                    help="how many store pages to search for --id / --top")
    ap.add_argument("--list", action="store_true", help="just list what is on offer")
    ap.add_argument("--preview", action="store_true",
                    help="also write a .png next to each .tex, to eyeball it")
    args = ap.parse_args()

    if not args.list and not args.id and not args.top:
        ap.error("give --list, --top N, or one or more --id")

    catalog = []
    for p in range(0, args.pages):                     # pageNo is zero-based
        try:
            page, eof = fetch_page(p)
        except Exception as e:
            print("page %d failed: %s" % (p, e), file=sys.stderr)
            break
        catalog += page
        if eof or not page:
            break
        if args.top and len(catalog) >= args.top and not args.id:
            break

    print("%d skins in catalog" % len(catalog))
    if args.list:
        for e in catalog:
            keys = set(e.get("textures", {}))
            missing = [k for k in TEX_KEYS if k not in keys]
            print("  %-5s %-28s %6d dl  %2d/%d textures%s"
                  % (e["id"], e.get("skinName", "?")[:28], e.get("downloadCount", 0),
                     len(TEX_KEYS) - len(missing), len(TEX_KEYS),
                     ("  missing " + ",".join(missing)) if missing else ""))
        return

    wanted = []
    if args.top:
        wanted += catalog[:args.top]
    for i in args.id:
        m = [e for e in catalog if e["id"] == i]
        if not m:
            print("skin id %d not found in the first %d pages" % (i, args.pages),
                  file=sys.stderr)
            continue
        if m[0] not in wanted:
            wanted.append(m[0])

    total = 0
    for e in wanted:
        print("%s (id %s)" % (e.get("skinName", "?"), e["id"]))
        total += save_skin(e, args.out, preview=args.preview)
    print("done — %d textures across %d skins" % (total, len(wanted)))


if __name__ == "__main__":
    main()
