/* ============================================================================
   ESP32 Mastermine — Pancake (ESP32-C5, ST7796 320x480, FT6336 capacitive touch)
   ============================================================================
   Minesweeper on the surface of a rotatable 3D cube, after Mastermine by Adam
   Soutar (https://mastermine.app). Swipe to turn the cube, pinch to zoom, and
   dress it in the user-created skins from the game's own skin store — either
   downloaded on the device over WiFi or sideloaded with sd_prep/fetch_skins.py.

   Standalone touch firmware sharing the H4W9 UI shell with ESP32_Scrabble and
   ESP32_FlipSocial: header with back button and status corner, footer nav bar,
   momentum list menus, chip settings rows, VLW smooth fonts throughout.

   Arduino IDE settings:
     Board            : ESP32C5 Dev Module
     Flash Size       : 8MB
     Partition Scheme : Custom  ->  partitions.csv in this folder
     Flash Frequency  : 80 MHz
     PSRAM            : Enabled     <-- required, the render sprite lives there

   Requires the patched TFT_eSPI-ESP32-C5 library with User_Setup_Select.h set
   to #include <User_Setup_marauder_pancake.h>.

   Two traps this sketch is written around, both paid for once already in
   ESP32_Scrabble:
     * No template functions in a .ino — Arduino's ctags prototype pass emits
       them without the template<> line. Templates live in headers.
     * Likewise no file-local struct/enum used in a .ino function signature.
       Every type here comes from a header.
   ============================================================================ */

#include "configs.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "ft6336.h"
#include "TouchKeyboard.h"
#include "theme.h"
#include "vlw.h"
#include "cube.h"
#include "puconfig.h"
#include "skin.h"
#include "skinstore.h"
#include "render3d.h"
#include "sfx.h"

// Picoware core (panel init, touch).
#include "src/Picoware/internal/boards.hpp"
#include "src/Picoware/internal/gui/draw.hpp"
#include "src/Picoware/internal/system/input.hpp"
#include "src/Picoware/internal/system/view.hpp"
#include "src/Picoware/internal/system/view_manager.hpp"
using namespace Picoware;

// Globals
#ifdef HAS_C5_SD
SPIClass sharedSPI(SPI);
#endif

static ViewManager *vm    = nullptr;   // owns Draw (panel) + InputManager (touch)
static TFT_eSPI    *tft   = nullptr;   // raw panel (from Draw) for the shell screens
static TouchInput  *touch = nullptr;   // touch source (from InputManager)
static Theme        theme;             // colour theme + accent + font + brightness

// Theme-driven colours (macros so every use follows the current theme).
#define COL_BG     (theme.bg())
#define COL_FG     (theme.fg())
#define COL_ACCENT (theme.hdr())
#define COL_DIM    (theme.dim())
#define COL_SEL    (theme.sel())
static const uint16_t COL_OK = 0x07E0;   // status green (theme-independent)

// The cube sits on a flat dark grey, not on the theme's background. rgb(30,30,30)
// is measured off the reference game's own screens — a near-black theme
// background swallows the cube's darker faces, and its cleared blocks are
// near-black themselves. Light themes keep their own background, where the
// problem does not arise.
static const uint16_t COL_GAME_BG_DARK = 0x18E3;   // rgb(30,30,30)

// Panel size comes from the board block in configs.h. The whole UI is portrait;
// nothing here rotates the panel.
static const int SCRW = TFT_WIDTH;
static const int SCRH = TFT_HEIGHT;

// Shell layout — matches H4W9 (header 28, nav 28, list rows 34).
static const int HDRH     = 28;
static const int NAVH     = 28;
#ifdef MARAUDER_V8
static const int ITEMH    = 26;
#else
static const int ITEMH    = 34;
#endif
static const int CONTENTY = HDRH;

// Game state. Declared before the first function so Arduino's generated
// prototypes land below the types they mention.
static Cube     g_cube;
static Skin     g_skin;
static CubeView g_view;
static bool     g_gameActive = false;

// Skin ids. Store skins are numbered from 1 by mastermine.app, so the built-in
// sets take 0 and a high sentinel that no store id will ever collide with.
static const uint16_t SKIN_ID_CLEAN   = 0;
static const uint16_t SKIN_ID_GOLD    = 0xFFFF;
static const uint16_t SKIN_ID_CLASSIC = 0xFFFE;

// Tap vs hold, matching the reference game's Controls screen exactly:
//   REVEAL   Hold
//   FLAG     Tap
//   CHORDING Hold  (on an already-revealed number)
//   TAP VS HOLD CUTOFF  0.25 s
// These are fixed — the reference offers them as settings, but on a panel this
// size the footer slot is better spent on Pause.
static const uint32_t HOLD_MS = 250;

// Persisted settings (/mine_cfg.json)
static uint8_t  g_diff    = DIFF_ADVANCED;
static uint8_t  g_n       = 8;
static uint8_t  g_pct     = 17;             // custom games; ~the game's own density
static uint16_t g_skinId  = SKIN_ID_CLEAN;  // the reference game's own default look

#ifndef HAS_CAP_TOUCH
// Resistive touch calibration (V8). Capacitive panels report real coordinates
// and need none of this.
static const char *TOUCH_CAL_FILE = "/mine_touch.dat";

static bool touchCalLoad(uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "r");
  if (!f) return false;
  bool ok = (f.read((uint8_t *)cal, sizeof(uint16_t) * 5) == sizeof(uint16_t) * 5);
  f.close();
  return ok;
}
static void touchCalSave(const uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "w");
  if (!f) return;
  f.write((const uint8_t *)cal, sizeof(uint16_t) * 5);
  f.close();
}
static void touchCalRun() {
  uint16_t cal[5];
  tft->fillScreen(TFT_BLACK);
  tft->setTextColor(TFT_WHITE, TFT_BLACK);
  tft->setTextDatum(MC_DATUM);
  tft->drawString("Touch Calibration", SCRW / 2, SCRH / 2 - 24, 4);
  tft->drawString("Tap each corner arrow", SCRW / 2, SCRH / 2 + 6, 2);
  tft->setTextDatum(TL_DATUM);
  delay(1500);
  tft->fillScreen(TFT_BLACK);
  tft->calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 15);
  tft->setTouch(cal);
  touchCalSave(cal);
}
static void touchCalInit() {
  uint16_t cal[5];
  if (touchCalLoad(cal)) tft->setTouch(cal);
  else                   touchCalRun();
}
#endif // !HAS_CAP_TOUCH

// Touch helpers
static bool waitTap(uint16_t &x, uint16_t &y, uint32_t timeoutMs = 0) {
  uint32_t start = millis();
  bool wasDown = touch->isPressed();
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) { x = touch->x(); y = touch->y(); return true; }
    wasDown = down;
    if (timeoutMs && (millis() - start) > timeoutMs) return false;
    delay(8);
    yield();
  }
}

// Theme / brightness plumbing
static void applyBrightness() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, theme.duty());
#else
  ledcWrite(0, theme.duty());
#endif
}
static void applyThemeToViewManager() {
  if (!vm) return;
  vm->setBackgroundColor(theme.bg());
  vm->setForegroundColor(theme.fg());
  vm->setSelectedColor(theme.sel());
}

// Smooth-font drawing
// Every string in the UI goes through these. They keep the old TFT_eSPI font
// numbers (1/2/4) at the call sites but render with the VLW smooth fonts.
//
// TFT_eSPI ignores the font-number argument to drawString once a smooth font is
// loaded, so the correct VLW array MUST be loaded first — that is what these
// wrappers are for.
static void drawStr(const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(*tft, fontNum, g_vlw_cur_tft);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  tft->drawString(buf, x, y);
}
static int16_t strWidth(const String &s, uint8_t fontNum) {
  return vlwTextWidth(vlwData(fontNum), s.c_str());
}
// Sprite variant. Each sprite carries its own loaded font, so it needs its own
// tracker rather than sharing the panel's.
//
// Deliberately NOT a template — see the header comment.
static void sprStr(TFT_eSprite &g, const uint8_t *&track,
                   const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(g, fontNum, track);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  g.drawString(buf, x, y);
}

// Status LED
#ifdef HAS_ACT_LED
static bool g_actLedReady = false;
static void ledActArm() {
  if (g_actLedReady) return;
  pinMode(ACT_LED_PIN, OUTPUT);
  digitalWrite(ACT_LED_PIN, LOW);
  g_actLedReady = true;
}
static void ledActSet(bool on) {
  ledActArm();
  digitalWrite(ACT_LED_PIN, (on && theme.led_bright > 0) ? HIGH : LOW);
}
static inline void ledOff()  { ledActSet(false); }
static inline void ledWifi() { ledActSet(true); }
static inline void ledBusy() { ledActSet(true); }
static inline void ledOk()   { ledActSet(true); }
static inline void ledErr()  { ledActSet(true); }
static inline void ledBlinkOk(uint16_t ms = 150) { ledActSet(true); delay(ms); ledActSet(false); }
static void ledSet(bool on) { ledActSet(on); }
#else
#ifdef RGB_BUILTIN
  #define PW_RGB_PIN RGB_BUILTIN
#else
  #define PW_RGB_PIN LED_BUILTIN
#endif
// WS2812-style: consecutive frames need a reset gap or the LED latches the
// first and passes the next down the chain.
static inline void ledGap() { delayMicroseconds(300); }
static void ledRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t s = theme.led_bright;
  ledGap();
  rgbLedWrite(PW_RGB_PIN, (uint8_t)((uint16_t)r * s / 20),
                          (uint8_t)((uint16_t)g * s / 20),
                          (uint8_t)((uint16_t)b * s / 20));
}
static inline void ledOff()  { ledGap(); rgbLedWrite(PW_RGB_PIN, 0, 0, 0); }
static inline void ledWifi() { ledRGB(255, 150, 0); }   // amber — scanning / connecting
static inline void ledBusy() { ledRGB(0,   80, 255); }  // blue  — SD / download work
static inline void ledOk()   { ledRGB(0,  255,   0); }  // green — success
static inline void ledErr()  { ledRGB(255,  0,   0); }  // red   — error
static inline void ledBlinkOk(uint16_t ms = 150) { ledOk(); delay(ms); ledOff(); }
static void ledSet(bool on) { if (on) ledWifi(); else ledOff(); }
#endif // HAS_ACT_LED

