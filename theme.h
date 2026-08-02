// theme.h
// Theme + accent + font-colour + brightness system for Pancake Picoware, ported
// from the H4W9 firmware (GitHub/H4W9). Named colour themes
// (bg/fg/header/dim); a 24-colour accent palette AND a 24-entry font-colour
// palette, each remembered PER THEME with a "Default" option; brightness. All
// persisted to SPIFFS (Marauder-style).

// Ported from ESP32_Scrabble, less its board-palette section: that drove the
// Scrabble tile colours out of board_theme.h, and here the tiles come from skin
// textures instead. Theme 0 is the same palette, renamed — it was called "Word
// Master" after Scrabble's reference app, which means nothing in this firmware.
#pragma once
#include <Arduino.h>
#include <SPIFFS.h>

// Colour themes (RGB565): bg, fg, header, dim, dark?, name
struct ThemeDef { uint16_t bg, fg, hdr, dim; bool dark; const char *name; };
static const ThemeDef PW_THEMES[] = {
    // Default. A near-black slate that lets a lit cube carry the screen.
    { 0x10C3, 0xFFFF, 0x1905, 0x7BEF, true,  "Slate"   },
    { 0x0000, 0xFFFF, 0x1082, 0x7BEF, true,  "Dark"    },
    { 0xFFFF, 0x0000, 0x4A69, 0x632C, false, "Light"   },
    { 0xF717, 0x51E3, 0x8B26, 0xB4AD, false, "Sepia"   },
    { 0x0000, 0xAD55, 0x0841, 0x4208, true,  "Night"   },
    { 0x2104, 0xE73C, 0x4209, 0x8410, true,  "Gray"    },
    { 0x0866, 0xFFFF, 0x190C, 0x73D4, true,  "Navy"    },
    { 0x0141, 0xCF99, 0x0242, 0x646C, true,  "Forest"  },
    { 0x28E2, 0xE6B6, 0x51C5, 0x93CB, true,  "Mocha"   },
    { 0x0105, 0xBF5E, 0x020A, 0x5CB4, true,  "Ocean"   },
    { 0x2085, 0xE65D, 0x420A, 0x9B74, true,  "Plum"    },
    { 0x0000, 0xFD80, 0x28E0, 0x82C0, true,  "Amber"   },
    { 0x2883, 0xF6BA, 0x5905, 0xAB6F, true,  "Rose"    },
    { 0xE7BD, 0x21C5, 0x650F, 0x7D51, false, "Mint"    },
    { 0x30C8, 0xFD4B, 0x718A, 0xD3CC, true,  "Sunset"  },
    { 0x0842, 0x07F9, 0x0249, 0x04B1, true,  "Cyber"   },
    { 0x20C2, 0xD5D1, 0x51C4, 0x93CB, true,  "Coffee"  },
    { 0xE79F, 0x1989, 0x6497, 0x8516, false, "Arctic"  },
    { 0x1801, 0xFE59, 0x6802, 0xBB0D, true,  "Crimson" },
    { 0xEF5F, 0x394A, 0x8BD7, 0x9476, false, "Lavender"},
    { 0x0000, 0xFFFF, 0x0841, 0x4208, true,  "Neon"    },
};
static const uint8_t PW_THEME_COUNT = 21;
static const uint8_t PW_THEME_NEON  = 20;
static const uint8_t PW_THEME_SLATE = 0;

static const uint16_t PW_NEON_HUES[] = {
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x041F, 0x781F, 0xF81F
};
static const uint8_t PW_NEON_COUNT = 8;

