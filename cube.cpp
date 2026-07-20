#include "cube.h"
#include "configs.h"
#include "puconfig.h"
#include <SD.h>
#include <esp_heap_caps.h>

// Face bases. U x V is the outward normal for every face, which is what lets
// the renderer decide visibility from the sign of the projected normal alone.
// Origins are in units of N. Every u/v component is 0 or +1; cellAtFace()
// relies on that when it maps a face square back to the block that owns it.
const FaceBasis CUBE_BASIS[CUBE_FACES] = {
  // ox oy oz   ux uy uz   vx vy vz   nx ny nz
  {  1, 0, 0,    0, 1, 0,   0, 0, 1,   1, 0, 0 },   // 0  +X
  {  0, 0, 0,    0, 0, 1,   0, 1, 0,  -1, 0, 0 },   // 1  -X
  {  0, 1, 0,    0, 0, 1,   1, 0, 0,   0, 1, 0 },   // 2  +Y
  {  0, 0, 0,    1, 0, 0,   0, 0, 1,   0,-1, 0 },   // 3  -Y
  {  0, 0, 1,    1, 0, 0,   0, 1, 0,   0, 0, 1 },   // 4  +Z
  {  0, 0, 0,    0, 1, 0,   1, 0, 0,   0, 0,-1 },   // 5  -Z
};

const char *const PU_NAMES[PU_COUNT] = {
  "Burst Clear", "Lightning", "Lifesaver", "Sonar",
};
// Shown on the Power-up Info screen, which word-wraps them, so these can be
// full sentences rather than the terse fragments a menu row needs.
const char *const PU_BLURBS[PU_COUNT] = {
  "Reveals a round patch two rings across, centred on the block you tap. Mines in it are flagged, not removed.",
  "Reveals two rings at right angles, running right around the cube. Mines on them are flagged.",
  "Activate to arm it. The next mine you tap is flagged instead of ending the game. Only spent when it saves you.",
  "Briefly shows the mines within two rings of the block you tap, then fades them back to hidden.",
};

const char *const DIFF_NAMES[DIFF_COUNT] = {
  "Beginner", "Advanced", "Expert", "Mastermine", "Custom",
};
const uint8_t  DIFF_N[DIFF_COUNT]     = { 5, 8, 12, 16, 8 };
// 17 in 98 and 51 in 296 are the game's own numbers; the other two hold the
// same ~17.2% density.
const uint16_t DIFF_MINES[DIFF_COUNT] = { 17, 51, 125, 233, 51 };

static void *cubeAlloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(bytes);
  return p;
}

void Cube::release() {
  if (_cell)  { free(_cell);  _cell  = nullptr; }
  if (_face)  { free(_face);  _face  = nullptr; }
  if (_pos)   { free(_pos);   _pos   = nullptr; }
  if (_grid)  { free(_grid);  _grid  = nullptr; }
  if (_nb)    { free(_nb);    _nb    = nullptr; }
  if (_nbn)   { free(_nbn);   _nbn   = nullptr; }
  if (_stack) { free(_stack); _stack = nullptr; }
  if (_sonar) { free(_sonar); _sonar = nullptr; }
  _n = 0; _cells = 0; _state = GS_READY;
}

// Enumerate the shell in a fixed order (z, then y, then x) and record each
// block's position and which faces it exposes.
void Cube::buildShell() {
  const int n = _n, hi = n - 1;
  uint32_t vol = (uint32_t)n * n * n;
  for (uint32_t i = 0; i < vol; i++) _grid[i] = -1;

  uint16_t c = 0;
  for (int z = 0; z < n; z++)
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++) {
        bool onShell = (x == 0 || x == hi || y == 0 || y == hi || z == 0 || z == hi);
        if (!onShell) continue;
        _grid[((uint32_t)z * n + y) * n + x] = (int16_t)c;
        _pos[c] = (uint16_t)(x | (y << 5) | (z << 10));
        uint8_t m = 0;
        if (x == hi) m |= 1 << 0;      // +X
        if (x == 0)  m |= 1 << 1;      // -X
        if (y == hi) m |= 1 << 2;      // +Y
        if (y == 0)  m |= 1 << 3;      // -Y
        if (z == hi) m |= 1 << 4;      // +Z
        if (z == 0)  m |= 1 << 5;      // -Z
        _face[c] = m;
        c++;
      }
}

