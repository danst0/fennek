#include "reader_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"
#include "services/epubproc.h"
#include "services/textdoc.h"

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_heap_caps.h>
#include <string.h>
#include <strings.h>

namespace {

using gui::Rect;

constexpr const char* kBookDir = "/books";
constexpr int kMaxBooks  = 512;
constexpr int kNameLen   = 64;
constexpr int kPathLen   = 256;
constexpr int kScanDepth = 2;   // Calibre-Layout: /books/Autor/Titel/Buch.epub

// --- Layout: Liste ------------------------------------------------------------
constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_Y = TOP + 2;
constexpr int HEADER_H = TOP + 26;
constexpr int ROW_H    = 30;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int VISIBLE  = 7;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;

const Rect kBack    {6,   BAR_Y, 72, 44};
const Rect kUp      {84,  BAR_Y, 72, 44};
const Rect kDown    {162, BAR_Y, 72, 44};
const Rect kRetrySD {20, 150, W - 40, 48};
const Rect kCancel  {20, 200, W - 40, 48};

// --- Layout: Lesen ---------------------------------------------------------------
// Default-Font Größe 1: 6x8 px. 38 Spalten, 27 Zeilen, Fußzeile mit Seite x/y.
constexpr int READ_COLS  = 38;
constexpr int READ_ROWS  = 27;
constexpr int READ_X     = 6;
constexpr int READ_Y     = TOP + 4;       // 28
constexpr int READ_LINEH = 10;
constexpr int FOOT_Y     = READ_Y + READ_ROWS * READ_LINEH + 2;  // 300

// --- Zustand -----------------------------------------------------------------------
enum Screen { LIST, CONVERTING, INDEXING, READ };
Screen s_screen = LIST;

struct BookFile { char name[kNameLen]; char path[kPathLen]; bool isEpub; };
BookFile* s_files = nullptr;
int       s_fileCount = -1;     // -1 = noch nicht gescannt
int       s_sel = 0, s_off = 0;

char      s_txtPath[kPathLen] = "";   // Pfad der (ggf. konvertierten) TXT
uint32_t  s_posKey = 0;               // NVS-Key (CRC des Originalpfads)
int       s_page = 0;
char*     s_pageBuf = nullptr;        // PSRAM, gewrappte Seitenzeilen

void markDirty() { appmgr::markDirty(); }

bool hasExt(const char* n, const char* e) {
  size_t ln = strlen(n), le = strlen(e);
  return ln > le && strcasecmp(n + ln - le, e) == 0;
}

int cmpFile(const void* a, const void* b) {
  return strcasecmp(((const BookFile*)a)->name, ((const BookFile*)b)->name);
}

// --- Bücher-Scan: häppchenweise + SD-Cache (Calibre: /books/Autor/Titel/) ----
// Drei Lehren vom Gerät (Calibre-Bibliothek, ~100 Autoren / 107 Root-Einträge):
// 1. openNextFile() öffnet jeden Eintrag (fopen inkl. FAT-Pfadauflösung) —
//    der Komplett-Scan dauerte >4 Minuten. getNextFileName() macht nur
//    readdir und liefert Name + Dir-Flag, das genügt hier.
// 2. spiLock über ein ganzes Verzeichnis blockiert den Audio-Task sekundenlang
//    (er braucht den Mutex für JEDEN Decode-Schritt, das I2S-DMA überbrückt
//    nur ~90 ms => Hintergrund-Musik stotterte). Deshalb max. kScanChunk
//    Einträge pro Lock; das offene Verzeichnis überlebt in s_scanDir
//    zwischen den Häppchen.
// 3. Das Ergebnis liegt als /.fennek/books.bin auf der SD und wird beim
//    nächsten Boot von dort geladen. Neu gescannt wird nur ohne Cache, per
//    'R' in der Liste oder über "Erneut suchen".
constexpr int kMaxStack  = 256;
constexpr int kScanChunk = 6;
constexpr const char* kBookCache    = "/.fennek/books.bin";
constexpr const char* kBookCacheTmp = "/.fennek/books.tmp";
constexpr uint32_t kBookCacheMagic = 0x534B424Du;  // "MBKS"
constexpr uint16_t kBookCacheVer   = 1;

char (*s_dirStack)[kPathLen] = nullptr;
int   s_dirStackN = 0;
bool  s_scanning  = false;
File  s_scanDir;                 // offenes Verzeichnis, überlebt scanStep-Aufrufe
bool  s_scanDirOpen   = false;
int   s_scanDirDepth  = 0;
bool  s_bootOpenTried = false;   // Auto-Open des letzten Buchs: 1x pro Boot

bool ensureFiles() {
  if (!s_files) {
    s_files = (BookFile*)heap_caps_malloc(sizeof(BookFile) * kMaxBooks, MALLOC_CAP_SPIRAM);
    if (!s_files) s_files = (BookFile*)malloc(sizeof(BookFile) * kMaxBooks);
  }
  return s_files != nullptr;
}

// Tiefe relativ zu /books ("/books" = Tiefe 0).
int dirDepth(const char* p) {
  int n = 0;
  for (; *p; p++) if (*p == '/') n++;
  return n - 1;
}

// Cursor in der Liste aufs zuletzt gelesene Buch stellen (kosmetisch).
void selectLastBook() {
  char last[kPathLen];
  settings::lastBook(last, sizeof(last));
  if (!last[0]) return;
  for (int i = 0; i < s_fileCount; i++) {
    if (strcmp(s_files[i].path, last) == 0) {
      s_sel = i;
      s_off = (i >= VISIBLE) ? i - VISIBLE + 1 : 0;
      return;
    }
  }
}

bool loadBookCache() {
  if (!board::sdReady() || !ensureFiles()) return false;
  uint32_t magic = 0, count = 0;
  uint16_t ver = 0, rec = 0;
  spiLock();
  File f = SD.open(kBookCache);
  bool ok = f &&
            f.read((uint8_t*)&magic, 4) == 4 && magic == kBookCacheMagic &&
            f.read((uint8_t*)&ver, 2) == 2 && ver == kBookCacheVer &&
            f.read((uint8_t*)&rec, 2) == 2 && rec == (uint16_t)sizeof(BookFile) &&
            f.read((uint8_t*)&count, 4) == 4 && count <= (uint32_t)kMaxBooks;
  spiUnlock();
  // Einträge in Häppchen lesen (Lock-Zeit kurz halten, s. o.).
  int n = 0;
  while (ok && n < (int)count) {
    int want = (int)count - n;
    if (want > 16) want = 16;
    spiLock();
    ok = f.read((uint8_t*)&s_files[n], want * sizeof(BookFile)) ==
         (int)(want * sizeof(BookFile));
    spiUnlock();
    n += want;
  }
  if (f) { spiLock(); f.close(); spiUnlock(); }
  if (!ok) return false;
  s_fileCount = (int)count;
  s_sel = 0; s_off = 0;
  selectLastBook();
  Serial.printf("[READER] Bücher-Cache geladen: %d Einträge\n", s_fileCount);
  return true;
}

void writeBookCache() {
  if (!board::sdReady() || s_fileCount < 0) return;
  spiLock();
  if (!SD.exists("/.fennek")) SD.mkdir("/.fennek");
  if (SD.exists(kBookCacheTmp)) SD.remove(kBookCacheTmp);
  File f = SD.open(kBookCacheTmp, FILE_WRITE);
  bool ok = (bool)f;
  if (ok) {
    uint16_t rec = (uint16_t)sizeof(BookFile);
    uint32_t count = (uint32_t)s_fileCount;
    f.write((const uint8_t*)&kBookCacheMagic, 4);
    f.write((const uint8_t*)&kBookCacheVer, 2);
    f.write((const uint8_t*)&rec, 2);
    ok = f.write((const uint8_t*)&count, 4) == 4;
  }
  spiUnlock();
  for (int i = 0; ok && i < s_fileCount; i += 16) {
    size_t n = (size_t)((s_fileCount - i > 16) ? 16 : s_fileCount - i);
    spiLock();
    ok = f.write((const uint8_t*)&s_files[i], n * sizeof(BookFile)) ==
         n * sizeof(BookFile);
    spiUnlock();
  }
  spiLock();
  if (f) f.close();
  if (ok) {
    if (SD.exists(kBookCache)) SD.remove(kBookCache);
    ok = SD.rename(kBookCacheTmp, kBookCache);
  } else if (f) {
    SD.remove(kBookCacheTmp);
  }
  spiUnlock();
  Serial.printf("[READER] Bücher-Cache %s (%d Einträge)\n",
                ok ? "geschrieben" : "FEHLER", s_fileCount);
}

void scanStart() {
  if (!s_dirStack) {
    s_dirStack = (char(*)[kPathLen])heap_caps_malloc(kMaxStack * kPathLen, MALLOC_CAP_SPIRAM);
    if (!s_dirStack) s_dirStack = (char(*)[kPathLen])malloc(kMaxStack * kPathLen);
  }
  s_fileCount = 0;
  s_sel = 0; s_off = 0;
  s_scanning = false;
  if (s_scanDirOpen) {             // Rest eines abgebrochenen Laufs aufräumen
    spiLock(); s_scanDir.close(); spiUnlock();
    s_scanDirOpen = false;
  }
  if (!ensureFiles() || !s_dirStack || !board::sdReady()) return;
  strncpy(s_dirStack[0], kBookDir, kPathLen - 1);
  s_dirStack[0][kPathLen - 1] = '\0';
  s_dirStackN = 1;
  s_scanning = true;
}

void scanFinish() {
  s_scanning = false;
  qsort(s_files, s_fileCount, sizeof(BookFile), cmpFile);
  s_sel = 0; s_off = 0;
  selectLastBook();
  writeBookCache();
  if (s_screen == LIST) markDirty();
}

// Ein Häppchen: max. kScanChunk Verzeichniseinträge unter einem spiLock.
// Stack-Abbau LIFO = Tiefensuche, hält den Stack klein.
void scanStep() {
  if (!s_scanning) return;
  if (s_fileCount >= kMaxBooks) {
    if (s_scanDirOpen) {
      spiLock(); s_scanDir.close(); spiUnlock();
      s_scanDirOpen = false;
    }
    scanFinish();
    return;
  }
  if (!s_scanDirOpen && s_dirStackN == 0) { scanFinish(); return; }

  spiLock();
  if (!s_scanDirOpen) {
    char dir[kPathLen];
    strncpy(dir, s_dirStack[--s_dirStackN], kPathLen - 1);
    dir[kPathLen - 1] = '\0';
    s_scanDir = SD.open(dir);
    if (!s_scanDir || !s_scanDir.isDirectory()) {
      if (s_scanDir) s_scanDir.close();
      spiUnlock();
      return;
    }
    s_scanDirOpen  = true;
    s_scanDirDepth = dirDepth(dir);
  }
  for (int i = 0; i < kScanChunk && s_fileCount < kMaxBooks; i++) {
    bool isDir = false;
    String entry = s_scanDir.getNextFileName(&isDir);   // readdir, kein fopen
    if (entry.length() == 0) {
      s_scanDir.close();
      s_scanDirOpen = false;
      break;
    }
    const char* full = entry.c_str();
    const char* nm = strrchr(full, '/');
    nm = nm ? nm + 1 : full;
    if (nm[0] == '.') continue;
    if (isDir) {
      if (s_scanDirDepth < kScanDepth && s_dirStackN < kMaxStack) {
        strncpy(s_dirStack[s_dirStackN], full, kPathLen - 1);
        s_dirStack[s_dirStackN][kPathLen - 1] = '\0';
        s_dirStackN++;
      }
    } else if (hasExt(nm, ".txt") || hasExt(nm, ".epub")) {
      BookFile& b = s_files[s_fileCount];
      strncpy(b.name, nm, kNameLen - 1); b.name[kNameLen - 1] = '\0';
      strncpy(b.path, full, kPathLen - 1); b.path[kPathLen - 1] = '\0';
      b.isEpub = hasExt(nm, ".epub");
      s_fileCount++;
    }
  }
  spiUnlock();
}

// Aktuelle Seite in den Puffer laden.
bool loadPage() {
  if (!s_pageBuf) {
    s_pageBuf = (char*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!s_pageBuf) s_pageBuf = (char*)malloc(8192);
    if (!s_pageBuf) return false;
  }
  return textdoc::page(s_page, s_pageBuf, 8192);
}

void enterRead() {
  int n = textdoc::pageCount();
  uint32_t saved = 0;
  if (settings::readPos(s_posKey, &saved) && (int)saved < n) s_page = (int)saved;
  else s_page = 0;
  loadPage();
  s_screen = READ;
  markDirty();
}

// TXT öffnen (nach evtl. EPUB-Konvertierung): Index laden oder bauen.
void openTxt() {
  if (!textdoc::open(s_txtPath, READ_COLS, READ_ROWS)) {
    s_screen = LIST;
    markDirty();
    return;
  }
  if (textdoc::indexReady()) enterRead();
  else { s_screen = INDEXING; markDirty(); }
}

void openBookPath(const char* path) {
  s_posKey = settings::crc32(path);
  settings::setLastBook(path);

  if (hasExt(path, ".epub")) {
    epubproc::buildCachePath(path, s_txtPath, sizeof(s_txtPath));
    spiLock();
    bool cached = SD.exists(s_txtPath);
    spiUnlock();
    if (cached) { openTxt(); return; }
    if (epubproc::convertBegin(path, s_txtPath)) {
      if (epubproc::convertDone()) { openTxt(); return; }
      s_screen = CONVERTING;
      markDirty();
    } else {
      markDirty();   // Fehler -> Liste bleibt, Log im Serial
    }
  } else {
    strncpy(s_txtPath, path, sizeof(s_txtPath) - 1);
    s_txtPath[sizeof(s_txtPath) - 1] = '\0';
    openTxt();
  }
}

void openBook(int idx) {
  if (idx < 0 || idx >= s_fileCount) return;
  openBookPath(s_files[idx].path);
}

// Beim ersten Betreten nach dem Boot direkt ins zuletzt gelesene Buch springen
// (die Seite kommt in enterRead() aus der NVS-Leseposition). Bewusst OHNE
// auf Scan oder Cache zu warten — der Pfad steht im NVS, und der Erst-Scan
// einer großen Bibliothek dauert sonst Minuten, bevor das Buch aufgeht.
bool autoOpenLastBook() {
  char last[kPathLen];
  settings::lastBook(last, sizeof(last));
  if (!last[0]) return false;
  spiLock();
  bool exists = SD.exists(last);
  spiUnlock();
  if (!exists) return false;
  Serial.printf("[READER] Resume: %s\n", last);
  openBookPath(last);
  return s_screen != LIST;
}

void turnPage(int delta) {
  int n = textdoc::pageCount();
  int np = s_page + delta;
  if (np < 0 || np >= n) return;
  s_page = np;
  settings::setReadPos(s_posKey, (uint32_t)s_page);
  loadPage();
  markDirty();
}

void leaveRead() {
  settings::setReadPos(s_posKey, (uint32_t)s_page);
  textdoc::close();
  s_screen = LIST;
  markDirty();
}

// --- Zeichnen --------------------------------------------------------------------
void drawList(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::ReaderBooks), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::NoSdCard), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::InsertCard), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }
  if (s_scanning) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::ReaderSearch), 2);
    char found[32];
    snprintf(found, sizeof(found), i18n::tr(i18n::Str::FmtReaderFound), s_fileCount);
    gui::printAt(g, 10, 110, found, 1);
    gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnHome), false);
    return;
  }
  if (s_fileCount <= 0) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::ReaderNone), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::ReaderHint), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }

  // Hinweis rechts in der Kopfzeile: Liste kommt aus dem Cache, 'R' scannt neu.
  g.setTextSize(1);
  g.setCursor(W - 96, HEADER_Y + 5);
  gui::print(g, i18n::tr(i18n::Str::ReaderRescan));

  for (int r = 0; r < VISIBLE && s_off + r < s_fileCount; r++) {
    int i = s_off + r;
    char lbl[80];
    uint32_t pos;
    bool started = settings::readPos(settings::crc32(s_files[i].path), &pos) && pos > 0;
    snprintf(lbl, sizeof(lbl), "%s%s", started ? "\x10 " : "", s_files[i].name);
    gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, false);
  }
  int cr = s_sel - s_off;
  if (cr >= 0 && cr < VISIBLE) {
    int y = LIST_Y + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }

  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnHome), false);
  if (s_off > 0)                      gui::drawButton(g, kUp,   i18n::tr(i18n::Str::BtnUp), false);
  if (s_off + VISIBLE < s_fileCount)  gui::drawButton(g, kDown, i18n::tr(i18n::Str::BtnDown), false);
}

