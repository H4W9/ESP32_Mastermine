#include "skinstore.h"
#include "configs.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <esp_heap_caps.h>

extern "C" {
#include "src/picojpeg/picojpeg.h"
}

static void *ssAlloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(bytes);
  return p;
}

// ArduinoJson v7 allocator that prefers PSRAM. A page of 20 skins carries 320
// texture URLs and runs to tens of kilobytes of JSON — too much to want on the
// DRAM heap next to the render sprite.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return ssAlloc(n); }
  void  deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    return q ? q : realloc(p, n);
  }
};
static PsramAllocator g_jsonAlloc;

void SkinStore::release() {
  if (_doc) { delete (JsonDocument *)_doc; _doc = nullptr; }
}

// One HTTPS GET into a PSRAM buffer.
//
// TLS is set to setInsecure() — the certificate is not verified. These are
// public, user-uploaded picture files on someone else's CDN: there is nothing
// secret going up, and the only thing a forged certificate could achieve is
// showing the player the wrong tile art. Pinning a CA we do not control would
// also break the feature silently the day mastermine.app rotates issuers.
bool SkinStore::httpGet(const String &url, uint8_t **buf, size_t *len) {
  *buf = nullptr;
  *len = 0;
  if (WiFi.status() != WL_CONNECTED) { strcpy(_err, "WiFi not connected"); return false; }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);
  if (!http.begin(client, url)) { strcpy(_err, "bad URL"); return false; }
  http.setUserAgent("ESP32_Mastermine/" FW_VERSION);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(_err, sizeof(_err), "HTTP %d", code);
    http.end();
    return false;
  }

  int total = http.getSize();
  // Anything much over a megabyte is not a 512x512 JPEG or a page of JSON.
  if (total > 0 && total > 1024 * 1024) { strcpy(_err, "response too large"); http.end(); return false; }

  size_t cap = (total > 0) ? (size_t)total : 96 * 1024;
  uint8_t *b = (uint8_t *)ssAlloc(cap + 1);
  if (!b) { strcpy(_err, "out of memory"); http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t lastData = millis();
  while (http.connected() && (total < 0 || got < (size_t)total)) {
    size_t avail = stream->available();
    if (avail) {
      if (got + avail > cap) {                       // chunked reply outgrew the guess
        if (cap >= 1024 * 1024) break;
        size_t ncap = cap * 2;
        uint8_t *nb = (uint8_t *)ssAlloc(ncap + 1);
        if (!nb) break;
        memcpy(nb, b, got);
        free(b);
        b = nb;
        cap = ncap;
      }
      int r = stream->readBytes(b + got, min(avail, cap - got));
      if (r > 0) { got += r; lastData = millis(); }
    } else {
      if (millis() - lastData > 10000) break;        // stalled
      delay(2);
    }
  }
  http.end();

  if (got == 0) { free(b); strcpy(_err, "empty response"); return false; }
  b[got] = 0;
  *buf = b;
  *len = got;
  return true;
}

int SkinStore::fetchPage(int page, StoreSkin *out, int maxN) {
  release();
  _eof = true;
  _err[0] = 0;
  if (page < 0) page = 0;

  String url = String("https://") + SKIN_API_HOST + SKIN_API_PATH + page;
  uint8_t *buf = nullptr;
  size_t   len = 0;
  if (!httpGet(url, &buf, &len)) return -1;

  JsonDocument *doc = new JsonDocument(&g_jsonAlloc);
  if (!doc) { free(buf); strcpy(_err, "out of memory"); return -1; }
  DeserializationError e = deserializeJson(*doc, (const char *)buf, len);
  free(buf);
  if (e) { delete doc; snprintf(_err, sizeof(_err), "JSON: %s", e.c_str()); return -1; }

  JsonArray arr = (*doc)["skins"].as<JsonArray>();
  if (arr.isNull()) { delete doc; strcpy(_err, "no skins in reply"); return -1; }
  _eof = (*doc)["eof"].as<bool>();

  int n = 0;
  for (JsonObject o : arr) {
    if (n >= maxN) break;
    out[n].id        = o["id"].as<uint16_t>();
    out[n].downloads = o["downloadCount"].as<uint32_t>();
    const char *nm   = o["skinName"];
    snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "?");
    JsonObject tx = o["textures"].as<JsonObject>();
    uint8_t cnt = 0;
    if (!tx.isNull())
      for (uint8_t k = 0; k < TK_COUNT; k++)
        if (!tx[TEX_KEY_NAMES[k]].isNull()) cnt++;
    out[n].nTextures = cnt;
    n++;
  }

  _doc = doc;                       // retained so download() has the URLs
  return n;
}

