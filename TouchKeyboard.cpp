// TouchKeyboard.cpp
// Self-contained touch QWERTY keyboard for Pancake Picoware.
// See TouchKeyboard.h. Renders with the VLW smooth fonts and the FT6336 driver.

#include "TouchKeyboard.h"

#ifdef HAS_TOUCH

#include <string.h>
#include <Arduino.h>
#include "vlw.h"

// Smooth-font wrappers, local to the keyboard because it draws onto a TFT_eSPI
// reference passed in rather than the sketch's global. Same contract as the
// sketch's drawStr()/strWidth(): call sites keep the old font numbers, text is
// in private-byte encoding and is converted to UTF-8 at draw time.
static const uint8_t *kb_vlw_cur = nullptr;
static void kbStr(TFT_eSPI &tft, const char *s, int32_t x, int32_t y, uint8_t f) {
  vlwLoad(tft, f, kb_vlw_cur);
  char buf[160];
  vlwPrivToUtf8(s, buf, sizeof(buf));
  tft.drawString(buf, x, y);
}
static void kbCentre(TFT_eSPI &tft, const char *s, int32_t x, int32_t y, uint8_t f) {
  uint8_t d = tft.getTextDatum();
  tft.setTextDatum(TC_DATUM);
  kbStr(tft, s, x, y, f);
  tft.setTextDatum(d);
}
static int16_t kbWidth(const char *s, uint8_t f) {
  return vlwTextWidth(vlwData(f), s);
}

#ifdef HAS_CAP_TOUCH
#  include "ft6336.h"
#else
#  define KB_XPT_THRESHOLD 600
#endif

// Layout
static const int KB_ROWS = 5;   // 4 char rows + 1 control row
static const int KB_COLS = 10;

static const char ROW0_ALPHA[] = "1234567890";
static const char ROW1_ALPHA[] = "qwertyuiop";
static const char ROW2_ALPHA[] = "asdfghjkl";
static const char ROW3_ALPHA[] = "zxcvbnm.";   // cols 0-7; cols 8-9 = CAPS key

static const char ROW0_SYM[] = "!@#$%^&*()";
static const char ROW1_SYM[] = "`~-_=+[]{}";
static const char ROW2_SYM[] = "\\|;:'\"<>";
// German letters live on the symbol layer. They are the firmware's PRIVATE byte
// codes (0x80 Ä, 0x82 Ö, 0x84 Ü, 0x86 ß), not UTF-8, so each stays one byte and
// the row stays indexable by column like every other row. The VLW font draws
// the real glyph for them, and normaliseWord() maps ß to SS when looking a word
// up (there is no eszett tile).
static const char ROW3_SYM[] = ",./?\x80\x82\x84\x86";   // centered

// Control row (row 4) columns:
//  0 = CANCEL   1 = SYM/ABC   2..7 = SPACE (6 cols)   8 = BKSP   9 = OK

enum KbLayout { KB_ALPHA = 0, KB_SYMBOLS };

// Geometry helpers — keyboard occupies the bottom half of the screen.
static inline int16_t kbH(uint16_t sh)   { return (int16_t)(sh / 2); }
static inline int16_t kbY(uint16_t sh)   { return (int16_t)(sh - kbH(sh)); }
static inline int16_t cellW(uint16_t sw) { return (int16_t)(sw / KB_COLS); }
static inline int16_t cellH(uint16_t sh) { return (int16_t)(kbH(sh) / KB_ROWS); }

