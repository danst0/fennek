#include "textdoc.h"
#include "core/board.h"
#include "core/settings.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr uint32_t kIdxMagic   = 0x5844494D;  // "MIDX"
constexpr uint16_t kIdxVersion = 1;
constexpr int      kMaxPages   = 16384;       // 64 KB PSRAM
constexpr size_t   kChunk      = 16384;
constexpr size_t   kPageBufCap = 16384;

// --- Umbruch-Engine (gemeinsam für Indexer und Renderer) ----------------------
typedef void (*LineFn)(void* ctx, const char* utf8Line);
typedef void (*PageFn)(void* ctx, uint32_t nextOff);   // Seite voll; nächste beginnt bei nextOff

struct WrapState {
  int      col = 0, line = 0;
  char     wb[160]; int wbLen = 0; int wbCols = 0;     // aktuelles Wort
  uint32_t wbStart = 0;                                // Dateioffset des Wortanfangs
  char     lb[200]; int lbLen = 0;                     // aktuelle Zeile (nur Render)
  uint8_t  pend[4]; int pendLen = 0; int pendNeed = 0; // UTF-8 über Chunk-Grenze
  bool     stopped = false;                            // Render: Seite fertig
};

struct WrapCfg {
  int    cols, rows;
  LineFn lf; PageFn pf; void* ctx;
  bool   stopAtPage;   // Render: nach einer vollen Seite anhalten
};

void endLine(WrapState& st, const WrapCfg& cfg, uint32_t nextOff) {
  if (cfg.lf) { st.lb[st.lbLen] = '\0'; cfg.lf(cfg.ctx, st.lb); }
  st.lbLen = 0;
  st.col = 0;
  st.line++;
  if (st.line >= cfg.rows) {
    st.line = 0;
    if (cfg.pf) cfg.pf(cfg.ctx, nextOff);
    if (cfg.stopAtPage) st.stopped = true;
  }
}

void placeWord(WrapState& st, const WrapCfg& cfg) {
  if (st.wbCols == 0) return;
  if (st.col > 0 && st.col + 1 + st.wbCols > cfg.cols) endLine(st, cfg, st.wbStart);
  if (st.stopped) return;
  if (st.col > 0) {
    if (st.lbLen < (int)sizeof(st.lb) - 1) st.lb[st.lbLen++] = ' ';
    st.col++;
  }
  if (st.lbLen + st.wbLen < (int)sizeof(st.lb) - 1) {
    memcpy(st.lb + st.lbLen, st.wb, st.wbLen);
    st.lbLen += st.wbLen;
  }
  st.col += st.wbCols;
  st.wbLen = 0;
  st.wbCols = 0;
}