// Game config (SPIFFS: /mine_cfg.json)
static void cfgLoad() {
  File f = SPIFFS.open("/mine_cfg.json", FILE_READ);
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return;
  if (!d["diff"].isNull()) { uint8_t v = d["diff"]; if (v < DIFF_COUNT) g_diff = v; }
  if (!d["n"].isNull())    { uint8_t v = d["n"];    if (v >= CUBE_N_MIN && v <= CUBE_N_MAX) g_n = v; }
  if (!d["pct"].isNull())  { uint8_t v = d["pct"];  if (v >= 5 && v <= 40) g_pct = v; }
  if (!d["skin"].isNull())      g_skinId    = d["skin"].as<uint16_t>();
}
static void cfgSave() {
  JsonDocument d;
  d["diff"] = g_diff;
  d["n"] = g_n;
  d["pct"] = g_pct;
  d["skin"] = g_skinId;
  File w = SPIFFS.open("/mine_cfg.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}

// Saved WiFi networks (SPIFFS: /mine_wifi.json = {"nets":[{"s","p"}]})
static const int WIFI_MAX_SAVED = 12;
static int wifiLoad(String *ss, String *pp, int maxN) {
  File f = SPIFFS.open("/mine_wifi.json", FILE_READ);
  if (!f) return 0;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return 0;
  JsonArray a = d["nets"].as<JsonArray>();
  if (a.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : a) {
    if (n >= maxN) break;
    ss[n] = v["s"].as<String>();
    pp[n] = v["p"].as<String>();
    n++;
  }
  return n;
}
static void wifiWriteAll(String *ss, String *pp, int n) {
  JsonDocument d;
  JsonArray a = d["nets"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = ss[i];
    o["p"] = pp[i];
  }
  File w = SPIFFS.open("/mine_wifi.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}
static void wifiSave(const String &ssid, const String &pass) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  os[m] = ssid; op[m] = pass; m++;                 // new entry first
  for (int i = 0; i < n && m < WIFI_MAX_SAVED; i++) {
    if (ss[i] == ssid) continue;                   // drop old duplicate
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}
static String wifiPassFor(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  for (int i = 0; i < n; i++) if (ss[i] == ssid) return pp[i];
  return "";
}
static void wifiForget(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (ss[i] == ssid) continue;
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}

// Battery fuel gauge (MAX17048, I2C 0x36, shared bus)
static int      g_battPct = -1;
static bool     g_battOk  = false;
static uint32_t g_battMs  = 0;
static void battInit() {
  Wire.beginTransmission(0x36);
  g_battOk = (Wire.endTransmission() == 0);
  Serial.println(g_battOk ? F("[Battery] MAX17048 OK") : F("[Battery] MAX17048 not found"));
}
static void battUpdate() {
  if (!g_battOk) return;
  Wire.beginTransmission(0x36);
  Wire.write(0x04);                          // SOC register
  if (Wire.endTransmission(false) != 0) { g_battOk = false; return; }
  Wire.requestFrom((uint8_t)0x36, (uint8_t)2);
  if (Wire.available() < 2) return;
  uint8_t hi = Wire.read();
  Wire.read();                               // fractional byte — discard
  g_battPct = (hi > 100) ? 100 : hi;
  g_battMs  = millis();
}

static volatile bool g_wifiConnecting = false;

// Rendering helpers
static void wifiArc(int cx, int cy, int r, uint16_t c) {
  for (int deg = -45; deg <= 45; deg += 2) {
    float a = deg * 0.0174533f;
    int x = cx + (int)lroundf(r * sinf(a));
    int y = cy - (int)lroundf(r * cosf(a));
    tft->drawPixel(x, y, c);
  }
}

static String memShort(size_t bytes) {
  if (bytes >= 1024UL * 1024UL) {
    uint32_t tenths = (uint32_t)((bytes * 10ULL) / (1024ULL * 1024ULL));
    return String(tenths / 10) + "." + String(tenths % 10) + "M";
  }
  return String((uint32_t)(bytes / 1024)) + "k";
}

// DRAM + PSRAM free, shown on EVERY screen. The two figures are STACKED rather
// than side by side, which costs about 40 px of width instead of 110, and they
// sit just right of the back chevron when there is one. Drawn in the same
// colour as the battery percentage so the status figures read as one set.
static bool g_hdrShowBack = false;   // set by drawHeader, read here
static int  g_hdrMemRight = 0;       // x the readout ends at, for title centring

static void drawHeaderMem() {
  String d = "D:" + memShort(ESP.getFreeHeap());
  size_t ps = ESP.getFreePsram();
  String p = ps ? ("P:" + memShort(ps)) : String("");

  int w = strWidth(d, 1);
  if (p.length()) { int pw = strWidth(p, 1); if (pw > w) w = pw; }

  const int x0 = g_hdrShowBack ? 46 : 6;
  g_hdrMemRight = x0 + w + 6;
  tft->fillRect(x0, 0, w + 6, HDRH, COL_ACCENT);
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(ML_DATUM);
  if (p.length()) {
    drawStr(d, x0, HDRH / 2 - 6, 1);
    drawStr(p, x0, HDRH / 2 + 6, 1);
  } else {
    drawStr(d, x0, HDRH / 2, 1);
  }
  tft->setTextDatum(TL_DATUM);
}

static void drawHeaderStatus() {
  if (g_battOk && (g_battMs == 0 || millis() - g_battMs > 10000)) battUpdate();
  const int clearW = 62;
  tft->fillRect(SCRW - clearW, 0, clearW, HDRH, COL_ACCENT);

  int rx = SCRW - 4;
  if (g_battPct >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_battPct);
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MR_DATUM);
    drawStr(pct, rx, HDRH / 2, 1);
    rx -= strWidth(pct, 1) + 8;
  }

  uint16_t wc = g_wifiConnecting                ? TFT_YELLOW
              : (WiFi.status() == WL_CONNECTED) ? COL_OK
                                                : TFT_RED;
  int cx = rx - 10, cy = HDRH / 2 + 5;
  tft->fillCircle(cx, cy, 1, wc);
  wifiArc(cx, cy, 4,  wc);
  wifiArc(cx, cy, 7,  wc);
  wifiArc(cx, cy, 10, wc);

  tft->setTextDatum(TL_DATUM);
}

static void drawChevron(int bx, int by, int bw, int bh, bool right, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2;
  if (right) tft->fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, col);
  else       tft->fillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, col);
}
static void drawPlusMinus(int bx, int by, int bw, int bh, bool plus, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
  tft->fillRect(cx - r, cy - 1, 2 * r, 2, col);
  if (plus) tft->fillRect(cx - 1, cy - r, 2, 2 * r, col);
}

// Width the status corner (WiFi icon + battery) reserves on the right.
static const int HDR_STATUS_W = 62;

static void drawHeader(const String &title, bool showBack) {
  g_hdrShowBack = showBack;
  tft->fillRect(0, 0, SCRW, HDRH, COL_ACCENT);
  if (showBack) {
    tft->fillRoundRect(2, 3, 40, 22, 4, COL_ACCENT);
    tft->drawRoundRect(2, 3, 40, 22, 4, theme.neon(3, COL_DIM));
    drawChevron(2, 3, 40, 22, false, COL_FG);
  }
  // Memory first, because it sets how far right the title may start.
  drawHeaderMem();
  // Centre the title in what is left between the readout and the status
  // corner, rather than on the panel — otherwise a long title overlaps one or
  // the other, which it does on the narrower V8 panel almost immediately.
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  drawStr(title, (g_hdrMemRight + SCRW - HDR_STATUS_W) / 2, HDRH / 2, 2);
  drawHeaderStatus();
  tft->setTextDatum(TL_DATUM);
}
static bool backTapped(uint16_t x, uint16_t y) {
  return (int)y < HDRH && (int)x < 48;
}

static void drawNav(const char *l, const char *m, const char *r) {
  int y = SCRH - NAVH, third = SCRW / 3, bh = NAVH - 10, by = y + 5, bw = third - 10;
  tft->fillRect(0, y, SCRW, NAVH, COL_BG);
  tft->drawFastHLine(0, y, SCRW, theme.edge());
  const char *L[3] = { l, m, r };
  for (int i = 0; i < 3; i++) {
    if (!L[i] || !L[i][0]) continue;
    int cx = i * third + third / 2, bx = cx - bw / 2;
    tft->fillRoundRect(bx, by, bw, bh, 5, COL_ACCENT);
    tft->drawRoundRect(bx, by, bw, bh, 5, theme.neon(i, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    drawStr(L[i], cx, by + bh / 2, 2);
  }
  tft->setTextDatum(TL_DATUM);
}
static int navHit(uint16_t x, uint16_t y) {
  if ((int)y < SCRH - NAVH) return -1;
  int c = (int)x / (SCRW / 3);
  return c > 2 ? 2 : c;
}

static void drawListRow(int y, const String &text, bool sel, bool arrow) {
  uint16_t bgc = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  tft->fillRect(0, y, SCRW, ITEMH, bgc);
  tft->setTextColor(COL_FG, bgc);
  tft->setTextDatum(ML_DATUM);
  drawStr(text, 12, y + ITEMH / 2, 2);
  if (arrow) drawChevron(SCRW - 26, y, 16, ITEMH, true, theme.neon(seed, COL_DIM));
  tft->drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  tft->setTextDatum(TL_DATUM);
}

// Which VLW array the current list sprite has loaded. A sprite is created and
// destroyed per scrollList() call and each carries its own font state, so this
// is reset right after createSprite() rather than persisting.
static const uint8_t *sprFont = nullptr;

static void drawRowSprite(TFT_eSprite &spr, int y, const String &text, bool arrow, int seed) {
  spr.fillRect(0, y, SCRW, ITEMH, COL_BG);
  spr.setTextColor(COL_FG, COL_BG);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, sprFont, text, 12, y + ITEMH / 2, 2);
  if (arrow) {
    int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
    spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  }
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

static void sprScrollBar(TFT_eSprite &spr, int viewH, int total, float scroll) {
  if (total <= viewH) return;
  const int bw = 4, bx = SCRW - bw - 1;
  spr.fillRect(bx, 0, bw, viewH, theme.edge());
  int thumbH = viewH * viewH / total; if (thumbH < 14) thumbH = 14;
  int maxS = total - viewH;
  int thumbY = (maxS > 0) ? (int)((scroll / (float)maxS) * (viewH - thumbH)) : 0;
  spr.fillRect(bx, thumbY, bw, thumbH, theme.neon(thumbY / 12, COL_DIM));
}

// scrollList return sentinels for footer-button taps (Back is SL_BACK).
static const int SL_BACK = -1, SL_F0 = -2, SL_F1 = -3, SL_F2 = -4;

static int scrollList(const String &title, String *rows, int n, bool arrow,
                      const char *fL = nullptr, const char *fM = nullptr, const char *fR = nullptr) {
  bool hasFooter = (fL && fL[0]) || (fM && fM[0]) || (fR && fR[0]);
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY - (hasFooter ? NAVH : 0);
  int total = n * ITEMH;
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  if (hasFooter) drawNav(fL ? fL : "", fM ? fM : "", fR ? fR : "");

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  sprFont = nullptr;              // fresh sprite: nothing loaded on it yet

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (haveSpr) {
      spr.fillSprite(COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawRowSprite(spr, y, rows[i], arrow, i);
      }
      sprScrollBar(spr, CH, total, scroll);
      spr.pushSprite(0, CY);
    } else {
      tft->fillRect(0, CY, SCRW, CH, COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawListRow(CY + y, rows[i], false, arrow);
      }
    }
  };
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); return SL_BACK; }
        if (hasFooter && (int)pY >= SCRH - NAVH) {
          int nh = navHit(pX, pY);
          if (haveSpr) spr.deleteSprite();
          return nh == 0 ? SL_F0 : nh == 2 ? SL_F2 : SL_F1;
        }
        if ((int)pY >= CY && (int)pY < CY + CH) {
          int idx = ((int)pY - CY + (int)scroll) / ITEMH;
          if (idx >= 0 && idx < n) { if (haveSpr) spr.deleteSprite(); return idx; }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }

    wasDown = down;
    if (need) render();
    delay(12);
  }
}

