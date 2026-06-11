#include "reader_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/settings.h"
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

// --- Bücher-Scan: häppchenweise (Calibre legt Bücher unter Autor/Titel/ ab) ----
// Der Scan läuft NICHT blockierend: onEnter füllt nur den Verzeichnis-Stack,
// tick() arbeitet pro Aufruf wenige Verzeichnisse ab (spiLock nur pro
// Verzeichnis). Wichtig, weil appmgr::begin() die letzte App schon im Boot
// wiederherstellt — ein synchroner Scan hier hielt das ganze Setup an.
void autoOpenLastBook();   // unten definiert (braucht openBook)

constexpr int kMaxStack = 256;
char (*s_dirStack)[kPathLen] = nullptr;
int   s_dirStackN = 0;
bool  s_scanning  = false;
bool  s_autoOpen  = false;   // nach Scan-Ende letztes Buch öffnen (1x pro Boot)

// Tiefe relativ zu /books ("/books" = Tiefe 0).
int dirDepth(const char* p) {
  int n = 0;
  for (; *p; p++) if (*p == '/') n++;
  return n - 1;
}

void scanStart(bool autoOpen) {
  if (!s_files) {
    s_files = (BookFile*)heap_caps_malloc(sizeof(BookFile) * kMaxBooks, MALLOC_CAP_SPIRAM);
    if (!s_files) s_files = (BookFile*)malloc(sizeof(BookFile) * kMaxBooks);
  }
  if (!s_dirStack) {
    s_dirStack = (char(*)[kPathLen])heap_caps_malloc(kMaxStack * kPathLen, MALLOC_CAP_SPIRAM);
    if (!s_dirStack) s_dirStack = (char(*)[kPathLen])malloc(kMaxStack * kPathLen);
  }
  s_fileCount = 0;
  s_sel = 0; s_off = 0;
  s_scanning = false;
  s_autoOpen = false;
  if (!s_files || !s_dirStack || !board::sdReady()) return;
  strncpy(s_dirStack[0], kBookDir, kPathLen - 1);
  s_dirStack[0][kPathLen - 1] = '\0';
  s_dirStackN = 1;
  s_scanning = true;
  s_autoOpen = autoOpen;
}

void scanFinish() {
  s_scanning = false;
  qsort(s_files, s_fileCount, sizeof(BookFile), cmpFile);
  s_sel = 0; s_off = 0;
  markDirty();
  if (s_autoOpen) { s_autoOpen = false; autoOpenLastBook(); }
}