int Cube::cellAtFace(uint8_t f, uint8_t i, uint8_t j) const {
  if (!ready() || f >= CUBE_FACES || i >= _n || j >= _n) return -1;
  const FaceBasis &b = CUBE_BASIS[f];
  const int hi = _n - 1;
  const int8_t nn[3] = { b.nx, b.ny, b.nz };
  const int8_t uu[3] = { b.ux, b.uy, b.uz };
  const int8_t vv[3] = { b.vx, b.vy, b.vz };
  int p[3];
  for (int a = 0; a < 3; a++) {
    if (nn[a] > 0)      p[a] = hi;
    else if (nn[a] < 0) p[a] = 0;
    else                p[a] = i * uu[a] + j * vv[a];
  }
  return at(p[0], p[1], p[2]);
}

// The corner points of every face this block exposes, in DOUBLED integer
// coordinates so a face offset stays whole. Up to 3 faces x 4 corners.
uint8_t Cube::cornerPoints(uint16_t c, uint32_t *out) const {
  uint8_t x, y, z;
  posOf(c, x, y, z);
  const uint32_t span = (uint32_t)(2 * _n + 2);
  uint8_t nOut = 0;
  const uint8_t m = _face[c];
  for (uint8_t f = 0; f < CUBE_FACES; f++) {
    if (!(m & (1 << f))) continue;
    const FaceBasis &b = CUBE_BASIS[f];
    const int8_t nn[3] = { b.nx, b.ny, b.nz };
    int base[3] = { 2 * x, 2 * y, 2 * z };
    int ax[2], na = 0;
    for (int a = 0; a < 3; a++) {
      if (nn[a]) base[a] += 1 + nn[a];        // 0 -> outer plane, +1 -> far side
      else       ax[na++] = a;
    }
    for (int da = 0; da <= 2; da += 2)
      for (int db = 0; db <= 2; db += 2) {
        int p[3] = { base[0], base[1], base[2] };
        p[ax[0]] += da;
        p[ax[1]] += db;
        uint32_t id = ((uint32_t)p[0] * span + p[1]) * span + p[2];
        bool dup = false;
        for (uint8_t k = 0; k < nOut; k++) if (out[k] == id) { dup = true; break; }
        if (!dup) out[nOut++] = id;
      }
  }
  return nOut;
}

// Neighbours: two blocks are adjacent when any of their exposed faces share a
// corner point, measured on the cube's surface. Candidates only ever come from
// the 26 blocks around this one — verified for n up to 20 — so this stays a
// local scan rather than needing a map over the whole lattice.
void Cube::buildNeighbours() {
  uint32_t mine[12], other[12];
  for (uint16_t c = 0; c < _cells; c++) {
    uint8_t nm = cornerPoints(c, mine);
    uint8_t x, y, z;
    posOf(c, x, y, z);
    uint16_t *nb = _nb + (uint32_t)c * 8;
    uint8_t cnt = 0;
    for (int dz = -1; dz <= 1 && cnt < 8; dz++)
      for (int dy = -1; dy <= 1 && cnt < 8; dy++)
        for (int dx = -1; dx <= 1 && cnt < 8; dx++) {
          if (!dx && !dy && !dz) continue;
          int o = at((int)x + dx, (int)y + dy, (int)z + dz);
          if (o < 0) continue;
          uint8_t no = cornerPoints((uint16_t)o, other);
          bool share = false;
          for (uint8_t a = 0; a < nm && !share; a++)
            for (uint8_t b = 0; b < no; b++)
              if (mine[a] == other[b]) { share = true; break; }
          if (share) nb[cnt++] = (uint16_t)o;
        }
    _nbn[c] = cnt;
  }
}

