// TouchKeyboard.h
// Self-contained touch QWERTY keyboard for Pancake Picoware.
// Modeled on the H4W9 touch keyboard (a self-contained touch keyboard) but stripped of
// the VLW-font / German / search-option baggage: uses built-in TFT_eSPI fonts,
// draws directly to the panel, and returns plain ASCII.
//
// Occupies the bottom half of the screen; the typed text previews in the top half.
// Layout (5 rows × 10 columns):
//   Row 0: 1 2 3 4 5 6 7 8 9 0
//   Row 1: q w e r t y u i o p
//   Row 2: a s d f g h j k l        (centered, 9 keys)
//   Row 3: z x c v b n m .   [CAPS CAPS]
//   Row 4: [X][SYM] [ SPACE ×6 ] [<X][OK]

#pragma once
#include "configs.h"

#ifdef HAS_TOUCH

#include <TFT_eSPI.h>
#include <stddef.h>

// Blocking touch-keyboard dialog.
//   tft      display to render on (drawn directly, bypassing any sprite buffer)
//   fg/bg    foreground/background colours
//   buffer   caller-allocated; may contain a pre-fill string on entry, holds the
//            null-terminated result on exit
//   bufLen   total size of buffer including the null terminator
//   title    optional prompt shown above the input box (may be nullptr)
//   password when true, typed characters preview as '*'
// Returns true if OK was pressed, false on Cancel.
bool touchKeyboardInput(TFT_eSPI& tft,
                        uint16_t  fg,
                        uint16_t  bg,
                        char*     buffer,
                        size_t    bufLen,
                        const char* title    = nullptr,
                        bool        password = false);

#endif // HAS_TOUCH