void wrapChar(WrapState& st, const WrapCfg& cfg, uint32_t cp, uint32_t cpOff, int cpBytes) {
  if (st.stopped) return;
  if (cp == '\n') {
    placeWord(st, cfg);
    if (!st.stopped) endLine(st, cfg, cpOff + cpBytes);
    return;
  }
  if (cp == ' ' || cp == '\t' || cp == '\r') {
    placeWord(st, cfg);
    return;
  }
  if (st.wbCols == 0) st.wbStart = cpOff;
  // Codepoint als UTF-8 ins Wort übernehmen.
  if (cp < 0x80) {
    if (st.wbLen < (int)sizeof(st.wb) - 1) st.wb[st.wbLen++] = (char)cp;
  } else if (cp <= 0x7FF) {
    if (st.wbLen < (int)sizeof(st.wb) - 2) {
      st.wb[st.wbLen++] = (char)(0xC0 | (cp >> 6));
      st.wb[st.wbLen++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (cp <= 0xFFFF) {
    if (st.wbLen < (int)sizeof(st.wb) - 3) {
      st.wb[st.wbLen++] = (char)(0xE0 | (cp >> 12));
      st.wb[st.wbLen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      st.wb[st.wbLen++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  st.wbCols++;
  // Überlange Wörter hart umbrechen.
  if (st.wbCols >= cfg.cols) placeWord(st, cfg);
}

// Bytes verarbeiten (UTF-8-Decoding inkl. Sequenzen über Chunk-Grenzen).
void wrapBytes(WrapState& st, const WrapCfg& cfg,
               const uint8_t* data, size_t len, uint32_t baseOff) {
  for (size_t i = 0; i < len && !st.stopped; i++) {
    uint8_t b = data[i];
    if (st.pendNeed > 0) {
      if ((b & 0xC0) == 0x80 && st.pendLen < 4) {
        st.pend[st.pendLen++] = b;
        if (st.pendLen - 1 == st.pendNeed) {
          // Sequenz komplett -> dekodieren.
          uint32_t cp = 0;
          int n = st.pendNeed;
          if (n == 1)      cp = ((uint32_t)(st.pend[0] & 0x1F) << 6) | (st.pend[1] & 0x3F);
          else if (n == 2) cp = ((uint32_t)(st.pend[0] & 0x0F) << 12) |
                                ((uint32_t)(st.pend[1] & 0x3F) << 6) | (st.pend[2] & 0x3F);
          else             cp = '?';   // 4-Byte (außerhalb BMP) -> '?'
          uint32_t off = baseOff + (uint32_t)i - st.pendLen + 1;
          wrapChar(st, cfg, cp, off, st.pendLen);
          st.pendNeed = 0;
          st.pendLen = 0;
        }
        continue;
      }
      // Ungültige Sequenz: verwerfen, Byte normal weiterbehandeln.
      st.pendNeed = 0;
      st.pendLen = 0;
    }
    if (b < 0x80) {
      wrapChar(st, cfg, b, baseOff + (uint32_t)i, 1);
    } else if ((b & 0xE0) == 0xC0) { st.pend[0] = b; st.pendLen = 1; st.pendNeed = 1; }
    else if ((b & 0xF0) == 0xE0)   { st.pend[0] = b; st.pendLen = 1; st.pendNeed = 2; }
    else if ((b & 0xF8) == 0xF0)   { st.pend[0] = b; st.pendLen = 1; st.pendNeed = 3; }
    // verirrte Continuation-Bytes: ignorieren
  }
}

void wrapFlush(WrapState& st, const WrapCfg& cfg, uint32_t endOff) {
  if (st.stopped) return;
  placeWord(st, cfg);
  if (!st.stopped && st.lbLen > 0) endLine(st, cfg, endOff);
}

// --- Dokument-Zustand -----------------------------------------------------------
char      s_path[256] = "";   // Calibre-Pfade (/books/Autor/Titel/…) sind lang
int       s_cols = 38, s_rows = 27;
uint32_t  s_fsize = 0;
uint32_t* s_pageOff = nullptr;   // PSRAM
int       s_pages = 0;           // Anzahl gespeicherter Offsets (>=1 wenn offen)
bool      s_ready = false;
bool      s_open  = false;

// Indexer-Lauf
File      s_file;
uint32_t  s_indexed = 0;
WrapState s_ixState;

void idxPath(char* out, size_t n) {
  snprintf(out, n, "/.fennek/idx/%08x.idx", (unsigned)settings::crc32(s_path));
}

void recordPage(void* /*ctx*/, uint32_t nextOff) {
  if (s_pages < kMaxPages) s_pageOff[s_pages++] = nextOff;
}

bool loadIndexCache() {
  char ip[48];
  idxPath(ip, sizeof(ip));
  bool ok = false;
  spiLock();
  File f;
  if (SD.exists(ip)) f = SD.open(ip);
  if (f) {
    uint32_t magic = 0, fsize = 0, count = 0;
    uint16_t ver = 0, cols = 0, rows = 0;
    if (f.read((uint8_t*)&magic, 4) == 4 && magic == kIdxMagic &&
        f.read((uint8_t*)&ver, 2) == 2 && ver == kIdxVersion &&
        f.read((uint8_t*)&cols, 2) == 2 && cols == (uint16_t)s_cols &&
        f.read((uint8_t*)&rows, 2) == 2 && rows == (uint16_t)s_rows &&
        f.read((uint8_t*)&fsize, 4) == 4 && fsize == s_fsize &&
        f.read((uint8_t*)&count, 4) == 4 && count >= 1 && count <= kMaxPages) {
      if (f.read((uint8_t*)s_pageOff, count * 4) == (int)(count * 4)) {
        s_pages = (int)count;
        ok = true;
      }
    }
    f.close();
  }
  spiUnlock();
  return ok;
}

void writeIndexCache() {
  char ip[48];
  idxPath(ip, sizeof(ip));
  spiLock();
  if (!SD.exists("/.fennek")) SD.mkdir("/.fennek");
  if (!SD.exists("/.fennek/idx")) SD.mkdir("/.fennek/idx");
  if (SD.exists("/.fennek/idx/.tmp")) SD.remove("/.fennek/idx/.tmp");
  File f = SD.open("/.fennek/idx/.tmp", FILE_WRITE);
  if (f) {
    uint16_t cols = (uint16_t)s_cols, rows = (uint16_t)s_rows;
    uint32_t count = (uint32_t)s_pages;
    f.write((const uint8_t*)&kIdxMagic, 4);
    f.write((const uint8_t*)&kIdxVersion, 2);
    f.write((const uint8_t*)&cols, 2);
    f.write((const uint8_t*)&rows, 2);
    f.write((const uint8_t*)&s_fsize, 4);
    f.write((const uint8_t*)&count, 4);
    f.write((const uint8_t*)s_pageOff, count * 4);
    f.close();
    if (SD.exists(ip)) SD.remove(ip);
    SD.rename("/.fennek/idx/.tmp", ip);
  }
  spiUnlock();
}

// --- Render-Sink ------------------------------------------------------------------
struct PageSink {
  char*  buf;
  size_t cap;
  size_t len;
  int    lines;
  int    maxLines;
};

void sinkLine(void* ctx, const char* utf8Line) {
  PageSink* s = (PageSink*)ctx;
  size_t l = strlen(utf8Line);
  if (s->len + l + 2 < s->cap) {
    memcpy(s->buf + s->len, utf8Line, l);
    s->len += l;
    s->buf[s->len++] = '\n';
  }
  s->lines++;
}

void sinkPageFull(void* ctx, uint32_t /*nextOff*/) {
  (void)ctx;
}

}  // namespace

namespace textdoc {

bool open(const char* path, int cols, int rows) {
  close();
  strncpy(s_path, path, sizeof(s_path) - 1);
  s_path[sizeof(s_path) - 1] = '\0';
  s_cols = cols;
  s_rows = rows;

  if (!s_pageOff) {
    s_pageOff = (uint32_t*)heap_caps_malloc(kMaxPages * 4, MALLOC_CAP_SPIRAM);
    if (!s_pageOff) s_pageOff = (uint32_t*)malloc(kMaxPages * 4);
    if (!s_pageOff) return false;
  }

  spiLock();
  s_file = SD.open(path);
  bool ok = (bool)s_file;
  s_fsize = ok ? s_file.size() : 0;
  spiUnlock();
  if (!ok || s_fsize == 0) return false;

  s_open = true;
  s_pageOff[0] = 0;
  s_pages = 1;
  s_indexed = 0;
  s_ixState = WrapState{};

  if (loadIndexCache()) {
    s_ready = true;
  } else {
    s_pageOff[0] = 0;
    s_pages = 1;
    s_ready = false;
  }
  return true;
}

bool indexReady() { return s_ready; }

void indexStep(uint32_t budgetBytes) {
  if (!s_open || s_ready) return;
  static uint8_t s_chunk[kChunk];

  WrapCfg cfg{s_cols, s_rows, nullptr, recordPage, nullptr, false};

  uint32_t processed = 0;
  while (processed < budgetBytes && s_indexed < s_fsize) {
    size_t want = kChunk;
    if (s_indexed + want > s_fsize) want = s_fsize - s_indexed;

    spiLock();
    s_file.seek(s_indexed);
    int rd = s_file.read(s_chunk, want);
    spiUnlock();
    if (rd <= 0) break;

    wrapBytes(s_ixState, cfg, s_chunk, (size_t)rd, s_indexed);
    s_indexed += (uint32_t)rd;
    processed += (uint32_t)rd;
  }

  if (s_indexed >= s_fsize) {
    wrapFlush(s_ixState, cfg, s_fsize);
    // Leere letzte Seite (Offset == Dateiende) verwerfen.
    while (s_pages > 1 && s_pageOff[s_pages - 1] >= s_fsize) s_pages--;
    s_ready = true;
    writeIndexCache();
    Serial.printf("[TXT] Index fertig: %d Seiten (%u Bytes)\n", s_pages, (unsigned)s_fsize);
  }
}

int indexPercent() {
  if (s_ready) return 100;
  if (!s_fsize) return 0;
  return (int)((uint64_t)s_indexed * 100 / s_fsize);
}

int pageCount() { return s_open ? s_pages : 0; }

bool page(int p, char* buf, size_t bufLen) {
  if (!s_open || !s_ready || p < 0 || p >= s_pages || !buf || bufLen < 2) return false;

  uint32_t from = s_pageOff[p];
  uint32_t to   = (p + 1 < s_pages) ? s_pageOff[p + 1] : s_fsize;
  if (to <= from) { buf[0] = '\0'; return true; }
  uint32_t len = to - from;
  if (len > kPageBufCap) len = kPageBufCap;

  uint8_t* tmp = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!tmp) tmp = (uint8_t*)malloc(len);
  if (!tmp) return false;

  spiLock();
  s_file.seek(from);
  int rd = s_file.read(tmp, len);
  spiUnlock();
  if (rd <= 0) { free(tmp); return false; }

  PageSink sink{buf, bufLen, 0, 0, s_rows};
  WrapState st;
  WrapCfg cfg{s_cols, s_rows, sinkLine, sinkPageFull, &sink, true};
  wrapBytes(st, cfg, tmp, (size_t)rd, from);
  wrapFlush(st, cfg, to);
  free(tmp);

  buf[sink.len] = '\0';
  return true;
}

void close() {
  if (s_open) {
    spiLock();
    s_file.close();
    spiUnlock();
  }
  s_open = false;
  s_ready = false;
  s_pages = 0;
  s_path[0] = '\0';
}

}  // namespace textdoc