bool Cube::begin(uint8_t n, uint16_t mines) {
  release();
  if (n < CUBE_N_MIN || n > CUBE_N_MAX) return false;

  _n     = n;
  _cells = cubeCellCount(n);

  uint32_t vol = (uint32_t)n * n * n;
  _cell  = (uint8_t  *)cubeAlloc(_cells);
  _face  = (uint8_t  *)cubeAlloc(_cells);
  _pos   = (uint16_t *)cubeAlloc((size_t)_cells * sizeof(uint16_t));
  _grid  = (int16_t  *)cubeAlloc(vol * sizeof(int16_t));
  _nbn   = (uint8_t  *)cubeAlloc(_cells);
  _nb    = (uint16_t *)cubeAlloc((size_t)_cells * 8 * sizeof(uint16_t));
  _stack = (uint16_t *)cubeAlloc((size_t)_cells * sizeof(uint16_t));
  _sonar = (uint8_t  *)cubeAlloc((_cells + 7) / 8);
  if (!_cell || !_face || !_pos || !_grid || !_nbn || !_nb || !_stack || !_sonar) {
    release();
    return false;
  }

  memset(_cell, 0, _cells);
  memset(_sonar, 0, (_cells + 7) / 8);

  _mines = mines;
  if (_mines > (uint16_t)(_cells - 10)) _mines = _cells - 10;
  if (_mines < 1) _mines = 1;

  _revealed = 0;
  _flags    = 0;
  _sinceReward = 0;
  _state    = GS_READY;
  _startMs  = 0;
  _endMs    = 0;
  _sonarMs  = 0;
  _lifeUsed = false;
  _lifeArmed = false;
  // Starting counts come from the editable XML on the card, not from a
  // constant here.
  for (int i = 0; i < PU_COUNT; i++) _held[i] = g_pu.start[i];

  buildShell();
  buildNeighbours();
  return true;
}

void Cube::placeMines(uint16_t safeCell) {
  for (uint16_t c = 0; c < _cells; c++) _cell[c] &= ~1;

  bool *banned = (bool *)cubeAlloc(_cells);
  if (banned) {
    memset(banned, 0, _cells);
    banned[safeCell] = true;
    const uint16_t *nb = neighbours(safeCell);
    for (uint8_t k = 0; k < _nbn[safeCell]; k++) banned[nb[k]] = true;
  }

  uint16_t placed = 0, guard = 0;
  while (placed < _mines && guard < 60000) {
    guard++;
    uint16_t c = (uint16_t)random(_cells);
    if (banned && banned[c]) continue;
    if (!banned && c == safeCell) continue;
    if (_cell[c] & 1) continue;
    _cell[c] |= 1;
    placed++;
  }
  if (placed < _mines) {
    for (uint16_t c = 0; c < _cells && placed < _mines; c++) {
      if (banned && banned[c]) continue;
      if (c == safeCell || (_cell[c] & 1)) continue;
      _cell[c] |= 1;
      placed++;
    }
  }
  _mines = placed;
  if (banned) free(banned);

  recountAdj();
}

void Cube::recountAdj() {
  for (uint16_t c = 0; c < _cells; c++) {
    uint8_t a = 0;
    const uint16_t *nb = neighbours(c);
    for (uint8_t k = 0; k < _nbn[c]; k++) if (_cell[nb[k]] & 1) a++;
    setAdj(c, a);
  }
}

uint32_t Cube::elapsedMs() const {
  if (_startMs == 0) return 0;
  if (_state == GS_WON || _state == GS_LOST) return _endMs - _startMs;
  return millis() - _startMs;
}

float Cube::sonarFade() const {
  if (_sonarMs == 0) return 0.0f;
  uint32_t dt = millis() - _sonarMs;
  if (dt >= SONAR_MS) return 0.0f;
  return 1.0f - (float)dt / (float)SONAR_MS;
}

// Iterative flood fill. Recursion would be the obvious way and the wrong one:
// a 20-cube can chain thousands of blocks deep and blow the stack.
void Cube::revealFlood(uint16_t c) {
  uint16_t sp = 0;
  _stack[sp++] = c;
  while (sp) {
    uint16_t k = _stack[--sp];
    if (stateOf(k) != CS_HIDDEN) continue;
    setState(k, CS_REVEALED);
    _revealed++;
    creditReveal();
    if (adj(k) != 0) continue;
    const uint16_t *nb = neighbours(k);
    for (uint8_t m = 0; m < _nbn[k]; m++) {
      uint16_t o = nb[m];
      if (stateOf(o) == CS_HIDDEN && sp < _cells) _stack[sp++] = o;
    }
  }
}

void Cube::creditReveal() {
  if (g_pu.every == 0) return;                    // awards switched off
  if (++_sinceReward < g_pu.every) return;
  _sinceReward = 0;
  // Hand out whichever powerup is furthest below its own cap, so a full stack
  // of one kind does not waste the award.
  int best = -1;
  int bestRoom = 0;
  for (uint8_t p = 0; p < PU_COUNT; p++) {
    int room = (int)g_pu.maxHeld[p] - (int)_held[p];
    if (room > bestRoom) { bestRoom = room; best = p; }
  }
  if (best >= 0) _held[best]++;
}

