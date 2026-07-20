#!/usr/bin/env python3
"""Check the cube model that cube.cpp implements.

The board is the SHELL OF A CUBE OF BLOCKS: N^3 - (N-2)^3 cells, not six faces
of squares. The game states this itself — its Advanced difficulty reads "51
mines in 296 total blocks", and 296 = 8^3 - 6^3 (six faces of squares would be
384).

Adjacency is measured on the SURFACE: two blocks are neighbours exactly when
any of their exposed faces share a corner point. That is the only rule that
caps at 8 neighbours, which it must, because the skin texture set stops at
tile8. Plain 26-connectivity in the volume peaks at 13 and face+edge
connectivity at 10; both are checked here and rejected, so the choice stays
justified rather than asserted.

The face bases are PARSED OUT OF cube.cpp, not copied here, so this check and
the firmware cannot drift apart.

    python check_cube.py [--max-n 12]
"""

import argparse
import os
import re
import sys
from collections import Counter
from itertools import product

HERE = os.path.dirname(os.path.abspath(__file__))
CUBE_CPP = os.path.join(HERE, "..", "cube.cpp")


def parse_bases(path):
    """Pull CUBE_BASIS out of cube.cpp as 6 tuples of 12 ints."""
    src = open(path, encoding="utf-8").read()
    m = re.search(r"const FaceBasis CUBE_BASIS\[CUBE_FACES\]\s*=\s*\{(.*?)\n\};",
                  src, re.S)
    if not m:
        sys.exit("could not find CUBE_BASIS in %s" % path)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    rows = re.findall(r"\{([^{}]*)\}", body)
    bases = []
    for r in rows:
        nums = [int(x) for x in re.findall(r"-?\d+", r)]
        if len(nums) != 12:
            sys.exit("expected 12 numbers per face, got %d: %r" % (len(nums), r))
        bases.append(nums)
    if len(bases) != 6:
        sys.exit("expected 6 faces, got %d" % len(bases))
    return bases


def shell(n):
    hi = n - 1
    return [(x, y, z) for z in range(n) for y in range(n) for x in range(n)
            if x in (0, hi) or y in (0, hi) or z in (0, hi)]


def faces_of(c, n, bases):
    """Indices of the faces this block exposes."""
    hi = n - 1
    out = []
    for f, b in enumerate(bases):
        nn = b[9:12]
        for a in range(3):
            if nn[a] > 0 and c[a] == hi:
                out.append(f)
            elif nn[a] < 0 and c[a] == 0:
                out.append(f)
    return out


def corner_points(c, n, bases):
    """Mirror of Cube::cornerPoints — doubled integer coords."""
    pts = set()
    for f in faces_of(c, n, bases):
        nn = bases[f][9:12]
        base = [2 * c[0], 2 * c[1], 2 * c[2]]
        ax = []
        for a in range(3):
            if nn[a]:
                base[a] += 1 + nn[a]
            else:
                ax.append(a)
        for da in (0, 2):
            for db in (0, 2):
                p = list(base)
                p[ax[0]] += da
                p[ax[1]] += db
                pts.add(tuple(p))
    return pts


def surface_neighbours(n, bases):
    s = shell(n)
    pts = {c: corner_points(c, n, bases) for c in s}
    sset = set(s)
    nb = {}
    for c in s:
        out = set()
        for d in product((-1, 0, 1), repeat=3):
            if d == (0, 0, 0):
                continue
            o = (c[0] + d[0], c[1] + d[1], c[2] + d[2])
            if o in sset and pts[c] & pts[o]:
                out.add(o)
        nb[c] = out
    return nb, pts, sset


def volume_neighbours(n, corners_only):
    """26-connectivity, or 18-connectivity when corner-only contacts are cut."""
    s = set(shell(n))
    nb = {}
    for c in s:
        out = set()
        for d in product((-1, 0, 1), repeat=3):
            if d == (0, 0, 0):
                continue
            if corners_only and sum(1 for v in d if v) == 3:
                continue
            o = (c[0] + d[0], c[1] + d[1], c[2] + d[2])
            if o in s:
                out.add(o)
        nb[c] = out
    return nb


def cell_at_face(f, i, j, n, bases):
    """Mirror of Cube::cellAtFace — which block owns square (i,j) of face f."""
    b = bases[f]
    uu, vv, nn = b[3:6], b[6:9], b[9:12]
    hi = n - 1
    p = [0, 0, 0]
    for a in range(3):
        if nn[a] > 0:
            p[a] = hi
        elif nn[a] < 0:
            p[a] = 0
        else:
            p[a] = i * uu[a] + j * vv[a]
    return tuple(p)


