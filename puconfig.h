#pragma once
#ifndef puconfig_h
#define puconfig_h

#include <Arduino.h>
#include "cube.h"

// Powerup tuning, kept in a hand-editable XML file on the SD card
// (/mastermine/powerups.xml) so the numbers can be changed without a reflash.
//
//   <powerups>
//     <award every="40"/>
//     <powerup name="burst"     start="1" max="3"/>
//     <powerup name="lightning" start="1" max="3"/>
//     <powerup name="lifesaver" start="1" max="3"/>
//     <powerup name="sonar"     start="1" max="3"/>
//   </powerups>
//
// The file is written with these defaults the first time it is missing, so
// there is always something to edit. Parsing is deliberately forgiving: it
// scans for tags and attributes rather than validating a document, unknown
// names are ignored, and anything malformed just leaves the default in place.
// A game console should not refuse to start over a stray angle bracket.

struct PowerupConfig {
  uint8_t  start[PU_COUNT];
  uint8_t  maxHeld[PU_COUNT];
  uint16_t every;          // safe reveals between awards; 0 disables awards

  void defaults();
  // Load from `path`, writing the default file first if it is not there.
  // Returns false only if the card could not be read or written at all.
  bool load(const char *path);
  bool save(const char *path) const;

  // XML attribute name for a powerup, matching PU_* order.
  static const char *key(uint8_t p);
};

extern PowerupConfig g_pu;

#endif // puconfig_h