void drawProgress(Adafruit_GFX& g, const char* what, int percent) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::AppReader), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
  gui::printAt(g, 10, 90, what, 2);
  int barX = 10, barY = 130, barW = W - 20, barH = 16;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  int fw = (barW - 2) * percent / 100;
  if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", percent);
  gui::printAt(g, 10, 156, pct, 1);
  gui::drawButton(g, kCancel, i18n::tr(i18n::Str::BtnCancel), false);
}

void drawRead(Adafruit_GFX& g) {
  if (!s_pageBuf) return;
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);

  // Seitenzeilen ausgeben.
  const char* p = s_pageBuf;
  int y = READ_Y;
  char line[200];
  while (*p && y < FOOT_Y - READ_LINEH + 2) {
    const char* nl = strchr(p, '\n');
    size_t l = nl ? (size_t)(nl - p) : strlen(p);
    if (l >= sizeof(line)) l = sizeof(line) - 1;
    memcpy(line, p, l);
    line[l] = '\0';
    g.setCursor(READ_X, y);
    gui::print(g, line);
    y += READ_LINEH;
    p = nl ? nl + 1 : p + l;
  }

  // Fußzeile: Seite x/y.
  g.drawFastHLine(0, FOOT_Y, W, GxEPD_BLACK);
  char foot[24];
  snprintf(foot, sizeof(foot), "%d / %d", s_page + 1, textdoc::pageCount());
  uint16_t bw, bh;
  g.setTextSize(1);
  gui::textBounds(g, foot, &bw, &bh);
  g.setCursor((W - (int)bw) / 2, FOOT_Y + 4);
  g.print(foot);
}