void Cube::loseAt(uint16_t c) {
  _cell[c] |= 8;
  setState(c, CS_REVEALED);
  _state = GS_LOST;
  _endMs = millis();
  for (uint16_t k = 0; k < _cells; k++)
    if ((_cell[k] & 1) && stateOf(k) == CS_HIDDEN) setState(k, CS_REVEALED);
}

void Cube::checkWin() {
  if (_state != GS_PLAYING) return;
  if (_revealed + _mines < _cells) return;
  _state = GS_WON;
  _endMs = millis();
  for (uint16_t k = 0; k < _cells; k++)
    if (stateOf(k) == CS_HIDDEN) { setState(k, CS_FLAGGED); _flags++; }
}

bool Cube::reveal(uint16_t c) {
  if (!ready() || c >= _cells) return false;
  if (_state == GS_WON || _state == GS_LOST) return false;
  CellState s = stateOf(c);
  if (s == CS_REVEALED || s == CS_FLAGGED) return false;

  if (_state == GS_READY) {
    placeMines(c);
    _state   = GS_PLAYING;
    _startMs = millis();
  }

  if (isMine(c)) {
    // A Lifesaver only saves you if you ARMED it. Holding one is not enough —
    // spending a powerup has to be the player's decision, not something the
    // game does on their behalf.
    if (_lifeArmed) {
      // The mine stays put and is FLAGGED, exactly as the clearing powerups
      // treat one. Removing it would change every count around it and rewrite
      // the puzzle underneath the player.
      _lifeArmed = false;
      _lifeUsed = true;
      if (stateOf(c) != CS_FLAGGED) { setState(c, CS_FLAGGED); _flags++; }
      checkWin();
      return true;
    }
    loseAt(c);
    return true;
  }
  revealFlood(c);
  checkWin();
  return true;
}

bool Cube::cycleFlag(uint16_t c) {
  if (!ready() || c >= _cells) return false;
  if (_state == GS_WON || _state == GS_LOST) return false;
  switch (stateOf(c)) {
    case CS_HIDDEN:   setState(c, CS_FLAGGED);  _flags++; return true;
    case CS_FLAGGED:  setState(c, CS_QUESTION); _flags--; return true;
    case CS_QUESTION: setState(c, CS_HIDDEN);             return true;
    default: return false;
  }
}

bool Cube::chord(uint16_t c) {
  if (!ready() || c >= _cells) return false;
  if (_state != GS_PLAYING) return false;
  if (stateOf(c) != CS_REVEALED) return false;
  uint8_t a = adj(c);
  if (a == 0) return false;

  const uint16_t *nb = neighbours(c);
  uint8_t flagged = 0;
  for (uint8_t k = 0; k < _nbn[c]; k++) if (stateOf(nb[k]) == CS_FLAGGED) flagged++;
  if (flagged != a) return false;

  bool acted = false;
  for (uint8_t k = 0; k < _nbn[c]; k++) {
    uint16_t o = nb[k];
    if (stateOf(o) != CS_HIDDEN) continue;
    acted = true;
    if (!reveal(o)) continue;
    if (_state == GS_LOST) return true;
  }
  if (acted) checkWin();
  return acted;
}

// Open the hidden neighbours of every revealed block that has no adjacent
// mines, until nothing more changes.
//
// The clearing powerups need this. They open blocks directly and then rebuild
// the adjacency counts, and because they REMOVE mines, blocks that already
// showed a number can drop to zero. A zero block with hidden neighbours is not
// a state ordinary play can produce — every zero opened by reveal() cascades —
// so without this pass the board is left with pockets that should have opened
// themselves, which is what "some cells have no number next to them after a
// powerup" was.
void Cube::cascadeZeros() {
  bool again = true;
  while (again) {
    again = false;
    for (uint16_t c = 0; c < _cells; c++) {
      if (stateOf(c) != CS_REVEALED || adj(c) != 0) continue;
      const uint16_t *nb = neighbours(c);
      for (uint8_t k = 0; k < _nbn[c]; k++) {
        uint16_t o = nb[k];
        // Flags are left alone: a flag on a block next to a zero is simply
        // wrong, and clearing it for the player would hide their mistake.
        if (stateOf(o) != CS_HIDDEN) continue;
        if (isMine(o)) continue;            // impossible when adj is 0; be safe
        setState(o, CS_REVEALED);
        _revealed++;
        creditReveal();
        again = true;
      }
    }
  }
}