static void statusLine(const char *msg, uint16_t col = 0xFFFF) {
  tft->fillRect(0, SCRH - 26, SCRW, 26, COL_BG);
  tft->setTextColor(col == 0xFFFF ? COL_FG : col, COL_BG);
  tft->setTextDatum(ML_DATUM);
  drawStr(msg, 8, SCRH - 13, 2);
  tft->setTextDatum(TL_DATUM);
}

// Settings chip rows: label + [<] value [>] (or [-] value [+])
static const int CHIP_W = 28, CHIP_H = 22;
static void chipGeom(const String &val, int &fwd_bx, int &bwd_bx, int &vx) {
  fwd_bx = SCRW - 8 - CHIP_W;
  int vw = strWidth(val.c_str(), 2);
  vx     = fwd_bx - 4 - vw;
  bwd_bx = vx - 4 - CHIP_W;
}
// Returns -1 none, 0 left/decrement, 1 right/increment. `val` must match draw.
static int chipHit(int y, const String &val, uint16_t x, uint16_t ty) {
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  if ((int)ty < by || (int)ty >= by + CHIP_H) return -1;
  if ((int)x >= fwd_bx && (int)x < fwd_bx + CHIP_W) return 1;
  if ((int)x >= bwd_bx && (int)x < bwd_bx + CHIP_W) return 0;
  return -1;
}

// Sprite versions of the settings rows.
//
// The settings list is composited into a sprite so it can SCROLL. Drawing it
// straight to the panel at fixed offsets — as ESP32_Scrabble does — runs off
// the bottom of the V8's 320 px screen once there are more than about eleven
// rows, and there are more than that here.
static void sprChevron(TFT_eSprite &g, int bx, int by, int bw, int bh, bool right, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2;
  if (right) g.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, col);
  else       g.fillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, col);
}
static void sprPlusMinus(TFT_eSprite &g, int bx, int by, int bw, int bh, bool plus, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
  g.fillRect(cx - r, cy - 1, 2 * r, 2, col);
  if (plus) g.fillRect(cx - 1, cy - r, 2, 2 * r, col);
}
static void sprChipRow(TFT_eSprite &g, int y, const String &label, const String &val,
                       bool pm, uint16_t valcol, int seed) {
  g.fillRect(0, y, SCRW, ITEMH, COL_BG);
  g.setTextColor(COL_FG, COL_BG);
  g.setTextDatum(ML_DATUM);
  sprStr(g, sprFont, label, 12, y + ITEMH / 2, 2);
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  g.fillRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  g.drawRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed, COL_DIM));
  g.fillRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  g.drawRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed + 4, COL_DIM));
  if (pm) {
    sprPlusMinus(g, bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    sprPlusMinus(g, fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  } else {
    sprChevron(g, bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    sprChevron(g, fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  }
  g.setTextColor(valcol ? valcol : COL_FG, COL_BG);
  g.setTextDatum(ML_DATUM);
  sprStr(g, sprFont, val, vx, y + ITEMH / 2, 2);
  g.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  g.setTextDatum(TL_DATUM);
}
static void sprInfoRow(TFT_eSprite &g, int y, const String &label, const String &val, int seed) {
  g.fillRect(0, y, SCRW, ITEMH, COL_BG);
  g.setTextColor(COL_FG, COL_BG);
  g.setTextDatum(ML_DATUM);
  sprStr(g, sprFont, label, 12, y + ITEMH / 2, 2);
  if (val.length()) {
    g.setTextColor(COL_DIM, COL_BG);
    int vw = strWidth(val.c_str(), 2);
    sprStr(g, sprFont, val, SCRW - 26 - vw, y + ITEMH / 2, 2);
  }
  sprChevron(g, SCRW - 26, y, 16, ITEMH, true, theme.neon(seed, COL_DIM));
  g.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  g.setTextDatum(TL_DATUM);
}

static void msgScreen(const char *title, const String &a, const String &b, uint16_t col) {
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  tft->setTextColor(col, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(a, SCRW / 2, SCRH / 2 - 20, 2);
  if (b.length()) {
    tft->setTextColor(COL_DIM, COL_BG);
    int y = SCRH / 2 + 6, maxW = SCRW - 24;
    String line = "", rest = b;
    while (rest.length() && y < SCRH - 20) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (strWidth(cand.c_str(), 2) <= maxW) { line = cand; }
      else { drawStr(line, SCRW / 2, y, 2); y += 20; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 20) drawStr(line, SCRW / 2, y, 2);
  }
  tft->setTextDatum(TL_DATUM);
  uint16_t x, y2; waitTap(x, y2);
}

// WiFi
static volatile int g_wifiReason = 0;
static volatile int g_wifiEvt = -1;
static bool g_manualDisconnect = false;
static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  g_wifiEvt = (int)event;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    g_wifiReason = info.wifi_sta_disconnected.reason;
}

static bool waitConnect(uint32_t timeoutMs, int spinnerY) {
  uint32_t start = millis(), lastAnim = 0;
  bool wasDown = touch->isPressed();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) return false;    // tap to cancel
    wasDown = down;
    if (millis() - lastAnim > 350) {
      lastAnim = millis();
      String d = "connecting";
      for (int i = 0; i < (dots = (dots + 1) % 4); i++) d += ".";
      tft->fillRect(0, spinnerY, SCRW, 20, COL_BG);
      tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
      drawStr(d, SCRW / 2, spinnerY + 8, 2);
      tft->setTextDatum(TL_DATUM);
    }
    delay(30);
  }
  return WiFi.status() == WL_CONNECTED;
}

// setBandMode(AUTO) is THE C5-specific line: it lets the dual-band radio pick.
static bool connectWiFi(const String &ssid, const String &pass) {
  g_wifiConnecting = true;
  g_manualDisconnect = false;
  ledWifi();
  g_wifiReason = 0;
  g_wifiEvt = -1;
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.scanDelete();
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(false);

  tft->fillScreen(COL_BG);
  drawHeader("WiFi", true);
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("Connecting to", SCRW / 2, SCRH / 2 - 22, 2);
  tft->setTextColor(COL_FG, COL_BG);
  drawStr(String("\"") + ssid + "\"", SCRW / 2, SCRH / 2 + 4, 4);
  tft->setTextDatum(TL_DATUM);

  bool ok = waitConnect(12000, SCRH / 2 + 34);
  g_wifiConnecting = false;
  ledOff();
  return ok;
}

static bool connectSaved(const String &ssid) {
  return connectWiFi(ssid, wifiPassFor(ssid));
}

// Background (re)connect — non-blocking so the menu stays responsive.
enum WbState { WB_IDLE, WB_SCAN, WB_CONNECT, WB_DONE };
static WbState  g_wb  = WB_IDLE;
static uint32_t g_wbT = 0;
static String   g_wbSs[WIFI_MAX_SAVED], g_wbPp[WIFI_MAX_SAVED];
static int      g_wbN = 0, g_wbIdx = 0;

static void wifiBgTry() {
  g_wifiReason = 0; g_wifiEvt = -1;
  WiFi.begin(g_wbSs[g_wbIdx].c_str(), g_wbPp[g_wbIdx].c_str());
  WiFi.setAutoReconnect(false);
  g_wbT = millis();
}

static void wifiBgBegin() {
  g_wbN = wifiLoad(g_wbSs, g_wbPp, WIFI_MAX_SAVED);
  if (g_wbN == 0) { g_wb = WB_IDLE; return; }
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true);                    // async — order by RSSI when done
  g_wbT = millis();
  g_wb = WB_SCAN;
  g_wifiConnecting = true;
}

static void wifiBgTick() {
  if (g_wb == WB_SCAN) {
    int r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING && millis() - g_wbT < 6000) return;
    if (r > 0) {
      int rssi[WIFI_MAX_SAVED];
      for (int i = 0; i < g_wbN; i++) {
        rssi[i] = -999;
        for (int j = 0; j < r; j++)
          if (WiFi.SSID(j) == g_wbSs[i] && WiFi.RSSI(j) > rssi[i]) rssi[i] = WiFi.RSSI(j);
      }
      for (int a = 0; a < g_wbN - 1; a++) {
        int best = a;
        for (int b = a + 1; b < g_wbN; b++) if (rssi[b] > rssi[best]) best = b;
        if (best != a) {
          int tr = rssi[a]; rssi[a] = rssi[best]; rssi[best] = tr;
          String ts = g_wbSs[a]; g_wbSs[a] = g_wbSs[best]; g_wbSs[best] = ts;
          String tp = g_wbPp[a]; g_wbPp[a] = g_wbPp[best]; g_wbPp[best] = tp;
        }
      }
    }
    WiFi.scanDelete();
    g_wbIdx = 0;
    wifiBgTry();
    g_wb = WB_CONNECT;
    return;
  }
  if (g_wb == WB_CONNECT) {
    if (WiFi.status() == WL_CONNECTED) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
    int maxTry = g_wbN < 2 ? g_wbN : 2;
    if (millis() - g_wbT > 8000) {
      if (++g_wbIdx >= maxTry) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
      wifiBgTry();
    }
  }
}

