#include "render3d.h"
#include <math.h>

// Light direction, fixed in CUBE space — so each of the six faces has a shade
// that never changes, however the cube is turned.
//
// It used to be fixed in VIEW space, which meant a face's brightness drifted
// continuously as you dragged. That reads as the colours shifting under your
// finger, which is exactly what you do not want on a board you are trying to
// read. Anchoring the light to the cube costs nothing — with the shading this
// gentle even the least-lit face is around 80% — and the cube now looks like a
// painted object rather than one under a moving lamp.
static const float LX = 0.35f, LY = 0.80f, LZ = 0.49f;

// Ambient floor plus the diffuse term. Gentle on purpose: in the reference's
// default look the three visible faces of a white block sample around
// rgb(255,255,255), rgb(250,250,252) and rgb(225,225,230), so the darkest face
// is still about 88% of the brightest. Earlier settings here were far harsher
// and turned the white set grey.
static const float SHADE_AMBIENT = 0.78f;
static const float SHADE_DIFFUSE = 0.22f;

// Per-channel shading exponents, 1.0 = a plain neutral multiply.
//
// These were briefly set warm (1.35 / 1.80) after measuring the reference's
// gold set, where a lit face samples rgb(254,253,129) and a shaded one
// rgb(112,80,33) — ratios that are genuinely not a neutral scale. That was the
// wrong conclusion: the warmth lives in the gold TEXTURE's own yellow-to-brown
// gradient, not in the lighting. Applying it as lighting turned the white
// default set beige, which the reference plainly is not. Left in place, and
// neutral, because a skin-specific tint may be worth having later.
static const float SHADE_POW_G = 1.0f;
static const float SHADE_POW_B = 1.0f;

bool CubeView::begin(TFT_eSPI *tft, int x, int y, int w, int h) {
  release();
  _tft = tft;
  _vx = x; _vy = y; _vw = w; _vh = h;
  _spr = new TFT_eSprite(tft);
  if (!_spr) return false;
  _spr->setColorDepth(16);
  if (_spr->createSprite(w, h) == nullptr) { delete _spr; _spr = nullptr; return false; }
  _buf = (uint16_t *)_spr->getPointer();
  if (!_buf) { _spr->deleteSprite(); delete _spr; _spr = nullptr; return false; }
  resetCamera();
  refit();
  return true;
}

void CubeView::release() {
  if (_spr) { _spr->deleteSprite(); delete _spr; _spr = nullptr; }
  _buf = nullptr;
}

// Fit the cube's worst-case silhouette into the viewport. Turned to a corner,
// the projection spans the body diagonal, N*sqrt(3) cells across. The blocks
// sit inside that, so the diagonal alone is the bound.
void CubeView::refit() {
  int n = (_cube && _cube->ready()) ? _cube->n() : 8;
  float span = (float)n * 1.7320508f;
  float m = (float)((_vw < _vh) ? _vw : _vh) * 0.92f;
  _fit  = m / span;
  _cell = _fit * _zoom;
}

void CubeView::setCamera(float yaw, float pitch) {
  const float cy = cosf(yaw), sy = sinf(yaw);
  const float cp = cosf(pitch), sp = sinf(pitch);
  // Rx(pitch) * Ry(yaw)
  _rot[0] =  cy;        _rot[1] = 0.0f;  _rot[2] =  sy;
  _rot[3] =  sp * sy;   _rot[4] = cp;    _rot[5] = -sp * cy;
  _rot[6] = -cp * sy;   _rot[7] = sp;    _rot[8] =  cp * cy;
}

void CubeView::resetCamera() { setCamera(0.7f, 0.55f); }

// Compose a drag increment in VIEW space: _rot = Rx(dpitch) * Ry(dyaw) * _rot.
// Because the increment is applied on the camera side, the axes it turns about
// are always the ones on screen, so the cube keeps following the finger no
// matter how far it has already been spun. There is no pole and no limit.
void CubeView::orbit(float dyaw, float dpitch) {
  const float cy = cosf(dyaw), sy = sinf(dyaw);
  const float cp = cosf(dpitch), sp = sinf(dpitch);
  // M = Rx(dpitch) * Ry(dyaw)
  const float m[9] = {
     cy,       0.0f,  sy,
     sp * sy,  cp,   -sp * cy,
    -cp * sy,  sp,    cp * cy,
  };
  float out[9];
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      out[r * 3 + c] = m[r * 3 + 0] * _rot[0 * 3 + c]
                     + m[r * 3 + 1] * _rot[1 * 3 + c]
                     + m[r * 3 + 2] * _rot[2 * 3 + c];
  memcpy(_rot, out, sizeof(_rot));
  renormalise();
}