// Touch polling (raw, no debounce — debounce handled below)
static bool kb_rawTouch(TFT_eSPI& tft, uint16_t* x, uint16_t* y) {
#ifdef HAS_CAP_TOUCH
    if (!ft6336_update(x, y)) return false;
    // FT6336 reports panel-native (portrait) coords; map to the active rotation so
    // they match the keyboard's tft.width()/height() layout.
    uint8_t  rot = tft.getRotation() & 3;
    uint16_t W0  = (rot & 1) ? (uint16_t)tft.height() : (uint16_t)tft.width();
    uint16_t H0  = (rot & 1) ? (uint16_t)tft.width()  : (uint16_t)tft.height();
    uint16_t rx = *x, ry = *y;
    switch (rot) {
        case 0: *x = rx;                      *y = ry;                      break;
        case 1: *x = ry;                      *y = (uint16_t)(W0 - 1 - rx); break;
        case 2: *x = (uint16_t)(W0 - 1 - rx); *y = (uint16_t)(H0 - 1 - ry); break;
        case 3: *x = (uint16_t)(H0 - 1 - ry); *y = rx;                      break;
    }
    return true;
#else
    return tft.getTouch(x, y, KB_XPT_THRESHOLD);
#endif
}

// Drawing
// Bounds of the SHOW/HIDE toggle (password mode only). Sits just below the
// input box, right-aligned. Geometry is deterministic from scrW + title so both
// the drawing code and the touch handler can compute it identically.
static void passToggleRect(uint16_t scrW, const char* title,
                           int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    int16_t titleH = (title && title[0]) ? 22 : 0;
    int16_t boxY = 6 + titleH;
    int16_t boxH = 26;
    w = 66;
    h = 22;
    x = (int16_t)scrW - 8 - w;
    y = boxY + boxH + 4;
}

// Input-box / cursor helpers
// Rect of the framed input box (so taps in it can reposition the cursor).
static void kbBoxRect(uint16_t scrW, const char* title,
                      int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    int16_t titleH = (title && title[0]) ? 22 : 0;
    x = 4; y = 6 + titleH; w = (int16_t)scrW - 8; h = 26;
}
// Pixel width of the buffer bytes from index a up to b, as drawn (mask => '*'), font 2.
static int16_t kbSubW(TFT_eSPI& tft, const char* buf, size_t a, size_t b, bool mask) {
    if (b <= a) return 0;
    char tmp[132]; size_t k = 0;
    for (size_t i = a; i < b && k < sizeof(tmp) - 1; i++) tmp[k++] = mask ? '*' : buf[i];
    tmp[k] = '\0';
    return (int16_t)kbWidth(tmp, 2);
}
// Character index nearest a tap at `relx` (px from the text's left edge).
static size_t kbIndexAt(TFT_eSPI& tft, const char* buf, size_t viewStart, int16_t relx, bool mask) {
    size_t n = strlen(buf);
    if (relx <= 0) return viewStart;
    int16_t acc = 0;
    for (size_t i = viewStart; i < n; i++) {
        char c = mask ? '*' : buf[i]; char one[2] = { c, '\0' };
        int16_t cw = (int16_t)kbWidth(one, 2);
        if (relx < acc + cw / 2) return i;
        acc += cw;
    }
    return n;
}
// Largest viewStart that still shows the end of the text (right-aligned to fit
// maxW). Clamps manual swipe-scrolling so it can't run off past the last char.
static size_t kbMaxView(TFT_eSPI& tft, const char* buf, int16_t maxW, bool mask) {
    size_t s = strlen(buf);
    int16_t w = 0;
    while (s > 0) {
        char c = mask ? '*' : buf[s - 1]; char one[2] = { c, '\0' };
        int16_t cw = (int16_t)kbWidth(one, 2);
        if (w + cw > maxW) break;
        w += cw; s--;
    }
    return s;
}
// Insert/delete at the cursor (not just the end).
static bool kbInsert(char* buf, size_t bufLen, size_t& cursor, char c) {
    size_t len = strlen(buf);
    if (len + 1 >= bufLen) return false;
    if (cursor > len) cursor = len;
    memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);   // shift incl. NUL
    buf[cursor] = c;
    cursor++;
    return true;
}
static bool kbBackspace(char* buf, size_t& cursor) {
    if (cursor == 0) return false;
    size_t len = strlen(buf);
    memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
    cursor--;
    return true;
}