static void scanFlow() {
  static String rows[41];
  for (;;) {
    tft->fillScreen(COL_BG);
    drawHeader("Scan", true);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr("Scanning...", SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
    ledSet(true);
    int nnet = WiFi.scanNetworks();
    ledSet(false);
    int rc = (nnet < 0) ? 0 : nnet;

    for (int i = 0; i < rc && i < 41; i++)
      rows[i] = WiFi.SSID(i) + "   ch" + WiFi.channel(i) + "  (" + WiFi.RSSI(i) + ")";

    int sel = scrollList("Scan", rows, rc, true, "Back", "Rescan", "");
    if (sel == SL_BACK || sel == SL_F0) return;
    if (sel == SL_F1) continue;

    int idx = sel;
    if (idx < 0 || idx >= rc) continue;
    String ssid = WiFi.SSID(idx);
    char pass[65] = {0};
    String sp = wifiPassFor(ssid);
    if (sp.length()) strncpy(pass, sp.c_str(), sizeof(pass) - 1);
    if (!touchKeyboardInput(*tft, COL_FG, COL_BG, pass, sizeof(pass),
                            (String("Password: ") + ssid).c_str(), true)) continue;
    if (connectWiFi(ssid, pass)) {
      wifiSave(ssid, pass);
      statusLine("Connected!", COL_OK);
      uint16_t a, bb; waitTap(a, bb);
      return;
    }
    statusLine((String("Failed (reason ") + g_wifiReason + "). Tap to re-scan.").c_str(), TFT_RED);
    uint16_t a, bb; waitTap(a, bb);
  }
}

static void wifiSetup() {
  static String rows[WIFI_MAX_SAVED];
  for (;;) {
    String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
    int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
    for (int i = 0; i < n; i++) {
      bool cur = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ss[i]);
      rows[i] = (cur ? String("* ") : String("")) + ss[i];
    }
    int sel = scrollList("WiFi Setup", rows, n, true, "Disconnect", "Scan", n > 0 ? "Forget" : "");
    if (sel == SL_BACK) return;
    if (sel == SL_F0) { WiFi.disconnect(true); g_manualDisconnect = true; continue; }
    if (sel == SL_F1) { scanFlow(); continue; }
    if (sel == SL_F2 && n > 0) {
      static String frows[WIFI_MAX_SAVED];
      for (int i = 0; i < n; i++) frows[i] = ss[i];
      int f = scrollList("Forget", frows, n, true);
      if (f >= 0 && f < n) wifiForget(ss[f]);
      continue;
    }
    if (sel >= 0 && sel < n) connectSaved(ss[sel]);
  }
}

static void wifiDebug() {
  // Repaint only when something actually changed — repainting per tap made this
  // flash on any stray touch.
  bool repaint = true;
  for (;;) {
   if (repaint) {
    repaint = false;
    tft->fillScreen(COL_BG);
    drawHeader("WiFi Debug", true);
    int y = CONTENTY + 10;
    tft->setTextColor(COL_FG, COL_BG);
    tft->setTextDatum(TL_DATUM);
    auto line = [&](const String &s) { drawStr(s, 12, y, 2); y += 24; };
    bool up = (WiFi.status() == WL_CONNECTED);
    line(String("Status:       ") + WiFi.status() + (up ? "  (connected)" : ""));
    line(String("Last event:   ") + g_wifiEvt);
    line(String("Disc reason:  ") + g_wifiReason);
    line(String("SSID:         ") + (up ? WiFi.SSID() : String("-")));
    line(String("Channel:      ") + (up ? String(WiFi.channel()) : String("-")));
    line(String("IP:           ") + (up ? WiFi.localIP().toString() : String("-")));
    line(String("RSSI:         ") + (up ? String(WiFi.RSSI()) : String("-")));
    line(String("Free heap:    ") + ESP.getFreeHeap());
    line(String("Free PSRAM:   ") + ESP.getFreePsram());
    drawNav("Disconnect", "", "Reconnect");
   }

    uint16_t x, ty;
    if (!waitTap(x, ty)) continue;
    if (backTapped(x, ty)) return;
    int nh = navHit(x, ty);
    if (nh == 0) { WiFi.disconnect(true); g_manualDisconnect = true; repaint = true; continue; }
    if (nh == 2) {
      repaint = true;
      String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
      int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
      if (n) connectSaved(ss[0]);
    }
  }
}

//  Skins
static const int SKIN_MAX_SD = 40;

static bool skinLoad(uint16_t id) {
  ledBusy();
  bool ok;
  if (id == SKIN_ID_CLEAN)        ok = g_skin.loadBuiltin(SKIN_CLEAN);
  else if (id == SKIN_ID_GOLD)    ok = g_skin.loadBuiltin(SKIN_GOLD);
  else if (id == SKIN_ID_CLASSIC) ok = g_skin.loadBuiltin(SKIN_CLASSIC);
  else                            ok = g_skin.loadFromSd(id);
  if (!ok) ok = g_skin.loadBuiltin(SKIN_CLEAN);   // last resort — always playable
  ledOff();
  return ok;
}

// Progress bar for a store download. A skin is a dozen 512x512 JPEGs over a
// slow link, so this is not optional.
static bool skinDlProgress(uint8_t done, uint8_t total, const char *key, void *ctx) {
  const int bw = SCRW - 48, bx = 24, by = SCRH / 2;
  static int lastPct = -1;
  int pct = total ? (done * 100 / total) : 0;
  if (pct != lastPct) {
    lastPct = pct;
    tft->drawRect(bx, by, bw, 14, COL_DIM);
    tft->fillRect(bx + 2, by + 2, (bw - 4) * pct / 100, 10, COL_OK);
    tft->fillRect(0, by + 24, SCRW, 20, COL_BG);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr(String(key) + "   " + done + "/" + total, SCRW / 2, by + 32, 2);
    tft->setTextDatum(TL_DATUM);
  }
  return true;
}

