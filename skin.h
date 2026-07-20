#pragma once
#ifndef skin_h
#define skin_h

#include <Arduino.h>

// Tile textures.
//
// A skin is the 12 tile faces the cube is drawn with. They come from the
// user-created skin store at https://mastermine.app/skins, either downloaded on
// the device (skinstore.*) or sideloaded by sd_prep/fetch_skins.py — both write
// the same files, so there is one code path here:
//
//     /mastermine/skins/<id>/meta.json
//     /mastermine/skins/<id>/<key>.tex
//
// .tex is a 16-byte header plus 64x64 RGB565, little-endian to match a
// uint16_t in ESP32 memory so it loads with no per-pixel swap. The store's
// originals are 512x512 JPEGs; 64x64 is already more than a tile ever gets on
// a 320 px panel.
//
// Plenty of published skins are incomplete — one in the top 40 ships only 4 of
// the 12 — so any key that is missing is filled in from the built-in set rather
// than left blank.

// 128, not 64. A skin tile can cover 60-100 px when the cube is zoomed in, and
// a 64 px source visibly mushes the thin strokes many published skins use for
// their digits. 128 costs 32 KB per texture on the card and ~520 KB of PSRAM
// for a whole skin's mip chain, both of which are cheap here.
static const uint16_t TEX_SIZE   = 128;  // must match TEX_SIZE in fetch_skins.py
static const uint8_t  TEX_LEVELS = 5;    // mips: 128, 64, 32, 16, 8
// Cards written before the resolution change hold 64 px textures; those still
// load, into level 1, and simply are not as sharp.
static const uint16_t TEX_SIZE_LEGACY = 64;

// Texture keys, in file-name order. Note "redSpot" is the store's name for the
// MINE face — confirmed by eye against skin 86 "retro clean".
// Built-in skins, drawn procedurally so nothing third-party ships in flash.
//   CLASSIC  the grey Windows-Minesweeper look
//   GOLD     the premium gold set from the reference game — palette measured
//            off the reference screenshots in example/, not eyeballed
enum SkinVariant : uint8_t {
  SKIN_CLEAN   = 0,   // the reference game's own default: white blocks, white numerals
  SKIN_GOLD    = 1,   // its premium set
  SKIN_CLASSIC = 2,   // grey Windows-Minesweeper
  SKIN_VARIANTS = 3,
};
extern const char *const SKIN_VARIANT_NAMES[SKIN_VARIANTS];

enum TexKey : uint8_t {
  TK_1 = 0, TK_2, TK_3, TK_4, TK_5, TK_6, TK_7, TK_8,
  TK_UNREVEALED = 8,
  TK_REVEALED   = 9,
  TK_FLAGGED    = 10,
  TK_MINE       = 11,
  TK_COUNT      = 12,
};
extern const char *const TEX_KEY_NAMES[TK_COUNT];

class Skin {
public:
  Skin() {}
  ~Skin() { release(); }

  // Build every face procedurally. Nothing third-party ships in flash, so the
  // game is playable on a card with no skins on it.
  bool loadBuiltin(uint8_t variant = SKIN_CLEAN);
  uint8_t variant() const { return _variant; }
  // Load skin <id> from the SD card, falling back to the built-in face for any
  // texture the skin does not define. Returns false only if nothing could be
  // allocated; a skin with missing pieces still loads.
  bool loadFromSd(uint16_t id);
  void release();

  bool        loaded() const { return _pix[0][0] != nullptr; }
  uint16_t    id()     const { return _id; }          // 0 = built-in
  const char *name()   const { return _name; }
  uint8_t     fromFile() const { return _fromFile; }  // how many keys came off the card

  // Pixels for one face at one mip level, row-major, (TEX_SIZE >> lvl) square.
  const uint16_t *level(uint8_t key, uint8_t lvl) const {
    if (key >= TK_COUNT || lvl >= TEX_LEVELS) return nullptr;
    return _pix[key][lvl];
  }
  static uint16_t levelSize(uint8_t lvl) { return (uint16_t)(TEX_SIZE >> lvl); }

  // Smallest mip that is still at least as big as the tile on screen.
  // Going smaller would visibly soften the art; going bigger wastes texture
  // bandwidth and aliases, because thin skin details strobe as the cube turns.
  static uint8_t levelFor(int screenPx) {
    for (int l = TEX_LEVELS - 1; l >= 0; l--)
      if ((int)levelSize((uint8_t)l) >= screenPx) return (uint8_t)l;
    return 0;                                    // bigger than the top mip
  }

  // Which texture a cell should wear. Kept here so the renderer never has to
  // know the key numbering.
  static uint8_t keyForRevealed(uint8_t adjCount) {
    return (adjCount >= 1 && adjCount <= 8) ? (uint8_t)(adjCount - 1) : (uint8_t)TK_REVEALED;
  }

  // Average colour of a face, used for the cube's silhouette edge and for the
  // half-resolution pass while the cube is being dragged.
  uint16_t averageOf(uint8_t key) const { return (key < TK_COUNT) ? _avg[key] : 0; }

private:
  bool allocKey(uint8_t key);
  // Fill the mip chain from level `from` downwards; `from` > 0 also doubles the
  // loaded level up to level 0, which is how a legacy 64 px texture loads.
  void buildMips(uint8_t key, uint8_t from);
  void genBuiltin(uint8_t key);
  // Returns the mip level the file filled, or -1 if it could not be read.
  int  readTex(const char *path, uint8_t key);

  uint16_t *_pix[TK_COUNT][TEX_LEVELS] = {};
  uint16_t  _avg[TK_COUNT] = {};
  uint16_t  _id = 0;
  uint8_t   _fromFile = 0;
  uint8_t   _variant = SKIN_CLEAN;     // which built-in fills any missing key
  char      _name[24] = "Clean";
};

// Skins present on the card, for the Settings picker. Scans SKIN_DIR for
// numbered subdirectories and reads the name out of each meta.json.
struct SkinEntry {
  uint16_t id;
  char     name[24];
};
int skinScanSd(SkinEntry *out, int maxN);

#endif // skin_h