// Partial redraw for typing/cursor moves: only the input-box interior (text +
// caret) and the char-count header. No top-area clear → the top doesn't flash.
static void drawTextLine(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, uint16_t scrH,
                         const char* title, const char* buffer,
                         size_t cursor, size_t& viewStart, size_t bufLen,
                         bool password, bool reveal, bool followCursor = true) {
    (void)scrH;
    int16_t titleH = (title && title[0]) ? 22 : 0;
    int16_t boxY = 6 + titleH, boxH = 26;
    int16_t tx = 8, ty = boxY + (boxH - 16) / 2;
    int16_t maxW = (int16_t)scrW - 8 - 12;
    const char* p = buffer ? buffer : "";
    size_t n = strlen(p);
    if (cursor > n) cursor = n;
    bool mask = password && !reveal;

    // Clear only the interior of the framed box.
    tft.fillRect(5, boxY + 1, (int16_t)scrW - 10, boxH - 2, bg);

    // Horizontal scroll. Follow mode keeps the cursor inside the visible window;
    // manual (swipe) mode honours the caller's viewStart as-is.
    if (followCursor) {
        if (cursor < viewStart) viewStart = cursor;
        while (viewStart < cursor && kbSubW(tft, p, viewStart, cursor, mask) > maxW) viewStart++;
    }

    char vis[132]; size_t k = 0; int16_t w = 0;
    for (size_t i = viewStart; i < n && k < sizeof(vis) - 1; i++) {
        char c = mask ? '*' : p[i]; char one[2] = { c, '\0' };
        int16_t cw = (int16_t)kbWidth(one, 2);
        if (w + cw > maxW) break;
        vis[k++] = c; w += cw;
    }
    vis[k] = '\0';
    tft.setTextColor(fg, bg);
    tft.setTextDatum(TL_DATUM);
    kbStr(tft, vis, tx, ty, 2);

    // Caret — only when the cursor falls within the visible window (when the text
    // is swiped so the cursor is off-screen, no caret is shown).
    if (cursor >= viewStart) {
        int16_t caretX = tx + kbSubW(tft, p, viewStart, cursor, mask);
        if (caretX <= (int16_t)scrW - 8) tft.fillRect(caretX, ty, 2, 16, fg);
    }

    // Char count "used/max" in the top-right of the header row.
    char cnt[20];
    snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)n, (unsigned)(bufLen > 0 ? bufLen - 1 : 0));
    int16_t cw = (int16_t)kbWidth(cnt, 2);
    tft.fillRect((int16_t)scrW - 8 - cw, 4, cw + 6, 18, bg);
    tft.setTextColor((uint16_t)0x8410, bg);   // gray — legible on light and dark
    tft.setTextDatum(TR_DATUM);
    kbStr(tft, cnt, (int16_t)scrW - 6, 6, 2);
    tft.setTextDatum(TL_DATUM);
}

// Full redraw of the whole top area (title, box frame, SHOW/HIDE toggle, text).
static void drawTextArea(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, uint16_t scrH,
                         const char* title, const char* buffer,
                         size_t cursor, size_t& viewStart, size_t bufLen,
                         bool password, bool reveal) {
    int16_t areaH = kbY(scrH);
    tft.fillRect(0, 0, (int16_t)scrW, areaH, bg);

    int16_t y = 6;
    if (title && title[0]) {
        tft.setTextColor(TFT_GREEN, bg);
        kbStr(tft, title, 4, y, 2);
        y += 22;
    }
    int16_t boxY = y, boxH = 26;
    tft.drawRect(4, boxY, (int16_t)scrW - 8, boxH, fg);

    if (password) {
        int16_t bx, by, bw, bh;
        passToggleRect(scrW, title, bx, by, bw, bh);
        uint16_t f = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x18E3;
        tft.fillRoundRect(bx, by, bw, bh, 4, f);
        tft.drawRoundRect(bx, by, bw, bh, 4, fg);
        tft.setTextColor(fg, f);
        tft.setTextDatum(MC_DATUM);
        kbStr(tft, reveal ? "HIDE" : "SHOW", bx + bw / 2, by + bh / 2, 2);
        tft.setTextDatum(TL_DATUM);
    }

    drawTextLine(tft, fg, bg, scrW, scrH, title, buffer, cursor, viewStart, bufLen, password, reveal);
}