def check(n, bases, verbose=False):
    nb, pts, sset = surface_neighbours(n, bases)
    want = n ** 3 - max(0, n - 2) ** 3
    assert len(nb) == want, "n=%d: %d cells, expected %d" % (n, len(nb), want)

    # The texture set stops at tile8, so nothing may exceed 8 neighbours.
    dist = Counter(len(v) for v in nb.values())
    assert max(dist) <= 8, "n=%d: a block has %d neighbours" % (n, max(dist))

    # Exactly the 8 corner blocks have 6; everything else has 8.
    assert set(dist) == {6, 8}, "n=%d: neighbour counts %r" % (n, dict(dist))
    assert dist[6] == 8, "n=%d: %d blocks with 6 neighbours, expected 8" % (n, dist[6])
    assert dist[8] == want - 8

    # Adjacency is mutual.
    for c, v in nb.items():
        for o in v:
            assert c in nb[o], "n=%d: %s->%s not mutual" % (n, c, o)

    # Every neighbour is within the 26-block neighbourhood, which is what lets
    # cube.cpp find them with a local scan instead of a lattice-wide map.
    for c, v in nb.items():
        for o in v:
            assert max(abs(o[a] - c[a]) for a in range(3)) == 1, \
                "n=%d: %s and %s are neighbours but not adjacent in space" % (n, c, o)

    # cellAtFace must cover every surface square exactly, and always land on a
    # block that really does expose that face. This is the indirection that
    # makes an edge block show one number on two faces.
    seen = Counter()
    for f in range(6):
        for j in range(n):
            for i in range(n):
                p = cell_at_face(f, i, j, n, bases)
                assert p in sset, "n=%d: face %d (%d,%d) -> %s not on the shell" % (
                    n, f, i, j, p)
                assert f in faces_of(p, n, bases), \
                    "n=%d: face %d (%d,%d) -> %s does not expose that face" % (
                        n, f, i, j, p)
                seen[p] += 1
    assert sum(seen.values()) == 6 * n * n
    # A block should appear once per face it exposes: 1, 2 or 3 times.
    for c in sset:
        assert seen[c] == len(faces_of(c, n, bases)), \
            "n=%d: block %s drawn %d times, exposes %d faces" % (
                n, c, seen[c], len(faces_of(c, n, bases)))
    multi = Counter(seen.values())

    if verbose:
        print("  n=%-3d cells=%-5d  corner-blocks(6nb)=%d  faces drawn 1/2/3 = %d/%d/%d  OK"
              % (n, want, dist[6], multi[1], multi[2], multi[3]))
    return True


def reject_volume_rules(bases, n=8):
    """Show why the surface rule is the one, rather than just asserting it."""
    for name, fn in (("26-connectivity", lambda: volume_neighbours(n, False)),
                     ("18-connectivity", lambda: volume_neighbours(n, True))):
        d = Counter(len(v) for v in fn().values())
        assert max(d) > 8, "%s unexpectedly caps at 8" % name
        print("  n=%d %-16s max %d neighbours -> rejected (textures stop at 8)"
              % (n, name, max(d)))
    d = Counter(len(v) for v in surface_neighbours(n, bases)[0].values())
    print("  n=%d %-16s max %d neighbours -> chosen" % (n, "surface", max(d)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-n", type=int, default=12)
    args = ap.parse_args()

    bases = parse_bases(CUBE_CPP)
    print("parsed %d face bases from cube.cpp" % len(bases))

    # U x V must equal the declared outward normal, or back-face culling in the
    # renderer inverts and you see the inside of the cube.
    for f, b in enumerate(bases):
        u, v, nvec = b[3:6], b[6:9], b[9:12]
        cross = (u[1] * v[2] - u[2] * v[1],
                 u[2] * v[0] - u[0] * v[2],
                 u[0] * v[1] - u[1] * v[0])
        assert tuple(cross) == tuple(nvec), \
            "face %d: U x V = %r but normal is declared %r" % (f, cross, nvec)
        # cellAtFace assumes every u/v component is 0 or +1.
        assert all(c in (0, 1) for c in u + v), \
            "face %d: u/v components must be 0 or +1, got %r %r" % (f, u, v)
    print("all 6 faces: U x V == declared outward normal, u/v non-negative  OK")

    print("adjacency rule:")
    reject_volume_rules(bases)

    # The game's own numbers, from its difficulty screen.
    assert 5 ** 3 - 3 ** 3 == 98, "Beginner should be 98 blocks"
    assert 8 ** 3 - 6 ** 3 == 296, "Advanced should be 296 blocks"
    print("  matches the game's stated 98 blocks (5x5) and 296 blocks (8x8)  OK")

    for n in range(4, args.max_n + 1):
        check(n, bases, verbose=True)
    print("cube model OK for n=4..%d" % args.max_n)


if __name__ == "__main__":
    main()
