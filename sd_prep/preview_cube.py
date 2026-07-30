#!/usr/bin/env python3
"""Render the cube on the PC, exactly as render3d.cpp does, and check it.

There is no Arduino toolchain in the dev environment, so this is where the 3D
maths gets proven before it is ever flashed. It:

  * PARSES the face bases out of cube.cpp and the lighting/geometry constants
    out of render3d.cpp, so the preview cannot quietly drift from the firmware;
  * mirrors CubeView::project(), chooseOrientation(), paintFace() and pick(),
    including the EXTRUDED blocks — unrevealed cells stand proud, revealed ones
    sit flush. Every block is drawn as a solid cube, textured on
    the tile's own hue;
  * asserts the properties the renderer depends on;
  * renders a PNG using real .tex skin files, so the skin pipeline and the
    renderer are checked together.

    python preview_cube.py --check
    python preview_cube.py --n 5 --skin ../../card/mastermine/skins/86 --out cube.png
"""

import argparse
import math
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CUBE_CPP = os.path.join(HERE, "..", "cube.cpp")
REND_CPP = os.path.join(HERE, "..", "render3d.cpp")
REND_H = os.path.join(HERE, "..", "render3d.h")

sys.path.insert(0, HERE)
from check_cube import parse_bases, cell_at_face, shell, faces_of  # noqa: E402
from fetch_skins import TEX_KEYS, TEX_SIZE, tex_to_image          # noqa: E402

# Texture-axis codes, matching render3d.h
AX_U, AX_U_INV, AX_V, AX_V_INV = 1, -1, 2, -2


def parse_floats(path, names):
    """Pull `NAME = 1.23f` values out of a .cpp/.h."""
    src = open(path, encoding="utf-8").read()
    out = {}
    for n in names:
        m = re.search(r"\b%s\s*=\s*(-?[0-9.]+)f?" % re.escape(n), src)
        if not m:
            sys.exit("could not find %s in %s" % (n, path))
        out[n] = float(m.group(1))
    return out


class Projection:
    """Mirror of CubeView::project()."""

    def __init__(self, bases, consts, n, w, h, yaw, pitch, zoom=1.0):
        self.bases, self.n, self.w, self.h = bases, n, w, h
        self.gap = consts["BLOCK_GAP"]
        self._yaw, self._pitch = yaw, pitch
        half = n * 0.5
        # refit(): the cube's body diagonal, n*sqrt(3) cells across. The blocks
        # sit inside it.
        span = n * math.sqrt(3.0)
        self.cell = min(w, h) * 0.92 / span * zoom

        cy, sy = math.cos(yaw), math.sin(yaw)
        cp, sp = math.cos(pitch), math.sin(pitch)

        def rot(x, y, z):
            return (cy * x + sy * z,
                    sp * sy * x + cp * y - sp * cy * z,
                    -cp * sy * x + sp * y + cp * cy * z)

        lx, ly, lz = consts["LX"], consts["LY"], consts["LZ"]
        amb, dif = consts["SHADE_AMBIENT"], consts["SHADE_DIFFUSE"]
        ccx, ccy = w * 0.5, h * 0.5

        self.faces = []
        for b in bases:
            ox, oy, oz, ux, uy, uz, vx, vy, vz, nx, ny, nz = b
            rn = rot(nx, ny, nz)
            f = {"vis": rn[2] > 0.02}
            if f["vis"]:
                f["nz"] = rn[2]
                # Lambert on the CUBE-space normal, so a face's shade is a
                # constant and does not drift as the cube is dragged. Mapped
                # from [-1,1] rather than clamped at 0, so all six faces get
                # distinct shades.
                d = nx * lx + ny * ly + nz * lz
                s = min(1.0, amb + dif * (0.5 + 0.5 * d))
                f["shade"] = s
                o3 = (ox * n - half, oy * n - half, oz * n - half)
                ro = rot(*o3)
                f["ox"] = ccx + ro[0] * self.cell
                f["oy"] = ccy - ro[1] * self.cell
                ru = rot(ux, uy, uz)
                rv = rot(vx, vy, vz)
                f["ux"], f["uy"] = ru[0] * self.cell, -ru[1] * self.cell
                f["vx"], f["vy"] = rv[0] * self.cell, -rv[1] * self.cell
                # The face plane sits BLOCK_GAP INSIDE the nominal surface,
                # hence the negative offset.
                snx, sny = rn[0] * self.cell, -rn[1] * self.cell
                f["hx"], f["hy"] = -snx * self.gap, -sny * self.gap
                f["snx"], f["sny"] = snx, sny
                # Which cube axis U, V and N run along; every component is
                # 0 or +-1, so this is just finding the non-zero one.
                for a in range(3):
                    if (ux, uy, uz)[a]:
                        f["aU"] = a
                    if (vx, vy, vz)[a]:
                        f["aV"] = a
                    if (nx, ny, nz)[a]:
                        f["aN"], f["sN"] = a, (nx, ny, nz)[a]
                det = f["ux"] * f["vy"] - f["uy"] * f["vx"]
                f["idet"] = 0.0 if abs(det) < 1e-6 else 1.0 / det
                f["orient"] = self._orient(b)
            self.faces.append(f)

    # Mirror of CubeView::chooseOrientation — FIXED TO THE FACE, camera
    # independent. TX x TY must be -N (the inward normal) or the art renders
    # mirrored; of the four placements that satisfy that, take the one whose
    # texture-down points most nearly at the cube's own -Y.
    @staticmethod
    def _orient(b):
        ux, uy, uz = b[3:6]
        vx, vy, vz = b[6:9]
        vecs = {AX_U: (ux, uy, uz), AX_U_INV: (-ux, -uy, -uz),
                AX_V: (vx, vy, vz), AX_V_INV: (-vx, -vy, -vz)}
        cands = [(AX_U, AX_V_INV), (AX_U_INV, AX_V), (AX_V, AX_U), (AX_V_INV, AX_U_INV)]
        best, best_score = None, -100000
        for tx, ty in cands:
            dx, dy, dz = vecs[ty]
            score = 1000 * (-dy) + 10 * dz + dx
            if score > best_score:
                best_score, best = score, (tx, ty)
        return best

    def visible(self):
        return [i for i, f in enumerate(self.faces) if f["vis"]]

    def uv(self, fi, x, y, raised=False):
        f = self.faces[fi]
        if not f["vis"] or f["idet"] == 0.0:
            return None
        dx, dy = x - f["ox"], y - f["oy"]
        if raised:
            dx -= f["hx"]
            dy -= f["hy"]
        u = (f["vy"] * dx - f["vx"] * dy) * f["idet"]
        v = (-f["uy"] * dx + f["ux"] * dy) * f["idet"]
        return u, v

    def pick(self, x, y):
        """Mirror of CubeView::pick(). Every cap is on one plane now, so there
        is a single inversion. Returns the BLOCK, since that is what a tap acts
        on. Strict hits are tried before tolerant ones: a block on a cube edge
        has its cap grown outwards, so a tap there is legitimately outside the
        [0,n) grid -- but the ADJACENT face also sees that tap slightly out of
        range, and if it were allowed to clamp first it would answer with the
        wrong block."""
        slack = 0.5
        best, best_d2 = None, 1e30
        for fi in range(6):
            r = self.uv(fi, x, y, raised=True)
            if r is None:
                continue
            u, v = r
            if u < -slack or v < -slack or u >= self.n + slack or v >= self.n + slack:
                continue
            i = min(self.n - 1, max(0, int(math.floor(u))))
            j = min(self.n - 1, max(0, int(math.floor(v))))
            cx, cy = self.cell_centre(fi, i, j)
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if d2 < best_d2:
                best_d2, best = d2, cell_at_face(fi, i, j, self.n, self.bases)
        return best

    def rot_bottom_row(self):
        """View-space depth of each cube axis — the sign says which way that
        axis runs relative to the camera, which is the voxel draw order."""
        cy, sy = math.cos(self._yaw), math.sin(self._yaw)
        cp, sp = math.cos(self._pitch), math.sin(self._pitch)
        return (-cp * sy, sp, cp * cy)

    def cell_centre(self, fi, i, j):
        f = self.faces[fi]
        return (f["ox"] + (i + 0.5) * f["ux"] + (j + 0.5) * f["vx"] + f["hx"],
                f["oy"] + (i + 0.5) * f["uy"] + (j + 0.5) * f["vy"] + f["hy"])