// --- Interaktion ------------------------------------------------------------------
void retrySD() {
  if (board::initSD()) scanStart();
  markDirty();
}

void moveSel(int delta) {
  if (s_fileCount <= 0) return;
  int ns = s_sel + delta;
  if (ns < 0) ns = s_fileCount - 1;
  if (ns >= s_fileCount) ns = 0;
  s_sel = ns;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  markDirty();
}

void onListTouch(int x, int y) {
  if (kBack.hit(x, y)) { appmgr::goHome(); return; }
  if (s_scanning) return;
  if (!board::sdReady() || s_fileCount <= 0) {
    if (kRetrySD.hit(x, y)) retrySD();
    return;
  }
  if (kUp.hit(x, y))   { s_off = (s_off > VISIBLE) ? s_off - VISIBLE : 0; markDirty(); return; }
  if (kDown.hit(x, y)) {
    if (s_off + VISIBLE < s_fileCount) { s_off += VISIBLE; markDirty(); }
    return;
  }
  if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
    int r = (y - LIST_Y) / ROW_H;
    if (s_off + r < s_fileCount) { s_sel = s_off + r; openBook(s_sel); }
  }
}

void onListKey(char k) {
  switch (k) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(+1); break;
    case '\r':          if (s_fileCount > 0) openBook(s_sel); break;
    case 'r': case 'R': if (!s_scanning) { scanStart(); markDirty(); } break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onReadKey(char k) {
  switch (k) {
    case 'd': case 'D': case 's': case 'S': case ' ': turnPage(+1); break;
    case 'a': case 'A': case 'w': case 'W':           turnPage(-1); break;
    case '\b':          leaveRead(); break;
    case 'q': case 'Q': leaveRead(); appmgr::goHome(); break;
    default: break;
  }
}

// --- App-Klasse ---------------------------------------------------------------------
class ReaderApp : public App {
 public:
  const char* id()   const override { return "Lesen"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppReader); }

  void onEnter() override {
    if (!board::sdReady()) return;
    // Liste bereitstellen: einmal pro Boot aus dem SD-Cache; ohne Cache
    // startet der häppchenweise Scan (tick()) — blockiert hier nichts.
    if (s_fileCount < 0 && !s_scanning) {
      if (!loadBookCache()) scanStart();
    }
    // Beim ersten Betreten nach dem Boot sofort ins letzte Buch — unabhängig
    // davon, ob Cache/Scan die Liste schon geliefert haben.
    if (!s_bootOpenTried) {
      s_bootOpenTried = true;
      autoOpenLastBook();
    }
  }

  void onLeave() override {
    if (s_screen == READ) settings::setReadPos(s_posKey, (uint32_t)s_page);
    if (s_screen == CONVERTING) { epubproc::convertCancel(); s_screen = LIST; }
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      switch (s_screen) {
        case LIST:       onListTouch(e.x, e.y); break;
        case CONVERTING:
          if (kCancel.hit(e.x, e.y)) { epubproc::convertCancel(); s_screen = LIST; markDirty(); }
          break;
        case INDEXING:
          if (kCancel.hit(e.x, e.y)) { textdoc::close(); s_screen = LIST; markDirty(); }
          break;
        case READ:
          if (e.y >= FOOT_Y) { leaveRead(); }
          else if (e.x >= W / 2) turnPage(+1);
          else                   turnPage(-1);
          break;
      }
    } else {
      switch (s_screen) {
        case LIST:       onListKey(e.key); break;
        case CONVERTING:
          if (e.key == 'q' || e.key == 'Q' || e.key == '\b') {
            epubproc::convertCancel(); s_screen = LIST; markDirty();
          }
          break;
        case INDEXING:
          if (e.key == 'q' || e.key == 'Q' || e.key == '\b') {
            textdoc::close(); s_screen = LIST; markDirty();
          }
          break;
        case READ:       onReadKey(e.key); break;
      }
    }
  }

  void tick() override {
    // Konvertierung/Indexierung zuerst (eigene Screens, Fortschritt ~2 s);
    // ein evtl. laufender Scan pausiert solange (Bus nicht doppelt belasten).
    if (s_screen == CONVERTING) {
      epubproc::convertStep();
      if (epubproc::convertDone())        { openTxt(); return; }
      else if (epubproc::convertFailed()) { s_screen = LIST; markDirty(); return; }
      refreshProgress(epubproc::convertPercent());
      return;
    }
    if (s_screen == INDEXING) {
      textdoc::indexStep(16384);
      if (textdoc::indexReady()) { enterRead(); return; }
      refreshProgress(textdoc::indexPercent());
      return;
    }
    // Bücher-Scan häppchenweise — läuft auch unterm Lesen (READ) im
    // Hintergrund weiter; Fortschritt nur im Listen-Screen zeichnen.
    if (s_scanning) {
      for (int i = 0; i < 4 && s_scanning; i++) scanStep();
      if (s_screen == LIST && s_scanning) refreshProgress(s_fileCount);
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    switch (s_screen) {
      case LIST:       drawList(g); break;
      case CONVERTING: drawProgress(g, "Konvertiere EPUB ...", epubproc::convertPercent()); break;
      case INDEXING:   drawProgress(g, "Indexiere Buch ...", textdoc::indexPercent()); break;
      case READ:       drawRead(g); break;
    }
  }

 private:
  uint32_t s_lastProgDraw = 0;
  int      s_lastPct = -1;

  void refreshProgress(int pct) {
    if (appmgr::isDirty()) return;
    uint32_t now = millis();
    if (pct != s_lastPct && now - s_lastProgDraw >= 2000) {
      s_lastProgDraw = now;
      s_lastPct = pct;
      appmgr::markDirty();
    }
  }
};

ReaderApp s_app;

}  // namespace

namespace reader_app {

App* get() { return &s_app; }

void debugScan() {
  uint32_t t0 = millis();
  scanStart();
  while (s_scanning) scanStep();   // schreibt am Ende auch den SD-Cache neu
  Serial.printf("[READER] %d Buch/Bücher unter %s (%lu ms):\n",
                s_fileCount, kBookDir, (unsigned long)(millis() - t0));
  for (int i = 0; i < s_fileCount; i++)
    Serial.printf("[READER]   %s\n", s_files[i].path);
}

}  // namespace reader_app
