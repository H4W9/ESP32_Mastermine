#include "puconfig.h"
#include "configs.h"
#include <SD.h>

PowerupConfig g_pu;

static const char *const PU_KEYS[PU_COUNT] = {
  "burst", "lightning", "lifesaver", "sonar",
};

const char *PowerupConfig::key(uint8_t p) {
  return (p < PU_COUNT) ? PU_KEYS[p] : "";
}

void PowerupConfig::defaults() {
  for (uint8_t p = 0; p < PU_COUNT; p++) { start[p] = 1; maxHeld[p] = 3; }
  every = 40;
}

bool PowerupConfig::save(const char *path) const {
  if (!SD.exists(MINE_DIR)) SD.mkdir(MINE_DIR);
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.println(F("<powerups>"));
  f.println(F("  <!-- Edit these and restart, or start a new game. -->"));
  f.println(F("  <!-- every: safe blocks revealed between awards; 0 = never -->"));
  f.printf("  <award every=\"%u\"/>\n", (unsigned)every);
  f.println(F("  <!-- start: how many you begin a game with; max: how many you can hold -->"));
  for (uint8_t p = 0; p < PU_COUNT; p++)
    f.printf("  <powerup name=\"%s\" start=\"%u\" max=\"%u\"/>\n",
             PU_KEYS[p], (unsigned)start[p], (unsigned)maxHeld[p]);
  f.println(F("</powerups>"));
  f.close();
  return true;
}

// Pull `name="value"` out of a tag body. Returns false if absent.
static bool attrOf(const String &tag, const char *name, String &out) {
  String needle = String(name) + "=";
  int at = tag.indexOf(needle);
  if (at < 0) return false;
  int q = at + needle.length();
  if (q >= (int)tag.length()) return false;
  char quote = tag[q];
  if (quote != '"' && quote != '\'') return false;
  int end = tag.indexOf(quote, q + 1);
  if (end < 0) return false;
  out = tag.substring(q + 1, end);
  return true;
}

static uint16_t clampU16(long v, long lo, long hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (uint16_t)v;
}

bool PowerupConfig::load(const char *path) {
  defaults();

  if (!SD.exists(path)) {
    // First run: leave a file to edit rather than silently using defaults.
    return save(path);
  }

  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  // Read the whole file — it is a few hundred bytes — then walk the tags.
  String doc;
  doc.reserve(1024);
  while (f.available() && doc.length() < 4096) doc += (char)f.read();
  f.close();

  int pos = 0;
  while (true) {
    int lt = doc.indexOf('<', pos);
    if (lt < 0) break;
    int gt = doc.indexOf('>', lt + 1);
    if (gt < 0) break;
    String tag = doc.substring(lt + 1, gt);
    pos = gt + 1;
    if (tag.startsWith("!") || tag.startsWith("/") || tag.startsWith("?")) continue;

    String v;
    if (tag.startsWith("award")) {
      if (attrOf(tag, "every", v)) every = clampU16(v.toInt(), 0, 10000);
      continue;
    }
    if (!tag.startsWith("powerup")) continue;

    String nm;
    if (!attrOf(tag, "name", nm)) continue;
    nm.toLowerCase();
    int which = -1;
    for (uint8_t p = 0; p < PU_COUNT; p++) if (nm == PU_KEYS[p]) { which = p; break; }
    if (which < 0) continue;                       // unknown name: ignore it

    if (attrOf(tag, "max", v))   maxHeld[which] = (uint8_t)clampU16(v.toInt(), 0, 99);
    if (attrOf(tag, "start", v)) start[which]   = (uint8_t)clampU16(v.toInt(), 0, 99);
    if (start[which] > maxHeld[which]) start[which] = maxHeld[which];
  }
  return true;
}