// Key identity for the control row + press feedback.
enum KeyKind { KK_NONE, KK_CHAR, KK_CAPS, KK_CANCEL, KK_LAYOUT, KK_SPACE, KK_BKSP, KK_OK };

// Control row (row 4) geometry. SPACE is 4 cells wide and centered; the four
// surrounding keys (CANCEL, SYM, BKSP, OK) are each 1.5 cells. Widening the side
// keys pushes OK and BKSP apart so an OK meant as backspace is far less likely.
// drawKeyboard and kbHit both derive their layout from here so they stay in sync.
struct CtrlKey { KeyKind kind; int16_t x, w; };
static const int KB_CTRL_N = 5;
static void kbCtrlKeys(int16_t cW, CtrlKey out[KB_CTRL_N]) {
    int16_t hw = cW / 2;                                   // half a cell
    out[0] = { KK_CANCEL, 0,                    (int16_t)(cW + hw) };
    out[1] = { KK_LAYOUT, (int16_t)(cW + hw),   (int16_t)(cW + hw) };
    out[2] = { KK_SPACE,  (int16_t)(3 * cW),    (int16_t)(4 * cW) };
    out[3] = { KK_BKSP,   (int16_t)(7 * cW),    (int16_t)(cW + hw) };
    out[4] = { KK_OK,     (int16_t)(8 * cW + hw), (int16_t)(cW + hw) };
}

// upper = render letters uppercase; capsMode = CAPS key look (0 off, 1 shift-once, 2 lock).
static void drawKeyboard(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                         uint16_t scrW, uint16_t scrH,
                         KbLayout layout, bool upper, int capsMode) {
    int16_t kY = kbY(scrH);
    int16_t kH = kbH(scrH);
    int16_t cW = cellW(scrW);
    int16_t cH = cellH(scrH);

    uint16_t key_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t key_fg = fg;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;

    tft.fillRect(0, kY, (int16_t)scrW, kH, key_bg);

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    // Rows 0-3: character rows
    for (int r = 0; r < 4; r++) {
        const char* row    = rows[r];
        int         rowLen = (int)strlen(row);
        int16_t     rowY   = kY + (int16_t)r * cH;

        int16_t xOff;
        if (layout == KB_ALPHA && r == 3) xOff = 0;                        // left-aligned for CAPS
        else xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);                // centered

        for (int i = 0; i < rowLen; i++) {
            int16_t kx = (int16_t)i * cW + xOff;
            tft.drawRect(kx, rowY, cW, cH, bdr);
            char c = row[i];
            if (layout == KB_ALPHA && r >= 1 && c >= 'a' && c <= 'z' && upper)
                c = (char)(c - 'a' + 'A');
            int16_t tcy = rowY + (cH - 16) / 2;
            tft.setTextColor(key_fg, key_bg);
            char c_str[2] = { c, '\0' };
            kbCentre(tft, c_str, kx + cW / 2, tcy, 2);
        }

        // CAPS key: cols 8-9 in alpha row 3.
        //   off (0) = grey "CAPS", shift-once (1) = yellow "Caps", lock (2) = green "CAPS".
        if (layout == KB_ALPHA && r == 3) {
            int16_t cx  = 8 * cW;
            int16_t cw2 = 2 * cW;
            uint16_t caps_bg = (capsMode == 2) ? (uint16_t)0x07E0
                             : (capsMode == 1) ? (uint16_t)0xFFE0 : key_bg;
            uint16_t caps_fg = (capsMode == 0) ? key_fg : (uint16_t)TFT_BLACK;
            const char* caps_lbl = (capsMode == 1) ? "Caps" : "CAPS";
            tft.fillRect(cx, rowY, cw2, cH, caps_bg);
            tft.drawRect(cx, rowY, cw2, cH, bdr);
            tft.setTextColor(caps_fg, caps_bg);
            kbCentre(tft, caps_lbl, cx + cW, rowY + (cH - 16) / 2, 2);
        }
    }

    // Row 4: control row (SPACE narrowed to 4 cells; side keys widened to 1.5).
    int16_t rowY  = kY + 4 * cH;
    int16_t ctrlY = rowY + (cH - 16) / 2;
    CtrlKey ck[KB_CTRL_N];
    kbCtrlKeys(cW, ck);

    // CANCEL
    tft.drawRect(ck[0].x, rowY, ck[0].w, cH, bdr);
    tft.setTextColor(TFT_RED, key_bg);
    kbCentre(tft, "X", ck[0].x + ck[0].w / 2, ctrlY, 2);

    // SYM / ABC toggle
    tft.drawRect(ck[1].x, rowY, ck[1].w, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    kbCentre(tft, layout == KB_ALPHA ? "SYM" : "ABC", ck[1].x + ck[1].w / 2, ctrlY, 2);

    // SPACE
    tft.fillRect(ck[2].x, rowY, ck[2].w, cH, key_bg);
    tft.drawRect(ck[2].x, rowY, ck[2].w, cH, bdr);
    tft.setTextColor(key_fg, key_bg);
    kbCentre(tft, "SPACE", ck[2].x + ck[2].w / 2, ctrlY, 2);

    // BKSP (left-arrow glyph)
    tft.drawRect(ck[3].x, rowY, ck[3].w, cH, bdr);
    {
        int16_t ax = ck[3].x + ck[3].w / 2 - 5;
        int16_t ay = rowY + cH / 2;
        tft.fillRect(ax + 3, ay - 3, 1, 1, key_fg);
        tft.fillRect(ax + 2, ay - 2, 2, 1, key_fg);
        tft.fillRect(ax + 1, ay - 1, 3, 1, key_fg);
        tft.fillRect(ax + 0, ay + 0, 4, 1, key_fg);
        tft.fillRect(ax + 1, ay + 1, 3, 1, key_fg);
        tft.fillRect(ax + 2, ay + 2, 2, 1, key_fg);
        tft.fillRect(ax + 3, ay + 3, 1, 1, key_fg);
        tft.fillRect(ax + 4, ay + 0, 6, 1, key_fg);
    }

    // OK
    tft.fillRect(ck[4].x, rowY, ck[4].w, cH, (uint16_t)0x07E0);
    tft.drawRect(ck[4].x, rowY, ck[4].w, cH, bdr);
    tft.setTextColor((uint16_t)TFT_BLACK, (uint16_t)0x07E0);
    kbCentre(tft, "OK", ck[4].x + ck[4].w / 2, ctrlY, 2);
}