// Gram-Schmidt on the rows. Thousands of composed increments would otherwise
// drift out of orthonormal and shear the cube.
void CubeView::renormalise() {
  float *a = _rot, *b = _rot + 3, *c = _rot + 6;
  float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (la < 1e-6f) { resetCamera(); return; }
  for (int i = 0; i < 3; i++) a[i] /= la;
  float d = b[0] * a[0] + b[1] * a[1] + b[2] * a[2];
  for (int i = 0; i < 3; i++) b[i] -= d * a[i];
  float lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  if (lb < 1e-6f) { resetCamera(); return; }
  for (int i = 0; i < 3; i++) b[i] /= lb;
  c[0] = a[1] * b[2] - a[2] * b[1];
  c[1] = a[2] * b[0] - a[0] * b[2];
  c[2] = a[0] * b[1] - a[1] * b[0];
}

void CubeView::setZoom(float z) {
  if (z < zoomMin()) z = zoomMin();
  if (z > zoomMax()) z = zoomMax();
  _zoom = z;
  _cell = _fit * _zoom;
}

// Project each face into screen space: where its (0,0) corner lands, and the
// two screen vectors that step one cell along i and j. Because the projection
// is orthographic those two vectors are the same for every tile on the face,
// which is what makes each tile an identical parallelogram.
void CubeView::project() {
  if (!_cube || !_cube->ready()) return;
  const int n = _cube->n();
  const float half = n * 0.5f;

  const float *R = _rot;
  auto rotX = [&](float x, float y, float z) { return R[0] * x + R[1] * y + R[2] * z; };
  auto rotY = [&](float x, float y, float z) { return R[3] * x + R[4] * y + R[5] * z; };
  auto rotZ = [&](float x, float y, float z) { return R[6] * x + R[7] * y + R[8] * z; };

  const float ccx = _vw * 0.5f, ccy = _vh * 0.5f;

  for (uint8_t f = 0; f < CUBE_FACES; f++) {
    const FaceBasis &b = CUBE_BASIS[f];
    FaceProj &p = _fp[f];

    // Back-face cull. CUBE_BASIS guarantees U x V is the outward normal
    // (asserted by sd_prep/check_cube.py), so a normal rotating to a positive
    // view-space z is pointing at the camera.
    float nz = rotZ((float)b.nx, (float)b.ny, (float)b.nz);
    if (nz <= 0.02f) { p.vis = false; continue; }
    p.vis = true;

    // Lambert on the CUBE-space normal, so this is a constant per face and
    // does not move while the cube is dragged. Mapped from [-1,1] rather than
    // clamped at 0, so all six faces get distinct shades instead of the three
    // facing away from the light collapsing to the same value.
    const float d = (float)b.nx * LX + (float)b.ny * LY + (float)b.nz * LZ;
    float s = SHADE_AMBIENT + SHADE_DIFFUSE * (0.5f + 0.5f * d);
    if (s > 1.0f) s = 1.0f;
    p.shade   = s;

    // Face origin, in cube coords centred on the origin.
    float ox3 = b.ox * n - half, oy3 = b.oy * n - half, oz3 = b.oz * n - half;
    p.ox = ccx + rotX(ox3, oy3, oz3) * _cell;
    p.oy = ccy - rotY(ox3, oy3, oz3) * _cell;      // screen y grows downward

    p.ux = rotX((float)b.ux, (float)b.uy, (float)b.uz) * _cell;
    p.uy = -rotY((float)b.ux, (float)b.uy, (float)b.uz) * _cell;
    p.vx = rotX((float)b.vx, (float)b.vy, (float)b.vz) * _cell;
    p.vy = -rotY((float)b.vx, (float)b.vy, (float)b.vz) * _cell;

    // Screen displacement of one cell along the OUTWARD normal. This one does
    // need the normal in VIEW space — the shading above deliberately uses the
    // cube-space normal instead, and the two must not be confused.
    const float snx =  rotX((float)b.nx, (float)b.ny, (float)b.nz) * _cell;
    const float sny = -rotY((float)b.nx, (float)b.ny, (float)b.nz) * _cell;
    p.snx = snx;
    p.sny = sny;
    // The outermost block's face sits BLOCK_GAP inside the nominal surface.
    // pick() inverts against this plane; paintBlocks() works out its own depth
    // per block.
    p.hx = -snx * BLOCK_GAP;
    p.hy = -sny * BLOCK_GAP;

    // Which cube axis each of U, V and N runs along. Every component is 0 or
    // +-1, so this is just finding the non-zero one.
    const int8_t uu[3] = { b.ux, b.uy, b.uz };
    const int8_t vv[3] = { b.vx, b.vy, b.vz };
    const int8_t nn[3] = { b.nx, b.ny, b.nz };
    for (uint8_t a = 0; a < 3; a++) {
      if (uu[a]) p.aU = a;
      if (vv[a]) p.aV = a;
      if (nn[a]) { p.aN = a; p.sN = nn[a]; }
    }

    float det = p.ux * p.vy - p.uy * p.vx;
    p.idet = (fabsf(det) < 1e-6f) ? 0.0f : 1.0f / det;
    chooseOrientation(p, b);
  }
}

