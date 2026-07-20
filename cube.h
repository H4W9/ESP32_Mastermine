#pragma once
#ifndef cube_h
#define cube_h

#include <Arduino.h>

// Mastermine board model: Minesweeper on the SHELL OF A CUBE OF BLOCKS.
//
// A cell is a CUBIE, not a square. The board is the hollow shell of an NxNxN
// grid of blocks, so it holds N^3 - (N-2)^3 cells. The game states this itself:
// its Advanced difficulty is "51 mines in 296 total blocks", and 296 = 8^3-6^3.
// Six faces of squares would have been 384.
//
// The consequence that matters everywhere else: a block on a cube EDGE is ONE
// cell showing the same number on two faces, and a block on a CORNER shows it
// on three. Reveal it from any of them and the whole block reveals.
//
// ADJACENCY is measured on the cube's SURFACE, not in the volume: two blocks
// are neighbours exactly when any of their exposed faces share a corner point.
// That is the only rule that caps at 8 neighbours — which it must, because the
// skin texture set stops at tile8. Plain 26-connectivity in the volume peaks at
// 13 and face+edge connectivity at 10; both were measured and rejected. The
// surface rule gives a strikingly clean distribution: the 8 corner blocks have
// 6 neighbours and every other block has exactly 8.
//
// This file is pure logic — no drawing, no globals, no TFT. render3d.* turns a
// Cube into pixels and the .ino drives both.

// Face numbering. U and V are the in-face axes, ordered so that U x V is the
// outward normal, which is what lets the renderer cull back faces by sign
// alone. Every u/v component is 0 or +1, which cube.cpp relies on when it maps
// a face square back to the block that owns it.
//   0 = +X   1 = -X   2 = +Y   3 = -Y   4 = +Z   5 = -Z
static const uint8_t CUBE_FACES = 6;

struct FaceBasis {
  int8_t ox, oy, oz;    // origin corner, in units of N (0 or 1)
  int8_t ux, uy, uz;    // +1 step along i
  int8_t vx, vy, vz;    // +1 step along j
  int8_t nx, ny, nz;    // outward normal
};
extern const FaceBasis CUBE_BASIS[CUBE_FACES];

// N range. The game's own smallest board is 5x5, and below 5 the shell is thick
// enough relative to the cube that it stops behaving like a surface. 20^3 is
// 2168 cells, beyond which the blocks are smaller than a fingertip anyway.
static const uint8_t CUBE_N_MAX = 20;
static const uint8_t CUBE_N_MIN = 4;

enum CellState : uint8_t {
  CS_HIDDEN   = 0,
  CS_REVEALED = 1,
  CS_FLAGGED  = 2,
  CS_QUESTION = 3,
};

enum GameState : uint8_t {
  GS_READY   = 0,   // board built, mines not placed yet (first tap is always safe)
  GS_PLAYING = 1,
  GS_WON     = 2,
  GS_LOST    = 3,
};

// The four powerups the reference game actually has (named in its achievement
// list). Note SHUFFLE is not among them and has been dropped.
enum Powerup : uint8_t {
  PU_BURST     = 0,   // clear a block and its ring, mines and all
  PU_LIGHTNING = 1,   // clear the whole ring of blocks encircling the cube
  PU_LIFESAVER = 2,   // passive: survive one mine instead of losing
  PU_SONAR     = 3,   // briefly show where the nearby mines are
  PU_COUNT     = 4,
};
extern const char *const PU_NAMES[PU_COUNT];
extern const char *const PU_BLURBS[PU_COUNT];

// Difficulties. Mine counts for Beginner and Advanced are the game's own
// figures (17 in 98, 51 in 296); Expert and Mastermine are extrapolated from
// that same ~17.2% density because the reference screenshots have them locked.
enum Difficulty : uint8_t {
  DIFF_BEGINNER   = 0,  // 5x5
  DIFF_ADVANCED   = 1,  // 8x8
  DIFF_EXPERT     = 2,  // 12x12
  DIFF_MASTERMINE = 3,  // 16x16
  DIFF_CUSTOM     = 4,
  DIFF_COUNT      = 5,
};
extern const char *const DIFF_NAMES[DIFF_COUNT];
extern const uint8_t     DIFF_N[DIFF_COUNT];
extern const uint16_t    DIFF_MINES[DIFF_COUNT];

// Number of shell blocks in an N-cube.
static inline uint16_t cubeCellCount(uint8_t n) {
  uint32_t inner = (n >= 2) ? (uint32_t)(n - 2) * (n - 2) * (n - 2) : 0;
  return (uint16_t)((uint32_t)n * n * n - inner);
}

// Starting counts, per-powerup caps and the award interval all live in
// PowerupConfig (puconfig.h), loaded from an editable XML file on the SD card.

// How long a sonar ping stays visible before it has faded back to hidden.
static const uint32_t SONAR_MS = 1000;

// Burst Clear's radius, in cell widths, measured between block centres in 3D.
// 2.5 takes everything two rings out along a row or a diagonal-and-one, but
// leaves the far corners of a 5x5 (2.83 away) outside — so the hole is round
// rather than square, and it wraps a cube edge without any special case.
static const float BURST_RADIUS = 2.5f;

class Cube {
public:
  Cube() {}
  ~Cube() { release(); }

  bool begin(uint8_t n, uint16_t mines);
  void release();