// Unified hit-test: which key is under (tx,ty), its on-screen rect, and (for a
// character key) the resulting char given `upper`. Geometry matches drawKeyboard.
struct KeyHit { KeyKind kind; int16_t kx, ky, kw, kh; char ch; };

static KeyHit kbHit(uint16_t tx, uint16_t ty, uint16_t scrW, uint16_t scrH,
                    KbLayout layout, bool upper) {
    KeyHit h; h.kind = KK_NONE; h.ch = 0; h.kx = h.ky = h.kw = h.kh = 0;
    int16_t kY = kbY(scrH), kH = kbH(scrH), cW = cellW(scrW), cH = cellH(scrH);
    if ((int16_t)ty < kY || (int16_t)ty >= kY + kH) return h;
    int row = ((int16_t)ty - kY) / cH;
    if (row < 0 || row >= KB_ROWS) return h;
    int16_t rowY = kY + (int16_t)row * cH;

    const char* alphaRows[4] = { ROW0_ALPHA, ROW1_ALPHA, ROW2_ALPHA, ROW3_ALPHA };
    const char* symRows[4]   = { ROW0_SYM,   ROW1_SYM,   ROW2_SYM,   ROW3_SYM   };
    const char** rows = (layout == KB_ALPHA) ? alphaRows : symRows;

    if (row <= 3) {
        const char* rowStr = rows[row];
        int rowLen = (int)strlen(rowStr);
        if (layout == KB_ALPHA && row == 3) {
            if ((int16_t)tx >= 8 * cW) {                         // CAPS cols 8-9
                h.kind = KK_CAPS; h.kx = 8 * cW; h.ky = rowY; h.kw = 2 * cW; h.kh = cH;
                return h;
            }
            int col = (int16_t)tx / cW;
            if (col < 0 || col >= rowLen) return h;
            char c = rowStr[col];
            if (c >= 'a' && c <= 'z' && upper) c = (char)(c - 'a' + 'A');
            h.kind = KK_CHAR; h.ch = c; h.kx = (int16_t)col * cW; h.ky = rowY; h.kw = cW; h.kh = cH;
            return h;
        }
        int16_t xOff = (int16_t)((KB_COLS - rowLen) * cW / 2);
        int col = ((int16_t)tx - xOff) / cW;
        if (col < 0 || col >= rowLen) return h;
        char c = rowStr[col];
        if (layout == KB_ALPHA && row >= 1 && c >= 'a' && c <= 'z' && upper)
            c = (char)(c - 'a' + 'A');
        h.kind = KK_CHAR; h.ch = c; h.kx = (int16_t)col * cW + xOff; h.ky = rowY; h.kw = cW; h.kh = cH;
        return h;
    }

    // Control row 4 — same geometry as drawKeyboard (SPACE 4 cells, sides 1.5).
    h.ky = rowY; h.kh = cH;
    CtrlKey ck[KB_CTRL_N];
    kbCtrlKeys(cW, ck);
    for (int i = 0; i < KB_CTRL_N; i++) {
        if ((int16_t)tx >= ck[i].x && (int16_t)tx < ck[i].x + ck[i].w) {
            h.kind = ck[i].kind; h.kx = ck[i].x; h.kw = ck[i].w; break;
        }
    }
    return h;
}