// Which way round to lay the texture on this face's tiles.
//
// The art is FIXED TO THE FACE: it is painted on and turns with the cube, the
// way markings on a physical object do. So this depends only on the cube's
// geometry, never on where the camera is — it is recomputed per frame purely
// because that costs a couple of dozen integer operations and keeps all the
// projection in one place.
//
// The one thing that must not happen is a MIRRORED face. A face's (U,V) basis
// has no relation to image space, and laying the texture on straight leaves
// half the cube showing backwards digits — a mirrored 5 is unreadable, in a
// game whose entire content is numbers you read.
//
// Mirroring is settled by handedness alone. An unmirrored image has texture-x
// to the right and texture-y DOWN, and screen y grows downward, so seen from
// outside the pair must be left-handed about the outward normal, i.e.
//
//     TX x TY = -N
//
// CUBE_BASIS guarantees U x V = +N, so exactly four of the eight placements
// qualify: (U,-V), (-U,V), (V,U) and (-V,-U).
//
// Of those four, take the one whose texture-DOWN points most nearly at the
// cube's own down (-Y). That makes all four side faces upright. On the top and
// bottom faces every candidate is perpendicular to Y and the primary term ties,
// so the tie-break prefers texture-down pointing at +Z — which is how you read
// the top of a box you are standing in front of. Every component here is 0 or
// +-1, so the scoring is integer.
void CubeView::chooseOrientation(FaceProj &p, const FaceBasis &b) {
  static const int8_t CAND[4][2] = {
    { AX_U,     AX_V_INV },
    { AX_U_INV, AX_V     },
    { AX_V,     AX_U     },
    { AX_V_INV, AX_U_INV },
  };
  auto vec3 = [&](int8_t s, int &x, int &y, int &z) {
    switch (s) {
      case AX_U:     x =  b.ux; y =  b.uy; z =  b.uz; break;
      case AX_U_INV: x = -b.ux; y = -b.uy; z = -b.uz; break;
      case AX_V:     x =  b.vx; y =  b.vy; z =  b.vz; break;
      default:       x = -b.vx; y = -b.vy; z = -b.vz; break;
    }
  };

  p.txSrc = CAND[0][0];
  p.tySrc = CAND[0][1];
  int best = -100000;
  for (int k = 0; k < 4; k++) {
    int dx, dy, dz;
    vec3(CAND[k][1], dx, dy, dz);        // texture-down, in cube space
    int score = 1000 * (-dy)             // prefer pointing at the cube's -Y
              +   10 * ( dz)             // tie-break on top/bottom: towards +Z
              +        ( dx);            // final deterministic tie-break
    if (score > best) { best = score; p.txSrc = CAND[k][0]; p.tySrc = CAND[k][1]; }
  }
}

