#pragma once
#ifndef render3d_h
#define render3d_h

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "cube.h"
#include "skin.h"

// Textured 3D cube renderer.
//
// ORTHOGRAPHIC, not perspective — that is the decision the whole thing rests
// on. Under an orthographic projection every cube face lands on screen as an
// exact parallelogram, so mapping a texture across a tile is affine: no
// perspective divide, no per-pixel division at all. The inner loop is two
// fixed-point adds, a texture fetch and three small LUT lookups for shading.
// (The real game uses a mild perspective; at these tile sizes the difference is
// a few pixels of taper and it is not worth a general-quad rasteriser.)
//
// Cells are drawn as EXTRUDED BLOCKS, not flat squares — that is what gives the
// cube its chunky, stepped silhouette and what makes an edge or corner of the
// cube read as solid. Every cell is a cube of its own, set into the surface;
// a revealed one sits flush, so cleared areas become pits with the neighbouring
// blocks' side walls showing into them.
//
// Everything composites into one off-screen PSRAM sprite and is pushed in a
// single call. Nothing here ever draws straight to the panel — a direct fill
// shows up as a flash.

class CubeView {
public:
  ~CubeView() { release(); }

  // Each block is a CUBE of side (1 - 2*BLOCK_GAP), centred in its cell.
  //
  // That single statement settles both things this geometry has to get right,
  // and it is worth stating because approximating it does not work:
  //
  //   * every visible face is (1-2g) square, the same on every block, because
  //     they are all the same cube;
  //   * an edge block's two faces MEET EXACTLY. Its +Z face lies at z = n-g
  //     and runs out to x = n-g; its +X face lies at x = n-g and runs out to
  //     z = n-g. Same edge, no gap, no overlap, sharp corner.
  //
  // The consequence that is easy to get backwards: the face plane sits BLOCK_GAP
  // *inside* the cube's nominal surface, not proud of it. Pushing the tops
  // outward instead and stretching the boundary ones to meet was the earlier
  // approach, and it is what made boundary blocks a different size from the
  // rest.
  static constexpr float BLOCK_GAP = 0.07f;

  // Allocate the compositing sprite for a viewport at (x,y) size w*h on the
  // panel. Returns false if the sprite would not allocate.
  bool begin(TFT_eSPI *tft, int x, int y, int w, int h);
  void release();
  bool ok() const { return _spr != nullptr; }

  void attach(Cube *c, Skin *s) { _cube = c; _skin = s; }

  // Camera orientation is a full 3x3 rotation, accumulated from the drag —
  // NOT Euler angles. Euler angles need the pitch clamped somewhere short of
  // straight up or the cube flips and the yaw axis inverts, and that clamp is
  // a wall you can feel when dragging. Composing each drag increment in VIEW
  // space instead means the cube tumbles freely in any direction forever, with
  // no boundary and no gimbal lock.
  void  resetCamera();
  void  orbit(float dyaw, float dpitch);
  // Set the orientation from Euler angles. Only used to place the default
  // view; the drag never goes through it.
  void  setCamera(float yaw, float pitch);
  void  setZoom(float z);
  float zoom()  const { return _zoom; }
  static float zoomMin() { return 0.75f; }
  static float zoomMax() { return 4.0f; }

  // Half-resolution pass: renders every other scanline and duplicates it.
  // Meant for while a drag or pinch is in flight, where the cube is moving and
  // the detail is not missed; the full-resolution frame lands when it settles.
  void setFast(bool f) { _fast = f; }
  bool fast() const { return _fast; }

  // Cell to outline in the accent colour (-1 for none).
  void setHighlight(int cell, uint16_t colour) { _hl = cell; _hlCol = colour; }

  // Paint the whole cube into the sprite and push it. `bg` fills the space
  // around the cube.
  void render(uint16_t bg);

  // Panel coordinates -> cell index, or -1 if the tap missed the cube. Uses
  // the projection from the last render(), so call it after one.
  // `faceOut`, if given, receives WHICH cube face the block was hit on (an
  // edge block has two, a corner three), or CUBE_FACES on a miss.
  int pick(int panelX, int panelY, uint8_t *faceOut = nullptr) const;