// Browse the live store, download a skin, make it current.
static void skinStoreScreen() {
  if (WiFi.status() != WL_CONNECTED) {
    msgScreen("Skin Store", "WiFi not connected",
              "Connect to a network in Settings -> WiFi Setup, then try again. "
              "You can also sideload skins with sd_prep/fetch_skins.py.", TFT_RED);
    return;
  }

  static SkinStore store;
  static StoreSkin entries[STORE_PAGE_MAX];
  static String rows[STORE_PAGE_MAX];
  int page = 0;

  for (;;) {
    tft->fillScreen(COL_BG);
    drawHeader("Skin Store", true);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr(String("Loading page ") + (page + 1) + "...", SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
    ledWifi();
    int n = store.fetchPage(page, entries, STORE_PAGE_MAX);
    ledOff();

    if (n < 0) {
      msgScreen("Skin Store", "Could not reach the store", store.lastError(), TFT_RED);
      return;
    }
    for (int i = 0; i < n; i++)
      rows[i] = String(entries[i].name) + "   " + entries[i].downloads + " dl" +
                (entries[i].nTextures < TK_COUNT
                   ? String("  (") + entries[i].nTextures + "/" + TK_COUNT + ")" : String(""));

    int sel = scrollList(String("Store p") + (page + 1), rows, n, true,
                         page > 0 ? "Prev" : "", "", store.eof() ? "" : "Next");
    if (sel == SL_BACK) { store.release(); return; }
    if (sel == SL_F0) { if (page > 0) page--; continue; }
    if (sel == SL_F2) { if (!store.eof()) page++; continue; }
    if (sel < 0 || sel >= n) continue;

    tft->fillScreen(COL_BG);
    drawHeader("Downloading", true);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr(entries[sel].name, SCRW / 2, SCRH / 2 - 40, 4);
    tft->setTextDatum(TL_DATUM);
    ledWifi();
    int got = store.download(sel, skinDlProgress, nullptr);
    ledOff();

    if (got <= 0) {
      msgScreen("Skin Store", "Download failed", store.lastError(), TFT_RED);
      continue;
    }
    g_skinId = entries[sel].id;
    cfgSave();
    skinLoad(g_skinId);
    msgScreen("Skin Store", String("Installed ") + entries[sel].name,
              got < TK_COUNT
                ? String("Got ") + got + " of " + TK_COUNT +
                  " textures. The rest use the built-in art - plenty of skins on "
                  "the store are published incomplete."
                : String("All ") + TK_COUNT + " textures installed.", COL_OK);
    store.release();
    return;
  }
}

// Skins already on the card, plus a way to the store.
static void skinsScreen() {
  static SkinEntry sd[SKIN_MAX_SD];
  static String rows[SKIN_MAX_SD + SKIN_VARIANTS];
  // The built-in sets head the list, then whatever is on the card.
  static const uint16_t BUILTIN_IDS[SKIN_VARIANTS] = {
    SKIN_ID_CLEAN, SKIN_ID_GOLD, SKIN_ID_CLASSIC };
  static const uint8_t BUILTIN_VAR[SKIN_VARIANTS] = {
    SKIN_CLEAN, SKIN_GOLD, SKIN_CLASSIC };
  const int nb = SKIN_VARIANTS;

  for (;;) {
    int n = skinScanSd(sd, SKIN_MAX_SD);
    for (int i = 0; i < nb; i++)
      rows[i] = String(g_skinId == BUILTIN_IDS[i] ? "* " : "")
              + SKIN_VARIANT_NAMES[BUILTIN_VAR[i]] + "  (built-in)";
    for (int i = 0; i < n; i++)
      rows[nb + i] = String(g_skinId == sd[i].id ? "* " : "") + sd[i].name;

    int sel = scrollList("Skins", rows, n + nb, true, "Back", "Get More", "");
    if (sel == SL_BACK || sel == SL_F0) return;
    if (sel == SL_F1) { skinStoreScreen(); continue; }
    if (sel < 0 || sel >= n + nb) continue;

    g_skinId = (sel < nb) ? BUILTIN_IDS[sel] : sd[sel - nb].id;
    cfgSave();
    skinLoad(g_skinId);
    return;
  }
}

//  Game screen
//
//  The cube is composited into one PSRAM sprite by CubeView and pushed whole,
//  so nothing on this screen ever draws to the panel mid-frame. The header,
//  banner and footer are painted once and only repainted when their contents
//  actually change — that is the no-flashing rule.
//
//    header  28   back chevron, mines left, clock
//    banner  20   powerup results, win/lose, hints
//    cube         everything between
//    nav     28   [Flag/Dig] [Power] [Recentre]
static const int G_BANH = 20;
static const int G_BANY = HDRH;
static const int G_CUBY = HDRH + G_BANH;
static const int G_CUBH = SCRH - G_CUBY - NAVH;

static String   g_banner;
static int      g_armed = -1;        // powerup armed for the next tap, or -1

static uint16_t gameBg() { return theme.dark() ? COL_GAME_BG_DARK : COL_BG; }

static String fmtClock(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[12];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}

// Repaints only when the text actually changes. The game loop asks twice a
// second, but the clock only ticks once, and repainting a strip that has not
// changed is exactly the kind of avoidable flicker this firmware is meant not
// to have. Pass force after something else has painted over the header.
static String g_hdrLast;
static void gDrawHeader(bool force = false) {
  String s = String(g_cube.minesLeft()) + "  mines   " + fmtClock(g_cube.elapsedMs());
  g_hdrShowBack = true;
  // Even when the title has not changed, the memory figures and status corner
  // are worth refreshing — they are the reason for the periodic call.
  if (!force && s == g_hdrLast) { drawHeaderMem(); drawHeaderStatus(); return; }
  g_hdrLast = s;
  g_hdrShowBack = true;
  tft->fillRect(0, 0, SCRW, HDRH, COL_ACCENT);
  tft->fillRoundRect(2, 3, 40, 22, 4, COL_ACCENT);
  tft->drawRoundRect(2, 3, 40, 22, 4, theme.neon(3, COL_DIM));
  drawChevron(2, 3, 40, 22, false, COL_FG);
  drawHeaderMem();
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  drawStr(s, (g_hdrMemRight + SCRW - HDR_STATUS_W) / 2, HDRH / 2, 2);
  tft->setTextDatum(TL_DATUM);
  drawHeaderStatus();
}

// Zoom chips.
//
// Only on boards without multitouch. The V8's XPT2046 is a resistive panel and
// cannot physically report a second contact, so there is no pinch gesture to be
// had there — it gets the same [-]/[+] chip pair the brightness setting uses.
// The Pancake's FT6336 tracks two fingers, so it pinches instead and keeps the
// banner width for text.
#ifndef HAS_MULTITOUCH
static const int GZ_W = 26, GZ_H = 18;
static int gzMinusX() { return SCRW - 2 * GZ_W - 12; }
static int gzPlusX()  { return SCRW - GZ_W - 6; }
static int gzY()      { return G_BANY + (G_BANH - GZ_H) / 2; }
static void gDrawZoomChips() {
  int y = gzY();
  for (int i = 0; i < 2; i++) {
    int x = i ? gzPlusX() : gzMinusX();
    tft->fillRoundRect(x, y, GZ_W, GZ_H, 4, COL_ACCENT);
    tft->drawRoundRect(x, y, GZ_W, GZ_H, 4, theme.neon(i + 5, COL_DIM));
    drawPlusMinus(x, y, GZ_W, GZ_H, i == 1, COL_FG);
  }
}
static int gZoomHit(int x, int y) {
  if (y < gzY() || y >= gzY() + GZ_H) return -1;
  if (x >= gzMinusX() && x < gzMinusX() + GZ_W) return 0;
  if (x >= gzPlusX()  && x < gzPlusX()  + GZ_W) return 1;
  return -1;
}
#endif

static void gDrawBanner() {
  tft->fillRect(0, G_BANY, SCRW, G_BANH, gameBg());
  int textW = SCRW;
#ifndef HAS_MULTITOUCH
  gDrawZoomChips();
  textW = gzMinusX() - 4;                 // keep the text clear of the chips
#endif
  if (!g_banner.length()) return;
  uint16_t c = (g_cube.state() == GS_LOST) ? TFT_RED
             : (g_cube.state() == GS_WON)  ? COL_OK : COL_DIM;
  tft->setTextColor(c, gameBg());
  tft->setTextDatum(MC_DATUM);
  drawStr(g_banner, textW / 2, G_BANY + G_BANH / 2, 2);
  tft->setTextDatum(TL_DATUM);
}

// Also change-detecting: the footer has few states, but it is consulted on
// every touch release, and repainting it each time is a visible flicker at the
// bottom of the screen.
static String g_navLast;
static void gDrawNav(bool force = false) {
  const char *m = (g_armed >= 0) ? "Cancel" : "Power";
  String sig = String("Pause|") + m;
  if (!force && sig == g_navLast) return;
  g_navLast = sig;
  drawNav("Pause", m, "Recentre");
}

// Pause: a small modal over the cube. Returns true to resume, false to leave
// for the main menu. The clock is not stopped — the reference does not either,
// and stopping it would make Pause a way to game the timer.
// Return values for pauseDialog. Kept as plain ints (not an enum return type):
// Arduino's ctags prototype pass inserts the prototype above any .ino-local enum
// definition, so an enum RETURN type there fails to compile — see the same rule
// for structs in the build notes.
static const int PA_RESUME = 0, PA_NEWGAME = 1, PA_MENU = 2;

static int pauseDialog() {
  const int bw = SCRW - 72, bh = 44, bx = 36, gap = 12;
  const int y0 = SCRH / 2 - 72;                 // Resume
  const int y1 = y0 + bh + gap;                 // New Game (middle)
  const int y2 = y1 + bh + gap;                 // Back to Menu
  const int boxTop = y0 - 54, boxH = (y2 + bh) - boxTop + 16;

  tft->fillRoundRect(20, boxTop, SCRW - 40, boxH, 10, COL_BG);
  tft->drawRoundRect(20, boxTop, SCRW - 40, boxH, 10, theme.neon(2, COL_DIM));
  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr("Paused", SCRW / 2, y0 - 26, 4);
  tft->setTextDatum(TL_DATUM);

  auto button = [&](int y, const char *label, int seed) {
    tft->fillRoundRect(bx, y, bw, bh, 8, COL_ACCENT);
    tft->drawRoundRect(bx, y, bw, bh, 8, theme.neon(seed, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    drawStr(label, SCRW / 2, y + bh / 2, 2);
    tft->setTextDatum(TL_DATUM);
  };
  button(y0, "Resume", 1);
  button(y1, "New Game", 3);
  button(y2, "Back to Menu", 5);

  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    const bool inCol = ((int)x >= bx && (int)x < bx + bw);
    if (inCol && (int)y >= y0 && (int)y < y0 + bh) { Sfx::play(Sfx::MODE_EXIT);  return PA_RESUME; }
    if (inCol && (int)y >= y1 && (int)y < y1 + bh) { Sfx::play(Sfx::MODE_ENTER); return PA_NEWGAME; }
    if (inCol && (int)y >= y2 && (int)y < y2 + bh) { Sfx::play(Sfx::MODE_EXIT);  return PA_MENU; }
    // A tap anywhere outside the box dismisses it — a modal should not trap the
    // player — which counts as Resume.
    if ((int)y < boxTop || (int)y > boxTop + boxH) return PA_RESUME;
  }
}

// Read touch for the game screen. On the Pancake this goes straight to the
// FT6336 so it can see a second finger for pinch-zoom; the V8's resistive panel
// can only ever report one contact, so it takes the shell's path and gets
// zoom buttons instead.
static uint8_t gTouch(int &x1, int &y1, int &x2, int &y2) {
#ifdef HAS_MULTITOUCH
  uint16_t ax, ay, bx, by;
  uint8_t n = ft6336_read_raw2(&ax, &ay, &bx, &by);
  if (n >= 1) { x1 = ax; y1 = ay; }
  if (n >= 2) { x2 = bx; y2 = by; }
  return n;
#else
  touch->run();
  if (!touch->isPressed()) return 0;
  x1 = touch->x();
  y1 = touch->y();
  return 1;
#endif
}

static void gApplyResult(bool changed) {
  if (!changed) return;
  // Lifesaver is passive — reveal() spends it on its own, so the only place it
  // can be reported is here, after the fact.
  if (g_cube.takeLifesaverUsed()) { g_banner = "Lifesaver! Mine flagged."; Sfx::play(Sfx::LIFE_SAVE); }
  if (g_cube.state() == GS_WON) {
    g_banner = String("Solved in ") + fmtClock(g_cube.elapsedMs()) + "!";
    Sfx::play(Sfx::WIN);
  }
  if (g_cube.state() == GS_LOST) {
    g_banner = "Boom.";
    Sfx::play(Sfx::BOOM);
    // Ring the mine that actually went off, so it is obvious which tap lost it.
    for (uint16_t c = 0; c < g_cube.cells(); c++)
      if (g_cube.isBoom(c)) { g_view.setHighlight((int)c, TFT_RED); break; }
  }
}

// A short tap: flag, dig, chord or fire an armed powerup, depending on mode and
// on what the cell already is.
static void gTapCell(uint16_t cell, bool longPress, uint8_t face) {
  if (g_armed >= 0) {
    uint8_t out = 0;
    if (g_cube.usePowerup((Powerup)g_armed, cell, out, face)) {
      switch (g_armed) {
        case PU_BURST:     g_banner = String("Burst revealed ") + out + " blocks";
                           Sfx::play(Sfx::BURST); break;
        case PU_LIGHTNING: g_banner = String("Lightning revealed ") + out + " blocks";
                           Sfx::play(Sfx::LIGHTNING); break;
        case PU_SONAR:     g_banner = (out == 0) ? String("Sonar: all clear")
                                                 : String("Sonar: ") + out + " mines";
                           Sfx::play(Sfx::SONAR); break;
        default:           g_banner = "Used"; break;
      }
    } else {
      g_banner = "Powerup unavailable";
    }
    g_armed = -1;
    return;
  }

  // The reference game's Controls screen maps REVEAL and CHORDING to the SAME
  // gesture (both "Hold") and FLAG to the other one — chording is not a
  // separate gesture, it is what a reveal means on an already-revealed number.
  //
  // Each action plays its own tick; a BOOM or WIN from gApplyResult is a
  // priority sound, so it pre-empts the tick when the move ends the game.
  if (!longPress) {
    bool ch = g_cube.cycleFlag(cell);
    if (ch) Sfx::play(Sfx::FLAG);
    gApplyResult(ch);
    return;
  }

  if (g_cube.stateOf(cell) == CS_REVEALED) {
    if (g_cube.adj(cell) > 0) {
      bool ch = g_cube.chord(cell);
      if (ch) Sfx::play(Sfx::CHORD);
      gApplyResult(ch);
    }
    return;
  }
  bool ch = g_cube.reveal(cell);
  if (ch) Sfx::play(Sfx::REVEAL);
  gApplyResult(ch);
}

// What each powerup does, on its own screen rather than crammed into the menu
// rows — those only have to say what you are holding.
static void powerupInfoScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("Power-up Info", true);
  int y = CONTENTY + 10;
  const int maxW = SCRW - 28;
  for (int i = 0; i < PU_COUNT && y < SCRH - 40; i++) {
    tft->setTextColor(COL_FG, COL_BG);
    tft->setTextDatum(TL_DATUM);
    drawStr(PU_NAMES[i], 12, y, 2);
    y += 18;
    // Word-wrapped, so a long blurb cannot run off the panel.
    tft->setTextColor(COL_DIM, COL_BG);
    String line = "", rest = PU_BLURBS[i];
    while (rest.length() && y < SCRH - 24) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (strWidth(cand, 2) <= maxW) line = cand;
      else { drawStr(line, 18, y, 2); y += 16; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 24) { drawStr(line, 18, y, 2); y += 16; }
    y += 6;
  }
  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

static void powerupDialog() {
  static String rows[PU_COUNT + 1];          // + the info row
  for (int i = 0; i < PU_COUNT; i++) {
    rows[i] = String(PU_NAMES[i]) + "   x" + g_cube.held((Powerup)i);
    if (i == PU_LIFESAVER && g_cube.lifeArmed()) rows[i] += "   ARMED";
  }
  rows[PU_COUNT] = "Power-up Info";

  int sel = scrollList("Power-ups", rows, PU_COUNT + 1, true, "Back", "", "");
  if (sel == PU_COUNT) { powerupInfoScreen(); return; }
  if (sel < 0 || sel >= PU_COUNT) return;
  if (g_cube.held((Powerup)sel) == 0) { g_banner = "None left"; return; }

  // Lifesaver takes no target: activating it arms it there and then. It is
  // never spent without the player asking for it.
  if (!g_cube.needsTarget((Powerup)sel)) {
    uint8_t out = 0;
    if (g_cube.usePowerup(PU_LIFESAVER, 0, out)) { g_banner = "Lifesaver armed"; Sfx::play(Sfx::LIFE_ARM); }
    else g_banner = out ? "Lifesaver already armed" : "Not yet - make a move first";
    g_armed = -1;
    return;
  }

  g_armed = sel;
  g_banner = (sel == PU_BURST)     ? "Tap a block to burst"
           : (sel == PU_LIGHTNING) ? "Tap a block to strike"
                                   : "Tap a block to scan";
}

static void gameScreen() {
  if (!g_cube.ready()) return;

  if (!g_view.ok() && !g_view.begin(tft, 0, G_CUBY, SCRW, G_CUBH)) {
    msgScreen("Mastermine", "Out of PSRAM",
              String("The cube needs an off-screen buffer of about ")
              + (uint32_t)(SCRW * G_CUBH * 2 / 1024) +
              " KB. Check Tools -> PSRAM -> Enabled.", TFT_RED);
    return;
  }
  g_view.attach(&g_cube, &g_skin);
  g_view.refit();

  tft->fillScreen(gameBg());
  gDrawHeader(true);
  gDrawBanner();
  gDrawNav(true);

  bool  dirty = true;
  int   downN = 0;
  int   px = 0, py = 0;
  uint32_t downT = 0;
  bool  moved = false, fired = false;
  float velYaw = 0, velPitch = 0;
  float pinch0 = 0, zoom0 = 1.0f;
  uint32_t lastT = millis(), lastClock = 0;
  String lastBanner = g_banner;

  // Drag smoothing. The FT6336 updates around 60 Hz while the cube now renders
  // faster than that, so a raw coordinate read repeats for a few frames and
  // then jumps when a fresh sample lands; capacitive coordinates also jitter a
  // few pixels even with the finger still. Driving the rotation from a smoothed
  // finger position (fx,fy easing toward the raw read) turns both the
  // stair-steps and the jitter into continuous motion. Higher = snappier and
  // less smooth; lower = smoother and laggier.
  const float DRAG_SMOOTH = 0.45f;
  float fx = 0, fy = 0, pfx = 0, pfy = 0;
  // Bridge brief capacitive dropouts: the sensor occasionally reports "no
  // touch" for a single frame mid-drag. Treated as a real lift it would zero
  // the fling velocity and reset the reference point, chopping the gesture into
  // a jump. Pretend the last touch persists for up to this many dropped frames.
  const int TOUCH_BRIDGE = 2;
  int lostFrames = 0;

  // Frame-rate profiler. Set MM_PROFILE 0 to remove it. While the cube is in
  // motion it prints, once a second over Serial, how a frame divides between
  // the sprite clear, the block fill and the SPI push — so tuning is aimed at
  // whichever actually dominates instead of guessed at.
#define MM_PROFILE 1
#if MM_PROFILE
  uint32_t profT0 = millis();
  uint32_t profN = 0, profFrameUs = 0, profWorstUs = 0;
  uint32_t profClear = 0, profBlocks = 0, profPush = 0;
  uint32_t profLastFrame = micros();
#endif

  for (;;) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    uint8_t n = gTouch(x1, y1, x2, y2);
    uint32_t now = millis();
    float dt = (now - lastT) / 1000.0f;
    if (dt <= 0) dt = 0.001f;
    lastT = now;

    // Bridge a brief single-finger dropout: keep the last touch alive, held at
    // its last position, so one dropped frame does not end the drag. Only for a
    // one-finger drag — a pinch would need the second point, which we do not
    // keep across frames, so a two-finger dropout is left to release normally.
    if (n == 0 && downN == 1 && lostFrames < TOUCH_BRIDGE) {
      lostFrames++;
      n = 1; x1 = px; y1 = py;
    } else if (n >= 1) {
      lostFrames = 0;
    }

    // Press edge
    if (n && !downN) {
      px = x1; py = y1; downT = now; moved = false; fired = false;
      velYaw = velPitch = 0;
      lostFrames = 0;
      if (n >= 2) { pinch0 = hypotf((float)(x2 - x1), (float)(y2 - y1)); zoom0 = g_view.zoom(); }
    }

    // Two fingers: pinch to zoom, and nothing else.
    //
    // A twist-to-roll gesture was tried here and removed: the FT6336 does not
    // keep the two contacts in a stable order, so the pair swaps identity
    // between frames and the angle between them jumps by half a turn, which
    // threw the cube around at random. Rotation stays a one-finger drag.
    if (n >= 2) {
      float d = hypotf((float)(x2 - x1), (float)(y2 - y1));
      if (pinch0 < 8.0f) { pinch0 = d; zoom0 = g_view.zoom(); }
      if (d > 8.0f) {
        g_view.setZoom(zoom0 * (d / pinch0));
        g_view.setFast(true);
        moved = true;
        dirty = true;
      }
    } else if (n == 1 && downN) {
      int dx = x1 - px, dy = y1 - py;
      if (!moved && (abs(dx) > 5 || abs(dy) > 5)) {
        // Start the smoother here, at the current point, so the accumulated
        // pre-move distance is not applied as one initial rotation kick.
        moved = true;
        fx = pfx = (float)x1; fy = pfy = (float)y1;
      }
      if (moved) {
        // Drag to orbit, driven by the SMOOTHED finger position (see the note
        // on DRAG_SMOOTH). Dividing by the viewport makes a full swipe about a
        // half turn regardless of panel size. The x term is positive so that
        // dragging right spins the cube's near face to the right, i.e. the
        // surface follows the finger.
        fx += DRAG_SMOOTH * ((float)x1 - fx);
        fy += DRAG_SMOOTH * ((float)y1 - fy);
        float dyaw   = (fx - pfx) * 3.2f / (float)SCRW;
        float dpitch = (fy - pfy) * 3.2f / (float)G_CUBH;
        pfx = fx; pfy = fy;
        g_view.orbit(dyaw, dpitch);
        // Track velocity as an EMA so the fling on release reflects the smoothed
        // motion, not whatever the last single (possibly held) frame did.
        velYaw   += 0.4f * (dyaw / dt - velYaw);
        velPitch += 0.4f * (dpitch / dt - velPitch);
        g_view.setFast(true);
        px = x1; py = y1;
        dirty = true;
      } else if (!fired && (now - downT) > HOLD_MS) {
        // Long press fires at the threshold rather than on release: waiting for
        // the lift makes "hold to dig" feel unresponsive.
        if (backTapped(px, py) || py >= SCRH - NAVH) {
          // header/footer — leave to the release handler
        } else {
          uint8_t face = CUBE_FACES;
          int cell = g_view.pick(px, py, &face);
          if (cell >= 0) { gTapCell((uint16_t)cell, true, face); fired = true; dirty = true; }
        }
      }
    }

    // Release edge
    if (!n && downN) {
      g_view.setFast(false);
      dirty = true;
      if (!moved && !fired) {
        if (backTapped(px, py)) {
          g_view.setHighlight(-1, 0);
          g_cube.save(SAVE_DIR "/auto.sav");
          return;
        }
#ifndef HAS_MULTITOUCH
        int gz = gZoomHit(px, py);
        if (gz >= 0) {
          g_view.setZoom(g_view.zoom() * (gz ? 1.25f : 0.8f));
          downN = n;
          dirty = true;
          delay(4);
          continue;
        }
#endif
        int nh = navHit(px, py);
        if (nh == 0) {
          int pa = pauseDialog();
          if (pa == PA_MENU) {
            g_cube.save(SAVE_DIR "/auto.sav");
            g_view.setHighlight(-1, 0);
            return;
          }
          if (pa == PA_NEWGAME) {
            // Build a fresh board with the current difficulty and play on. The
            // new game autosaves on its own exit, so the old one is simply
            // replaced — no point checkpointing it first.
            if (!startGame()) { g_view.setHighlight(-1, 0); return; }
            g_view.setHighlight(-1, 0);
            g_view.refit();
            g_view.resetCamera();
            g_view.setZoom(1.0f);
          } else {
            g_cube.save(SAVE_DIR "/auto.sav");   // Resume: checkpoint
          }
          tft->fillScreen(gameBg());
          gDrawHeader(true); gDrawBanner(); gDrawNav(true);
          dirty = true;
        }
        else if (nh == 1) { if (g_armed >= 0) { g_armed = -1; g_banner = ""; }
                            else powerupDialog();
                            tft->fillScreen(gameBg());
                            gDrawHeader(true); gDrawBanner(); gDrawNav(true); }
        else if (nh == 2) { g_view.resetCamera(); g_view.setZoom(1.0f); }
        else if (py >= G_CUBY && py < G_CUBY + G_CUBH) {
          uint8_t face = CUBE_FACES;
          int cell = g_view.pick(px, py, &face);
          if (cell >= 0) gTapCell((uint16_t)cell, false, face);
        }
        gDrawNav();
      } else if (fabsf(velYaw) > 0.4f || fabsf(velPitch) > 0.4f) {
        // Keep the spin going, same feel as the momentum lists.
      } else {
        velYaw = velPitch = 0;
      }
    }

    // Momentum spin, decaying like the list fling.
    if (!n && (fabsf(velYaw) > 0.05f || fabsf(velPitch) > 0.05f)) {
      g_view.orbit(velYaw * dt, velPitch * dt);
      float decay = expf(-2.6f * dt);
      velYaw *= decay;
      velPitch *= decay;
      dirty = true;
    }

    downN = n;

    // A sonar ping shows the nearby mines and fades them back to hidden over a
    // second, so the view has to keep repainting for as long as it lasts even
    // though nothing is being touched.
    if (g_cube.sonarActive()) dirty = true;

    // While the cube is moving, nothing else may draw to the panel: a drag or a
    // momentum spin is repainting the whole cube region every frame, and the
    // header draws straight to the panel over the same SPI bus. Repainting it
    // mid-motion cost an SPI stall twice a second — a periodic hitch in an
    // otherwise smooth rotation. Defer it until the cube comes to rest; the
    // clock/mem readout is never more than a breath stale.
    const bool moving = dirty && (moved || fabsf(velYaw) > 0.05f ||
                                  fabsf(velPitch) > 0.05f);
    if (g_banner != lastBanner) { lastBanner = g_banner; gDrawBanner(); }
    if (!moving && now - lastClock > 500) { lastClock = now; gDrawHeader(); }

    if (dirty) {
      g_view.render(gameBg());
      dirty = false;
#if MM_PROFILE
      if (moving) {
        const uint32_t nowUs = micros();
        const uint32_t frame = nowUs - profLastFrame;
        profLastFrame = nowUs;
        profN++;
        profFrameUs += frame;
        if (frame > profWorstUs) profWorstUs = frame;
        profClear  += g_view.usClear();
        profBlocks += g_view.usBlocks();
        profPush   += g_view.usPush();
      }
#endif
    }
#if MM_PROFILE
    else {
      profLastFrame = micros();          // don't count idle gaps as a frame
    }
    if (profN && millis() - profT0 >= 1000) {
      Serial.printf("cube: %lu fps  frame avg %lu us (worst %lu)  "
                    "clear %lu  blocks %lu  push %lu\n",
                    (unsigned long)profN,
                    (unsigned long)(profFrameUs / profN),
                    (unsigned long)profWorstUs,
                    (unsigned long)(profClear / profN),
                    (unsigned long)(profBlocks / profN),
                    (unsigned long)(profPush / profN));
      profT0 = millis();
      profN = profFrameUs = profWorstUs = 0;
      profClear = profBlocks = profPush = 0;
    }
#endif
    Sfx::update();
    delay(2);
  }
}

//  New game / settings / about
// The size and mine count a new game would use. Named difficulties carry the
// reference game's own mine counts; a custom cube uses a density instead.
static uint8_t curN() { return (g_diff == DIFF_CUSTOM) ? g_n : DIFF_N[g_diff]; }
static uint16_t curMines() {
  if (g_diff != DIFF_CUSTOM) return DIFF_MINES[g_diff];
  return (uint16_t)(((uint32_t)cubeCellCount(g_n) * g_pct + 50) / 100);
}

static bool startGame() {
  const uint8_t  n     = curN();
  const uint16_t mines = curMines();

  tft->fillScreen(COL_BG);
  drawHeader("New Game", true);
  tft->setTextColor(COL_DIM, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(String("Building ") + n + "x" + n + " cube...", SCRW / 2, SCRH / 2 - 12, 2);
  drawStr(String(mines) + " mines in " + cubeCellCount(n) + " blocks",
          SCRW / 2, SCRH / 2 + 12, 2);
  tft->setTextDatum(TL_DATUM);

  ledBusy();
  bool ok = g_cube.begin(n, mines);
  ledOff();
  if (!ok) {
    msgScreen("New Game", "Could not build the cube",
              "Out of memory at that size. Try a smaller cube in Settings.", TFT_RED);
    return false;
  }
  g_gameActive = true;
  g_banner = "";
  g_armed = -1;
  return true;
}

static void newGameFlow() {
  if (!g_skin.loaded()) skinLoad(g_skinId);
  if (!startGame()) return;
  gameScreen();
}

static void aboutScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("About", true);

#ifdef MARAUDER_V8
  const int dName = 26, dSub = 17, dAuth = 18, dRule = 6, dRow = 17, dGap = 2, dRule2 = 6, dCred = 16;
  const int valX = 92;
#else
  const int dName = 32, dSub = 22, dAuth = 24, dRule = 10, dRow = 21, dGap = 4, dRule2 = 8, dCred = 20;
  const int valX = 120;
#endif

  int cx = SCRW / 2, y = CONTENTY + 12;

  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(FW_NAME, cx, y, 4); y += dName;
  drawStr(String("Version ") + FW_VERSION, cx, y, 2); y += dSub;
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("UI by " FW_AUTHOR, cx, y, 2); y += dAuth;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(1, theme.edge())); y += dRule;

  tft->setTextDatum(TL_DATUM);
  auto row = [&](const char *label, const String &value) {
    tft->setTextColor(COL_DIM, COL_BG); drawStr(label, 16, y, 2);
    tft->setTextColor(COL_FG, COL_BG);  drawStr(value, valX, y, 2);
    y += dRow;
  };
  row("Board",   BOARD_NAME);
  row("MCU",     BOARD_MCU);
  row("Display", BOARD_DISPLAY);
  row("Touch",   BOARD_TOUCH);
  {
    size_t ps = ESP.getPsramSize();
    if (ps >= 1024 * 1024)  row("PSRAM", String((unsigned)((ps + 512 * 1024) / (1024 * 1024))) + " MB");
    else if (ps > 0)        row("PSRAM", String((unsigned)(ps / 1024)) + " KB");
    else                    row("PSRAM", "None");
  }
  row("Built",   __DATE__);
  row("Commit",  FW_COMMIT);

  y += dGap;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(2, theme.edge())); y += dRule2;

  // Credits, word-wrapped. They were fixed single lines and ran off the right
  // edge — "Skins are player-made, from mastermine.app" is wider than the V8
  // panel and wider than the Pancake's once the margins are taken off.
  tft->setTextColor(COL_DIM, COL_BG);
  tft->setTextDatum(TL_DATUM);
  const int credMaxW = SCRW - 32;
  static const char *const CREDITS[] = {
    "After Mastermine by Adam Soutar",
    "Skins are player-made, from mastermine.app",
  };
  for (int c = 0; c < 2 && y < SCRH - 30; c++) {
    String line = "", rest = CREDITS[c];
    while (rest.length() && y < SCRH - 30) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (strWidth(cand, 2) <= credMaxW) line = cand;
      else { drawStr(line, 16, y, 2); y += dCred; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 30) { drawStr(line, 16, y, 2); y += dCred; }
  }

  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