// Accent palette (selection highlight): 24 darks + 24 lights
static const uint16_t PW_ACCENT_DARK[] = {
    0x0460, 0x0282, 0x03CA, 0x0350, 0x03D9, 0x047B, 0x0291, 0x0339,
    0x0213, 0x2813, 0x5016, 0x500F, 0x6009, 0xA00C, 0x8803, 0x5800,
    0xB1E3, 0x6181, 0x7280, 0x8240, 0xA360, 0xA460, 0x52E0, 0x3504,
};
static const uint16_t PW_ACCENT_LIGHT[] = {
    0xB696, 0xB737, 0xB79A, 0xAF3C, 0xBF7D, 0xBEFF, 0xB69E, 0xBEDF,
    0xC69F, 0xCDBF, 0xD5DF, 0xE61F, 0xFDBB, 0xFDDB, 0xFE5A, 0xFDB7,
    0xFE15, 0xDDF4, 0xFD40, 0xFEAD, 0xFF12, 0xFFD5, 0xCEF3, 0xCFF3,
};
static const char *const PW_ACCENT_NAMES[] = {
    "Green","Forest","Mint","Teal","Cyan","Sky","Steel","Blue",
    "Navy","Indigo","Violet","Purple","Magenta","Pink","Crimson","Red",
    "Coral","Brown","Orange","Amber","Gold","Yellow","Olive","Lime",
};
static const uint8_t PW_ACCENT_COUNT = 24;

// Font-colour palette (H4W9 Font Color). Index 0 = "Default" (theme fg).
static const char *const PW_FONTCOL_NAMES[] = {
    "Default", "White", "Silver", "Light Gray", "Gray", "Dim Gray",
    "Dark Gray", "Charcoal", "Black",
    "Red", "Orange", "Amber", "Yellow", "Lime", "Green", "Teal",
    "Cyan", "Sky", "Blue", "Indigo", "Purple", "Magenta", "Pink", "Brown"
};
static const uint16_t PW_FONTCOL_VAL[] = {
    0x0000, 0xFFFF, 0xC618, 0xAD55, 0x8410, 0x6B4D, 0x4208, 0x2104, 0x0000,
    0xF800, 0xFC60, 0xFD20, 0xFFE0, 0xAFE5, 0x07E0, 0x0594, 0x07FF, 0x5D1F,
    0x001F, 0x4019, 0x801F, 0xF81F, 0xFD9F, 0xA145,
};
static const uint8_t PW_FONTCOL_COUNT = 24;

// Theme state (persisted to SPIFFS: /pico_ui.dat)
// acc_by_theme / fc_by_theme are indexed by theme; value 0 = "Default", so each
// theme keeps its own accent + font-colour choices (defaults per theme).
struct Theme {
  uint8_t theme_idx  = PW_THEME_SLATE;
  uint8_t bright     = 15;                     // screen backlight, 0..19
  uint8_t led_bright = 4;                      // RGB status LED, 0..20 (0 = off)
  bool    sound      = true;                   // buzzer sound effects on/off
  uint8_t acc_by_theme[PW_THEME_COUNT];        // 0 = Default (themeHighlight), else accent+1
  uint8_t fc_by_theme[PW_THEME_COUNT];         // 0 = Default (theme fg), else PW_FONTCOL_VAL idx

  Theme() {
    for (uint8_t i = 0; i < PW_THEME_COUNT; i++) { acc_by_theme[i] = 0; fc_by_theme[i] = 0; }
  }

  uint8_t  ti()   const { return theme_idx % PW_THEME_COUNT; }
  uint8_t  accSel() const { return acc_by_theme[ti()]; }
  uint8_t  fcSel()  const { return fc_by_theme[ti()]; }

  uint16_t bg()      const { return PW_THEMES[ti()].bg; }
  uint16_t themeFg() const { return PW_THEMES[ti()].fg; }
  uint16_t hdr()     const { return PW_THEMES[ti()].hdr; }
  uint16_t dim()     const { return PW_THEMES[ti()].dim; }
  bool     dark()    const { return PW_THEMES[ti()].dark; }

  // Global font colour: Default follows the theme fg, else the palette colour.
  uint16_t fg() const {
    uint8_t f = fcSel();
    return (f == 0 || f >= PW_FONTCOL_COUNT) ? themeFg() : PW_FONTCOL_VAL[f];
  }
  // Per-theme "Default" accent: blend bg toward fg ~32% (H4W9 themeHighlight).
  uint16_t themeHighlight() const {
    uint16_t a = bg(), b = themeFg();
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg5 = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int t = 82;
    int r = ar + (br - ar) * t / 255, g = ag + (bg5 - ag) * t / 255, bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
  }
  uint16_t sel() const {
    if (accSel() == 0) return themeHighlight();
    uint8_t a = (accSel() - 1) % PW_ACCENT_COUNT;
    return dark() ? PW_ACCENT_DARK[a] : PW_ACCENT_LIGHT[a];
  }