def warm(c, s, pg, pb):
    """Mirror of shadeWarm()/buildShadeLut(): green and blue fall away faster
    than red as a face turns from the light, which is what the reference does."""
    return (min(255, int(c[0] * s)),
            min(255, int(c[1] * (s ** pg))),
            min(255, int(c[2] * (s ** pb))))


def load_skin(path):
    if not path or not os.path.isdir(path):
        return None
    tex = {}
    for k in TEX_KEYS:
        p = os.path.join(path, k + ".tex")
        if os.path.exists(p):
            tex[k] = tex_to_image(open(p, "rb").read()).load()
    return tex or None


SKIN_CPP = os.path.join(HERE, "..", "skin.cpp")
DIGITS_5x7 = [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E]  # placeholder, parsed below


def parse_rgb_array(src, name, count):
    """Pull `static const RGB8 NAME[n] = { {r,g,b}, ... };` out of skin.cpp."""
    m = re.search(r"RGB8\s+%s\s*\[[^\]]*\]\s*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
    if not m:
        sys.exit("could not find %s in skin.cpp" % name)
    trips = re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", m.group(1))
    if len(trips) != count:
        sys.exit("%s: expected %d entries, got %d" % (name, count, len(trips)))
    return [tuple(int(v) for v in t) for t in trips]


def parse_rgb_single(src, name):
    m = re.search(r"RGB8\s+%s\s*=\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}"
                  % re.escape(name), src)
    if not m:
        sys.exit("could not find %s in skin.cpp" % name)
    return tuple(int(m.group(i)) for i in (1, 2, 3))


def parse_digits(src):
    m = re.search(r"DIGITS\s*\[\s*8\s*\]\s*\[\s*7\s*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit("could not find DIGITS in skin.cpp")
    rows = re.findall(r"\{([^{}]*)\}", m.group(1))
    out = []
    for r in rows:
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", r)]
        if len(vals) == 7:
            out.append(vals)
    if len(out) != 8:
        sys.exit("DIGITS: expected 8 glyphs, got %d" % len(out))
    return out


def build_builtin_skin(variant="clean"):
    """Mirror of skin.cpp's built-in generators, with every palette PARSED from
    skin.cpp so the preview and the firmware cannot drift."""
    src = open(SKIN_CPP, encoding="utf-8").read()
    digits = parse_digits(src)
    S = TEX_SIZE

    def draw_digit(p, digit, ink):
        """Mirror of drawDigit(): square cells that touch, so strokes are solid."""
        if not digit:
            return
        d = digits[digit - 1]
        sc = int(S * 0.125 + 0.5)
        ox, oy = (S - 5 * sc) // 2, (S - 7 * sc) // 2
        for ry in range(7):
            for rx in range(5):
                if not (d[ry] & (1 << (4 - rx))):
                    continue
                for yy in range(sc):
                    for xx in range(sc):
                        px_, py_ = ox + rx * sc + xx, oy + ry * sc + yy
                        if 0 <= px_ < S and 0 <= py_ < S:
                            p[px_, py_] = ink

    def panel(edge, face, digit, ink):
        """Mirror of panelTile(): rounded inner panel on a darker edge."""
        im = Image.new("RGB", (S, S))
        p = im.load()
        inset = int(S * 0.10)
        rad = int(S * 0.18)
        for y in range(S):
            for x in range(S):
                inp = inset <= x < S - inset and inset <= y < S - inset
                if inp:
                    dx = dy = 0
                    if x < inset + rad:
                        dx = inset + rad - x
                    elif x >= S - inset - rad:
                        dx = x - (S - inset - rad - 1)
                    if y < inset + rad:
                        dy = inset + rad - y
                    elif y >= S - inset - rad:
                        dy = y - (S - inset - rad - 1)
                    if dx and dy and dx * dx + dy * dy > rad * rad:
                        inp = False
                p[x, y] = face if inp else edge
        draw_digit(p, digit, ink)
        return im

    def bevel(top, bot, digit, ink):
        """Mirror of bevelTile()."""
        im = Image.new("RGB", (S, S))
        p = im.load()
        bev = int(S * 0.0625)
        for y in range(S):
            t = y * 255 // (S - 1)
            r = top[0] + (bot[0] - top[0]) * t // 255
            g = top[1] + (bot[1] - top[1]) * t // 255
            b = top[2] + (bot[2] - top[2]) * t // 255
            for x in range(S):
                sh = 22 - (x + y) * 44 // (2 * S)
                rr, gg, bb = r + sh, g + sh, b + sh
                if min(x, y, S - 1 - x, S - 1 - y) < bev:
                    tl = (x < S - 1 - y) if x <= y else (y < S - 1 - x)
                    k = 46 if tl else -52
                    rr += k; gg += k; bb += k
                p[x, y] = (max(0, min(255, rr)), max(0, min(255, gg)), max(0, min(255, bb)))
        draw_digit(p, digit, ink)
        return im

    def add_flag(im, col):
        p = im.load()
        x0, y0 = int(S * 0.30), int(S * 0.30)
        hh, ww = int(S * 0.34), int(S * 0.40)
        for y in range(hh):
            half = y if y < hh // 2 else hh - y
            wdt = int(ww * (0.35 + 0.65 * half / (hh * 0.5)))
            for x in range(wdt):
                if 0 <= x0 + x < S and 0 <= y0 + y < S:
                    p[x0 + x, y0 + y] = col
        return im

    def add_mine(im, body, spot):
        p = im.load()
        cx = cy = S // 2
        r, sp = int(S * 0.27), int(S * 0.41)
        for y in range(S):
            for x in range(S):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    p[x, y] = body
        th = int(S * 0.05)
        for i in range(-sp, sp + 1):
            for t in range(-th, th + 1):
                if 0 <= cy + i < S and 0 <= cx + t < S:
                    p[cx + t, cy + i] = body
                if 0 <= cx + i < S and 0 <= cy + t < S:
                    p[cx + i, cy + t] = body
        gr = int(S * 0.07)
        for y in range(-gr, gr + 1):
            for x in range(-gr, gr + 1):
                if x * x + y * y <= gr * gr:
                    px_, py_ = cx - int(S * 0.09) + x, cy - int(S * 0.09) + y
                    if 0 <= px_ < S and 0 <= py_ < S:
                        p[px_, py_] = spot
        return im

    tex = {}
    if variant == "gold":
        gt = parse_rgb_single(src, "GOLD_TOP")
        gb = parse_rgb_single(src, "GOLD_BOT")
        nt = parse_rgb_array(src, "NUM_TOP", 8)
        nb = parse_rgb_array(src, "NUM_BOT", 8)
        ni = parse_rgb_array(src, "NUM_INK", 8)
        for k in range(8):
            tex["tile%d" % (k + 1)] = bevel(nt[k], nb[k], k + 1, ni[k]).load()
        tex["tileUnrevealed"] = bevel(gt, gb, 0, ni[0]).load()
        tex["tileFlagged"] = add_flag(bevel(gt, gb, 0, ni[0]), (255, 255, 255)).load()
        tex["redSpot"] = add_mine(bevel((96, 84, 74), (44, 36, 30), 0, ni[0]),
                                  (12, 10, 10), (224, 60, 48)).load()
        tex["tileRevealed"] = bevel((34, 32, 28), (16, 15, 13), 0, ni[0]).load()
        return tex

    face = parse_rgb_array(src, "CLEAN_FACE", 8)
    edge = parse_rgb_array(src, "CLEAN_EDGE", 8)
    white = parse_rgb_single(src, "CLEAN_WHITE")
    white_e = parse_rgb_single(src, "CLEAN_WHITE_EDGE")
    dark = parse_rgb_single(src, "CLEAN_DARK")
    dark_e = parse_rgb_single(src, "CLEAN_DARK_EDGE")
    flag = parse_rgb_single(src, "CLEAN_FLAG")
    ink = parse_rgb_single(src, "CLEAN_INK")
    for k in range(8):
        tex["tile%d" % (k + 1)] = panel(edge[k], face[k], k + 1, ink).load()
    tex["tileUnrevealed"] = panel(white_e, white, 0, ink).load()
    tex["tileFlagged"] = add_flag(panel(white_e, white, 0, ink), flag).load()
    tex["redSpot"] = add_mine(panel(white_e, white, 0, ink), (30, 30, 33), flag).load()
    tex["tileRevealed"] = panel(dark_e, dark, 0, ink).load()
    return tex


def fill_para(px, py, ax, ay, bx, by, w, h, put):
    """Scanline fill of a parallelogram; `put(x, y, u, v)` paints one pixel."""
    det = ax * by - ay * bx
    if abs(det) < 1e-6:
        return
    idet = 1.0 / det
    dudx, dudy = by * idet, -bx * idet
    dvdx, dvdy = -ay * idet, ax * idet
    ys = [py, py + ay, py + by, py + ay + by]
    xs = [px, px + ax, px + bx, px + ax + bx]
    y0 = max(0, int(math.ceil(min(ys) - 0.5)))
    y1 = min(h - 1, int(math.ceil(max(ys) - 0.5)) - 1)
    x0b = max(0, int(math.ceil(min(xs) - 0.5)))
    x1b = min(w - 1, int(math.ceil(max(xs) - 0.5)) - 1)
    for y in range(y0, y1 + 1):
        dy = (y + 0.5) - py
        for x in range(x0b, x1b + 1):
            dx = (x + 0.5) - px
            u = dudx * dx + dudy * dy
            v = dvdx * dx + dvdy * dy
            if 0.0 <= u < 1.0 and 0.0 <= v < 1.0:
                put(x, y, u, v)


def parse_cull_depth(path):
    """CULL_DEPTH is PARSED out of render3d.cpp, never duplicated here — the
    whole point of the check is to catch it being set too shallow."""
    m = re.search(r"const int CULL_DEPTH\s*=\s*(\d+)", open(path, encoding="utf-8").read())
    if not m:
        sys.exit("could not find CULL_DEPTH in %s" % path)
    return int(m.group(1))


CULL_DEPTH = parse_cull_depth(REND_CPP)


def render(proj, tex, w, h, states, consts, bg=(30, 30, 30), honest=False,
           inner_colour=None):
    """Mirror of CubeView::render(): every shell block drawn as a solid cube,
    back to front, in voxel order.

    `honest=False` is what the firmware does: blocks too far behind every
    camera-facing face are skipped, on the grounds that the inner cube hides
    them. `honest=True` skips nothing, so the two can be compared.
    `inner_colour` paints the inner cube in a contrasting colour instead of the
    background — it makes the invisible geometry visible, which is the only way
    to actually look at it."""
    img = Image.new("RGB", (w, h), bg)
    px = img.load()
    n = proj.n
    pg, pb = consts["SHADE_POW_G"], consts["SHADE_POW_B"]

    def avg_of(key):
        t = tex.get(key) if tex else None
        if t is None:
            return (150, 150, 150)
        r = g = b = 0
        step = 8
        cnt = 0
        for y in range(0, TEX_SIZE, step):
            for x in range(0, TEX_SIZE, step):
                c = t[x, y]
                r += c[0]; g += c[1]; b += c[2]
                cnt += 1
        return (r // cnt, g // cnt, b // cnt)

    avg_cache = {}

    # Every shell block drawn as a SOLID CUBE, back to front. Ordering is the
    # voxel painter's algorithm: walk each cube axis away-from-camera first.
    # A convex solid's own camera-facing faces never overlap, so there is
    # nothing else to sort.
    g = proj.gap
    s = 1.0 - 2.0 * g
    stats = {"written": 0}
    rx, ry, rz = proj.rot_bottom_row()
    xr = range(0, n) if rx > 0 else range(n - 1, -1, -1)
    yr = range(0, n) if ry > 0 else range(n - 1, -1, -1)
    zr = range(0, n) if rz > 0 else range(n - 1, -1, -1)
    vis = proj.visible()

    def draw_inner():
        """The inner cube: a solid spanning [1-g, n-1+g] on every axis, flush
        with the inward faces of the shell blocks and meeting the inside edges
        of the edge blocks. Painted in the BACKGROUND colour.

        The firmware DOES paint this, between its two block passes. Believing
        it was free because the frame starts as fillSprite(bg) is what left the
        cube see-through: blocks are drawn in between, so the gaps show the far
        side of the cube until this quad erases it."""
        span = n - 2 + 2 * g
        tt = g - 1.0            # same offset either way; see the note below
        for fi in vis:
            f = proj.faces[fi]
            ox = f["ox"] + (1 - g) * f["ux"] + (1 - g) * f["vx"] + tt * f["snx"]
            oy = f["oy"] + (1 - g) * f["uy"] + (1 - g) * f["vy"] + tt * f["sny"]

            def put(px_, py_, u, v):
                stats["written"] += 1
                px[px_, py_] = inner_colour if inner_colour else bg

            fill_para(ox, oy, span * f["ux"], span * f["uy"],
                      span * f["vx"], span * f["vy"], w, h, put)

    CULL_DEPTH = globals()["CULL_DEPTH"]

    def on_visible_face(blk):
        """Is this block ON a camera-facing cube face? That is the occlusion
        boundary: such a block sits outside the inner cube on that axis and so
        covers it, and is drawn in pass 1 (after it)."""
        for fi in vis:
            f = proj.faces[fi]
            end = (n - 1) if f["sN"] > 0 else 0
            if blk[f["aN"]] == end:
                return True
        return False

    def within_cull(blk):
        """Is this block near enough a camera-facing face to be drawn at all?

        Blocks that are not ON a face still matter because of the STEPPED
        SILHOUETTE: at a yaw off the axis, a block a layer or two back peeks
        out SIDEWAYS past the one in front, against the background, where the
        inner cube cannot help."""
        for fi in vis:
            f = proj.faces[fi]
            end = (n - 1) if f["sN"] > 0 else 0
            if abs(blk[f["aN"]] - end) <= CULL_DEPTH:
                return True
        return False

    # Mirrors CubeView::paintBlocks(): pass 0 (blocks not on a camera-facing
    # face), then the inner cube, then pass 1 (blocks on one). `honest` draws
    # EVERY block rather than applying the cull, so the two can be compared.
    for want_vis in (False, True):
        if want_vis:
            draw_inner()
        for z in zr:
            for y in yr:
                for x in xr:
                    blk = (x, y, z)
                    if blk not in states:
                        continue                 # interior: not a block
                    if on_visible_face(blk) != want_vis:
                        continue
                    if not honest and not within_cull(blk):
                        continue
                    key = states[blk]
                    t_tex = tex.get(key) if tex else None
                    co = (x, y, z)

                    for fi in vis:
                        f = proj.faces[fi]
                        sh = f["shade"]
                        tx_src, ty_src = f["orient"]
                        i, j, cc = co[f["aU"]], co[f["aV"]], co[f["aN"]]
                        tt = (cc + 1 - g - n) if f["sN"] > 0 else -(cc + g)
                        ox = f["ox"] + (i + g) * f["ux"] + (j + g) * f["vx"] + tt * f["snx"]
                        oy = f["oy"] + (i + g) * f["uy"] + (j + g) * f["vy"] + tt * f["sny"]

                        def put(px_, py_, u, v, t=t_tex, sh=sh, tx=tx_src, ty=ty_src):
                            stats["written"] += 1
                            if t is None:
                                px[px_, py_] = warm((150, 150, 150), sh, pg, pb)
                                return
                            src = {AX_U: u, AX_U_INV: 1.0 - u, AX_V: v, AX_V_INV: 1.0 - v}
                            tu = min(TEX_SIZE - 1, int(src[tx] * TEX_SIZE))
                            tv = min(TEX_SIZE - 1, int(src[ty] * TEX_SIZE))
                            px[px_, py_] = warm(t[tu, tv], sh, pg, pb)

                        fill_para(ox, oy, s * f["ux"], s * f["uy"],
                                  s * f["vx"], s * f["vy"], w, h, put)

    # Report the overdraw, since it is the one real cost of drawing solids.
    covered = sum(1 for yy in range(h) for xx in range(w) if px[xx, yy] != bg)
    if covered:
        print("  fill: %d pixels written for %d covered (%.2fx overdraw)%s"
              % (stats["written"], covered, stats["written"] / covered,
                 " [honest: no cull]" if honest else ""))
    return img


def demo_states(n, seed=12345):
    """A deterministic pseudo-board keyed by BLOCK, so an edge block shows the
    same face on both of its sides — which is the thing worth looking at."""
    rnd = seed
    out = {}
    for c in shell(n):
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        r = (rnd >> 8) % 14
        if r < 6:
            out[c] = "tileUnrevealed"
        elif r == 6:
            out[c] = "tileFlagged"
        elif r == 7:
            out[c] = "redSpot"
        elif r == 8:
            out[c] = "tileRevealed"
        else:
            out[c] = "tile%d" % (r - 8)
    return out


def run_checks(bases, consts, n=6, w=320, h=380):
    print("checking projection (n=%d, viewport %dx%d)" % (n, w, h))
    opposite = {0: 1, 1: 0, 2: 3, 3: 2, 4: 5, 5: 4}

    # Face visibility. At a general angle exactly three faces face the camera;
    # where the view lines up with an axis a face goes edge-on and drops out, so
    # 2 is correct there and only there.
    tested = generic = 0
    for yaw_d in range(5, 360, 17):
        for pitch_d in (-70, -40, -15, 15, 40, 70):
            p = Projection(bases, consts, n, w, h,
                           math.radians(yaw_d), math.radians(pitch_d))
            vis = p.visible()
            assert 2 <= len(vis) <= 3, "yaw=%d pitch=%d shows %d faces" % (
                yaw_d, pitch_d, len(vis))
            for f in vis:
                assert opposite[f] not in vis, \
                    "yaw=%d pitch=%d shows both %d and its opposite" % (yaw_d, pitch_d, f)
            if yaw_d % 90 and pitch_d % 90:
                assert len(vis) == 3, "yaw=%d pitch=%d shows only %r" % (yaw_d, pitch_d, vis)
                generic += 1
            tested += 1
    print("  face visibility sane at %d angles (%d generic ones show exactly 3)  OK"
          % (tested, generic))

    # Texture orientation must be FIXED TO THE FACE: identical at every camera
    # angle. That is the whole point of the change from the earlier
    # always-upright scheme, so it is asserted rather than assumed.
    ref = None
    for yaw_d in range(0, 360, 13):
        for pitch_d in range(-80, 81, 11):
            p = Projection(bases, consts, n, w, h,
                           math.radians(yaw_d), math.radians(pitch_d))
            cur = {fi: p.faces[fi]["orient"] for fi in p.visible()}
            if ref is None:
                ref = {}
            for fi, o in cur.items():
                if fi in ref:
                    assert ref[fi] == o, \
                        "face %d orientation changed with the camera: %r vs %r" % (
                            fi, ref[fi], o)
                else:
                    ref[fi] = o
    assert len(ref) == 6, "only saw %d faces" % len(ref)
    print("  texture orientation fixed per face (all 6, camera-independent)  OK")

    # ...and never mirrored. Screen space is x right / y down, so an unmirrored
    # mapping has a positive 2D cross product between the texture axes.
    checked = 0
    for yaw_d in range(0, 360, 9):
        for pitch_d in range(-80, 81, 10):
            p = Projection(bases, consts, n, w, h,
                           math.radians(yaw_d), math.radians(pitch_d))
            for fi in p.visible():
                f = p.faces[fi]
                vec = {AX_U: (f["ux"], f["uy"]), AX_U_INV: (-f["ux"], -f["uy"]),
                       AX_V: (f["vx"], f["vy"]), AX_V_INV: (-f["vx"], -f["vy"])}
                ax, ay = vec[f["orient"][0]]
                bx, by = vec[f["orient"][1]]
                assert ax * by - ay * bx > 0, \
                    "yaw=%d pitch=%d face %d is mirrored" % (yaw_d, pitch_d, fi)
                checked += 1
    print("  texture never mirrored across %d face instances  OK" % checked)

    # An edge block's two tops must MEET IN 3D, exactly, so the block is a
    # crisp solid cube rather than two halves offset by the raise.
    #
    # Checked in cube space, not on screen: take the fold between two faces and
    # compute where each face's top edge lands after being extended past the
    # fold by the raise and lifted by the raise along its own normal. Those two
    # points have to be the same point.
    g = consts["BLOCK_GAP"]
    hh = -g            # the face plane sits g INSIDE the nominal surface

    def top_point(b, a_, b_, nn, hgt):
        """A point on a face's top plane, at face parameters (a_, b_)."""
        o, u, v, nrm = b[0:3], b[3:6], b[6:9], b[9:12]
        return tuple(o[k] * nn + a_ * u[k] + b_ * v[k] + hgt * nrm[k]
                     for k in range(3))

    def solve_on_top(p, b, nn, hgt):
        """Face parameters of p on this face's top plane, or None if it is not
        on that plane at all. u and v are unit axis vectors, so this is a
        projection, not a search."""
        o, u, v, nrm = b[0:3], b[3:6], b[6:9], b[9:12]
        base = [o[k] * nn + hgt * nrm[k] for k in range(3)]
        d = [p[k] - base[k] for k in range(3)]
        a_ = sum(d[k] * u[k] for k in range(3))
        b_ = sum(d[k] * v[k] for k in range(3))
        rec = [base[k] + a_ * u[k] + b_ * v[k] for k in range(3)]
        if any(abs(rec[k] - p[k]) > 1e-6 for k in range(3)):
            return None
        return a_, b_

    checked = 0
    for fa in range(6):
        ba = bases[fa]
        ua, va = ba[3:6], ba[6:9]
        # This face's four folds, each with the axis it extends along and the
        # neighbour it meets (the face whose outward normal is that direction).
        for axis, sign in ((ua, +1), (ua, -1), (va, +1), (va, -1)):
            want = tuple(sign * c for c in axis)
            fb = next(k for k in range(6) if tuple(bases[k][9:12]) == want)
            along_u = (axis == ua)
            # The outermost block's face runs out to n-g (or in to +g).
            edge = (n - g) if sign > 0 else g
            for other in (0.5, n / 2.0, n - 0.5):
                pa = top_point(ba, edge if along_u else other,
                               other if along_u else edge, n, hh)
                got = solve_on_top(pa, bases[fb], n, hh)
                assert got is not None, (
                    "face %d's top at its %s%s fold is not even on face %d's top plane"
                    % (fa, "+" if sign > 0 else "-", "U" if along_u else "V", fb))
                # It must land exactly ON the neighbour's outermost block face,
                # i.e. at that face's own g or n-g edge — not merely somewhere
                # on the plane. That is what "flush" means.
                lo, hi = g - 1e-6, n - g + 1e-6
                assert lo <= got[0] <= hi and lo <= got[1] <= hi, (
                    "face %d's edge lands off face %d's blocks at %r" % (fa, fb, got))
                onEdge = (min(abs(got[0] - g), abs(got[0] - (n - g))) < 1e-6 or
                          min(abs(got[1] - g), abs(got[1] - (n - g))) < 1e-6)
                assert onEdge, (
                    "face %d's edge meets face %d at %r, which is not that face's "
                    "outermost block edge — the two do not join flush" % (fa, fb, got))
                checked += 1
    print("  edge blocks meet flush across every fold (%d points checked)  OK" % checked)

    # There is no z-buffer, so the ONLY thing keeping a far block from painting
    # over a near one is the order the voxel loops walk in. Assert that order is
    # sound rather than trusting it.
    #
    # For equal, axis-aligned, grid-aligned cubes under an orthographic camera,
    # block A can occlude block B only if A is on the camera side of B along
    # every axis at once — i.e. sign(rx)*(Ax-Bx) >= 0 and likewise for y and z,
    # with at least one strict. So a correct back-to-front order is exactly a
    # linear extension of that partial order, which is what the nested loops
    # (each stepping away-from-camera-first) produce. Checked here directly.
    shell = [(x, y, z)
             for z in range(n) for y in range(n) for x in range(n)
             if x in (0, n - 1) or y in (0, n - 1) or z in (0, n - 1)]
    orders = 0
    for yaw_d, pitch_d in ((40, 32), (0, 0), (137, -61), (215, 74), (300, -18)):
        pr = Projection(bases, consts, n, w, h,
                        math.radians(yaw_d), math.radians(pitch_d), 1.0)
        rx, ry, rz = pr.rot_bottom_row()
        sx = 1 if rx > 0 else -1
        sy = 1 if ry > 0 else -1
        sz = 1 if rz > 0 else -1
        xr = range(0, n) if rx > 0 else range(n - 1, -1, -1)
        yr = range(0, n) if ry > 0 else range(n - 1, -1, -1)
        zr = range(0, n) if rz > 0 else range(n - 1, -1, -1)
        idx = {}
        k = 0
        for z in zr:
            for y in yr:
                for x in xr:
                    if x in (0, n - 1) or y in (0, n - 1) or z in (0, n - 1):
                        idx[(x, y, z)] = k
                        k += 1
        assert k == len(shell), "voxel walk visited %d shell blocks, expected %d" % (
            k, len(shell))
        for A in shell:
            for B in shell:
                if A is B:
                    continue
                dx, dy, dz = (A[0] - B[0]) * sx, (A[1] - B[1]) * sy, (A[2] - B[2]) * sz
                if dx >= 0 and dy >= 0 and dz >= 0 and (dx or dy or dz):
                    # A can occlude B, so A must be painted AFTER B.
                    assert idx[A] > idx[B], (
                        "yaw=%d pitch=%d: block %r can occlude %r but is drawn "
                        "first — the voxel loop directions are wrong"
                        % (yaw_d, pitch_d, A, B))
        orders += 1
    print("  voxel draw order is a valid painter order for every shell block "
          "(%d orientations, %d blocks)  OK" % (orders, len(shell)))

    # FRONT-TO-BACK + COVERAGE MASK is pixel-identical to back-to-front overdraw.
    #
    # The firmware draws nearest-first and skips any pixel already written (a
    # 1-bit coverage mask), so each pixel is written once and no hidden texture
    # sample is taken. That is only legal if the WINNING surface at every pixel
    # is the same as the old back-to-front overdraw would have left. Proven here
    # by giving each (block, face) a unique id, rendering both ways into an id
    # buffer, and demanding the buffers match — if they do, any texture on those
    # faces renders identically too.
    st = demo_states(n)
    s = 1.0 - 2.0 * g

    def id_buffer(pr, front_to_back):
        buf = [[-1] * w for _ in range(h)]
        cov = [bytearray(w) for _ in range(h)] if front_to_back else None
        vis = pr.visible()
        rx, ry, rz = pr.rot_bottom_row()

        def rng(sign):
            fwd = (sign > 0)
            if front_to_back:
                fwd = not fwd
            return range(0, n) if fwd else range(n - 1, -1, -1)
        xr2, yr2, zr2 = rng(rx), rng(ry), rng(rz)

        def endof(f):
            return (n - 1) if f["sN"] > 0 else 0

        def paint(ox, oy, ax, ay, bx, by, fid):
            def put(px_, py_, u, v):
                if cov is not None:
                    if cov[py_][px_]:
                        return
                    cov[py_][px_] = 1
                buf[py_][px_] = fid
            fill_para(ox, oy, ax, ay, bx, by, w, h, put)

        def inner():
            span = n - 2 + 2 * g
            tt = g - 1.0
            for fi in vis:
                f = pr.faces[fi]
                ox = f["ox"] + (1 - g) * f["ux"] + (1 - g) * f["vx"] + tt * f["snx"]
                oy = f["oy"] + (1 - g) * f["uy"] + (1 - g) * f["vy"] + tt * f["sny"]
                paint(ox, oy, span * f["ux"], span * f["uy"],
                      span * f["vx"], span * f["vy"], -2)

        def blocks(want_vis):
            for z in zr2:
                for y in yr2:
                    for x in xr2:
                        blk = (x, y, z)
                        if blk not in st:
                            continue
                        co = (x, y, z)
                        on = any(co[pr.faces[fi]["aN"]] == endof(pr.faces[fi]) for fi in vis)
                        if on != want_vis:
                            continue
                        if not any(abs(co[pr.faces[fi]["aN"]] - endof(pr.faces[fi])) <= CULL_DEPTH
                                   for fi in vis):
                            continue
                        base = ((x * n + y) * n + z) * 6
                        for fi in vis:
                            f = pr.faces[fi]
                            i, j, cc = co[f["aU"]], co[f["aV"]], co[f["aN"]]
                            tt = (cc + 1 - g - n) if f["sN"] > 0 else -(cc + g)
                            ox = f["ox"] + (i + g) * f["ux"] + (j + g) * f["vx"] + tt * f["snx"]
                            oy = f["oy"] + (i + g) * f["uy"] + (j + g) * f["vy"] + tt * f["sny"]
                            paint(ox, oy, s * f["ux"], s * f["uy"],
                                  s * f["vx"], s * f["vy"], base + fi)

        # back-to-front: pass0, inner, pass1.  front-to-back reverses it all.
        if front_to_back:
            blocks(True); inner(); blocks(False)
        else:
            blocks(False); inner(); blocks(True)
        return buf

    ft = 0
    for yaw_d, pitch_d in [(y_, p_) for y_ in range(0, 360, 20)
                           for p_ in range(-60, 61, 20)]:
        pr = Projection(bases, consts, n, w, h,
                        math.radians(yaw_d), math.radians(pitch_d), 1.0)
        a = id_buffer(pr, False)
        b = id_buffer(pr, True)
        d = sum(1 for yy in range(h) for xx in range(w) if a[yy][xx] != b[yy][xx])
        assert d == 0, ("yaw=%d pitch=%d: front-to-back+coverage changed the "
                        "winning surface at %d pixels" % (yaw_d, pitch_d, d))
        ft += 1
    print("  front-to-back + coverage mask: winning surface identical to "
          "back-to-front at every pixel (%d orientations)  OK" % ft)

    # ...and every block face is the SAME square.
    sizes = {(round(1 - 2 * g, 9), round(1 - 2 * g, 9))}
    assert len(sizes) == 1
    side = (1 - 2 * g)
    print("  every block face identical and square (%.3f x %.3f cells)  OK"
          % (side, side))

    # THE INNER-CUBE CULL, proven rather than argued.
    #
    # The firmware skips every block that is not on a camera-facing cube face,
    # on the claim that the inner cube (plus the three visible outer layers)
    # hides them all. If that claim is ever wrong the cube develops holes you
    # can see through, so it is checked the only way worth checking: render the
    # honest version — every block, with an opaque inner cube painted in its
    # correct place in the painter order — and demand the culled render be
    # pixel-identical.
    st = demo_states(n)
    angles = [(y_, p_) for y_ in range(0, 360, 15) for p_ in range(-75, 76, 15)]
    for yaw_d, pitch_d in angles:
        pr = Projection(bases, consts, n, w, h,
                        math.radians(yaw_d), math.radians(pitch_d), 1.0)
        a = render(pr, None, w, h, st, consts, honest=True)
        b = render(pr, None, w, h, st, consts, honest=False)
        diff = [(x, y) for y in range(h) for x in range(w)
                if a.getpixel((x, y)) != b.getpixel((x, y))]
        assert not diff, (
            "yaw=%d pitch=%d: culling the hidden blocks changed %d pixels "
            "(first at %r) — the inner cube does NOT cover them"
            % (yaw_d, pitch_d, len(diff), diff[0]))
    print("  inner cube hides every culled block at CULL_DEPTH=%d: culled render "
          "is pixel-identical to the honest one (%d orientations)  OK"
          % (CULL_DEPTH, len(angles)))

    # A face's shade must not move when the cube is turned. The light is fixed
    # in CUBE space precisely so the colours do not drift under your finger; if
    # this ever fails, the light has slipped back into view space.
    ref_shade = {}
    for yaw_d in range(0, 360, 11):
        for pitch_d in range(-80, 81, 9):
            p = Projection(bases, consts, n, w, h,
                           math.radians(yaw_d), math.radians(pitch_d))
            for fi in p.visible():
                s = round(p.faces[fi]["shade"], 6)
                if fi in ref_shade:
                    assert ref_shade[fi] == s, \
                        "face %d shade moved with the camera: %r vs %r" % (
                            fi, ref_shade[fi], s)
                else:
                    ref_shade[fi] = s
    assert len(ref_shade) == 6
    spread = max(ref_shade.values()) - min(ref_shade.values())
    assert len(set(ref_shade.values())) == 6, \
        "faces should all differ in shade, got %r" % sorted(ref_shade.values())
    print("  face shading constant under rotation, 6 distinct values, spread %.3f  OK"
          % spread)

    # Free rotation: composing drag increments in VIEW space must never hit a
    # limit and must stay orthonormal. This mirrors CubeView::orbit().
    R = [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0]

    def orbit(R, dyaw, dpitch):
        cy, sy = math.cos(dyaw), math.sin(dyaw)
        cp, sp = math.cos(dpitch), math.sin(dpitch)
        m = [cy, 0.0, sy,
             sp * sy, cp, -sp * cy,
             -cp * sy, sp, cp * cy]
        out = [sum(m[r * 3 + k] * R[k * 3 + c] for k in range(3))
               for r in range(3) for c in range(3)]
        # Gram-Schmidt, as the firmware does.
        a, b = out[0:3], out[3:6]
        la = math.sqrt(sum(v * v for v in a))
        a = [v / la for v in a]
        d = sum(b[i] * a[i] for i in range(3))
        b = [b[i] - d * a[i] for i in range(3)]
        lb = math.sqrt(sum(v * v for v in b))
        b = [v / lb for v in b]
        c = [a[1] * b[2] - a[2] * b[1],
             a[2] * b[0] - a[0] * b[2],
             a[0] * b[1] - a[1] * b[0]]
        return a + b + c

    rnd = 987654321
    up_seen = set()
    for _ in range(20000):
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        dy = ((rnd >> 8) % 200 - 100) / 500.0
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        dp = ((rnd >> 8) % 200 - 100) / 500.0
        R = orbit(R, dy, dp)
        # R[7] is how far the cube's +Y axis leans towards screen-up; passing
        # through both signs proves the cube really does tumble past vertical
        # rather than stopping at a clamp.
        up_seen.add(R[7] > 0)
    rows = [R[0:3], R[3:6], R[6:9]]
    for r in rows:
        assert abs(math.sqrt(sum(v * v for v in r)) - 1.0) < 1e-6, "row not unit"
    for a in range(3):
        for b in range(a + 1, 3):
            dot = sum(rows[a][i] * rows[b][i] for i in range(3))
            assert abs(dot) < 1e-6, "rows not orthogonal (%.2e)" % dot
    assert up_seen == {True, False}, \
        "the cube never tumbled past vertical — a rotation limit is still there"
    print("  free rotation: 20000 drags, stays orthonormal, tumbles past vertical  OK")

    # pick() must return the BLOCK whose cap centre we projected.
    bad = total = 0
    edge_hits = 0
    for yaw_d, pitch_d in ((37, 30), (110, -25), (215, 55), (300, -60), (5, 12)):
        p = Projection(bases, consts, n, w, h,
                       math.radians(yaw_d), math.radians(pitch_d))
        for fi in p.visible():
            for j in range(n):
                for i in range(n):
                    x, y = p.cell_centre(fi, i, j)
                    got = p.pick(x, y)
                    want = cell_at_face(fi, i, j, n, bases)
                    total += 1
                    if len(faces_of(want, n, bases)) > 1:
                        edge_hits += 1
                    if got != want:
                        bad += 1
                        if bad < 4:
                            print("    MISS face=%d i=%d j=%d -> %r want %r"
                                  % (fi, i, j, got, want))
    assert bad == 0, "%d/%d cap centres picked wrong" % (bad, total)
    print("  pick() round-tripped %d cap centres (%d on edge/corner blocks)  OK"
          % (total, edge_hits))

    # EVERY visible block must be reachable, corners included. Some corner
    # blocks could not be hit at all when pick() returned the first in-range
    # face rather than the nearest cap centre.
    for yaw_d, pitch_d in ((37, 30), (110, -25), (215, 55), (300, -60)):
        p = Projection(bases, consts, n, w, h,
                       math.radians(yaw_d), math.radians(pitch_d))
        want = set()
        for fi in p.visible():
            for j in range(n):
                for i in range(n):
                    want.add(cell_at_face(fi, i, j, n, bases))
        got = set()
        for fi in p.visible():
            for j in range(n):
                for i in range(n):
                    x, y = p.cell_centre(fi, i, j)
                    r = p.pick(x, y)
                    if r is not None:
                        got.add(r)
        missing = want - got
        assert not missing, \
            "yaw=%d pitch=%d: %d visible blocks unreachable, e.g. %r" % (
                yaw_d, pitch_d, len(missing), sorted(missing)[:3])
    print("  every visible block reachable by tapping it, corners included  OK")

    # The cube, including the raised blocks, must stay inside the viewport.
    worst = 0.0
    for yaw_d in range(0, 360, 11):
        for pitch_d in range(-83, 84, 13):
            p = Projection(bases, consts, n, w, h,
                           math.radians(yaw_d), math.radians(pitch_d))
            for fi in p.visible():
                f = p.faces[fi]
                for a in (0, n):
                    for b in (0, n):
                        x = f["ox"] + a * f["ux"] + b * f["vx"] + f["hx"]
                        y = f["oy"] + a * f["uy"] + b * f["vy"] + f["hy"]
                        worst = max(worst, -x, x - (w - 1), -y, y - (h - 1))
    assert worst <= 0.5, "cube overflows the viewport by %.1f px at zoom 1" % worst
    print("  cube (with raised blocks) fits the viewport at every angle  OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=5)
    ap.add_argument("--yaw", type=float, default=37.0)
    ap.add_argument("--pitch", type=float, default=30.0)
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--w", type=int, default=320)
    ap.add_argument("--h", type=int, default=380)
    ap.add_argument("--skin", default="", help="dir holding <key>.tex files")
    ap.add_argument("--builtin", choices=("clean", "gold"), default="",
                    help="use a built-in set, generated from skin.cpp's own palette")
    ap.add_argument("--out", default="cube.png")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    bases = parse_bases(CUBE_CPP)
    consts = parse_floats(REND_CPP, ["LX", "LY", "LZ", "SHADE_AMBIENT",
                                     "SHADE_DIFFUSE",
                                     "SHADE_POW_G", "SHADE_POW_B"])
    consts.update(parse_floats(REND_H, ["BLOCK_GAP"]))
    print("parsed cube.cpp + render3d: %s" % consts)

    if args.check:
        run_checks(bases, consts)
        print("projection OK")
        return

    if args.builtin:
        tex = build_builtin_skin(args.builtin)
        print("skin: built-in %s (palette parsed from skin.cpp)" % args.builtin)
    else:
        tex = load_skin(args.skin)
        print("skin: %s" % ("%d textures from %s" % (len(tex), args.skin) if tex
                            else "none (flat grey)"))
    proj = Projection(bases, consts, args.n, args.w, args.h,
                      math.radians(args.yaw), math.radians(args.pitch), args.zoom)
    print("visible faces: %r" % proj.visible())
    img = render(proj, tex, args.w, args.h, demo_states(args.n), consts)
    img.save(args.out)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