#ifdef HAS_CAP_TOUCH
static const int SET_N = 13;
#else
static const int SET_N = 14;
#endif
static const int SET_CHIP_LAST = 9;    // rows 0..9 carry chips (3 is an info row)

static String setChipVal(int row) {
  switch (row) {
    case 0: return theme.themeName();
    case 1: return theme.accentName();
    case 2: return theme.fontColName();
    case 4: return DIFF_NAMES[g_diff];
    case 5: return String(curN()) + "x" + curN();
    // Shown against the block total, because the number of blocks in a shell
    // is not something you can eyeball from N (an 8-cube is 296, not 384).
    case 6: return String(curMines()) + "/" + cubeCellCount(curN());
    case 7: return String(theme.bright + 1) + "/20";
    case 8: return String(theme.led_bright) + "/20";
    case 9: return theme.sound ? "On" : "Off";
  }
  return "";
}

static void drawSettingRowSpr(TFT_eSprite &g, int row, int y) {
  switch (row) {
    case 0: sprChipRow(g, y, "Theme",      setChipVal(0), false, 0, row); break;
    case 1: sprChipRow(g, y, "Accent",     setChipVal(1), false, 0, row); break;
    case 2: sprChipRow(g, y, "Font Color", setChipVal(2), false, theme.fontColPreview(), row); break;
    case 3: sprInfoRow(g, y, "Skin",       g_skin.loaded() ? String(g_skin.name()) : String("Built-in"), row); break;
    case 4: sprChipRow(g, y, "Difficulty", setChipVal(4), false, 0, row); break;
    case 5: sprChipRow(g, y, "Cube Size",  setChipVal(5), true,  0, row); break;
    case 6: sprChipRow(g, y, "Mines",      setChipVal(6), true,  0, row); break;
    case 7: sprChipRow(g, y, "Brightness", setChipVal(7), true,  0, row); break;
    case 8: sprChipRow(g, y, "LED",        setChipVal(8), true,  0, row); break;
    case 9: sprChipRow(g, y, "Sound",      setChipVal(9), false, 0, row); break;
    case 10: sprInfoRow(g, y, "WiFi Setup", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""), row); break;
    case 11: sprInfoRow(g, y, "WiFi Debug", "", row); break;
    case 12: sprInfoRow(g, y, "About",      "", row); break;
#ifndef HAS_CAP_TOUCH
    case 13: sprInfoRow(g, y, "Calibrate Touch", "", row); break;
#endif
  }
}

