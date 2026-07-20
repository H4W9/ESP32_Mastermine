#pragma once
#ifndef skinstore_h
#define skinstore_h

#include <Arduino.h>
#include "skin.h"

// The Mastermine skin store, over WiFi.
//
//   GET https://mastermine.app/api/topSkins?pageNo=N   (no auth)
//   -> {"success":true, "skins":[ ...20... ], "eof":false}
//   Pages are ZERO-BASED; "eof" marks the last one. Each entry is
//   {id, skinName, downloadCount, score, textures:{key: url}} and every texture
//   is a 512x512 JPEG on store.mastermine.app. All of that was measured against
//   the live API, not guessed — see sd_prep/fetch_skins.py, which speaks the
//   same protocol and writes byte-identical files.
//
// Downloaded textures are decoded at 1/8 scale (src/picojpeg, reduce mode) and
// written as 64x64 .tex under SKIN_DIR, which is what skin.cpp reads.

static const int STORE_PAGE_MAX = 20;    // entries the API returns per page

struct StoreSkin {
  uint16_t id;
  uint32_t downloads;
  char     name[28];
  uint8_t  nTextures;      // how many of TEX_KEYS this skin actually defines
};

// Progress while a skin downloads. `done`/`total` count textures; `key` is the
// one in flight. Return false to abort.
typedef bool (*SkinDlProgress)(uint8_t done, uint8_t total, const char *key, void *ctx);

class SkinStore {
public:
  ~SkinStore() { release(); }

  // Fetch one page. Keeps the page's JSON so download() can find texture URLs
  // without a second round trip. Returns the entry count, or -1 on error.
  int  fetchPage(int page, StoreSkin *out, int maxN);
  bool eof() const { return _eof; }
  const char *lastError() const { return _err; }

  // Download every texture of the entry at `index` within the page most
  // recently fetched, converting to .tex under SKIN_DIR/<id>/.
  // Returns the number of textures written.
  int  download(int index, SkinDlProgress cb, void *ctx);

  void release();

private:
  bool httpGet(const String &url, uint8_t **buf, size_t *len);

  void *_doc = nullptr;      // JsonDocument, heap-allocated so PSRAM backs it
  bool  _eof = true;
  char  _err[48] = "";
};

// Decode a JPEG held in memory down to a TEX_SIZE x TEX_SIZE RGB565 buffer.
// Exposed for reuse/testing; `dst` must hold TEX_SIZE*TEX_SIZE uint16_t.
bool jpegToTex(const uint8_t *jpeg, size_t len, uint16_t *dst);

// Write a decoded texture out in the same .tex format fetch_skins.py produces.
bool texWrite(const char *path, const uint16_t *pix);

#endif // skinstore_h
