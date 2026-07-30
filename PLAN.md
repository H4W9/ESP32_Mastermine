# ESP32 Mastermine

Minesweeper on the surface of a rotatable 3D cube, for the Marauder **Pancake**
(ESP32-C5, ST7796 320x480, FT6336 capacitive touch) and **Marauder V8**
(ILI9341 240x320, XPT2046 resistive).

After [Mastermine](https://mastermine.app) by Adam Soutar. Swipe to turn the
cube, pinch to zoom, and dress it in the player-made skins from the game's own
skin store — downloaded on the device over WiFi, or sideloaded from a PC.

Shares the H4W9 UI shell with `ESP32_Scrabble` and `ESP32_FlipSocial`: header
with back button and status corner, footer nav bar, momentum list menus, chip
settings rows, VLW smooth fonts throughout, and no flashing from redraws.

---

## Build

Arduino IDE only.

| Setting | Value |
|---|---|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| Partition Scheme | Custom → `partitions.csv` |
| Flash Frequency | 80 MHz |
| PSRAM | **Enabled** (required — the render sprite lives there) |

Pick the board in `configs.h` (`MARAUDER_PANCAKE` / `MARAUDER_V8`) and point
`TFT_eSPI-ESP32-C5/User_Setup_Select.h` at the matching `User_Setup_marauder_*.h`.

**Libraries:** TFT_eSPI (the patched ESP32-C5 fork), ArduinoJson v7.
`picojpeg` is vendored in `src/picojpeg/` — see the note there — so there is no
JPEG-decoder library dependency.

---

## Architecture

| File | Role |
|---|---|
| `ESP32_Mastermine.ino` | UI shell, screens, gesture handling, WiFi |
| `cube.h/.cpp` | Board model — pure logic, no drawing |
| `render3d.h/.cpp` | Textured orthographic cube renderer |
| `skin.h/.cpp` | Tile textures: `.tex` loading, mips, built-in fallback art |
| `skinstore.h/.cpp` | The live skin store: browse, download, JPEG decode |
| `theme.h`, `vlw.h`, `fonts_vlw.h` | Shared H4W9 shell (theme, smooth fonts) |
| `src/Picoware/` | Panel + touch + view manager |
| `src/picojpeg/` | Vendored public-domain JPEG decoder |
| `sd_prep/` | PC-side tools and the correctness checks |

### A cell is a BLOCK, not a square

The board is the hollow shell of an N×N×N grid of blocks: **N³ − (N−2)³ cells**.
The game states this itself — its Advanced difficulty reads *"51 mines in 296
total blocks"*, and 296 = 8³−6³. Six faces of squares would have been 384.

The consequence that shows up everywhere else: a block on a cube **edge is one
cell showing the same number on two faces**, and a **corner block on three**.
Reveal it from any of them and the whole block reveals. That is what makes the
edges and corners read correctly.

**Adjacency is measured on the surface, not in the volume**: two blocks are
neighbours exactly when any of their exposed faces share a corner point. This is
not a guess — it is the only rule that caps at 8 neighbours, which it must,
because the skin texture set stops at `tile8`. `check_cube.py` measures the
alternatives and rejects them in the output: plain 26-connectivity peaks at 13,
face+edge connectivity at 10. The surface rule also gives a strikingly clean
distribution — the 8 corner blocks have 6 neighbours and **every** other block
has exactly 8.

Verified for n=4..12: cell counts match `N³−(N−2)³` (98 for 5×5 and 296 for 8×8,
the game's own figures), adjacency mutual and never above 8, every neighbour
within the 26-block neighbourhood (which is what lets the firmware find them
with a local scan), and `cellAtFace` covering every surface square exactly once
per face its block exposes.

### Orthographic projection is the load-bearing decision

Under an orthographic projection every cube face lands on screen as an exact
parallelogram, so mapping a texture across a tile is **affine** — no perspective
divide, no per-pixel division at all. The inner loop is two fixed-point adds, a
texture fetch and three small LUT lookups for shading.

It also means the at-most-3 visible faces can never overlap, so there is **no
z-buffer and no polygon sorting**: cull back faces by the sign of the rotated
normal's z and paint what is left in any order.

Per scanline the rasteriser solves for the exact x range where both texture
coordinates are inside [0,1) rather than testing every pixel in the bounding
box — a sheared tile's bbox is up to twice its area. Sampling on pixel centres
with half-open intervals makes adjacent tiles abut exactly: no seams, no
double-writes.

### Cells are extruded blocks, not flat squares

This is what makes the cube read as a solid object and gives its edges and
corners their stepped, chunky silhouette — see `example/`.

* **Each block is a cube of side `1 − 2·BLOCK_GAP`, centred in its cell, and is
  drawn as one solid** — all three of its camera-facing faces textured, in one
  pass, with no special case anywhere:

  * every visible face is `(1−2g)` square — the *same* square on every block,
    because they are all the same cube;
  * an edge block's two faces **meet exactly**. Its +Z face lies at `z = n−g`
    and runs out to `x = n−g`; its +X face lies at `x = n−g` and runs out to
    `z = n−g`. Same edge. No gap, no overlap, sharp corner — and a corner block's
    three faces meet at the single point `(n−g, n−g, n−g)`.

  The part that is easy to get backwards: the face plane sits `BLOCK_GAP`
  **inside** the cube's nominal surface, not proud of it.

  Three approximations were tried first and all failed, each in a way the next
  one caused: pushing tops *outward* and stretching the boundary ones to meet
  (flush, but boundary faces a different size); stopping tops at the fold
  (uniform, but every edge block visibly split); bridging that split with a
  chamfer (not a crisp edge). Modelling the actual cube gives both properties
  at once and is simpler than any of them.

  `preview_cube.py --check` asserts both: all 24 folds land exactly on the
  neighbour's outermost block edge, and every face is the same square.
* A cleared block with no adjacent mines uses a near-black face, which is what
  turns swept regions dark the way the reference does.

### One solid per block — no side-wall quads, no face ordering

`paintBlocks()` walks the voxel grid once and draws each shell block's three
camera-facing faces, all textured with that block's own tile.

This replaced an earlier design where a block was a textured cap plus two
flat-shaded side "lips". That design needed a growing pile of rules to stay
correct — skip the lip where a block reaches the cube's edge (it lands exactly
in the neighbouring face's plane, and with no z-buffer whichever paints second
wins); draw all lips before all caps so a nearer face's lips could not land on
a farther face's caps; sort the three visible faces farthest-first. Every one
of those rules existed to paper over the fact that a block is a solid and was
not being drawn as one. Drawing the solid deletes all of them, and the slivers
of a block visible in the gap between its neighbours are now its real textured
faces rather than a flat approximation of them.

**Ordering.** For equal, grid-aligned cubes under an orthographic camera, block
A can occlude B only if A is on the camera side of B along *every* axis at once.
So both a back-to-front and a front-to-back order are just linear extensions of
that partial order, produced by choosing the direction of the three voxel loops
(`_rot[6..8]` signs). `preview_cube.py --check` asserts the walk is a valid
painter order over several orientations.

**Front-to-back with a coverage mask — the overdraw is gone.** Drawing
back-to-front is correct but wasteful: a face in the interior of a cube face is
almost entirely painted over by the block in front of it, so most texture
samples and writes are thrown away. Profiling put the waste squarely on the
lateral (gap-wall) faces — ~90% of their pixels overdrawn — while caps were 0%
wasted. Clipping was a dead end (the surviving slivers move all over the face
with the camera angle), so instead the renderer draws **nearest-first** and
keeps a **1-bit coverage mask**: a pixel already written is skipped, sample and
all. Each pixel is now written exactly once. Measured fill drops from **3.28× to
~1.0×**, and on the Pancake that is a PSRAM-bandwidth win as much as a compute
one — the compositing sprite and the textures both live in PSRAM, whose slow
random access was the real bottleneck.

This is lossless: the *frontmost opaque surface* wins each pixel either way.
`preview_cube.py --check` proves it the strong way — it gives every (block,face)
a unique id, renders an id-buffer both back-to-front and front-to-back+mask, and
demands they match at every pixel over 126 orientations. If the surface that
wins a pixel is identical, any texture on it renders identically.

The mask is ~15 KB, allocated in **internal** RAM (a PSRAM mask would defeat its
own purpose); if it fails to allocate, `_cov` is null, the test macros no-op,
and the renderer falls back to plain front-to-back overdraw — slower but still
correct. The old half-resolution `step=2` drag pass is **gone**: full-res
front-to-back writes fewer pixels than half-res-with-overdraw did, so `_fast`
now only selects point sampling over bilinear.

### The inner cube, and the cull it enables

The shell is hollow, so between the near blocks you could see through the gaps,
across the empty middle, and out the far side — the blocks behind showed
through. The fix is a solid **inner cube** filling the centre, spanning
`[1−g, n−1+g]` on every axis: flush with the inward faces of the shell blocks,
its edges meeting the inside edges of the edge blocks, painted in the
background colour.

It is **not free**, and believing it was is what shipped the see-through bug.
The frame does start as `fillSprite(bg)`, but blocks are drawn between then and
the moment the gaps matter, so the gaps show the far side until this quad
erases it. `paintBlocks()` therefore draws in three stages: pass 0 (blocks not
on a camera-facing face), then `paintInner()`, then pass 1 (blocks on one).
That split is exactly the occlusion boundary — a pass-0 block has the inner
cube on its camera side along every axis, a pass-1 block sits outside it on its
face's axis.

Plugging the middle lets pass 0 be **culled**: a block more than `CULL_DEPTH`
layers behind every camera-facing face is now hidden by the inner cube and is
not drawn. `CULL_DEPTH` is **2**, not 0, and that is the subtlety — the
silhouette is a staircase, so at a yaw off the axis a block a layer or two back
peeks out *sideways* past the one in front, against the background, where the
inner cube cannot help. Measured over 264 orientations: dropping pass 0
entirely leaks at 72 of them, depth 1 at 16, depth 2 at none.

`preview_cube.py --check` proves two separate things. That the cull is safe:
the culled render is pixel-identical to one that draws every block, at all 264
orientations, with `CULL_DEPTH` parsed out of the source so setting it too
shallow fails the suite. And that the inner cube earns its place: painted in
red instead of `bg`, it covers up to 4320 px at the straight-on view — every
one a pixel that would otherwise have shown the far side of the cube through a
gap.

The overdraw the cull leaves behind — mostly the lateral gap-wall faces — is
removed losslessly by the front-to-back coverage mask described under the
renderer above, so there is no remaining "clip the slivers" work to do; the
slivers are simply the pixels the mask lets through.

### Texture orientation is fixed to the face

The art is painted on and turns with the cube, the way markings on a physical
object do — so orientation depends only on geometry, never on the camera.

The one thing that must not happen is a **mirrored** face: a mirrored 5 is
unreadable in a game whose entire content is numbers you read. Mirroring is
settled by handedness alone. An unmirrored image has texture-x right and
texture-y **down**, and screen y grows downward, so seen from outside the pair
must satisfy `TX × TY = −N`. `CUBE_BASIS` gives `U × V = +N`, so exactly four of
the eight placements qualify. Of those, take the one whose texture-down points
most nearly at the cube's own −Y; that makes all four side faces upright, and on
the top and bottom the tie-break prefers texture-down towards +Z — how you read
the top of a box you are standing in front of.

`preview_cube.py --check` asserts both properties: never mirrored, and identical
at every camera angle.

### Sharpness

Two things were making published skins look wrong, and both are fixed:

* **Textures are 128 px, not 64.** A tile covers 60–100 px zoomed in, and 64 px
  visibly mushed the thin strokes many skins use for their digits. The
  downloader now decodes the JPEG in **full** and box-averages it into a 128 px
  accumulator as the MCUs come out — the whole 512×512 image is never held. The
  old DC-only 1/8 decode remains as a fallback. `.tex` version 2; version 1
  files still load, into mip level 1.
* **Bilinear sampling when the cube is still.** Point sampling made thin strokes
  ragged and made them crawl as the cube turned. A drag still point-samples at
  half resolution, where the motion hides it.

Shading is a **neutral** multiply. It was briefly warm — the reference's gold
set measures `rgb(254,253,129)` lit against `rgb(112,80,33)` shaded, which is
not a neutral scale — but that was the wrong conclusion: the warmth is in the
gold texture's own yellow-to-brown gradient, not the lighting. Applied as
lighting it turned the white default set beige. The per-channel exponents are
still there, set to 1.0, in case a skin-specific tint is ever wanted.

### Lighting is fixed to the cube, not the camera

Each of the six faces has a shade that **never changes**, however the cube is
turned. The light used to be fixed in view space, which meant a face's
brightness drifted continuously under your finger as you dragged — the colours
appeared to shift while you were trying to read the board.

The Lambert term is mapped from [-1,1] rather than clamped at 0, so all six
faces get distinct shades instead of the three facing away collapsing to the
same value. With the shading this gentle (spread 0.176) even the least-lit face
is around 80%, so nothing goes dark when it turns to the front.
`preview_cube.py --check` asserts the shade is constant across camera angles and
that all six values differ.

### Rotation has no boundary

Camera orientation is a full 3×3 matrix accumulated from the drag, **not Euler
angles**. Euler needs the pitch clamped short of vertical or the cube flips and
the yaw axis inverts — and that clamp is a wall you can feel. Each drag
increment is composed in **view space** (`R = Rx(dp)·Ry(dy)·R`), so the axes it
turns about are always the ones on screen and the cube tumbles freely in any
direction forever. The rows are re-orthonormalised by Gram-Schmidt after every
increment, since thousands of composed rotations would otherwise drift and shear
the cube.

The checker composes 20 000 random drags and asserts the matrix stays
orthonormal and that the cube genuinely passes through vertical in both
directions — which is what proves the limit is gone rather than just widened.

### The sprite buffer is byte-swapped

A `TFT_eSprite`'s 16bpp buffer does **not** hold native RGB565 —
`TFT_eSprite::drawPixel` does `color = (color >> 8) | (color << 8)` before
storing, and `fillSprite`, `fillRect` and `drawLine` all do the same. This
renderer writes into that buffer directly through `getPointer()` for speed, so
it has to match.

Getting it wrong does not garble the picture in an obvious way — it shifts every
hue. The symptom was a cube with inexplicable cyan and purple faces while the
shell's own drawing, which goes through TFT_eSPI, stayed perfectly correct.

The fix costs nothing per pixel: the swap is baked into the shading tables. For
a native word `(R<<11)|(G<<5)|B` the swapped word is
`((G&7)<<13)|(B<<8)|(R<<3)|(G>>3)`, and each channel's share of that is
pre-computed, so the inner loop is still three lookups and two ORs.
`fillPara` swaps its constant once per quad. The packing was verified exact over
all 458 752 channel/shade combinations.

### Background

On a dark theme the cube sits on a flat **rgb(30,30,30)**, measured off the
reference's own screens, rather than on the theme background — a near-black
background swallows the cube's darker faces, and its cleared blocks are
near-black themselves. Light themes keep their own background, where the problem
does not arise.

### No flashing

The cube composites into one PSRAM sprite and is pushed whole; nothing on the
game screen draws to the panel mid-frame. Header, banner and footer are
change-detecting — they repaint only when their text actually changes, not on
every tap or every clock tick. The settings list scrolls inside a sprite for the
same reason (and because a fixed-offset list overflows the V8's 320 px panel at
this many rows — a bug inherited from `ESP32_Scrabble` and fixed here).

While a drag or pinch is in flight the renderer point-samples the textures
instead of bilinear-filtering them (the motion hides the aliasing); the crisp
bilinear frame lands when the gesture settles. It stays full-resolution
throughout — the front-to-back coverage mask made the old half-resolution drag
pass unnecessary, since full-res now writes fewer pixels than the half-res
overdraw did.

---

## Skins

Three skins are **built in**, drawn procedurally in `skin.cpp` so nothing
third-party ships in flash:

* **Clean** — the reference game's own default, and ours: white blocks, white
  numerals, a rounded inner panel a shade lighter than the tile edge. Hues taken
  from the skin swatch on its shop screen (cyan 1, amber 2, indigo 3, red-orange
  4).
* **Gold** — its premium set. Palette *measured* off the screenshots in
  `example/` (gold `rgb(255,246,150)`→`rgb(198,142,38)`; caps cyan/white/violet
  for 1/2/3).
* **Classic** — the grey Windows-Minesweeper look.

For both Clean and Gold, digits **4–8 appear in no reference material**, so
those continue the ramp sensibly rather than pretending to be sourced.

Everything else is **user-created content** from <https://mastermine.app/skins>,
downloaded to your own SD card. **None of it is redistributed with this
firmware**, and any texture a published skin omits falls back to the built-in.

### The store API, as measured against the live service (2026-07)

```
GET https://mastermine.app/api/topSkins?pageNo=N        no auth
-> {"success": true, "skins": [ ...20 entries... ], "eof": false}
```

* Pages are **zero-based**; `pageNo=0` is the most-downloaded page.
* `eof` goes true on the last page — that is the signal to stop paginating.
* Entry: `{id, skinName, downloadCount, score, textures: {key: url}}`
* 16 texture keys: `tile1`–`tile8`, `tileUnrevealed`, `tileRevealed`,
  `tileFlagged`, `redSpot`, `menu1`–`menu4`.
* **`redSpot` is the mine face** (confirmed by eye against skin 86).
* `menu1`–`menu4` are skipped: this hardware draws its own menus.
* Every texture is a **512×512 JPEG** (checked across 23 textures: all 512×512,
  all H2V2 chroma subsampling).
* **Skins are frequently incomplete.** In the top 40, one ships only 4 of the 12
  gameplay textures. Any missing key falls back to the built-in art.

### On-card format

```
/mastermine/skins/<id>/meta.json
/mastermine/skins/<id>/<key>.tex     16-byte header + 128x128 RGB565 LE = 32784 bytes
/mastermine/games/auto.sav
/mastermine/powerups.xml
```

Little-endian because that is a `uint16_t` in ESP32 memory, so the payload loads
with no per-pixel swap. Mips (64/32/16/8) are generated in PSRAM at load.

The firmware and `sd_prep/fetch_skins.py` write **byte-identical** files, so
there is one code path for reading them.

### Why picojpeg is vendored

picojpeg has a **reduce** mode that decodes only the DC coefficient of each 8×8
block — skipping AC dequantisation, the IDCT and chroma upsampling entirely.
That is a 1/8-scale decode, and 512/8 = 64 exactly, so it lands precisely on the
64×64 we cache, at a fraction of the time and memory of a full decode. Bodmer's
`JPEGDecoder` wraps picojpeg but hardcodes `reduce = 0`, so it cannot be asked
for this. picojpeg itself is public domain.

TLS uses `setInsecure()`. These are public, user-uploaded picture files on
someone else's CDN: nothing secret goes up, and the only thing a forged
certificate could achieve is showing the wrong tile art. Pinning a CA we do not
control would also break the feature the day mastermine.app rotates issuers.

---

## Controls

Taken from the reference game's own Controls screen (`example/controls.jpeg`):
**Reveal = Hold, Flag = Tap, Chording = Hold, tap-vs-hold cutoff 0.25 s.**
Note chording is not a separate gesture — it is what a reveal means when it
lands on an already-revealed number.

| Gesture | Action |
|---|---|
| Swipe | Rotate the cube freely, with momentum; the surface follows the finger and there is no limit in any direction |
| Pinch | Zoom — **Pancake only** |
| `[-]` / `[+]` chips | Zoom — **V8 only** (resistive panels cannot report a second contact) |
| Tap | Flag |
| Hold (250 ms) | Reveal, or chord if the block is already a revealed number |
| Footer | **Pause** · Powerups · Recentre |

Pause opens a modal with **Resume** and **Back to Menu** (a tap outside it
resumes). The clock keeps running — the reference does the same, and stopping it
would make Pause a way to game the timer. The reference exposes tap/hold as
settings; on a panel this size the footer slot is better spent on Pause, so they
are fixed here.

Powerups — the four the reference game actually has, taken from its achievement
list. Note **Shuffle is not one of them** and has been dropped:

| | |
|---|---|
| **Burst Clear** | reveals a **round** patch three rings across, flagging any mines in it |
| **Lightning** | reveals **two rings at right angles**, each running right around the cube |
| **Lifesaver** | activate to **arm** it; the next mine you tap is flagged instead of ending the game |
| **Sonar** | shows the mines within two rings, fading back to hidden over one second |

**Nothing fires on its own.** Lifesaver used to be passive — held meant
protected — which spent the player's item without asking. Arming is now an
explicit choice, and an armed Lifesaver is only consumed when it actually saves
you. The menu rows show just the name and how many you hold; a **Power-up Info**
row explains what each one does.

**Burst** walks `BURST_RINGS` (3) rings out over the neighbour graph, then keeps
only what is within `BURST_RADIUS` (3.5 cells) in 3D. The graph walk bounds the
effect and wraps a cube edge correctly; the distance test rounds the square off,
since the far corner of a 3-ring square sits at 4.24 and drops out. 37 blocks on
a face interior. (This was two rings / radius 2.5 / 21 blocks; one ring bigger.)

Distance alone is not enough — on a small cube the sphere reaches blocks around
the edge that are nowhere near that many rings away across the surface, and the
patch ballooned (33 for the old two-ring version). `check_powerups.py` caught
that; it now derives the expected disc from `BURST_RINGS` and `BURST_RADIUS`
parsed out of `cube.h`, so changing either can't drift from the check.

**No powerup ever removes a mine.** They reveal the safe blocks in their area
and **flag** the mines, so the puzzle underneath is the same one afterwards —
Lifesaver included, which flags the mine you tapped rather than defusing it.
Deleting mines would change every count around the area and quietly rewrite the
board under the player. `check_powerups.py` enforces it against the source.

**Lightning** reveals two full rings at right angles, in the two directions of
the **face you tapped** — always, wherever on that face you aim it. That is why
`pick()` now returns the face as well as the block: an edge block lies on two
faces and a corner block on three, and "which way is across" is a property of
the face, not the block.

A ring along an axis at a value is the shell blocks with that coordinate that
*also* have one of their other two coordinates extreme. When the value is
interior that second clause is automatic and the ring is the 4(n−1) band; when
it is 0 or n−1 the clause makes it that face's **perimeter** — also 4(n−1),
also a real ring through the target. That second clause is the whole fix: the
old rule refused to run a bolt along an extreme axis (it would have been a whole
face), so it fired only one bolt at an edge and none at a corner. Now both bolts
are always genuine rings, 54 blocks from a face centre (touching all six faces),
54 at an edge, 48 at a corner.

Both are checked by `sd_prep/check_powerups.py`, which parses the burst
constants out of `cube.h` and asserts the shapes — that Burst is the disc those
constants describe, and that Lightning gives two 4(n−1) rings on **every** face
of the target, corners included.

The clearing powerups run a **zero-cascade** afterwards. They open blocks
directly rather than through `reveal()`, so a revealed zero can be left sitting
next to hidden neighbours — a state ordinary play cannot produce, because
`reveal()` always cascades. Without the pass the board keeps pockets that should
have opened. (No mine is ever removed, so no count changes — the cascade is
purely about finishing the flood the direct opens started.)

Sonar's fade is a real cross-fade in the renderer: the block is drawn with the
mine texture blended towards its normal hidden face, so it dissolves rather than
blinking off. The game loop keeps repainting for as long as a ping is alive.

**Starting counts, per-powerup caps and the award interval are not compiled in.**
They live in a hand-editable XML file on the card, `/mastermine/powerups.xml`,
written with the defaults the first time it is missing:

```xml
<powerups>
  <award every="40"/>                                 <!-- 0 = never award -->
  <powerup name="burst"     start="1" max="3"/>
  <powerup name="lightning" start="1" max="3"/>
  <powerup name="lifesaver" start="1" max="3"/>
  <powerup name="sonar"     start="1" max="3"/>
</powerups>
```

The parser is deliberately forgiving — it scans for tags and attributes rather
than validating a document, ignores unknown names, and leaves the default in
place for anything malformed. A game console should not refuse to start over a
stray angle bracket.

Difficulties, with the game's own mine counts where it states them:
**Beginner** 5×5 (17 mines in 98 blocks) · **Advanced** 8×8 (51 in 296) ·
**Expert** 12×12 · **Mastermine** 16×16 · **Custom** (N up to 20, 5–40 %).
Expert and Mastermine hold the same ~17.2 % density, because the reference has
them locked behind progression and does not show the numbers.

---

## PC tools (`sd_prep/`)

```bash
# Prove the cube geometry (parses CUBE_BASIS out of cube.cpp)
python check_cube.py --max-n 14

# Prove the projection and picking (parses cube.cpp + render3d.cpp)
python preview_cube.py --check

# Prove the powerup shapes (parses BURST_RADIUS out of cube.h)
python check_powerups.py --n 8

# Look at it — a built-in set, generated from skin.cpp's own palette
python preview_cube.py --n 5 --builtin clean --w 512 --h 512 --out cube.png
python preview_cube.py --n 5 --builtin gold  --w 512 --h 512 --out gold.png

# ...or a real downloaded skin
python preview_cube.py --n 5 --skin ../../card/mastermine/skins/86 --out cube.png

# Browse and sideload skins
python fetch_skins.py --list
python fetch_skins.py --top 5 --out E:/
python fetch_skins.py --id 86 --out E:/ --preview
```

`check_cube.py` and `preview_cube.py` **parse their constants out of the C++**
rather than duplicating them, so the checks cannot quietly drift from the
firmware. Needs Pillow; HTTP is stdlib.

---

## Status

The first version compiled, flashed and ran on the Pancake (see `example/`).
Everything since — the block model, extruded blocks, fixed-per-face
orientation, the Gold skin, reference controls, inverted drag, the four
powerups and the sonar fade — is checked on the PC side but **has not been
compiled or run on hardware**; there is no Arduino toolchain in the environment
it was written in.

Verified here:
* the cube model for n=4..12: cell counts matching the game's own 98 and 296,
  the adjacency rule chosen by measurement rather than assumption, and
  `cellAtFace` covering every surface square exactly once per exposed face
* projection: face visibility, `pick()` round-tripping every cap centre for both
  raised and flush blocks (600 of them on edge or corner blocks), texture
  orientation never mirrored *and* provably camera-independent, cube plus raised
  blocks fitting the viewport at every angle
* the skin pipeline end-to-end against the live store, plus a rendered preview
  of the Gold set built from `skin.cpp`'s own parsed palette

**Saves from before the block model will not load** — the format magic went from
`MMS1` to `MMS2` on purpose, because an old save has the wrong cell count.

Still to check on hardware:
* Arduino compile for Pancake, then V8
* frame rate — the front-to-back coverage mask cut fill from ~3.3× to ~1×, so
  this should be much improved over the first solid-cube build; confirm the drag
  is smooth and the settle is instant
* an on-device skin download start to finish
* a full game to a win and to a loss on 5³, then 12³ for the performance case

Known deviation from the reference: it uses a mild **perspective** projection
and this uses an orthographic one. That is what keeps tile mapping exactly
affine, picking a closed-form inverse, and the inner loop free of divides.
Adding perspective means a general-quad rasteriser and an iterative pick, so it
is deliberately deferred rather than overlooked.