  // Fit the cube to the viewport at zoom 1. Recomputed on attach/resize.
  void refit();

private:
  struct FaceProj {
    bool    vis;
    float   ox, oy;        // screen position of the face's (0,0) corner
    float   ux, uy;        // one cell step along i
    float   vx, vy;        // one cell step along j
    float   hx, hy;        // screen offset of the outermost block's face plane
    float   snx, sny;      // screen offset of ONE CELL along the outward normal
    uint8_t aU, aV, aN;    // which cube axis U, V and N run along
    int8_t  sN;            // +1 if N points along +axis, -1 if -axis
    float   idet;          // 1 / (ux*vy - uy*vx), for picking
    float   shade;         // 0..1 Lambert term for the cap
    // This face's shading tables, built once per frame. They live here rather
    // than as one shared set because the draw loop is BLOCK-major: it visits
    // f=0,1,2 for every block, so a single set with a "did the shade change"
    // cache would miss on every single face and rebuild ~500 times a frame.
    const uint16_t *lutR, *lutG, *lutB;
    // How the square texture is laid onto this face's tiles. Each is one of
    // AX_U / AX_U_INV / AX_V / AX_V_INV. Fixed to the face by geometry.
    int8_t  txSrc, tySrc;
  };

  // Texture-axis sources, as signed codes so a negative means "run backwards".
  static const int8_t AX_U     =  1;
  static const int8_t AX_U_INV = -1;
  static const int8_t AX_V     =  2;
  static const int8_t AX_V_INV = -2;

  // Re-orthonormalise the rotation. Composing thousands of small increments
  // accumulates round-off that would slowly shear the cube.
  void renormalise();
  void project();
  // Build face `f`'s shading tables and point its FaceProj at them. Called
  // once per visible face per frame, never from the inner loop.
  void buildShadeLut(uint8_t f, float shade);
  // Draw every shell block as a solid cube, back to front.
  void paintBlocks();
  // The background-coloured solid filling the hollow centre. Drawn between the
  // two block passes — see paintBlocks(). Not optional: without it you see the
  // far side of the cube through the gaps between the near blocks.
  void paintInner(uint16_t bg);
  // Flat-shaded parallelogram, for the inner cube.
  void fillPara(float px, float py, float ax, float ay, float bx, float by,
                uint16_t colour);
  // Textured parallelogram at (px,py) spanned by (ax,ay) and (bx,by). When
  // `blendTex` is given the two textures are cross-faded by `blendK` (0 = all
  // `tex`, 255 = all `blendTex`), which is how a sonar ping fades away.
  void paintTile(float px, float py, float ax, float ay, float bx, float by,
                 const FaceProj &fp, const uint16_t *tex, int ts,
                 const uint16_t *blendTex, uint8_t blendK);
  void outlineCell(uint16_t cell);
  static void chooseOrientation(FaceProj &p, const FaceBasis &b);

  TFT_eSPI   *_tft  = nullptr;
  TFT_eSprite *_spr = nullptr;
  uint16_t   *_buf  = nullptr;     // sprite pixels, written directly
  int _vx = 0, _vy = 0, _vw = 0, _vh = 0;

  Cube *_cube = nullptr;
  Skin *_skin = nullptr;

  // Row-major: a cube-space vector v maps to view space as _rot * v.
  float _rot[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
  float _zoom = 1.0f;
  float _cell = 10.0f;             // projected size of one cell, in pixels
  float _fit  = 10.0f;             // _cell at zoom 1
  bool  _fast = false;
  int   _hl = -1;
  uint16_t _hlCol = 0xFFFF;
  float _sonarFade = 0.0f;         // sampled once per frame
  uint16_t _bg = 0;                // this frame's background, for the inner cube

  FaceProj _fp[CUBE_FACES];
  // Shading tables, one set per face. Each entry is that channel's PRE-SHIFTED
  // contribution to a BYTE-SWAPPED RGB565 word, so `lutR[r]|lutG[g]|lutB[b]`
  // is already in the order a TFT_eSprite buffer wants — see buildShadeLut().
  // 1536 bytes for all six, against rebuilding them hundreds of times a frame.
  uint16_t _lutR[CUBE_FACES][32], _lutG[CUBE_FACES][64], _lutB[CUBE_FACES][32];
};

#endif // render3d_h