// picojpeg feeds itself through a callback; this walks a buffer already in RAM.
struct PjSrc {
  const uint8_t *data;
  size_t len, pos;
};
static unsigned char pjFeed(unsigned char *pBuf, unsigned char buf_size,
                            unsigned char *pBytes_actually_read, void *pCallback_data) {
  PjSrc *s = (PjSrc *)pCallback_data;
  size_t n = s->len - s->pos;
  if (n > buf_size) n = buf_size;
  memcpy(pBuf, s->data + s->pos, n);
  s->pos += n;
  *pBytes_actually_read = (unsigned char)n;
  return 0;
}

// Full-resolution decode, box-averaged down to TEX_SIZE.
//
// The DC-only "reduce" path below is exact for a 1/8 scale, but 512/8 is 64,
// and 64 px is not enough: a tile covers 60-100 px when the cube is zoomed in
// and the thin strokes many published skins use for their digits turn to mush.
// So decode properly and average into a TEX_SIZE accumulator as the MCUs come
// out — the whole 512x512 image is never held, only the 128x128 sums.
static bool jpegToTexFull(const uint8_t *jpeg, size_t len, uint16_t *dst) {
  PjSrc src = { jpeg, len, 0 };
  pjpeg_image_info_t info;
  if (pjpeg_decode_init(&info, pjFeed, &src, 0) != 0) return false;
  const int W = info.m_width, H = info.m_height;
  if (W < (int)TEX_SIZE || H < (int)TEX_SIZE) return false;   // let the caller fall back

  const size_t n = (size_t)TEX_SIZE * TEX_SIZE;
  uint32_t *acc = (uint32_t *)ssAlloc(n * 3 * sizeof(uint32_t));
  uint16_t *cnt = (uint16_t *)ssAlloc(n * sizeof(uint16_t));
  if (!acc || !cnt) { if (acc) free(acc); if (cnt) free(cnt); return false; }
  memset(acc, 0, n * 3 * sizeof(uint32_t));
  memset(cnt, 0, n * sizeof(uint16_t));

  int mx = 0, my = 0;
  bool ok = true;
  for (;;) {
    unsigned char st = pjpeg_decode_mcu();
    if (st == PJPG_NO_MORE_BLOCKS) break;
    if (st != 0) { ok = false; break; }
    if (my >= info.m_MCUSPerCol) break;

    for (int by = 0; by < info.m_MCUHeight; by += 8) {
      for (int bx = 0; bx < info.m_MCUWidth; bx += 8) {
        // Block base offset inside the MCU buffer, matching picojpeg's layout
        // (x*8 + y*16 with x,y stepping by 8); within a block the row stride
        // is 8.
        const uint32_t ofs = (uint32_t)(bx * 8) + (uint32_t)(by * 16);
        const uint8_t *pr = info.m_pMCUBufR + ofs;
        const uint8_t *pg = info.m_pMCUBufG + ofs;
        const uint8_t *pb = info.m_pMCUBufB + ofs;
        for (int yy = 0; yy < 8; yy++) {
          const int sy = my * info.m_MCUHeight + by + yy;
          if (sy >= H) break;
          const int ty = sy * TEX_SIZE / H;
          for (int xx = 0; xx < 8; xx++) {
            const int sx = mx * info.m_MCUWidth + bx + xx;
            if (sx >= W) break;
            const int tx = sx * TEX_SIZE / W;
            const size_t o = (size_t)ty * TEX_SIZE + tx;
            uint8_t r, g, b;
            if (info.m_scanType == PJPG_GRAYSCALE) {
              r = g = b = pr[yy * 8 + xx];
            } else {
              r = pr[yy * 8 + xx];
              g = pg[yy * 8 + xx];
              b = pb[yy * 8 + xx];
            }
            acc[o * 3 + 0] += r;
            acc[o * 3 + 1] += g;
            acc[o * 3 + 2] += b;
            cnt[o]++;
          }
        }
      }
    }
    if (++mx >= info.m_MCUSPerRow) { mx = 0; my++; }
  }

  if (ok) {
    for (size_t o = 0; o < n; o++) {
      uint32_t c = cnt[o] ? cnt[o] : 1;
      uint32_t r = acc[o * 3 + 0] / c, g = acc[o * 3 + 1] / c, b = acc[o * 3 + 2] / c;
      dst[o] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
  free(acc);
  free(cnt);
  return ok;
}

bool jpegToTex(const uint8_t *jpeg, size_t len, uint16_t *dst) {
  // Try the sharp path first; fall back to the DC-only one, which is cheap and
  // never fails, if the image is small or the full decode gives up.
  if (jpegToTexFull(jpeg, len, dst)) return true;

  PjSrc src = { jpeg, len, 0 };
  pjpeg_image_info_t info;

  // reduce = 1: decode only the DC coefficient of each 8x8 block, i.e. a
  // 1/8-scale image, skipping AC dequantisation, the IDCT and chroma
  // upsampling.
  if (pjpeg_decode_init(&info, pjFeed, &src, 1) != 0) return false;

  const int bpr = info.m_MCUWidth / 8;          // reduced pixels per MCU, across
  const int bpc = info.m_MCUHeight / 8;         // and down
  const int rw  = info.m_MCUSPerRow * bpr;      // reduced image size
  const int rh  = info.m_MCUSPerCol * bpc;
  if (rw <= 0 || rh <= 0 || rw > 256 || rh > 256) return false;

  uint16_t *red = (uint16_t *)ssAlloc((size_t)rw * rh * sizeof(uint16_t));
  if (!red) return false;
  memset(red, 0, (size_t)rw * rh * sizeof(uint16_t));

  int mx = 0, my = 0;
  for (;;) {
    unsigned char st = pjpeg_decode_mcu();
    if (st == PJPG_NO_MORE_BLOCKS) break;
    if (st != 0) { free(red); return false; }
    if (my >= info.m_MCUSPerCol) break;

    for (int by = 0; by < bpc; by++) {
      for (int bx = 0; bx < bpr; bx++) {
        // Block base offset inside the MCU buffer, matching picojpeg's layout
        // (x*8 + y*16 with x,y stepping by 8). In reduce mode only the first
        // byte of each block is meaningful.
        int ofs = bx * 64 + by * 128;
        uint8_t r, g, b;
        if (info.m_scanType == PJPG_GRAYSCALE) {
          r = g = b = info.m_pMCUBufR[ofs];
        } else {
          r = info.m_pMCUBufR[ofs];
          g = info.m_pMCUBufG[ofs];
          b = info.m_pMCUBufB[ofs];
        }
        int ox = mx * bpr + bx, oy = my * bpc + by;
        if (ox < rw && oy < rh)
          red[oy * rw + ox] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }

    if (++mx >= info.m_MCUSPerRow) { mx = 0; my++; }
  }

  // Usually rw == rh == 64 already (a 512x512 source). Resample anyway so a
  // skin uploaded at some other size still yields a valid texture.
  if (rw == TEX_SIZE && rh == TEX_SIZE) {
    memcpy(dst, red, (size_t)TEX_SIZE * TEX_SIZE * sizeof(uint16_t));
  } else {
    for (int y = 0; y < TEX_SIZE; y++) {
      int sy = (int)((uint32_t)y * rh / TEX_SIZE);
      for (int x = 0; x < TEX_SIZE; x++) {
        int sx = (int)((uint32_t)x * rw / TEX_SIZE);
        dst[y * TEX_SIZE + x] = red[sy * rw + sx];
      }
    }
  }
  free(red);
  return true;
}

bool texWrite(const char *path, const uint16_t *pix) {
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  // Version 2 = 128 px textures; version 1 was 64 and still loads.
  uint8_t hdr[16] = { 'M', 'M', 'T', 'X', 2,
                      (uint8_t)TEX_SIZE, (uint8_t)TEX_SIZE, 0 };
  bool ok = (f.write(hdr, 16) == 16);
  size_t want = (size_t)TEX_SIZE * TEX_SIZE * sizeof(uint16_t);
  ok = ok && (f.write((const uint8_t *)pix, want) == want);
  f.close();
  return ok;
}

int SkinStore::download(int index, SkinDlProgress cb, void *ctx) {
  _err[0] = 0;
  if (!_doc) { strcpy(_err, "no page loaded"); return 0; }
  JsonDocument *doc = (JsonDocument *)_doc;
  JsonArray arr = (*doc)["skins"].as<JsonArray>();
  if (arr.isNull() || index < 0 || index >= (int)arr.size()) { strcpy(_err, "bad index"); return 0; }

  JsonObject o = arr[index].as<JsonObject>();
  uint16_t id = o["id"].as<uint16_t>();
  JsonObject tx = o["textures"].as<JsonObject>();
  if (tx.isNull()) { strcpy(_err, "skin has no textures"); return 0; }

  if (!SD.exists(MINE_DIR)) SD.mkdir(MINE_DIR);
  if (!SD.exists(SKIN_DIR)) SD.mkdir(SKIN_DIR);
  char dir[64];
  snprintf(dir, sizeof(dir), SKIN_DIR "/%u", (unsigned)id);
  if (!SD.exists(dir)) SD.mkdir(dir);

  uint16_t *pix = (uint16_t *)ssAlloc((size_t)TEX_SIZE * TEX_SIZE * sizeof(uint16_t));
  if (!pix) { strcpy(_err, "out of memory"); return 0; }

  int written = 0;
  for (uint8_t k = 0; k < TK_COUNT; k++) {
    const char *key = TEX_KEY_NAMES[k];
    if (cb && !cb(k, TK_COUNT, key, ctx)) break;      // user cancelled
    const char *url = tx[key];
    if (!url || !*url) continue;                      // skin.cpp draws its own

    uint8_t *jb = nullptr;
    size_t   jl = 0;
    if (!httpGet(String(url), &jb, &jl)) continue;    // keep going; partial is fine
    bool ok = jpegToTex(jb, jl, pix);
    free(jb);
    if (!ok) continue;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s.tex", dir, key);
    if (texWrite(path, pix)) written++;
  }
  free(pix);

  // meta.json, in the same shape fetch_skins.py writes.
  char mpath[80];
  snprintf(mpath, sizeof(mpath), "%s/meta.json", dir);
  if (SD.exists(mpath)) SD.remove(mpath);
  File mf = SD.open(mpath, FILE_WRITE);
  if (mf) {
    JsonDocument m;
    m["id"] = id;
    m["name"] = o["skinName"];
    m["downloadCount"] = o["downloadCount"];
    serializeJson(m, mf);
    mf.close();
  }

  if (cb) cb(TK_COUNT, TK_COUNT, "done", ctx);
  if (written == 0) strcpy(_err, "no textures downloaded");
  return written;
}
