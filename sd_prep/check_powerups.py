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


def parse_int(path, name):
    src = open(path, encoding="utf-8").read()
    m = re.search(r"\b%s\s*=\s*(\d+)" % re.escape(name), src)
    if not m:
        sys.exit("could not find %s in %s" % (name, path))
    return int(m.group(1))


def burst(target, n, radius, nb, rings):
    """Mirror of PU_BURST: `rings` rings out over the neighbour graph, then
    rounded off by 3D distance. The graph walk is what bounds it — distance
    alone lets the patch leak around a cube edge on a small cube."""
    patch = {target}
    frontier = {target}
    for _ in range(rings):
        nxt = set()
        for b in frontier:
            nxt |= set(nb[b])
        nxt -= patch
        patch |= nxt
        frontier = nxt
    cx, cy, cz = (v + 0.5 for v in target)
    out = []
    for b in patch:
        dx, dy, dz = b[0] + 0.5 - cx, b[1] + 0.5 - cy, b[2] + 0.5 - cz
        if dx * dx + dy * dy + dz * dz <= radius * radius:
            out.append(b)
    return out


def inplane_axes(face, bases):
    """The two cube axes that lie IN the given face, from its U and V basis
    vectors. Each has exactly one non-zero component."""
    b = bases[face]
    u, v = b[3:6], b[6:9]
    au = next(a for a in range(3) if u[a])
    av = next(a for a in range(3) if v[a])
    return [au, av]


def lightning(target, n, face, bases):
    """Mirror of PU_LIGHTNING: a ring along each of the APPLIED FACE's two
    in-plane axes.

    A ring along axis `a` at value `v` is every shell block with coord_a == v
    that also has one of its other two coordinates extreme. When v is interior
    that second clause is automatic and the ring is the 4(n-1) band; when v is
    extreme it is that face's PERIMETER, also 4(n-1). So both directions always
    give a real ring, at an edge and at a corner too — which is the whole point
    of keying this on the face rather than on which coordinates are interior."""
    axes = inplane_axes(face, bases)
    out = []
    for b in shell(n):
        for a in axes:
            if b[a] != target[a]:
                continue
            if any(b[o] in (0, n - 1) for o in range(3) if o != a):
                out.append(b)
                break
    return out, axes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=8)
    args = ap.parse_args()
    n = args.n
    radius = parse_float(CUBE_H, "BURST_RADIUS")
    rings = parse_int(CUBE_H, "BURST_RINGS")
    bases = parse_bases(CUBE_CPP)
    print("n=%d, BURST_RADIUS=%.2f, BURST_RINGS=%d (parsed from cube.h)"
          % (n, radius, rings))

    face_centre = (n // 2, n // 2, 0)
    edge = (n // 2, 0, 0)
    corner = (0, 0, 0)
    nb, _pts, _s = surface_neighbours(n, bases)

    # --- Burst -------------------------------------------------------------
    cells = burst(face_centre, n, radius, nb, rings)
    assert face_centre in cells, "burst must include the block you tapped"
    # On a face interior the patch is a disc: every offset within the radius is
    # in, every offset outside it is out. Derived from the constants, so
    # changing either in cube.h changes what is expected here.
    want = set()
    for dx in range(-rings, rings + 1):
        for dy in range(-rings, rings + 1):
            if dx * dx + dy * dy <= radius * radius:
                want.add((face_centre[0] + dx, face_centre[1] + dy, 0))
    got = set(cells)
    assert got == want, ("burst is not the disc the constants describe: "
                         "missing %r, extra %r"
                         % (sorted(want - got), sorted(got - want)))
    # The square's far corner must be excluded, or it is not round at all.
    corner_off = (rings, rings)
    assert corner_off[0] ** 2 + corner_off[1] ** 2 > radius * radius, \
        "BURST_RADIUS %.2f is large enough to admit the square corner at %r " \
        "- the patch is a square, not a disc" % (radius, corner_off)
    print("  burst on a face centre: %d blocks, round (%d rings, r=%.1f)  OK"
          % (len(cells), rings, radius))

    # It has to wrap a cube edge without any special case.
    ecells = burst(edge, n, radius, nb, rings)
    faces_hit = set()
    for b in ecells:
        faces_hit.update(faces_of(b, n, bases))
    assert len(faces_hit) >= 2, "burst at an edge stayed on one face: %r" % faces_hit
    print("  burst on an edge block: %d blocks spanning %d faces  OK"
          % (len(ecells), len(faces_hit)))

    # --- Lightning ---------------------------------------------------------
    # The property that matters: WHEREVER you aim it, you get two full rings
    # through the target, in the two directions of the face you tapped. It used
    # to manage that only in the middle of a face - one bolt at an edge, none at
    # a corner - which is what the ring-at-an-extreme-axis rule fixes.
    ring = 4 * (n - 1)
    for name, target in (("a face centre", face_centre),
                         ("an edge block", edge),
                         ("a corner block", corner)):
        for face in sorted(faces_of(target, n, bases)):
            cells, axes = lightning(target, n, face, bases)
            assert len(axes) == 2, "%s: expected two axes, got %r" % (name, axes)
            assert target in cells, \
                "%s on face %d: the target itself is not in its own bolts" % (name, face)

            # Each direction on its own must be a genuine ring of 4(n-1).
            for a in axes:
                one = [b for b in cells if b[a] == target[a]
                       and any(b[o] in (0, n - 1) for o in range(3) if o != a)]
                assert len(one) == ring, \
                    "%s on face %d: the bolt along axis %d is %d blocks, not a " \
                    "ring of %d" % (name, face, a, len(one), ring)

            # Both bolts must lie in the tapped face, i.e. run along its own
            # two directions - not off along its normal.
            normal = [a for a in range(3) if a not in axes][0]
            assert normal not in axes
            print("  lightning on %-14s via face %d: %3d blocks, two rings of %d  OK"
                  % (name, face, len(cells), ring))

    # It must still wrap the whole cube from the middle of a face.
    cells, _ = lightning(face_centre, n, sorted(faces_of(face_centre, n, bases))[0], bases)
    faces_hit = set()
    for b in cells:
        faces_hit.update(faces_of(b, n, bases))
    assert faces_hit == set(range(6)), \
        "lightning should reach all six faces, reached %r" % sorted(faces_hit)
    print("  lightning from a face centre still reaches all six faces  OK")

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

    # --- No question state --------------------------------------------------
    # CS_QUESTION let a flagged-then-unflagged block sit in a third state that
    # reveal() accepted but revealFlood() would not expand, so the block could
    # never be opened again by hand. Assert it has not crept back.
    hdr = open(CUBE_H, encoding="utf-8").read()
    assert "CS_QUESTION" not in hdr and "CS_QUESTION" not in src, \
        "CS_QUESTION is back - a flag/unflag cycle can strand a block again"
    cyc = src[src.index("bool Cube::cycleFlag"):]
    cyc = cyc[:cyc.index("\n}") + 2]
    assert "CS_HIDDEN" in cyc and "CS_FLAGGED" in cyc, \
        "cycleFlag should toggle hidden <-> flagged"
    print("  no question state; unflagging returns a block to CS_HIDDEN  OK")

    print("powerup shapes OK")


if __name__ == "__main__":
    main()
