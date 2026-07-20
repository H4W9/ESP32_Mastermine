#!/usr/bin/env python3
"""Check the shapes Burst Clear and Lightning carve out of the cube.

Mirrors Cube::usePowerup, with BURST_RADIUS parsed out of cube.h so the check
and the firmware cannot drift.

    python check_powerups.py [--n 8]
"""

import argparse
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
CUBE_H = os.path.join(HERE, "..", "cube.h")

sys.path.insert(0, HERE)
from check_cube import shell, parse_bases, faces_of, surface_neighbours   # noqa: E402

CUBE_CPP = os.path.join(HERE, "..", "cube.cpp")


def parse_float(path, name):
    src = open(path, encoding="utf-8").read()
    m = re.search(r"\b%s\s*=\s*(-?[0-9.]+)f?" % re.escape(name), src)
    if not m:
        sys.exit("could not find %s in %s" % (name, path))
    return float(m.group(1))


def burst(target, n, radius, nb):
    """Mirror of PU_BURST: two rings out over the neighbour graph, then rounded
    off by 3D distance. The graph walk is what bounds it — distance alone lets
    the patch leak around a cube edge on a small cube."""
    patch = {target}
    ring1 = set(nb[target])
    patch |= ring1
    for b in ring1:
        patch |= set(nb[b])
    cx, cy, cz = (v + 0.5 for v in target)
    out = []
    for b in patch:
        dx, dy, dz = b[0] + 0.5 - cx, b[1] + 0.5 - cy, b[2] + 0.5 - cz
        if dx * dx + dy * dy + dz * dz <= radius * radius:
            out.append(b)
    return out


def lightning(target, n):
    """Mirror of PU_LIGHTNING: the rings through the target on every axis where
    its coordinate is interior, up to two."""
    axes = [a for a in range(3) if 0 < target[a] < n - 1][:2]
    if not axes:
        return [target], axes
    out = [b for b in shell(n) if any(b[a] == target[a] for a in axes)]
    return out, axes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=8)
    args = ap.parse_args()
    n = args.n
    radius = parse_float(CUBE_H, "BURST_RADIUS")
    bases = parse_bases(CUBE_CPP)
    print("n=%d, BURST_RADIUS=%.2f (parsed from cube.h)" % (n, radius))

    face_centre = (n // 2, n // 2, 0)
    edge = (n // 2, 0, 0)
    corner = (0, 0, 0)
    nb, _pts, _s = surface_neighbours(n, bases)

    # --- Burst -------------------------------------------------------------
    cells = burst(face_centre, n, radius, nb)
    assert face_centre in cells, "burst must include the block you tapped"
    # Round, not square: the four far diagonals of a 5x5 must be outside.
    for dx, dy in ((2, 2), (2, -2), (-2, 2), (-2, -2)):
        p = (face_centre[0] + dx, face_centre[1] + dy, 0)
        assert p not in cells, "burst is square, not round: %r included" % (p,)
    # ...but the straight two-out and the knight-ish ones are inside.
    for dx, dy in ((2, 0), (0, 2), (-2, 0), (0, -2), (2, 1), (1, 2)):
        p = (face_centre[0] + dx, face_centre[1] + dy, 0)
        assert p in cells, "burst too small: %r missing" % (p,)
    # 21 regardless of cube size — the graph walk bounds it, so a small cube
    # does not get a disproportionately huge hole.
    assert len(cells) == 21, "expected a 21-block disc, got %d" % len(cells)
    print("  burst on a face centre: %d blocks, round (5x5 corners excluded)  OK"
          % len(cells))

    # It has to wrap a cube edge without any special case.
    ecells = burst(edge, n, radius, nb)
    faces_hit = set()
    for b in ecells:
        faces_hit.update(faces_of(b, n, bases))
    assert len(faces_hit) >= 2, "burst at an edge stayed on one face: %r" % faces_hit
    print("  burst on an edge block: %d blocks spanning %d faces  OK"
          % (len(ecells), len(faces_hit)))

    # --- Lightning ---------------------------------------------------------
    cells, axes = lightning(face_centre, n)
    assert len(axes) == 2, "a face-centre block should give two rings, got %r" % axes
    # Two rings of 4(n-1), overlapping in the 2 blocks where both match.
    ring = 4 * (n - 1)
    assert len(cells) == 2 * ring - 2, \
        "expected %d blocks (two rings less their crossing), got %d" % (2 * ring - 2, len(cells))
    # The cross must run right around the cube: every face is touched.
    faces_hit = set()
    for b in cells:
        faces_hit.update(faces_of(b, n, bases))
    assert faces_hit == set(range(6)), \
        "lightning should reach all six faces, reached %r" % sorted(faces_hit)
    print("  lightning on a face centre: %d blocks, 2 rings, all 6 faces  OK" % len(cells))

    cells, axes = lightning(edge, n)
    assert len(axes) == 1, "an edge block has one interior axis, got %r" % axes
    assert len(cells) == ring, "expected one ring of %d, got %d" % (ring, len(cells))
    print("  lightning on an edge block: one ring of %d  OK" % len(cells))

    cells, axes = lightning(corner, n)
    assert axes == [] and cells == [corner], "a corner block should fall back to itself"
    print("  lightning on a corner block: falls back to the block itself  OK")

    # --- No powerup may remove a mine ---------------------------------------
    # They reveal safe blocks and FLAG mines; the puzzle underneath has to be
    # the same one afterwards. Deleting mines would change every count around
    # the area. Checked against the source rather than by simulation, because
    # it is the one property that must hold in cube.cpp itself.
    src = open(CUBE_CPP, encoding="utf-8").read()
    body = src[src.index("bool Cube::usePowerup"):]
    body = body[:body.index("\n// Save format")]
    # The bit that clears a mine is `_cell[..] &= ~1`.
    assert "&= ~1" not in body, \
        "usePowerup still clears a mine bit somewhere:\n%s" % body[:200]
    clear_fn = src[src.index("void Cube::clearBlock"):]
    clear_fn = clear_fn[:clear_fn.index("\n}") + 2]
    assert "&= ~1" not in clear_fn, "clearBlock still removes mines"
    assert "CS_FLAGGED" in clear_fn, "clearBlock should flag a mine, not skip it"
    reveal_fn = src[src.index("bool Cube::reveal"):]
    reveal_fn = reveal_fn[:reveal_fn.index("\nbool Cube::cycleFlag")]
    assert "&= ~1" not in reveal_fn, "the Lifesaver path still removes the mine"
    print("  no powerup removes a mine; mines are flagged instead  OK")

    print("powerup shapes OK")


if __name__ == "__main__":
    main()