// Restore ONE key to its normal look (used on release so only the pressed key
// repaints — no full-keyboard flash). Only needed for keys whose neighbours don't
// change: character, space, backspace. (Layout/CAPS changes redraw the whole board.)
static void drawOneKey(TFT_eSPI& tft, const KeyHit& h, uint16_t fg, uint16_t bg) {
    uint16_t key_bg = (bg == TFT_WHITE) ? (uint16_t)0xBDF7 : (uint16_t)0x1082;
    uint16_t key_fg = fg;
    uint16_t bdr    = (bg == TFT_WHITE) ? (uint16_t)0x8430 : (uint16_t)0x4208;
    tft.fillRect(h.kx, h.ky, h.kw, h.kh, key_bg);
    tft.drawRect(h.kx, h.ky, h.kw, h.kh, bdr);
    int16_t cx = h.kx + h.kw / 2, ty = h.ky + (h.kh - 16) / 2;
    tft.setTextColor(key_fg, key_bg);
    if (h.kind == KK_CHAR) { char s[2] = { h.ch, '\0' }; kbCentre(tft, s, cx, ty, 2); }
    else if (h.kind == KK_SPACE) kbCentre(tft, "SPACE", cx, ty, 2);
    else if (h.kind == KK_BKSP) {                       // backspace arrow glyph
        int16_t ax = h.kx + h.kw / 2 - 5, ay = h.ky + h.kh / 2;
        tft.fillRect(ax + 3, ay - 3, 1, 1, key_fg);
        tft.fillRect(ax + 2, ay - 2, 2, 1, key_fg);
        tft.fillRect(ax + 1, ay - 1, 3, 1, key_fg);
        tft.fillRect(ax + 0, ay + 0, 4, 1, key_fg);
        tft.fillRect(ax + 1, ay + 1, 3, 1, key_fg);
        tft.fillRect(ax + 2, ay + 2, 2, 1, key_fg);
        tft.fillRect(ax + 3, ay + 3, 1, 1, key_fg);
        tft.fillRect(ax + 4, ay + 0, 6, 1, key_fg);
    }
}

