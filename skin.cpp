#include "skin.h"
#include "configs.h"
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

const char *const SKIN_VARIANT_NAMES[SKIN_VARIANTS] = { "Clean", "Gold", "Classic" };

const char *const TEX_KEY_NAMES[TK_COUNT] = {
  "tile1", "tile2", "tile3", "tile4", "tile5", "tile6", "tile7", "tile8",
  "tileUnrevealed", "tileRevealed", "tileFlagged", "redSpot",
};

static const char TEX_MAGIC[4] = { 'M', 'M', 'T', 'X' };

static void *skinAlloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(bytes);
  return p;
}

static inline uint16_t rgb565(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct RGB8 { uint8_t r, g, b; };

// 5x7 digits 1-8. Drawn with round pen strokes rather than square pixels (see
// drawDigit), which turns this coarse grid into something much closer to the
// rounded numerals the reference game uses, and the mip chain anti-aliases the
// rest away at the sizes a tile is actually seen.
static const uint8_t DIGITS[8][7] = {
  { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   // 1
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   // 2
  { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   // 3
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   // 4
  { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   // 5
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   // 6
  { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   // 7
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   // 8
};

// Classic Minesweeper number colours, for the Classic set only.
static const RGB8 CLASSIC_NUM[8] = {
  {   0,   0, 255 }, {   0, 128,   0 }, { 255,   0,   0 }, {   0,   0, 128 },
  { 128,   0,   0 }, {   0, 128, 128 }, { 128,   0, 128 }, { 100, 100, 100 },
};

// The premium GOLD set. Palette measured off the reference screenshots in
// example/: the lit gold face samples around rgb(254,253,129) and the shaded
// one around rgb(112,80,33). Digits 4-8 appear in none of those shots, so they
// continue the ramp sensibly rather than pretending to be sourced.
static const RGB8 GOLD_TOP = { 255, 246, 150 };
static const RGB8 GOLD_BOT = { 198, 142,  38 };
static const RGB8 NUM_TOP[8] = {
  { 150, 224, 236 }, { 252, 252, 248 }, { 206, 196, 248 }, { 250, 206, 120 },
  { 250, 168, 156 }, { 176, 236, 190 }, { 198, 208, 228 }, { 206, 206, 210 },
};
static const RGB8 NUM_BOT[8] = {
  {  74, 150, 165 }, { 198, 198, 190 }, { 126, 112, 200 }, { 206, 148,  46 },
  { 206,  96,  84 }, { 104, 180, 126 }, { 126, 142, 172 }, { 136, 136, 144 },
};
static const RGB8 NUM_INK[8] = {
  {  20,  40,  48 }, {  40,  40,  44 }, {  32,  26,  64 }, {  60,  38,   8 },
  {  70,  18,  14 }, {  16,  56,  30 }, {  28,  36,  54 }, {  36,  36,  40 },
};

// The CLEAN set — the reference game's default theme, and ours.
//
// Hues measured off the skin swatch on its shop screen (example/): 1 cyan,
// 2 amber, 3 indigo, 4 red-orange, with WHITE numerals and a rounded inner
// panel a shade lighter than the tile edge. That swatch is a small, dim JPEG
// thumbnail, so the exact values here are lifted from the crisp icon-style
// render instead and only the hues cross-checked against it.
// 5-8 appear in neither and continue the ramp.
static const RGB8 CLEAN_FACE[8] = {
  {  95, 192, 226 }, { 240, 172,  58 }, { 104,  88, 190 }, { 228,  98,  68 },
  {  60, 180, 165 }, { 232, 110, 160 }, {  96, 116, 140 }, { 120, 120, 128 },
};
static const RGB8 CLEAN_EDGE[8] = {
  {  78, 168, 203 }, { 214, 148,  44 }, {  86,  72, 166 }, { 202,  80,  54 },
  {  48, 158, 145 }, { 206,  92, 140 }, {  80,  98, 120 }, { 102, 102, 110 },
};
static const RGB8 CLEAN_WHITE      = { 246, 246, 248 };
static const RGB8 CLEAN_WHITE_EDGE = { 224, 224, 230 };
static const RGB8 CLEAN_DARK       = {  42,  42,  45 };
static const RGB8 CLEAN_DARK_EDGE  = {  28,  28,  31 };
static const RGB8 CLEAN_FLAG       = { 222,  58,  52 };
static const RGB8 CLEAN_INK        = { 255, 255, 255 };

bool Skin::allocKey(uint8_t key) {
  for (uint8_t l = 0; l < TEX_LEVELS; l++) {
    if (_pix[key][l]) continue;
    uint16_t s = levelSize(l);
    _pix[key][l] = (uint16_t *)skinAlloc((size_t)s * s * sizeof(uint16_t));
    if (!_pix[key][l]) return false;
  }
  return true;
}

void Skin::release() {
  for (uint8_t k = 0; k < TK_COUNT; k++)
    for (uint8_t l = 0; l < TEX_LEVELS; l++)
      if (_pix[k][l]) { free(_pix[k][l]); _pix[k][l] = nullptr; }
  _id = 0;
  _fromFile = 0;
  _variant = SKIN_CLEAN;
  strcpy(_name, "Clean");
}

// Box-average each level down from the one above, starting at `from`, and
// record the face's mean colour on the way through. Starting below 0 is how a
// legacy 64 px texture is loaded: it fills level 1 and level 0 is doubled up
// from it so the renderer still has a full chain.
void Skin::buildMips(uint8_t key, uint8_t from) {
  if (from > 0) {
    // Upscale the one level above the loaded one, by pixel doubling.
    const uint16_t *src = _pix[key][from];
    uint16_t       *dst = _pix[key][from - 1];
    uint16_t ss = levelSize(from), ds = levelSize(from - 1);
    for (uint16_t y = 0; y < ds; y++)
      for (uint16_t x = 0; x < ds; x++)
        dst[y * ds + x] = src[(y / 2) * ss + (x / 2)];
    from--;
  }
  for (uint8_t l = from + 1; l < TEX_LEVELS; l++) {
    const uint16_t *src = _pix[key][l - 1];
    uint16_t       *dst = _pix[key][l];
    uint16_t ss = levelSize(l - 1), ds = levelSize(l);
    for (uint16_t y = 0; y < ds; y++) {
      for (uint16_t x = 0; x < ds; x++) {
        uint16_t a = src[(y * 2) * ss + x * 2];
        uint16_t b = src[(y * 2) * ss + x * 2 + 1];
        uint16_t c = src[(y * 2 + 1) * ss + x * 2];
        uint16_t d = src[(y * 2 + 1) * ss + x * 2 + 1];
        uint16_t r = (((a >> 11) & 0x1F) + ((b >> 11) & 0x1F) + ((c >> 11) & 0x1F) + ((d >> 11) & 0x1F)) >> 2;
        uint16_t g = (((a >> 5) & 0x3F) + ((b >> 5) & 0x3F) + ((c >> 5) & 0x3F) + ((d >> 5) & 0x3F)) >> 2;
        uint16_t bl = ((a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F)) >> 2;
        dst[y * ds + x] = (uint16_t)((r << 11) | (g << 5) | bl);
      }
    }
  }
  const uint16_t *s = _pix[key][TEX_LEVELS - 1];
  uint16_t n = levelSize(TEX_LEVELS - 1);
  uint32_t r = 0, g = 0, b = 0;
  for (int i = 0; i < n * n; i++) {
    r += (s[i] >> 11) & 0x1F;
    g += (s[i] >> 5) & 0x3F;
    b += s[i] & 0x1F;
  }
  int cnt = n * n;
  _avg[key] = (uint16_t)(((r / cnt) << 11) | ((g / cnt) << 5) | (b / cnt));
}

// A digit stamped from the 5x7 grid with SQUARE cells that touch, so strokes
// come out solid and unbroken. A round pen was tried and looked worse: at the
// size a tile is actually seen the overlapping discs read as a ragged blob
// rather than a numeral. The mip chain does the smoothing instead.
static void drawDigit(uint16_t *p, int S, uint8_t digit, RGB8 ink) {
  if (!digit || digit > 8) return;
  const uint8_t *d = DIGITS[digit - 1];
  const uint16_t col = rgb565(ink.r, ink.g, ink.b);
  const int sc = (int)(S * 0.125f + 0.5f);     // one grid step, in pixels
  const int w = 5 * sc, h = 7 * sc;
  const int ox = (S - w) / 2, oy = (S - h) / 2;
  for (int ry = 0; ry < 7; ry++)
    for (int rx = 0; rx < 5; rx++) {
      if (!(d[ry] & (1 << (4 - rx)))) continue;
      for (int yy = 0; yy < sc; yy++)
        for (int xx = 0; xx < sc; xx++) {
          int px = ox + rx * sc + xx, py = oy + ry * sc + yy;
          if (px >= 0 && px < S && py >= 0 && py < S) p[py * S + px] = col;
        }
    }
}

// A flat tile with a rounded inner panel, which is how the Clean set reads:
// a slightly darker edge with a lighter rounded square sitting on it.
static void panelTile(uint16_t *p, RGB8 edge, RGB8 face, uint8_t digit, RGB8 ink) {
  const int S = TEX_SIZE;
  const uint16_t ec = rgb565(edge.r, edge.g, edge.b);
  const uint16_t fc = rgb565(face.r, face.g, face.b);
  const int inset = (int)(S * 0.10f);
  const int rad   = (int)(S * 0.18f);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      bool inPanel = (x >= inset && x < S - inset && y >= inset && y < S - inset);
      if (inPanel) {
        // Round the panel's corners.
        int dx = 0, dy = 0;
        if (x < inset + rad)          dx = inset + rad - x;
        else if (x >= S - inset - rad) dx = x - (S - inset - rad - 1);
        if (y < inset + rad)          dy = inset + rad - y;
        else if (y >= S - inset - rad) dy = y - (S - inset - rad - 1);
        if (dx && dy && dx * dx + dy * dy > rad * rad) inPanel = false;
      }
      p[y * S + x] = inPanel ? fc : ec;
    }
  drawDigit(p, S, digit, ink);
}

// A bevelled tile with a vertical gradient, used by the Gold set.
static void bevelTile(uint16_t *p, RGB8 top, RGB8 bot, uint8_t digit, RGB8 ink) {
  const int S = TEX_SIZE;
  const int bev = (int)(S * 0.0625f);
  for (int y = 0; y < S; y++) {
    int t = y * 255 / (S - 1);
    int r = top.r + (bot.r - top.r) * t / 255;
    int g = top.g + (bot.g - top.g) * t / 255;
    int b = top.b + (bot.b - top.b) * t / 255;
    for (int x = 0; x < S; x++) {
      // A soft diagonal sheen, which is what makes the gold read as metal
      // rather than flat paint.
      int sh = 22 - (x + y) * 44 / (2 * S);
      int rr = r + sh, gg = g + sh, bb = b + sh;
      int d = min(min(x, y), min(S - 1 - x, S - 1 - y));
      if (d < bev) {
        bool tl = (x <= y) ? (x < S - 1 - y) : (y < S - 1 - x);
        int k = tl ? 46 : -52;
        rr += k; gg += k; bb += k;
      }
      p[y * S + x] = rgb565(rr, gg, bb);
    }
  }
  drawDigit(p, S, digit, ink);
}

// A pennant, as the reference draws it: no pole, just the flag.
static void drawFlag(uint16_t *p, RGB8 col) {
  const int S = TEX_SIZE;
  const uint16_t c = rgb565(col.r, col.g, col.b);
  const int x0 = (int)(S * 0.30f), y0 = (int)(S * 0.30f);
  const int hh = (int)(S * 0.34f), ww = (int)(S * 0.40f);
  for (int y = 0; y < hh; y++) {
    int half = (y < hh / 2) ? y : (hh - y);
    int w = (int)(ww * (0.35f + 0.65f * (float)half / (hh * 0.5f)));
    for (int x = 0; x < w; x++) {
      int px = x0 + x, py = y0 + y;
      if (px >= 0 && px < S && py >= 0 && py < S) p[py * S + px] = c;
    }
  }
}

static void drawMine(uint16_t *p, RGB8 body, RGB8 spot) {
  const int S = TEX_SIZE;
  const uint16_t bc = rgb565(body.r, body.g, body.b);
  const uint16_t sc = rgb565(spot.r, spot.g, spot.b);
  const int cx = S / 2, cy = S / 2;
  const int r = (int)(S * 0.27f), sp = (int)(S * 0.41f);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= r * r) p[y * S + x] = bc;
    }
  const int th = (int)(S * 0.05f);
  for (int i = -sp; i <= sp; i++)
    for (int t = -th; t <= th; t++) {
      if (cy + i >= 0 && cy + i < S && cx + t >= 0 && cx + t < S) p[(cy + i) * S + cx + t] = bc;
      if (cx + i >= 0 && cx + i < S && cy + t >= 0 && cy + t < S) p[(cy + t) * S + cx + i] = bc;
    }
  const int gr = (int)(S * 0.07f);
  for (int y = -gr; y <= gr; y++)
    for (int x = -gr; x <= gr; x++)
      if (x * x + y * y <= gr * gr) {
        int px = cx - (int)(S * 0.09f) + x, py = cy - (int)(S * 0.09f) + y;
        if (px >= 0 && px < S && py >= 0 && py < S) p[py * S + px] = sc;
      }
}

static void genCleanInto(uint16_t *p, uint8_t key) {
  if (key <= TK_8) {
    panelTile(p, CLEAN_EDGE[key], CLEAN_FACE[key], (uint8_t)(key + 1), CLEAN_INK);
    return;
  }
  if (key == TK_UNREVEALED) {
    panelTile(p, CLEAN_WHITE_EDGE, CLEAN_WHITE, 0, CLEAN_INK);
    return;
  }
  if (key == TK_FLAGGED) {
    panelTile(p, CLEAN_WHITE_EDGE, CLEAN_WHITE, 0, CLEAN_INK);
    drawFlag(p, CLEAN_FLAG);
    return;
  }
  if (key == TK_MINE) {
    panelTile(p, CLEAN_WHITE_EDGE, CLEAN_WHITE, 0, CLEAN_INK);
    drawMine(p, RGB8{ 30, 30, 33 }, CLEAN_FLAG);
    return;
  }
  // TK_REVEALED — a cleared block with no neighbouring mines. Near-black, which
  // is what turns swept regions into the dark pockets the reference shows.
  panelTile(p, CLEAN_DARK_EDGE, CLEAN_DARK, 0, CLEAN_INK);
}

static void genGoldInto(uint16_t *p, uint8_t key) {
  if (key <= TK_8) {
    bevelTile(p, NUM_TOP[key], NUM_BOT[key], (uint8_t)(key + 1), NUM_INK[key]);
    return;
  }
  if (key == TK_UNREVEALED) { bevelTile(p, GOLD_TOP, GOLD_BOT, 0, NUM_INK[0]); return; }
  if (key == TK_FLAGGED) {
    bevelTile(p, GOLD_TOP, GOLD_BOT, 0, NUM_INK[0]);
    drawFlag(p, RGB8{ 255, 255, 255 });
    return;
  }
  if (key == TK_MINE) {
    bevelTile(p, RGB8{ 96, 84, 74 }, RGB8{ 44, 36, 30 }, 0, NUM_INK[0]);
    drawMine(p, RGB8{ 12, 10, 10 }, RGB8{ 224, 60, 48 });
    return;
  }
  bevelTile(p, RGB8{ 34, 32, 28 }, RGB8{ 16, 15, 13 }, 0, NUM_INK[0]);
}

// The grey Windows-Minesweeper pastiche.
static void genClassicInto(uint16_t *p, uint8_t key) {
  const int S = TEX_SIZE;
  const RGB8 face = { 192, 192, 192 }, flat = { 189, 189, 189 };
  const bool raised = (key == TK_UNREVEALED || key == TK_FLAGGED || key == TK_MINE);
  const RGB8 bg = raised ? face : flat;
  const uint16_t light = rgb565(255, 255, 255), shadow = rgb565(128, 128, 128);
  const uint16_t base = rgb565(bg.r, bg.g, bg.b);
  const int bev = (int)(S * 0.0625f);

  for (int i = 0; i < S * S; i++) p[i] = base;
  if (raised) {
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        int d = min(min(x, y), min(S - 1 - x, S - 1 - y));
        if (d >= bev) continue;
        bool tl = (x <= y) ? (x < S - 1 - y) : (y < S - 1 - x);
        p[y * S + x] = tl ? light : shadow;
      }
  } else {
    for (int i = 0; i < S; i++) { p[i] = shadow; p[i * S] = shadow; }
  }

  if (key <= TK_8)            drawDigit(p, S, (uint8_t)(key + 1), CLASSIC_NUM[key]);
  else if (key == TK_FLAGGED) drawFlag(p, RGB8{ 255, 0, 0 });
  else if (key == TK_MINE)    drawMine(p, RGB8{ 0, 0, 0 }, RGB8{ 255, 255, 255 });
}

void Skin::genBuiltin(uint8_t key) {
  switch (_variant) {
    case SKIN_GOLD:    genGoldInto(_pix[key][0], key); break;
    case SKIN_CLASSIC: genClassicInto(_pix[key][0], key); break;
    default:           genCleanInto(_pix[key][0], key); break;
  }
  buildMips(key, 0);
}

bool Skin::loadBuiltin(uint8_t variant) {
  release();
  _variant = (variant < SKIN_VARIANTS) ? variant : SKIN_CLEAN;
  for (uint8_t k = 0; k < TK_COUNT; k++) {
    if (!allocKey(k)) { release(); return false; }
    genBuiltin(k);
  }
  _id = 0;
  _fromFile = 0;
  strncpy(_name, SKIN_VARIANT_NAMES[_variant], sizeof(_name) - 1);
  _name[sizeof(_name) - 1] = 0;
  return true;
}

// Read one .tex. Payload byte order is little-endian, i.e. already a uint16_t
// as the ESP32 stores it, so this is a straight read. Returns the mip level it
// filled, or -1 on failure: a current file is TEX_SIZE and lands on level 0, a
// legacy 64 px one lands on level 1.
int Skin::readTex(const char *path, uint8_t key) {
  File f = SD.open(path, FILE_READ);
  if (!f) return -1;
  uint8_t hdr[16];
  if (f.read(hdr, 16) != 16 || memcmp(hdr, TEX_MAGIC, 4) != 0) { f.close(); return -1; }
  const uint16_t w = hdr[5], h = hdr[6];    // both fit a byte: 64 or 128
  int level;
  if (w == TEX_SIZE && h == TEX_SIZE)                    level = 0;
  else if (w == TEX_SIZE_LEGACY && h == TEX_SIZE_LEGACY) level = 1;
  else { f.close(); return -1; }

  size_t want = (size_t)levelSize((uint8_t)level) * levelSize((uint8_t)level) * sizeof(uint16_t);
  bool ok = (f.read((uint8_t *)_pix[key][level], want) == (int)want);
  f.close();
  return ok ? level : -1;
}

bool Skin::loadFromSd(uint16_t id) {
  release();
  char dir[64];
  snprintf(dir, sizeof(dir), SKIN_DIR "/%u", (unsigned)id);

  for (uint8_t k = 0; k < TK_COUNT; k++) {
    if (!allocKey(k)) { release(); return false; }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.tex", dir, TEX_KEY_NAMES[k]);
    int lvl = readTex(path, k);
    if (lvl >= 0) {
      buildMips(k, (uint8_t)lvl);
      _fromFile++;
    } else {
      // Missing or corrupt: draw our own. Incomplete skins are common on the
      // store, and a blank face would be unreadable rather than merely plain.
      genBuiltin(k);
    }
  }

  _id = id;
  strcpy(_name, "Skin");
  char mpath[80];
  snprintf(mpath, sizeof(mpath), "%s/meta.json", dir);
  File mf = SD.open(mpath, FILE_READ);
  if (mf) {
    JsonDocument d;
    if (!deserializeJson(d, mf)) {
      const char *nm = d["name"];
      if (nm && *nm) { strncpy(_name, nm, sizeof(_name) - 1); _name[sizeof(_name) - 1] = 0; }
    }
    mf.close();
  }
  return true;
}

int skinScanSd(SkinEntry *out, int maxN) {
  File root = SD.open(SKIN_DIR);
  if (!root || !root.isDirectory()) { if (root) root.close(); return 0; }

  int n = 0;
  for (File e = root.openNextFile(); e && n < maxN; e = root.openNextFile()) {
    if (!e.isDirectory()) { e.close(); continue; }
    const char *base = strrchr(e.name(), '/');
    base = base ? base + 1 : e.name();
    char *end = nullptr;
    long id = strtol(base, &end, 10);
    if (end == base || *end != 0 || id <= 0 || id > 65535) { e.close(); continue; }

    out[n].id = (uint16_t)id;
    snprintf(out[n].name, sizeof(out[n].name), "Skin %ld", id);
    char mpath[80];
    snprintf(mpath, sizeof(mpath), SKIN_DIR "/%ld/meta.json", id);
    File mf = SD.open(mpath, FILE_READ);
    if (mf) {
      JsonDocument d;
      if (!deserializeJson(d, mf)) {
        const char *nm = d["name"];
        if (nm && *nm) { strncpy(out[n].name, nm, sizeof(out[n].name) - 1); out[n].name[sizeof(out[n].name) - 1] = 0; }
      }
      mf.close();
    }
    n++;
    e.close();
  }
  root.close();
  return n;
}
