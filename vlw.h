// vlw.h
// Smooth (anti-aliased) font layer, ported from the ESP32 Library reader.
//
// The whole UI renders with VLW fonts instead of TFT_eSPI's bitmap fonts. Two
// reasons: they look far better at these sizes, and — the deciding one — the
// VLW files carry REAL Ä Ö Ü ä ö ü ß glyphs. The bitmap fonts have none, which
// is why the earlier build had to transliterate umlauts to "AE"/"OE" and paint
// a two-pixel diaeresis by hand on tiles. All of that goes away here.
//
// Text is carried around in the firmware's private byte encoding (0x80 = Ä,
// 0x81 = ä, 0x82 = Ö, … 0x86 = ß) exactly as the word lists and the reader use
// it. drawStr() converts private → UTF-8 at the point of drawing, because
// TFT_eSPI's smooth-font drawString() speaks UTF-8.
//
// Call sites keep the old TFT_eSPI font numbers (1 / 2 / 4) so the shell code
// reads the same; vlwForNum() maps them onto real pixel sizes.

#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "fonts_vlw.h"

// Map the legacy TFT_eSPI font numbers the shell uses onto VLW_FONTS indices.
//   1 (8 px bitmap)  -> 10 px smooth
//   2 (16 px bitmap) -> 14 px smooth   (the UI workhorse)
//   4 (26 px bitmap) -> 22 px smooth
static inline uint8_t vlwIdxForNum(uint8_t fontNum) {
  switch (fontNum) {
    case 1:  return 0;   // vlw_10
    case 2:  return 2;   // vlw_14
    case 4:  return 4;   // vlw_22
    case 6:  return 5;   // vlw_28 — headline
    default: return 2;
  }
}

static inline uint32_t vlwBE32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// Private code -> Unicode. Plain ASCII passes straight through.
static inline uint16_t vlwPrivToUnicode(uint8_t c) {
  switch (c) {
    case 0x80: return 0xC4; case 0x81: return 0xE4;   // Ä ä
    case 0x82: return 0xD6; case 0x83: return 0xF6;   // Ö ö
    case 0x84: return 0xDC; case 0x85: return 0xFC;   // Ü ü
    case 0x86: return 0xDF;                           // ß
    default:   return c;
  }
}

// UTF-8 -> private code, for text arriving from the touch keyboard. Returns the
// number of input bytes consumed; 0 if the sequence isn't one we can represent.
static inline uint8_t vlwUtf8ToPriv(const uint8_t *p, uint8_t &out) {
  if (p[0] < 0x80) { out = p[0]; return 1; }
  if (p[0] == 0xC3 && p[1]) {
    switch (p[1]) {
      case 0x84: out = 0x80; return 2;   // Ä
      case 0xA4: out = 0x81; return 2;   // ä
      case 0x96: out = 0x82; return 2;   // Ö
      case 0xB6: out = 0x83; return 2;   // ö
      case 0x9C: out = 0x84; return 2;   // Ü
      case 0xBC: out = 0x85; return 2;   // ü
      case 0x9F: out = 0x86; return 2;   // ß
    }
  }
  return 0;
}

// xAdvance (px) of a glyph in a VLW flash array, or -1 if the glyph is absent.
static inline int16_t vlwAdvance(const uint8_t *font, uint16_t uni) {
  uint16_t gCount = (uint16_t)vlwBE32(font);
  const uint8_t *m = font + 24;
  for (uint16_t i = 0; i < gCount; i++, m += 28)
    if ((uint16_t)vlwBE32(m) == uni) return (int16_t)(uint8_t)vlwBE32(m + 12);
  return -1;
}

// TFT_eSPI's guessed space width for a smooth font: (ascent + descent) * 2 / 7.
static inline int16_t vlwSpaceWidth(const uint8_t *font) {
  int16_t ascent  = (int16_t)vlwBE32(font + 16);
  int16_t descent = (int16_t)vlwBE32(font + 20);
  return (int16_t)(((ascent + descent) * 2) / 7);
}

// Pixel width of a private-code string — matches how TFT_eSPI advances the
// cursor (xAdvance per glyph; spaceWidth for ' '; spaceWidth+1 when absent).
static inline int16_t vlwTextWidth(const uint8_t *font, const char *s) {
  int16_t sw = vlwSpaceWidth(font), w = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    if (*p == ' ') { w += sw; continue; }
    int16_t a = vlwAdvance(font, vlwPrivToUnicode(*p));
    w += (a < 0) ? (int16_t)(sw + 1) : a;
  }
  return w;
}

// Private-code string -> UTF-8, so the smooth-font drawString renders the real
// glyphs. Umlauts are U+00xx, so two bytes each.
static inline void vlwPrivToUtf8(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && o + 3 < n; p++) {
    uint16_t u = vlwPrivToUnicode(*p);
    if (u < 0x80) {
      out[o++] = (char)u;
    } else {
      out[o++] = (char)(0xC0 | (u >> 6));
      out[o++] = (char)(0x80 | (u & 0x3F));
    }
  }
  out[o] = 0;
}

// Which VLW array is currently loaded on each target, so a redundant loadFont()
// (which re-parses the whole array) is skipped. TFT_eSPI keeps one loaded font
// per object, so tft and each sprite are tracked separately.
static const uint8_t *g_vlw_cur_tft = nullptr;

// Load a VLW font by legacy font number. Templated so the same code binds to
// TFT_eSPI and TFT_eSprite — their methods are not virtual, so a base-class
// pointer would call the wrong override (the same reason BibleDrawUTF8.h exists
// in the reader firmware).
template <class GFX>
static inline void vlwLoad(GFX &g, uint8_t fontNum, const uint8_t *&track) {
  const uint8_t *f = VLW_FONTS[vlwIdxForNum(fontNum)].data;
  if (track == f) return;
  g.loadFont(f);
  track = f;
}

// Line height (px) for a legacy font number.
static inline uint8_t vlwLineH(uint8_t fontNum) {
  return VLW_FONTS[vlwIdxForNum(fontNum)].lineH;
}
static inline const uint8_t *vlwData(uint8_t fontNum) {
  return VLW_FONTS[vlwIdxForNum(fontNum)].data;
}