// Momentary press feedback: draw the key inverted (fg fill, bg text).
static void kbFlashKey(TFT_eSPI& tft, const KeyHit& h, uint16_t fg, uint16_t bg) {
    if (h.kind == KK_NONE) return;
    tft.fillRect(h.kx, h.ky, h.kw, h.kh, fg);
    int16_t cx = h.kx + h.kw / 2, ty = h.ky + (h.kh - 16) / 2;
    tft.setTextColor(bg, fg);
    char s[2] = { h.ch, '\0' };
    switch (h.kind) {
        case KK_CHAR:   kbCentre(tft, s, cx, ty, 2); break;
        case KK_SPACE:  kbCentre(tft, "SPACE", cx, ty, 2); break;
        case KK_LAYOUT: kbCentre(tft, "SYM", cx, ty, 2); break;
        case KK_CANCEL: kbCentre(tft, "X", cx, ty, 2); break;
        case KK_OK:     kbCentre(tft, "OK", cx, ty, 2); break;
        case KK_CAPS:   kbCentre(tft, "CAPS", cx, ty, 2); break;
        case KK_BKSP:   kbCentre(tft, "<", cx, ty, 2); break;
        default: break;
    }
}

// Public API
bool touchKeyboardInput(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                        char* buffer, size_t bufLen,
                        const char* title, bool password) {
    if (!buffer || bufLen < 2) return false;

    uint16_t scrW = (uint16_t)tft.width();
    uint16_t scrH = (uint16_t)tft.height();
    KbLayout layout   = KB_ALPHA;
    bool     capsLock = false;   // all-caps (hold CAPS)
    bool     shiftOnce = false;  // uppercase next letter only (tap CAPS)
    bool     reveal   = false;
    size_t   cursor    = strlen(buffer);   // insertion point within the text
    size_t   viewStart = 0;                // first visible char (horizontal scroll)

    auto upperNow = [&]() { return capsLock || shiftOnce; };
    auto capsMode = [&]() { return capsLock ? 2 : (shiftOnce ? 1 : 0); };
    // Partial redraw (typing / cursor move) vs. full (reveal toggle, first draw).
    auto redrawText = [&]() { drawTextLine(tft, fg, bg, scrW, scrH, title, buffer, cursor, viewStart, bufLen, password, reveal); };
    auto redrawAll  = [&]() { drawTextArea(tft, fg, bg, scrW, scrH, title, buffer, cursor, viewStart, bufLen, password, reveal); };

    const uint32_t CAPS_HOLD_MS = 450;   // hold CAPS this long → caps-lock
    const uint32_t MIN_FLASH_MS = 55;    // keep the press highlight visible at least this long

    redrawAll();
    drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());

    for (;;) {
        uint16_t tx = 0, ty = 0;
        if (!kb_rawTouch(tft, &tx, &ty)) { delay(5); yield(); continue; }

        // SHOW / HIDE toggle (password fields) — handled before the keys.
        if (password) {
            int16_t bx, by, bw, bh;
            passToggleRect(scrW, title, bx, by, bw, bh);
            if ((int16_t)tx >= bx && (int16_t)tx < bx + bw &&
                (int16_t)ty >= by && (int16_t)ty < by + bh) {
                uint16_t rx, ry; while (kb_rawTouch(tft, &rx, &ry)) { delay(8); yield(); }
                reveal = !reveal;
                redrawAll();                 // toggle label changes too → full redraw
                continue;
            }
        }

        // Input box gesture: a tap moves the cursor to that character; a horizontal
        // swipe scrolls the text so long entries can be scrolled to any edit point.
        {
            int16_t bx, by, bw, bh; kbBoxRect(scrW, title, bx, by, bw, bh);
            if ((int16_t)tx >= bx && (int16_t)tx < bx + bw &&
                (int16_t)ty >= by && (int16_t)ty < by + bh) {
                bool    mask    = password && !reveal;
                int16_t maxW    = (int16_t)scrW - 8 - 12;
                size_t  maxView = kbMaxView(tft, buffer, maxW, mask);
                int16_t downX   = (int16_t)tx, lastX = downX;
                int16_t travel  = 0, accum = 0;
                bool    swiping = false;
                uint16_t rx = tx, ry = ty;
                for (;;) {
                    if (!kb_rawTouch(tft, &rx, &ry)) break;      // released
                    int16_t cx = (int16_t)rx;
                    int16_t ad = cx - downX; if (ad < 0) ad = -ad;
                    if (ad > travel) travel = ad;
                    if (travel > 8) swiping = true;              // past jitter → it's a swipe
                    if (swiping) {
                        accum += lastX - cx;                     // >0 : finger moved left
                        while (accum > 0 && viewStart < maxView) {
                            char c = mask ? '*' : buffer[viewStart]; char one[2] = { c, '\0' };
                            int16_t cw = (int16_t)kbWidth(one, 2);
                            if (accum < cw) break;
                            accum -= cw; viewStart++;
                        }
                        while (accum < 0 && viewStart > 0) {
                            char c = mask ? '*' : buffer[viewStart - 1]; char one[2] = { c, '\0' };
                            int16_t cw = (int16_t)kbWidth(one, 2);
                            if (-accum < cw) break;
                            accum += cw; viewStart--;
                        }
                        lastX = cx;
                        drawTextLine(tft, fg, bg, scrW, scrH, title, buffer,
                                     cursor, viewStart, bufLen, password, reveal, false);
                    }
                    delay(8); yield();
                }
                if (!swiping) {                                  // tap → place the cursor
                    cursor = kbIndexAt(tft, buffer, viewStart, downX - 8, mask);
                    redrawText();
                }
                // A swipe leaves the manually-scrolled view in place (already drawn).
                continue;
            }
        }

        KeyHit h = kbHit(tx, ty, scrW, scrH, layout, upperNow());
        if (h.kind == KK_NONE) {                       // tap outside any key — wait for release
            uint16_t rx, ry; while (kb_rawTouch(tft, &rx, &ry)) { delay(8); yield(); }
            continue;
        }

        // Momentary press highlight; wait for release (or a CAPS hold), min flash time.
        kbFlashKey(tft, h, fg, bg);
        uint32_t t0 = millis();
        bool held = false;
        for (;;) {
            uint16_t rx, ry;
            bool down = kb_rawTouch(tft, &rx, &ry);
            uint32_t el = millis() - t0;
            if (h.kind == KK_CAPS && down && el >= CAPS_HOLD_MS) { held = true; break; }
            if (!down && el >= MIN_FLASH_MS) break;
            delay(8); yield();
        }
        if (held) { uint16_t rx, ry; while (kb_rawTouch(tft, &rx, &ry)) { delay(8); yield(); } }

        switch (h.kind) {
            case KK_CHAR: {
                bool oneShot = shiftOnce && !capsLock;      // this letter reverts the case
                if (kbInsert(buffer, bufLen, cursor, h.ch)) {
                    if (oneShot) shiftOnce = false;
                    redrawText();
                }
                // Only the pressed key changed, UNLESS a one-shot shift just reverted
                // every letter's case → then a full redraw is unavoidable.
                if (oneShot) drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                else         drawOneKey(tft, h, fg, bg);
                break;
            }
            case KK_SPACE:
                if (kbInsert(buffer, bufLen, cursor, ' ')) redrawText();
                drawOneKey(tft, h, fg, bg);
                break;
            case KK_BKSP:
                if (kbBackspace(buffer, cursor)) redrawText();
                drawOneKey(tft, h, fg, bg);
                break;
            case KK_LAYOUT:                                 // whole board changes → full redraw
                layout = (layout == KB_ALPHA) ? KB_SYMBOLS : KB_ALPHA;
                drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                break;
            case KK_CAPS:                                   // case + CAPS key change → full redraw
                if (held)          { capsLock = !capsLock; shiftOnce = false; }  // hold → lock
                else if (capsLock) { capsLock = false; }                        // tap while locked → off
                else               { shiftOnce = !shiftOnce; }                  // tap → one-shot shift
                drawKeyboard(tft, fg, bg, scrW, scrH, layout, upperNow(), capsMode());
                break;
            case KK_CANCEL: return false;
            case KK_OK:     return true;
            default: break;
        }
    }
}

#endif // HAS_TOUCH