// Shading lookup tables, one set per face.
//
// They also carry the BYTE SWAP. A TFT_eSprite's 16bpp buffer holds each pixel
// with its bytes reversed — TFT_eSprite::drawPixel does `color = (color >> 8) |
// (color << 8)` before storing — and this renderer writes into that buffer
// directly through getPointer() for speed, so it has to match. Getting this
// wrong does not garble the picture in an obvious way: it shifts every hue,
// which showed up as a cube with inexplicable cyan and purple faces while the
// shell's own drawing, which goes through TFT_eSPI, stayed correct.
//
// Baking it in here is free. For a native word (R<<11)|(G<<5)|B, the swapped
// word is ((G&7)<<13) | (B<<8) | (R<<3) | (G>>3), and each channel's share of
// that can be pre-computed, so the inner loop is still three lookups and two
// ORs.
void CubeView::buildShadeLut(float shade) {
  int16_t key = (int16_t)(shade * 255.0f + 0.5f);
  if (key == _lutShade) return;
  _lutShade = key;
  const uint16_t kr = (uint16_t)(shade * 256.0f);
  const uint16_t kg = (uint16_t)(powf(shade, SHADE_POW_G) * 256.0f);
  const uint16_t kb = (uint16_t)(powf(shade, SHADE_POW_B) * 256.0f);
  for (int i = 0; i < 32; i++) {
    uint16_t r = (uint16_t)((i * kr) >> 8);
    uint16_t b = (uint16_t)((i * kb) >> 8);
    _lutR[i] = (uint16_t)(r << 3);
    _lutB[i] = (uint16_t)(b << 8);
  }
  for (int i = 0; i < 64; i++) {
    uint16_t g = (uint16_t)((i * kg) >> 8);
    _lutG[i] = (uint16_t)(((g & 0x7) << 13) | (g >> 3));
  }
}

// Native RGB565 -> the byte order a TFT_eSprite buffer holds.
static inline uint16_t sprSwap(uint16_t c) {
  return (uint16_t)((c >> 8) | (c << 8));
}