// Open one block for the revealing powerups.
//
// Mines are NOT removed — the board a powerup leaves behind has to be the same
// puzzle, just with more of it known. A mine in the area is FLAGGED, everything
// else is revealed with its true number. Deleting mines instead would change
// every count around the area and quietly rewrite the puzzle under the player.
void Cube::clearBlock(uint16_t c) {
  if (isMine(c)) {
    if (stateOf(c) != CS_FLAGGED) { setState(c, CS_FLAGGED); _flags++; }
    return;
  }
  if (stateOf(c) == CS_FLAGGED) _flags--;      // a flag that was simply wrong
  if (stateOf(c) != CS_REVEALED) { setState(c, CS_REVEALED); _revealed++; }
}

bool Cube::usePowerup(Powerup p, uint16_t target, uint8_t &out) {
  out = 0;
  if (!ready() || p >= PU_COUNT || _held[p] == 0) return false;
  if (_state == GS_WON || _state == GS_LOST) return false;
  if (_state == GS_READY) return false;      // mines are not placed yet
  if (needsTarget(p) && target >= _cells) return false;

  switch (p) {
    case PU_BURST: {
      // A round patch two rings across.
      //
      // Two steps, and both matter. First walk TWO RINGS out over the
      // neighbour graph, which is what bounds the effect to "two rings" and
      // wraps a cube edge correctly, because the adjacency already does. Then
      // keep only what is within BURST_RADIUS in 3D, which rounds the square
      // off — the far corners of a 5x5 sit at 2.83 and drop out.
      //
      // Distance alone is not enough: on a small cube a 2.5-radius sphere
      // reaches blocks around the edge that are nowhere near two rings away
      // over the surface, and the patch balloons (33 blocks instead of 21 on a
      // 5-cube). The graph walk is what keeps it honest.
      uint16_t patch[96];
      uint8_t np = 0;
      patch[np++] = target;
      const uint16_t *nb1 = neighbours(target);
      for (uint8_t k = 0; k < _nbn[target] && np < 96; k++) patch[np++] = nb1[k];
      const uint8_t ring1End = np;
      for (uint8_t a = 1; a < ring1End; a++) {
        const uint16_t *nb2 = neighbours(patch[a]);
        for (uint8_t k = 0; k < _nbn[patch[a]] && np < 96; k++) {
          uint16_t o = nb2[k];
          bool dup = false;
          for (uint8_t m = 0; m < np; m++) if (patch[m] == o) { dup = true; break; }
          if (!dup) patch[np++] = o;
        }
      }

      uint8_t tx, ty, tz;
      posOf(target, tx, ty, tz);
      const float cx = tx + 0.5f, cy = ty + 0.5f, cz = tz + 0.5f;
      const float r2 = BURST_RADIUS * BURST_RADIUS;
      uint16_t cnt = 0;
      for (uint8_t a = 0; a < np; a++) {
        uint8_t bx, by, bz;
        posOf(patch[a], bx, by, bz);
        const float dx = bx + 0.5f - cx, dy = by + 0.5f - cy, dz = bz + 0.5f - cz;
        if (dx * dx + dy * dy + dz * dz > r2) continue;
        clearBlock(patch[a]);
        cnt++;
      }
      cascadeZeros();
      out = (uint8_t)(cnt > 255 ? 255 : cnt);
      break;
    }
    case PU_LIGHTNING: {
      // A cross of bolts right around the cube: BOTH the ring through the
      // target horizontally and the one vertically, which together wrap the
      // cube twice at right angles.
      //
      // A ring is the set of shell blocks sharing one coordinate with the
      // target. That is 4(n-1) blocks when the coordinate is INTERIOR; at 0 or
      // n-1 the same set is a whole face, so only interior axes qualify. A
      // face-centre block has exactly two of those — its face's horizontal and
      // vertical — which is the pair we want. An edge block has one, a corner
      // block none.
      uint8_t x, y, z;
      posOf(target, x, y, z);
      const uint8_t co[3] = { x, y, z };
      int axes[2], na = 0;
      for (int a = 0; a < 3 && na < 2; a++)
        if (co[a] != 0 && co[a] != _n - 1) axes[na++] = a;

      if (na == 0) {                          // a corner block: nothing interior
        clearBlock(target);
        cascadeZeros();
        out = 1;
        break;
      }
      uint16_t cnt = 0;
      for (uint16_t c = 0; c < _cells; c++) {
        uint8_t bx, by, bz;
        posOf(c, bx, by, bz);
        const uint8_t bc[3] = { bx, by, bz };
        bool onRing = false;
        for (int k = 0; k < na; k++)
          if (bc[axes[k]] == co[axes[k]]) { onRing = true; break; }
        if (!onRing) continue;
        clearBlock(c);                        // the two rings cross; counted once
        cnt++;
      }
      cascadeZeros();
      out = (uint8_t)(cnt > 255 ? 255 : cnt);
      break;
    }
    case PU_LIFESAVER:
      // Arms it. The block itself is spent now, and the protection lasts until
      // a mine actually uses it. Arming a second one would burn it for nothing.
      if (_lifeArmed) { out = 1; return false; }
      _lifeArmed = true;
      out = 0;
      break;
    case PU_SONAR: {
      // Mark the mines within two rings of the target. The renderer shows them
      // and fades them back to hidden over SONAR_MS.
      memset(_sonar, 0, (_cells + 7) / 8);
      bool *seen = (bool *)cubeAlloc(_cells);
      if (!seen) return false;
      memset(seen, 0, _cells);
      seen[target] = true;
      uint16_t ring[128];
      uint8_t rn = 0;
      const uint16_t *nb = neighbours(target);
      for (uint8_t k = 0; k < _nbn[target]; k++) {
        seen[nb[k]] = true;
        if (rn < 128) ring[rn++] = nb[k];
      }
      for (uint8_t k = 0; k < rn; k++) {
        const uint16_t *nb2 = neighbours(ring[k]);
        for (uint8_t m = 0; m < _nbn[ring[k]]; m++) seen[nb2[m]] = true;
      }
      uint16_t cnt = 0;
      for (uint16_t c = 0; c < _cells; c++)
        if (seen[c] && (_cell[c] & 1) && stateOf(c) != CS_REVEALED) {
          _sonar[c >> 3] |= (uint8_t)(1 << (c & 7));
          cnt++;
        }
      free(seen);
      _sonarMs = millis();
      out = (uint8_t)(cnt > 255 ? 255 : cnt);
      break;
    }
    default: return false;
  }

  _held[p]--;
  checkWin();
  return true;
}