// Ein Verzeichnis vom Stack abarbeiten (LIFO = Tiefensuche, hält den Stack klein).
void scanStep() {
  if (!s_scanning) return;
  if (s_dirStackN == 0 || s_fileCount >= kMaxBooks) { scanFinish(); return; }

  char dir[kPathLen];
  strncpy(dir, s_dirStack[--s_dirStackN], kPathLen - 1);
  dir[kPathLen - 1] = '\0';
  int depth = dirDepth(dir);

  spiLock();
  File d = SD.open(dir);
  if (d && d.isDirectory()) {
    File f;
    while ((f = d.openNextFile()) && s_fileCount < kMaxBooks) {
      const char* nm = f.name();
      if (nm[0] == '.') { f.close(); continue; }
      if (f.isDirectory()) {
        if (depth < kScanDepth && s_dirStackN < kMaxStack)
          snprintf(s_dirStack[s_dirStackN++], kPathLen, "%s/%s", dir, nm);
      } else if (hasExt(nm, ".txt") || hasExt(nm, ".epub")) {
        BookFile& b = s_files[s_fileCount];
        strncpy(b.name, nm, kNameLen - 1); b.name[kNameLen - 1] = '\0';
        snprintf(b.path, kPathLen, "%s/%s", dir, nm);
        b.isEpub = hasExt(nm, ".epub");
        s_fileCount++;
      }
      f.close();
    }
  }
  if (d) d.close();
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

void openBook(int idx) {
  if (idx < 0 || idx >= s_fileCount) return;
  BookFile& b = s_files[idx];
  s_posKey = settings::crc32(b.path);
  settings::setLastBook(b.path);

  if (b.isEpub) {
    epubproc::buildCachePath(b.path, s_txtPath, sizeof(s_txtPath));
    spiLock();
    bool cached = SD.exists(s_txtPath);
    spiUnlock();
    if (cached) { openTxt(); return; }
    if (epubproc::convertBegin(b.path, s_txtPath)) {
      if (epubproc::convertDone()) { openTxt(); return; }
      s_screen = CONVERTING;
      markDirty();
    } else {
      markDirty();   // Fehler -> Liste bleibt, Log im Serial
    }
  } else {
    strncpy(s_txtPath, b.path, sizeof(s_txtPath) - 1);
    s_txtPath[sizeof(s_txtPath) - 1] = '\0';
    openTxt();
  }
}

// Beim ersten Betreten nach dem Boot direkt ins zuletzt gelesene Buch springen
// (die Seite kommt in enterRead() aus der NVS-Leseposition).
void autoOpenLastBook() {
  char last[kPathLen];
  settings::lastBook(last, sizeof(last));
  if (!last[0]) return;
  for (int i = 0; i < s_fileCount; i++) {
    if (strcmp(s_files[i].path, last) == 0) {
      s_sel = i;
      s_off = (i >= VISIBLE) ? i - VISIBLE + 1 : 0;
      Serial.printf("[READER] Resume: %s\n", last);
      openBook(i);
      return;
    }
  }
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
  gui::printAt(g, 6, HEADER_Y, "Bücher", 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, "Keine SD-Karte", 2);
    gui::printAt(g, 10, 110, "Karte einlegen, dann:", 1);
    gui::drawButton(g, kRetrySD, "Erneut suchen", false);
    return;
  }
  if (s_scanning) {
    gui::printAt(g, 10, 80, "Suche Bücher ...", 2);
    gui::drawButton(g, kBack, "Home", false);
    return;
  }
  if (s_fileCount <= 0) {
    gui::printAt(g, 10, 80, "Keine Bücher", 2);
    gui::printAt(g, 10, 110, ".txt/.epub nach /books legen.", 1);
    gui::drawButton(g, kRetrySD, "Erneut suchen", false);
    return;
  }

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

  gui::drawButton(g, kBack, "Home", false);
  if (s_off > 0)                      gui::drawButton(g, kUp,   "Hoch", false);
  if (s_off + VISIBLE < s_fileCount)  gui::drawButton(g, kDown, "Runter", false);
}

void drawProgress(Adafruit_GFX& g, const char* what, int percent) {
  gui::printAt(g, 6, HEADER_Y, "Lesen", 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
  gui::printAt(g, 10, 90, what, 2);
  int barX = 10, barY = 130, barW = W - 20, barH = 16;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  int fw = (barW - 2) * percent / 100;
  if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", percent);
  gui::printAt(g, 10, 156, pct, 1);
  gui::drawButton(g, kCancel, "Abbrechen", false);
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
  if (board::initSD()) scanStart(false);
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
  const char* name() const override { return "Lesen"; }

  void onEnter() override {
    // Nur Stack füllen — der Scan selbst läuft häppchenweise in tick().
    if (s_fileCount < 0 && board::sdReady()) scanStart(true);
  }

  void onLeave() override {
    s_autoOpen = false;   // Nutzer ist weg — nicht später ins Buch springen
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
    // Bücher-Scan häppchenweise (ein paar Verzeichnisse pro Tick).
    if (s_scanning) {
      for (int i = 0; i < 4 && s_scanning; i++) scanStep();
      return;
    }
    // Konvertierung/Indexierung häppchenweise; Fortschritt alle ~2 s zeichnen.
    if (s_screen == CONVERTING) {
      epubproc::convertStep();
      if (epubproc::convertDone())        { openTxt(); return; }
      else if (epubproc::convertFailed()) { s_screen = LIST; markDirty(); return; }
      refreshProgress(epubproc::convertPercent());
    } else if (s_screen == INDEXING) {
      textdoc::indexStep(16384);
      if (textdoc::indexReady()) { enterRead(); return; }
      refreshProgress(textdoc::indexPercent());
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
  scanStart(false);
  while (s_scanning) scanStep();
  Serial.printf("[READER] %d Buch/Bücher unter %s:\n", s_fileCount, kBookDir);
  for (int i = 0; i < s_fileCount; i++)
    Serial.printf("[READER]   %s\n", s_files[i].path);
}

}  // namespace reader_app