  const char *themeName()   const { return PW_THEMES[ti()].name; }
  bool        accentIsDefault() const { return accSel() == 0; }
  const char *accentName()  const { return accSel() == 0 ? "Default"
                                          : PW_ACCENT_NAMES[(accSel() - 1) % PW_ACCENT_COUNT]; }
  const char *fontColName() const { return PW_FONTCOL_NAMES[fcSel() % PW_FONTCOL_COUNT]; }
  // Colour to draw the Font Color value text in (previews the choice).
  uint16_t    fontColPreview() const {
    uint8_t f = fcSel();
    return (f == 0 || f >= PW_FONTCOL_COUNT) ? themeFg() : PW_FONTCOL_VAL[f];
  }
  uint16_t edge() const { return dark() ? 0x2104 : 0xC618; }

  bool     isNeon() const { return ti() == PW_THEME_NEON; }
  uint16_t neon(int seed, uint16_t def) const {
    return isNeon() ? PW_NEON_HUES[((seed % PW_NEON_COUNT) + PW_NEON_COUNT) % PW_NEON_COUNT] : def;
  }
  uint8_t duty() const {
    int d = 5 + (int)bright * (250 - 5) / 19;
    if (d < 5) d = 5; if (d > 255) d = 255;
    return (uint8_t)d;
  }

  void cycleTheme(bool fwd) {
    theme_idx = fwd ? (uint8_t)((ti() + 1) % PW_THEME_COUNT)
                    : (uint8_t)(ti() == 0 ? PW_THEME_COUNT - 1 : ti() - 1);
  }
  void cycleAccent(bool fwd) {           // 0=Default, 1..24 accents
    uint8_t total = PW_ACCENT_COUNT + 1, &c = acc_by_theme[ti()];
    c = fwd ? (uint8_t)((c + 1) % total) : (uint8_t)(c == 0 ? total - 1 : c - 1);
  }
  void cycleFontCol(bool fwd) {
    uint8_t &c = fc_by_theme[ti()];
    c = fwd ? (uint8_t)((c + 1) % PW_FONTCOL_COUNT)
            : (uint8_t)(c == 0 ? PW_FONTCOL_COUNT - 1 : c - 1);
  }

  void defaults() {
    theme_idx = PW_THEME_SLATE; bright = 15; led_bright = 4; sound = true;
    for (uint8_t i = 0; i < PW_THEME_COUNT; i++) { acc_by_theme[i] = 0; fc_by_theme[i] = 0; }
    // Slate reads flat under the generic bg->fg blend, so give it an explicit
    // accent (PW_ACCENT_NAMES[7] = "Blue").
    acc_by_theme[PW_THEME_SLATE] = 8;
  }
  void load() {
    defaults();
    File f = SPIFFS.open("/pico_ui.dat", FILE_READ);
    if (f && f.size() >= (size_t)(2 + 2 * PW_THEME_COUNT)) {
      theme_idx = (uint8_t)f.read();
      bright    = (uint8_t)f.read();
      f.read(acc_by_theme, PW_THEME_COUNT);
      f.read(fc_by_theme, PW_THEME_COUNT);
      if (f.available() > 0) led_bright = (uint8_t)f.read();   // newer files append this
      if (f.available() > 0) sound = (f.read() != 0);          // ...and this, newer still
    }
    if (f) f.close();
    if (theme_idx >= PW_THEME_COUNT) theme_idx = PW_THEME_SLATE;
    if (bright > 19) bright = 15;
    if (led_bright > 20) led_bright = 4;
    for (uint8_t i = 0; i < PW_THEME_COUNT; i++) {
      if (acc_by_theme[i] > PW_ACCENT_COUNT)   acc_by_theme[i] = 0;
      if (fc_by_theme[i]  >= PW_FONTCOL_COUNT)  fc_by_theme[i]  = 0;
    }
  }
  void save() {
    File f = SPIFFS.open("/pico_ui.dat", FILE_WRITE);
    if (!f) return;
    f.write(theme_idx);
    f.write(bright);
    f.write(acc_by_theme, PW_THEME_COUNT);
    f.write(fc_by_theme, PW_THEME_COUNT);
    f.write(led_bright);
    f.write(sound ? 1 : 0);
    f.close();
  }
};