// Save format "MMS2": a fixed header then one byte per block. The mine layout
// lives in those bytes, so a resumed game is the same board, not a re-roll.
// Bumped from MMS1 when the board changed from face squares to shell blocks —
// an old save has a different cell count and must not load — and again to MMS3
// when the armed-Lifesaver flag joined the header.
static const char SAVE_MAGIC[4] = { 'M', 'M', 'S', '3' };

bool Cube::save(const char *path) const {
  if (!ready()) return false;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  uint32_t elapsed = elapsedMs();
  f.write((const uint8_t *)SAVE_MAGIC, 4);
  f.write(&_n, 1);
  uint8_t st = (uint8_t)_state;
  f.write(&st, 1);
  f.write((const uint8_t *)&_mines, 2);
  f.write((const uint8_t *)&_revealed, 2);
  f.write((const uint8_t *)&_flags, 2);
  f.write((const uint8_t *)&_sinceReward, 2);
  f.write(_held, PU_COUNT);
  uint8_t armed = _lifeArmed ? 1 : 0;
  f.write(&armed, 1);
  f.write((const uint8_t *)&elapsed, 4);
  f.write(_cell, _cells);
  f.close();
  return true;
}

bool Cube::load(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  char magic[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, SAVE_MAGIC, 4) != 0) { f.close(); return false; }
  uint8_t n = 0, st = 0;
  f.read(&n, 1);
  f.read(&st, 1);
  if (n < CUBE_N_MIN || n > CUBE_N_MAX) { f.close(); return false; }
  if (!begin(n, 1)) { f.close(); return false; }

  uint16_t mines = 0, rev = 0, fl = 0, since = 0;
  uint32_t elapsed = 0;
  f.read((uint8_t *)&mines, 2);
  f.read((uint8_t *)&rev, 2);
  f.read((uint8_t *)&fl, 2);
  f.read((uint8_t *)&since, 2);
  f.read(_held, PU_COUNT);
  uint8_t armed = 0;
  f.read(&armed, 1);
  f.read((uint8_t *)&elapsed, 4);
  bool ok = (f.read(_cell, _cells) == (int)_cells);
  f.close();
  if (!ok) { release(); return false; }
  _lifeArmed = (armed != 0);

  _mines = mines; _revealed = rev; _flags = fl; _sinceReward = since;
  _state = (GameState)st;
  _startMs = millis() - elapsed;
  _endMs   = millis();
  _sonarMs = 0;
  return true;
}