// Editing Cube Size or Mines only makes sense for a custom cube, so touching
// either switches the difficulty to Custom rather than silently doing nothing.
static void setGoCustom() {
  if (g_diff == DIFF_CUSTOM) return;
  g_n = DIFF_N[g_diff];
  // Carry the preset's density over so the numbers do not jump when the row is
  // first touched.
  uint16_t cells = cubeCellCount(g_n);
  g_pct = cells ? (uint8_t)(((uint32_t)DIFF_MINES[g_diff] * 100 + cells / 2) / cells) : 17;
  if (g_pct < 5) g_pct = 5;
  if (g_pct > 40) g_pct = 40;
  g_diff = DIFF_CUSTOM;
}

static void settingsFlow() {
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY;
  const int total = SET_N * ITEMH;

  tft->fillScreen(COL_BG);
  drawHeader("Settings", true);

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  sprFont = nullptr;

  // The settings list is sprite-only — the rows scroll, and scrolling a
  // direct-drawn list flashes. Say so rather than leaving a blank screen.
  if (!haveSpr) {
    msgScreen("Settings", "Out of memory",
              String("Settings needs an off-screen buffer of about ")
              + (uint32_t)(SCRW * CH * 2 / 1024) +
              " KB. Check Tools -> PSRAM -> Enabled.", TFT_RED);
    return;
  }

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (haveSpr) {
      spr.fillSprite(COL_BG);
      for (int i = 0; i < SET_N; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawSettingRowSpr(spr, i, y);
      }
      sprScrollBar(spr, CH, total, scroll);
      spr.pushSprite(0, CY);
    }
  };
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t tx = touch->x(), ty = touch->y();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) {
          if (haveSpr) spr.deleteSprite();
          ledOff();
          cfgSave();
          return;
        }
        int rowY = (int)pY - CY + (int)scroll;
        int row = rowY / ITEMH;
        if (row >= 0 && row < SET_N && rowY >= 0) {
          // Chip hit-testing works in the row's own coordinates, so feed it the
          // scroll-corrected y rather than the raw touch.
          int localY = rowY - row * ITEMH;
          int h = (row <= SET_CHIP_LAST)
                    ? chipHit(0, setChipVal(row), pX, (uint16_t)localY) : -1;
          if (row != 8) ledOff();       // LED preview only while on the LED row
          switch (row) {
            case 0: if (h >= 0) { theme.cycleTheme(h); theme.save(); applyThemeToViewManager();
                                  tft->fillScreen(COL_BG); drawHeader("Settings", true); } break;
            case 1: if (h >= 0) { theme.cycleAccent(h); theme.save(); applyThemeToViewManager(); } break;
            case 2: if (h >= 0) { theme.cycleFontCol(h); theme.save(); applyThemeToViewManager();
                                  drawHeader("Settings", true); } break;
            case 3: if (haveSpr) spr.deleteSprite();
                    skinsScreen();
                    tft->fillScreen(COL_BG); drawHeader("Settings", true);
                    if (haveSpr) { haveSpr = (spr.createSprite(SCRW, CH) != nullptr); sprFont = nullptr; }
                    break;
            case 4: if (h >= 0) { g_diff = (uint8_t)((g_diff + (h ? 1 : DIFF_COUNT - 1)) % DIFF_COUNT); } break;
            case 5: if (h >= 0) { setGoCustom();
                                  if (h == 0 && g_n > CUBE_N_MIN) g_n--;
                                  else if (h == 1 && g_n < CUBE_N_MAX) g_n++; } break;
            case 6: if (h >= 0) { setGoCustom();
                                  if (h == 0 && g_pct > 5) g_pct--;
                                  else if (h == 1 && g_pct < 40) g_pct++; } break;
            case 7: if (h == 0 && theme.bright > 0)  theme.bright--;
                    else if (h == 1 && theme.bright < 19) theme.bright++;
                    if (h >= 0) { theme.save(); applyBrightness(); } break;
            case 8: if (h == 0 && theme.led_bright > 0)  theme.led_bright--;
                    else if (h == 1 && theme.led_bright < 20) theme.led_bright++;
                    if (h >= 0) { theme.save(); ledWifi(); } break;
            case 9: if (h >= 0) { theme.sound = !theme.sound; theme.save();
                                  Sfx::setEnabled(theme.sound);
                                  if (theme.sound) Sfx::play(Sfx::LIFE_ARM); } break;
            case 10: if (haveSpr) spr.deleteSprite();
                     wifiSetup();
                     tft->fillScreen(COL_BG); drawHeader("Settings", true);
                     if (haveSpr) { haveSpr = (spr.createSprite(SCRW, CH) != nullptr); sprFont = nullptr; }
                     break;
            case 11: if (haveSpr) spr.deleteSprite();
                     wifiDebug();
                     tft->fillScreen(COL_BG); drawHeader("Settings", true);
                     if (haveSpr) { haveSpr = (spr.createSprite(SCRW, CH) != nullptr); sprFont = nullptr; }
                     break;
            case 12: if (haveSpr) spr.deleteSprite();
                     aboutScreen();
                     tft->fillScreen(COL_BG); drawHeader("Settings", true);
                     if (haveSpr) { haveSpr = (spr.createSprite(SCRW, CH) != nullptr); sprFont = nullptr; }
                     break;
#ifndef HAS_CAP_TOUCH
            case 13: if (haveSpr) spr.deleteSprite();
                     touchCalRun();
                     tft->fillScreen(COL_BG); drawHeader("Settings", true);
                     if (haveSpr) { haveSpr = (spr.createSprite(SCRW, CH) != nullptr); sprFont = nullptr; }
                     break;
#endif
            default: break;
          }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }

    wasDown = down;
    if (need) render();
    Sfx::update();
    delay(12);
  }
}