// The same warm curve, applied directly to one colour — for the flat side
// walls, which are painted in the tile's own hue so a teal block has teal
// sides and a gold one gold sides.
static inline uint16_t shadeWarm(uint16_t c, float s) {
  const uint16_t kr = (uint16_t)(s * 256.0f);
  const uint16_t kg = (uint16_t)(powf(s, SHADE_POW_G) * 256.0f);
  const uint16_t kb = (uint16_t)(powf(s, SHADE_POW_B) * 256.0f);
  uint16_t r = ((((c >> 11) & 0x1F) * kr) >> 8);
  uint16_t g = ((((c >> 5) & 0x3F) * kg) >> 8);
  uint16_t b = (((c & 0x1F) * kb) >> 8);
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Scanline solver shared by the textured cap and the flat side walls.
//
// Per scanline we solve for the exact x range where both parallelogram
// coordinates are inside [0,1) rather than testing every pixel in the bounding
// box — a sheared quad's bbox is up to twice its area, so the test would be
// thrown away half the time. Sampling on pixel centres with half-open intervals
// makes adjacent tiles abut exactly: tile i's last pixel is one before tile
// i+1's first, with no seam and no double-write.
#define R3D_SPAN_SETUP()                                                       \
  float x0f = px, x1f = px, y0f = py, y1f = py;                                \
  const float cxs[3] = { px + ax, px + bx, px + ax + bx };                      \
  const float cys[3] = { py + ay, py + by, py + ay + by };                      \
  for (int k = 0; k < 3; k++) {                                                \
    if (cxs[k] < x0f) x0f = cxs[k];                                            \
    if (cxs[k] > x1f) x1f = cxs[k];                                            \
    if (cys[k] < y0f) y0f = cys[k];                                            \
    if (cys[k] > y1f) y1f = cys[k];                                            \
  }                                                                            \
  int yTop = (int)ceilf(y0f - 0.5f), yBot = (int)ceilf(y1f - 0.5f) - 1;        \
  if (yTop < 0) yTop = 0;                                                      \
  if (yBot > _vh - 1) yBot = _vh - 1;                                          \
  if (yTop > yBot) return;                                                     \
  const float det = ax * by - ay * bx;                                         \
  if (fabsf(det) < 1e-6f) return;                                              \
  const float idet = 1.0f / det;                                               \
  const float dudx =  by * idet, dudy = -bx * idet;                            \
  const float dvdx = -ay * idet, dvdy =  ax * idet;

#define R3D_SPAN_ROW(yv)                                                       \
    const float dy = ((yv) + 0.5f) - py;                                       \
    float lo = -1e30f, hi = 1e30f;                                             \
    const float u0 = dudy * dy, v0 = dvdy * dy;                                \
    if (fabsf(dudx) < 1e-9f) { if (u0 < 0.0f || u0 >= 1.0f) continue; }        \
    else {                                                                     \
      float a1 = (0.0f - u0) / dudx + px, b1 = (1.0f - u0) / dudx + px;        \
      if (a1 > b1) { float t = a1; a1 = b1; b1 = t; }                          \
      if (a1 > lo) lo = a1;                                                    \
      if (b1 < hi) hi = b1;                                                    \
    }                                                                          \
    if (fabsf(dvdx) < 1e-9f) { if (v0 < 0.0f || v0 >= 1.0f) continue; }        \
    else {                                                                     \
      float a2 = (0.0f - v0) / dvdx + px, b2 = (1.0f - v0) / dvdx + px;        \
      if (a2 > b2) { float t = a2; a2 = b2; b2 = t; }                          \
      if (a2 > lo) lo = a2;                                                    \
      if (b2 < hi) hi = b2;                                                    \
    }                                                                          \
    if (hi <= lo) continue;                                                    \
    int xL = (int)ceilf(lo - 0.5f), xR = (int)ceilf(hi - 0.5f) - 1;            \
    if (xL < 0) xL = 0;                                                        \
    if (xR > _vw - 1) xR = _vw - 1;                                            \
    if (xL > xR) continue;

// Flat-shaded parallelogram, for the side walls of a raised block.
void CubeView::fillPara(float px, float py, float ax, float ay,
                        float bx, float by, uint16_t colour) {
  R3D_SPAN_SETUP()
  // One swap for the whole quad — the buffer is byte-reversed, see buildShadeLut.
  colour = sprSwap(colour);
  const int step = _fast ? 2 : 1;
  for (int y = yTop; y <= yBot; y += step) {
    R3D_SPAN_ROW(y)
    uint16_t *dst = _buf + (size_t)y * _vw + xL;
    for (int x = xL; x <= xR; x++) *dst++ = colour;
    if (step == 2 && y + 1 <= yBot)
      memcpy(_buf + (size_t)(y + 1) * _vw + xL,
             _buf + (size_t)y * _vw + xL,
             (size_t)(xR - xL + 1) * sizeof(uint16_t));
  }
}

// Textured cap of a block, with the skin texture mapped affinely across it.
void CubeView::paintTile(float px, float py, float ax, float ay, float bx, float by,
                         const FaceProj &fp, const uint16_t *tex, int ts,
                         const uint16_t *blendTex, uint8_t blendK) {
  if (!tex) return;
  R3D_SPAN_SETUP()

  const int32_t sdu = (int32_t)(dudx * ts * 65536.0f);
  const int32_t sdv = (int32_t)(dvdx * ts * 65536.0f);
  const int step = _fast ? 2 : 1;

  for (int y = yTop; y <= yBot; y += step) {
    R3D_SPAN_ROW(y)

    const float dxp = (xL + 0.5f) - px;
    const int32_t su = (int32_t)((dudx * dxp + u0) * ts * 65536.0f);
    const int32_t sv = (int32_t)((dvdx * dxp + v0) * ts * 65536.0f);

    // Apply the face's texture orientation by re-labelling the two
    // interpolators rather than transforming texel coordinates per pixel — it
    // costs a few operations per scanline instead of per pixel.
    const int32_t top = ((int32_t)ts << 16) - 1;
    int32_t sx, dsx, sy, dsy;
    switch (fp.txSrc) {
      case AX_U:     sx = su;       dsx =  sdu; break;
      case AX_U_INV: sx = top - su; dsx = -sdu; break;
      case AX_V:     sx = sv;       dsx =  sdv; break;
      default:       sx = top - sv; dsx = -sdv; break;
    }
    switch (fp.tySrc) {
      case AX_U:     sy = su;       dsy =  sdu; break;
      case AX_U_INV: sy = top - su; dsy = -sdu; break;
      case AX_V:     sy = sv;       dsy =  sdv; break;
      default:       sy = top - sv; dsy = -sdv; break;
    }

    uint16_t *dst = _buf + (size_t)y * _vw + xL;
    if (!blendTex && !_fast) {
      // Bilinear. Point sampling is what made fine skin art look broken —
      // thin digit strokes land between texels and come out ragged, and they
      // crawl as the cube turns. This costs four fetches and three lerps a
      // pixel, so it runs only when the cube is STILL; a drag falls through to
      // the point-sampled path below, where the motion hides it anyway.
      for (int x = xL; x <= xR; x++) {
        int tu = sx >> 16, tv = sy >> 16;
        int fu = (sx >> 8) & 0xFF, fv = (sy >> 8) & 0xFF;
        if (tu < 0) { tu = 0; fu = 0; } else if (tu >= ts - 1) { tu = ts - 1; fu = 0; }
        if (tv < 0) { tv = 0; fv = 0; } else if (tv >= ts - 1) { tv = ts - 1; fv = 0; }
        const uint16_t *row0 = tex + tv * ts + tu;
        const uint16_t *row1 = row0 + (fv ? ts : 0);
        uint16_t c00 = row0[0], c10 = row0[fu ? 1 : 0];
        uint16_t c01 = row1[0], c11 = row1[fu ? 1 : 0];
        int iu = 256 - fu, iv = 256 - fv;
        int w00 = (iu * iv) >> 8, w10 = (fu * iv) >> 8;
        int w01 = (iu * fv) >> 8, w11 = (fu * fv) >> 8;
        int r = (((c00 >> 11) & 0x1F) * w00 + ((c10 >> 11) & 0x1F) * w10 +
                 ((c01 >> 11) & 0x1F) * w01 + ((c11 >> 11) & 0x1F) * w11) >> 8;
        int g = (((c00 >> 5) & 0x3F) * w00 + ((c10 >> 5) & 0x3F) * w10 +
                 ((c01 >> 5) & 0x3F) * w01 + ((c11 >> 5) & 0x3F) * w11) >> 8;
        int b = ((c00 & 0x1F) * w00 + (c10 & 0x1F) * w10 +
                 (c01 & 0x1F) * w01 + (c11 & 0x1F) * w11) >> 8;
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        *dst++ = (uint16_t)(_lutR[r] | _lutG[g] | _lutB[b]);
        sx += dsx;
        sy += dsy;
      }
    } else if (!blendTex) {
      for (int x = xL; x <= xR; x++) {
        int tu = sx >> 16, tv = sy >> 16;
        // Clamp rather than test: rounding can put the very edge pixel one
        // texel outside, and a clamp is two operations against a branch.
        if (tu < 0) tu = 0; else if (tu >= ts) tu = ts - 1;
        if (tv < 0) tv = 0; else if (tv >= ts) tv = ts - 1;
        uint16_t c = tex[tv * ts + tu];
        *dst++ = (uint16_t)(_lutR[(c >> 11) & 0x1F] |
                            _lutG[(c >> 5) & 0x3F] |
                            _lutB[c & 0x1F]);
        sx += dsx;
        sy += dsy;
      }
    } else {
      // Cross-fade path, used only by a sonar ping and only for a second.
      const uint16_t k = blendK, ik = (uint16_t)(255 - blendK);
      for (int x = xL; x <= xR; x++) {
        int tu = sx >> 16, tv = sy >> 16;
        if (tu < 0) tu = 0; else if (tu >= ts) tu = ts - 1;
        if (tv < 0) tv = 0; else if (tv >= ts) tv = ts - 1;
        uint16_t a = tex[tv * ts + tu];
        uint16_t b = blendTex[tv * ts + tu];
        uint16_t r = ((((a >> 11) & 0x1F) * ik) + (((b >> 11) & 0x1F) * k)) >> 8;
        uint16_t g = ((((a >> 5) & 0x3F) * ik) + (((b >> 5) & 0x3F) * k)) >> 8;
        uint16_t bl = (((a & 0x1F) * ik) + ((b & 0x1F) * k)) >> 8;
        *dst++ = (uint16_t)(_lutR[r & 0x1F] | _lutG[g & 0x3F] | _lutB[bl & 0x1F]);
        sx += dsx;
        sy += dsy;
      }
    }

    // Half-resolution pass: copy the row we just drew onto the one we skipped.
    if (step == 2 && y + 1 <= yBot) {
      memcpy(_buf + (size_t)(y + 1) * _vw + xL,
             _buf + (size_t)y * _vw + xL,
             (size_t)(xR - xL + 1) * sizeof(uint16_t));
    }
  }
}

// Draw every shell block as a SOLID CUBE, back to front.
//
// A block is a cube of side (1-2g), so the honest thing to do is draw its
// camera-facing faces — all three of them — and let ordering sort out what is
// hidden. That removes every special case the old face-by-face walk needed:
// no separate "side wall" quads, no rule about which sides to skip at a cube
// edge, no two-pass ordering trick, and the slivers you see through the gaps
// are the real textured faces of the neighbouring blocks rather than a flat
// approximation of them.
//
// Ordering is the classic voxel painter's algorithm: walk each cube axis in
// the direction that goes away-from-camera first. For equal axis-aligned cubes
// on a lattice that is exactly back-to-front, and a convex solid's own
// camera-facing faces never overlap each other, so no sorting is needed at all.
//
// The cost is overdraw: two of a block's three drawn faces end up almost
// entirely covered by its neighbours. See PLAN.md for the measured figure.
void CubeView::paintBlocks() {
  const int n = _cube->n();
  const float g = BLOCK_GAP;
  const float s = 1.0f - 2.0f * g;

  // Which cube axes run toward the camera, from the rotation's bottom row.
  const int stepX = (_rot[6] > 0.0f) ? 1 : -1;
  const int stepY = (_rot[7] > 0.0f) ? 1 : -1;
  const int stepZ = (_rot[8] > 0.0f) ? 1 : -1;
  const int fromX = (stepX > 0) ? 0 : n - 1, toX = (stepX > 0) ? n : -1;
  const int fromY = (stepY > 0) ? 0 : n - 1, toY = (stepY > 0) ? n : -1;
  const int fromZ = (stepZ > 0) ? 0 : n - 1, toZ = (stepZ > 0) ? n : -1;

  // One mip per face; every block face on a face is the same size on screen.
  uint8_t lvl[CUBE_FACES];
  int     ts[CUBE_FACES];
  for (uint8_t f = 0; f < CUBE_FACES; f++) {
    if (!_fp[f].vis) continue;
    const float side = sqrtf(_fp[f].ux * _fp[f].ux + _fp[f].uy * _fp[f].uy) * s;
    lvl[f] = Skin::levelFor((int)(side + 0.5f));
    ts[f]  = Skin::levelSize(lvl[f]);
  }

  for (int z = fromZ; z != toZ; z += stepZ) {
    for (int y = fromY; y != toY; y += stepY) {
      for (int x = fromX; x != toX; x += stepX) {
        const int ci = _cube->at(x, y, z);
        if (ci < 0) continue;                  // interior: not a block
        const uint16_t c = (uint16_t)ci;

        uint8_t key;
        switch (_cube->stateOf(c)) {
          case CS_FLAGGED:  key = TK_FLAGGED; break;
          case CS_QUESTION: key = TK_UNREVEALED; break;
          case CS_REVEALED: key = _cube->isMine(c) ? (uint8_t)TK_MINE
                                                   : Skin::keyForRevealed(_cube->adj(c)); break;
          default:          key = TK_UNREVEALED; break;
        }

        // A sonar ping shows the mine under a still-hidden block and fades it
        // back to hidden over a second.
        bool ping = (_sonarFade > 0.0f && _cube->sonarShows(c) &&
                     _cube->stateOf(c) != CS_REVEALED);
        const uint8_t blendK = ping ? (uint8_t)((1.0f - _sonarFade) * 255.0f) : 0;

        const int co[3] = { x, y, z };
        for (uint8_t f = 0; f < CUBE_FACES; f++) {
          const FaceProj &p = _fp[f];
          if (!p.vis) continue;

          // Where this block sits in the face's grid, and how far its face for
          // this direction lies inside the nominal outer plane.
          const int i = co[p.aU], j = co[p.aV], cc = co[p.aN];
          const float t = (p.sN > 0) ? ((float)cc + 1.0f - g - (float)n)
                                     : -((float)cc + g);

          const float ox = p.ox + ((float)i + g) * p.ux + ((float)j + g) * p.vx + t * p.snx;
          const float oy = p.oy + ((float)i + g) * p.uy + ((float)j + g) * p.vy + t * p.sny;

          buildShadeLut(p.shade);
          const uint16_t *tex = _skin->level(key, lvl[f]);
          const uint16_t *blend = ping ? tex : nullptr;
          paintTile(ox, oy, s * p.ux, s * p.uy, s * p.vx, s * p.vy, p,
                    ping ? _skin->level(TK_MINE, lvl[f]) : tex,
                    ts[f], blend, blendK);
        }
      }
    }
  }
}

// Where a block sits in a face's (i,j) grid. Each of U and V has exactly one
// non-zero component (guaranteed by CUBE_BASIS and asserted by check_cube.py),
// so this is just picking out the right coordinate.
static void faceIJ(uint8_t f, uint8_t x, uint8_t y, uint8_t z, int &i, int &j) {
  const FaceBasis &b = CUBE_BASIS[f];
  const uint8_t c[3] = { x, y, z };
  i = b.ux ? c[0] : (b.uy ? c[1] : c[2]);
  j = b.vx ? c[0] : (b.vy ? c[1] : c[2]);
}

void CubeView::outlineCell(uint16_t cell) {
  if (!_cube || !_cube->ready()) return;
  uint8_t bx8, by8, bz8;
  _cube->posOf(cell, bx8, by8, bz8);
  // A block can show on up to three faces; outline it on the first visible one.
  uint8_t mask = _cube->faceMask(cell);
  uint8_t f = CUBE_FACES;
  for (uint8_t k = 0; k < CUBE_FACES; k++)
    if ((mask & (1 << k)) && _fp[k].vis) { f = k; break; }
  if (f >= CUBE_FACES) return;
  int i, j;
  faceIJ(f, bx8, by8, bz8, i, j);

  const FaceProj &p = _fp[f];
  float x = p.ox + i * p.ux + j * p.vx + p.hx;
  float y = p.oy + i * p.uy + j * p.vy + p.hy;
  int ax = (int)x, ay = (int)y;
  int bx = (int)(x + p.ux), by = (int)(y + p.uy);
  int cx = (int)(x + p.ux + p.vx), cy = (int)(y + p.uy + p.vy);
  int dx = (int)(x + p.vx), dy = (int)(y + p.vy);
  _spr->drawLine(ax, ay, bx, by, _hlCol);
  _spr->drawLine(bx, by, cx, cy, _hlCol);
  _spr->drawLine(cx, cy, dx, dy, _hlCol);
  _spr->drawLine(dx, dy, ax, ay, _hlCol);
}

void CubeView::render(uint16_t bg) {
  if (!_spr || !_cube || !_cube->ready() || !_skin || !_skin->loaded()) return;
  project();
  _sonarFade = _cube->sonarFade();       // sampled once so a frame is coherent
  _spr->fillSprite(bg);
  _lutShade = -1;                        // force a LUT rebuild for this frame

  // No face ordering to do: blocks are drawn as solids in voxel order, and a
  // convex solid's own camera-facing faces never overlap each other.
  paintBlocks();

  if (_hl >= 0 && _hl < (int)_cube->cells()) outlineCell((uint16_t)_hl);
  _spr->pushSprite(_vx, _vy);
}

int CubeView::pick(int panelX, int panelY) const {
  if (!_cube || !_cube->ready()) return -1;
  const float x = (float)(panelX - _vx), y = (float)(panelY - _vy);
  if (x < 0 || y < 0 || x >= _vw || y >= _vh) return -1;
  const int n = _cube->n();

  // Every block face on a given cube face is coplanar, so there is a single
  // plane to invert against per face.
  //
  // Of the candidates that produces, take the one whose cap CENTRE is nearest
  // the tap. Trying the faces in order and returning the first in-range hit is
  // not good enough: near a cube edge or corner two or three faces all produce
  // a plausible answer, and whichever happened to be tested first won — which
  // is why some corner blocks could not be hit at all. Nearest-centre has no
  // such bias and needs no tolerance juggling.
  const float slack = 0.5f;
  int best = -1;
  float bestD2 = 1e30f;
  for (uint8_t f = 0; f < CUBE_FACES; f++) {
    const FaceProj &p = _fp[f];
    if (!p.vis || p.idet == 0.0f) continue;
    const float dx = x - p.ox - p.hx, dy = y - p.oy - p.hy;
    const float u = ( p.vy * dx - p.vx * dy) * p.idet;
    const float v = (-p.uy * dx + p.ux * dy) * p.idet;
    // A little slack so a tap landing in a groove still finds its block.
    if (u < -slack || v < -slack || u >= n + slack || v >= n + slack) continue;
    int i = (int)floorf(u), j = (int)floorf(v);
    if (i < 0) i = 0; else if (i >= n) i = n - 1;
    if (j < 0) j = 0; else if (j >= n) j = n - 1;
    int ci = _cube->cellAtFace(f, (uint8_t)i, (uint8_t)j);
    if (ci < 0) continue;
    const float ccx = p.ox + (i + 0.5f) * p.ux + (j + 0.5f) * p.vx + p.hx;
    const float ccy = p.oy + (i + 0.5f) * p.uy + (j + 0.5f) * p.vy + p.hy;
    const float ex = x - ccx, ey = y - ccy;
    const float d2 = ex * ex + ey * ey;
    if (d2 < bestD2) { bestD2 = d2; best = ci; }
  }
  return best;
}