  bool     ready()  const { return _cell != nullptr; }
  uint8_t  n()      const { return _n; }
  uint16_t cells()  const { return _cells; }
  uint16_t mines()  const { return _mines; }
  GameState state() const { return _state; }
  uint16_t revealed() const { return _revealed; }
  uint16_t flags()  const { return _flags; }
  int16_t  minesLeft() const { return (int16_t)_mines - (int16_t)_flags; }
  uint32_t elapsedMs() const;

  // Block position and lookup. Cells are numbered in the order they are found
  // scanning z, y, x; _grid maps a coordinate back to the cell, or -1 when the
  // coordinate is inside the shell.
  void posOf(uint16_t c, uint8_t &x, uint8_t &y, uint8_t &z) const {
    uint16_t p = _pos[c];
    x = (uint8_t)(p & 31);
    y = (uint8_t)((p >> 5) & 31);
    z = (uint8_t)((p >> 10) & 31);
  }
  int at(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= _n || y >= _n || z >= _n) return -1;
    return _grid[((uint32_t)z * _n + y) * _n + x];
  }
  // Which block owns square (i,j) of face f. This is the indirection that makes
  // an edge block show the same number on both of its faces.
  int cellAtFace(uint8_t f, uint8_t i, uint8_t j) const;
  // Bit f set means the block shows a face in direction f.
  uint8_t faceMask(uint16_t c) const { return _face[c]; }

  CellState stateOf(uint16_t c) const { return (CellState)((_cell[c] >> 1) & 3); }
  uint8_t   adj(uint16_t c)     const { return (uint8_t)(_cell[c] >> 4); }
  bool      isMine(uint16_t c)  const { return (_cell[c] & 1) != 0; }
  bool      isBoom(uint16_t c)  const { return (_cell[c] & 8) != 0; }

  uint8_t         neighbourCount(uint16_t c) const { return _nbn[c]; }
  const uint16_t *neighbours(uint16_t c)     const { return _nb + (uint32_t)c * 8; }

  bool reveal(uint16_t c);
  bool cycleFlag(uint16_t c);
  bool chord(uint16_t c);

  // Powerups. Every one of them is spent only when the player activates it —
  // nothing fires on its own. Lifesaver takes no target: activating it ARMS it,
  // and it is then spent by the next mine you would have died to.
  uint8_t held(Powerup p) const { return _held[p]; }
  bool    needsTarget(Powerup p) const { return p != PU_LIFESAVER; }
  bool    lifeArmed() const { return _lifeArmed; }
  bool    usePowerup(Powerup p, uint16_t target, uint8_t &out);
  // True when the last reveal() was saved by an armed Lifesaver; cleared on read.
  bool    takeLifesaverUsed() { bool v = _lifeUsed; _lifeUsed = false; return v; }

  // Sonar: marks blocks whose mines are briefly shown, then fade back to
  // hidden over SONAR_MS. The renderer asks for the fade factor each frame.
  bool  sonarShows(uint16_t c) const {
    return _sonar && (_sonar[c >> 3] & (1 << (c & 7))) != 0;
  }
  // 1.0 right after the ping, 0.0 once it has faded out; 0 when inactive.
  float sonarFade() const;
  bool  sonarActive() const { return sonarFade() > 0.0f; }

  bool save(const char *path) const;
  bool load(const char *path);

private:
  void  buildShell();
  void  buildNeighbours();
  void  placeMines(uint16_t safeCell);
  void  recountAdj();
  void  setState(uint16_t c, CellState s) { _cell[c] = (uint8_t)((_cell[c] & ~0x06) | ((uint8_t)s << 1)); }
  void  setAdj(uint16_t c, uint8_t a)     { _cell[c] = (uint8_t)((_cell[c] & 0x0F) | (a << 4)); }
  void  revealFlood(uint16_t c);
  void  clearBlock(uint16_t c);
  // Open the hidden neighbours of every revealed block that has no adjacent
  // mines, repeatedly, until nothing changes.
  void  cascadeZeros();
  void  loseAt(uint16_t c);
  void  checkWin();
  void  creditReveal();
  // The 12 surface corner points of a block, in doubled integer coords.
  uint8_t cornerPoints(uint16_t c, uint32_t *out) const;

  uint8_t   _n       = 0;
  uint16_t  _cells   = 0;
  uint16_t  _mines   = 0;
  uint16_t  _revealed = 0;
  uint16_t  _flags    = 0;
  uint16_t  _sinceReward = 0;
  GameState _state   = GS_READY;
  uint32_t  _startMs = 0;
  uint32_t  _endMs   = 0;
  uint32_t  _sonarMs = 0;
  bool      _lifeUsed = false;
  bool      _lifeArmed = false;
  uint8_t   _held[PU_COUNT] = { 1, 1, 1, 1 };

  // bit0 mine · bits1-2 CellState · bit3 detonated · bits4-7 adjacency count
  uint8_t  *_cell = nullptr;
  uint8_t  *_face = nullptr;   // exposed-face bitmask per cell
  uint16_t *_pos  = nullptr;   // packed x | y<<5 | z<<10
  int16_t  *_grid = nullptr;   // n^3 coordinate -> cell, or -1
  uint16_t *_nb   = nullptr;   // 8 slots per cell
  uint8_t  *_nbn  = nullptr;
  uint16_t *_stack = nullptr;
  uint8_t  *_sonar = nullptr;  // one bit per cell
};

#endif // cube_h