// Main menu (H4W9-style large rounded buttons)
static const char *MENU_ITEMS[] = { "New Game", "Continue", "Skins", "Settings" };
static const int    MENU_COUNT  = 4;
static const int    MENU_MARGIN = 16;
static const int    MENU_TOP    = CONTENTY + 12;
static const int    MENU_GAP    = 12;
static int menuBtnH() {
  int avail = SCRH - MENU_TOP - 12;
  return (avail - (MENU_COUNT - 1) * MENU_GAP) / MENU_COUNT;
}
static int menuBtnY(int i) { return MENU_TOP + i * (menuBtnH() + MENU_GAP); }
static int menuButtonAt(uint16_t x, uint16_t y) {
  if ((int)x < MENU_MARGIN || (int)x >= SCRW - MENU_MARGIN) return -1;
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int by = menuBtnY(i);
    if ((int)y >= by && (int)y < by + bh) return i;
  }
  return -1;
}

static void drawMenu() {
  tft->fillScreen(COL_BG);
  drawHeader(FW_NAME, false);
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = menuBtnY(i);
    tft->fillRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, COL_ACCENT);
    tft->drawRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, theme.neon(i * 3, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
#ifdef MARAUDER_V8
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 2);
#else
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 4);
#endif
  }
  tft->setTextDatum(TL_DATUM);
}

static void openMenuItem(int i) {
  switch (i) {
    case 0: newGameFlow(); break;
    case 1: {                                   // Continue
      if (!g_skin.loaded()) skinLoad(g_skinId);
      if (g_gameActive && g_cube.ready()) { gameScreen(); break; }
      if (g_cube.load(SAVE_DIR "/auto.sav")) {
        g_gameActive = true;
        g_banner = "";
        g_armed = -1;
        gameScreen();
      } else {
        msgScreen("Continue", "No saved game",
                  "Start a new game - it is saved automatically when you leave "
                  "the cube.", COL_DIM);
      }
      break;
    }
    case 2: skinsScreen();  break;
    case 3: settingsFlow(); break;
    default: break;
  }
  drawMenu();
}

static bool mainMenuStart(ViewManager *viewManager) {
  drawMenu();
  return true;
}

static void mainMenuRun(ViewManager *viewManager) {
  static bool wasDown = false;
  TouchInput *t = viewManager->getInputManager()->getTouch();
  bool down = t->isPressed();
  if (down && !wasDown) {
    uint16_t x = t->x(), y = t->y();
    int btn = menuButtonAt(x, y);
    if (btn >= 0) { Sfx::play(Sfx::MODE_ENTER); openMenuItem(btn); }
  }
  wasDown = down;

  // Idle refresh of ONLY the header status corner + memory readout. The menu
  // buttons depend on neither, so never repaint the whole screen here — doing
  // so made the menu flash repeatedly while a background connect cycled.
  static uint32_t lastRefresh = 0;
  static int lastStatus = -2;
  static bool lastConn = false;
  if (WiFi.status() != lastStatus || g_wifiConnecting != lastConn || millis() - lastRefresh > 4000) {
    lastRefresh = millis();
    lastStatus  = WiFi.status();
    lastConn    = g_wifiConnecting;
    drawHeaderStatus();
    drawHeaderMem();
  }
}

static const PROGMEM View mainMenuView = View("MainMenu", mainMenuRun, mainMenuStart, nullptr);

// Arduino entry points
void setup() {
  randomSeed(esp_random());
#ifndef DEVELOPER
  esp_log_level_set("*", ESP_LOG_NONE);
#endif

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);
  Serial.println(F("[" BOARD_NAME "] Mastermine starting..."));

  // Backlight off during init (PWM).
  pinMode(TFT_BL, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 0);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);
#endif

  // SD (shared FSPI bus on ESP32-C5) — must be up before ViewManager (Storage).
#ifdef HAS_C5_SD
  sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  delay(100);
  if (!SD.begin(SD_CS, sharedSPI)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
  else Serial.println(F("[" BOARD_NAME "] SD OK"));
#else
  if (!SD.begin(SD_CS)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
#endif

  if (!SPIFFS.begin(true)) Serial.println(F("[" BOARD_NAME "] SPIFFS mount failed"));
  else                     Serial.println(F("[" BOARD_NAME "] SPIFFS OK"));

#ifdef HAS_PSRAM
  // The render sprite is a few hundred KB and lives in PSRAM. Without it the
  // game screen reports "Out of PSRAM" — check Tools -> PSRAM -> Enabled.
  if (!psramInit()) Serial.println(F("[" BOARD_NAME "] PSRAM unavailable"));
#endif

#ifdef HAS_CAP_TOUCH
  ft6336_init();                       // also opens the shared I2C bus
#else
  Wire.begin(I2C_SDA, I2C_SCL, 400000U);
#endif
  battInit();

  theme.load();
  cfgLoad();

  Sfx::init();
  Sfx::setEnabled(theme.sound);

  ledOff();

#ifdef MARAUDER_V8
  vm    = new ViewManager(MarauderV8Config);
#else
  vm    = new ViewManager(PancakeConfig);
#endif
  tft   = vm->getDraw()->display->getTFT();
  touch = vm->getInputManager()->getTouch();
  applyThemeToViewManager();

  applyBrightness();

#ifndef HAS_CAP_TOUCH
  if (touch) touch->attachTFT(tft);
  touchCalInit();
#endif

  // Make sure the data directories exist, so a first download has somewhere to
  // land without every caller checking.
  if (!SD.exists(MINE_DIR)) SD.mkdir(MINE_DIR);
  if (!SD.exists(SKIN_DIR)) SD.mkdir(SKIN_DIR);
  if (!SD.exists(SAVE_DIR)) SD.mkdir(SAVE_DIR);

  // Powerup tuning lives in a hand-editable XML file on the card; the first
  // run writes it out with the defaults so there is something to edit.
  g_pu.load(PU_FILE);

  WiFi.onEvent(wifiEvent);
  WiFi.mode(WIFI_STA);

  // Show the menu at once, then connect to saved WiFi in the background. The
  // skin is loaded lazily for the same reason — it is an SD read and the menu
  // should not wait on it.
  vm->add(&mainMenuView);
  vm->set("MainMenu");
  wifiBgBegin();
  drawHeaderStatus();

  Serial.println(F("[" BOARD_NAME "] Ready."));
}

void loop() {
  vm->run();
  wifiBgTick();
  Sfx::update();

  // Reconnect watchdog: ONLY on the drop edge (connected -> lost), make one
  // reconnect pass. If it fails we stay disconnected rather than retrying
  // forever — the LED goes off instead of pulsing amber.
  static bool wasConnected = false;
  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (wasConnected && !nowConnected && !g_manualDisconnect && (g_wb == WB_DONE || g_wb == WB_IDLE)) {
    wifiBgBegin();
  }
  wasConnected = nowConnected;

  static bool ledState = false;
  if (g_wifiConnecting != ledState) { ledState = g_wifiConnecting; ledSet(ledState); }

  delay(5);
}
