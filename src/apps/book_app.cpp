#include "book_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"
#include "services/audio.h"

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_heap_caps.h>
#include <string.h>
#include <strings.h>

namespace {

using gui::Rect;

constexpr const char* kBookDir  = "/audiobooks";
constexpr int kMaxBooks    = 128;
constexpr int kMaxChapters = 256;
constexpr int kNameLen     = 64;
constexpr int kPathLen     = 192;

// --- Layout -------------------------------------------------------------------
constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_Y = TOP + 2;
constexpr int HEADER_H = TOP + 26;
constexpr int ROW_H    = 30;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int VISIBLE  = 7;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;   // 268

const Rect kBack    {6,       BAR_Y, 72, 44};
const Rect kUp      {84,      BAR_Y, 72, 44};
const Rect kDown    {162,     BAR_Y, 72, 44};
const Rect kRetrySD {20, 150, W - 40, 48};

// Player-Screen
constexpr int PROG_Y = 116;
constexpr int PROG_H = 50;
const Rect kBtnBack30 {6,   180, 70, 52};
const Rect kBtnPlay   {85,  180, 70, 52};
const Rect kBtnFwd30  {164, 180, 70, 52};
const Rect kBtnPrevCh {6,   240, 70, 44};
const Rect kBtnSleep  {85,  240, 70, 44};
const Rect kBtnNextCh {164, 240, 70, 44};
const Rect kBtnList   {6,   292, W - 12, 26};

// --- Zustand --------------------------------------------------------------------
enum Screen { LIST, PLAYER };
Screen s_screen = LIST;

struct Book { char name[kNameLen]; char path[kPathLen]; bool isFile; };
Book*    s_books = nullptr;          // PSRAM
int      s_bookCount = -1;           // -1 = noch nicht gescannt

// Geöffnetes Buch
char     s_bookName[kNameLen] = "";
char     s_bookPath[kPathLen] = "";
uint32_t s_bookKey = 0;              // CRC32 des Buchpfads (Bookmark-Key)
typedef char ChapterPath[kPathLen];
ChapterPath* s_chap = nullptr;       // PSRAM
int      s_chapCount = 0;

int      s_sel = 0, s_off = 0;       // Listen-Cursor + Scroll-Fenster

// Bookmark-Verfolgung (für Sofort-Save bei Pause/Eviction/Stop).
bool     s_wasPlaying = false;
uint16_t s_lastFile = 0;
uint32_t s_lastPos  = 0;
uint32_t s_lastSave = 0;

uint32_t s_lastProg     = 0;
uint32_t s_lastShownSec = 999999;
int      s_progCount    = 0;

constexpr uint32_t kProgressMs = 3000;

void markDirty() { appmgr::markDirty(); }

bool isAudioFile(const char* n) {
  auto ext = [&](const char* e) {
    size_t ln = strlen(n), le = strlen(e);
    return ln > le && strcasecmp(n + ln - le, e) == 0;
  };
  return ext(".mp3") || ext(".m4a") || ext(".aac") || ext(".flac") || ext(".wav");
}

// Natürlicher Vergleich: Ziffernfolgen numerisch ("Kap 2" < "Kap 10").
int naturalCmp(const char* a, const char* b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      unsigned long va = strtoul(a, (char**)&a, 10);
      unsigned long vb = strtoul(b, (char**)&b, 10);
      if (va != vb) return (va < vb) ? -1 : 1;
    } else {
      int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
      if (ca != cb) return ca - cb;
      a++; b++;
    }
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int cmpBook(const void* x, const void* y) {
  return naturalCmp(((const Book*)x)->name, ((const Book*)y)->name);
}
int cmpChap(const void* x, const void* y) {
  return naturalCmp((const char*)x, (const char*)y);
}

// /audiobooks scannen: Unterordner = Bücher, lose Audio-Dateien = 1-Kapitel-Bücher.
void scanBooks() {
  if (!s_books) {
    s_books = (Book*)heap_caps_malloc(sizeof(Book) * kMaxBooks, MALLOC_CAP_SPIRAM);
    if (!s_books) s_books = (Book*)malloc(sizeof(Book) * kMaxBooks);
  }
  s_bookCount = 0;
  if (!s_books || !board::sdReady()) return;

  spiLock();
  File d = SD.open(kBookDir);
  if (d && d.isDirectory()) {
    File f;
    while ((f = d.openNextFile()) && s_bookCount < kMaxBooks) {
      const char* nm = f.name();
      if (f.isDirectory() || isAudioFile(nm)) {
        Book& b = s_books[s_bookCount];
        strncpy(b.name, nm, kNameLen - 1); b.name[kNameLen - 1] = '\0';
        snprintf(b.path, kPathLen, "%s/%s", kBookDir, nm);
        b.isFile = !f.isDirectory();
        s_bookCount++;
      }
      f.close();
    }
  }
  if (d) d.close();
  spiUnlock();

  qsort(s_books, s_bookCount, sizeof(Book), cmpBook);
  s_sel = 0; s_off = 0;
}

// Kapitel eines Buchs einsammeln (eine Ordner-Ebene, natürliche Sortierung).
int scanChapters(const Book& b) {
  if (!s_chap) {
    s_chap = (ChapterPath*)heap_caps_malloc(sizeof(ChapterPath) * kMaxChapters, MALLOC_CAP_SPIRAM);
    if (!s_chap) s_chap = (ChapterPath*)malloc(sizeof(ChapterPath) * kMaxChapters);
  }
  s_chapCount = 0;
  if (!s_chap) return 0;

  if (b.isFile) {
    strncpy(s_chap[0], b.path, kPathLen - 1); s_chap[0][kPathLen - 1] = '\0';
    s_chapCount = 1;
    return 1;
  }

  spiLock();
  File d = SD.open(b.path);
  if (d && d.isDirectory()) {
    File f;
    while ((f = d.openNextFile()) && s_chapCount < kMaxChapters) {
      if (!f.isDirectory() && isAudioFile(f.name()) && f.size() > 0) {
        snprintf(s_chap[s_chapCount], kPathLen, "%s/%s", b.path, f.name());
        s_chapCount++;
      }
      f.close();
    }
  }
  if (d) d.close();
  spiUnlock();

  qsort(s_chap, s_chapCount, sizeof(ChapterPath), cmpChap);
  return s_chapCount;
}

// Bookmark sofort sichern (Pause/Eviction/Verlassen).
void saveBookmarkNow() {
  if (s_bookKey && (s_lastFile || s_lastPos))
    settings::setBookBookmark(s_bookKey, s_lastFile, s_lastPos);
}

// Buch öffnen: Kapitel scannen, Bookmark lesen, Wiedergabe starten.
void openBook(int idx) {
  if (idx < 0 || idx >= s_bookCount) return;
  saveBookmarkNow();   // evtl. laufendes Buch sichern

  Book& b = s_books[idx];
  strncpy(s_bookName, b.name, kNameLen - 1); s_bookName[kNameLen - 1] = '\0';
  strncpy(s_bookPath, b.path, kPathLen - 1); s_bookPath[kPathLen - 1] = '\0';
  s_bookKey = settings::crc32(b.path);

  if (scanChapters(b) == 0) { s_bookKey = 0; markDirty(); return; }

  uint16_t fileIdx = 0; uint32_t posSec = 0;
  if (settings::bookBookmark(s_bookKey, &fileIdx, &posSec)) {
    if (fileIdx >= s_chapCount) { fileIdx = 0; posSec = 0; }
  }

  audio::queueBegin(audio::Owner::Book);
  for (int i = 0; i < s_chapCount; i++) audio::queueAdd(s_chap[i]);
  // Hörbuch: weder Shuffle noch Repeat; am Buchende stoppen.
  audio::setShuffle(false);
  audio::setRepeat(audio::Repeat::Off);
  audio::queueCommit(fileIdx, posSec > 5 ? posSec - 5 : 0);

  s_lastFile = fileIdx; s_lastPos = posSec;
  s_lastSave = millis();
  s_screen = PLAYER;
  markDirty();
}

// --- Zeichnen -------------------------------------------------------------------
void drawList(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::BookHeader), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::NoSdCard), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::InsertCard), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }
  if (s_bookCount <= 0) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::BookNone), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::BookHint1), 1);
    gui::printAt(g, 10, 124, i18n::tr(i18n::Str::BookHint2), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }

  for (int r = 0; r < VISIBLE && s_off + r < s_bookCount; r++) {
    int i = s_off + r;
    // Bücher mit Bookmark markieren (>>).
    char lbl[80];
    uint16_t fi; uint32_t ps;
    bool bm = settings::bookBookmark(settings::crc32(s_books[i].path), &fi, &ps);
    snprintf(lbl, sizeof(lbl), "%s%s", bm ? "\x10 " : "", s_books[i].name);
    gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, false);
  }
  // Cursor
  int cr = s_sel - s_off;
  if (cr >= 0 && cr < VISIBLE) {
    int y = LIST_Y + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }

  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnHome), false);
  if (s_off > 0)                       gui::drawButton(g, kUp,   i18n::tr(i18n::Str::BtnUp), false);
  if (s_off + VISIBLE < s_bookCount)   gui::drawButton(g, kDown, i18n::tr(i18n::Str::BtnDown), false);
}

void drawTimeAndBar(Adafruit_GFX& g, const audio::Status& st) {
  char t1[12], t2[12];
  snprintf(t1, sizeof(t1), "%lu:%02lu", (unsigned long)(st.posSec / 60), (unsigned long)(st.posSec % 60));
  snprintf(t2, sizeof(t2), "%lu:%02lu", (unsigned long)(st.durSec / 60), (unsigned long)(st.durSec % 60));
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(2);
  g.setCursor(6, PROG_Y + 8); g.print(t1);
  if (st.durSec > 0) {
    int16_t bx, by; uint16_t bw, bh;
    g.getTextBounds(t2, 0, 0, &bx, &by, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, PROG_Y + 8); g.print(t2);
  }
  int barX = 6, barY = PROG_Y + 34, barW = W - 12, barH = 12;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (st.durSec > 0 && st.posSec <= st.durSec) {
    int fw = (int)((uint64_t)(barW - 2) * st.posSec / st.durSec);
    if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  }
}

void drawProgStrip(Adafruit_GFX& g) {
  g.fillRect(0, PROG_Y, W, PROG_H, GxEPD_WHITE);
  drawTimeAndBar(g, audio::status());
}

void drawPlayer(Adafruit_GFX& g) {
  audio::Status st = audio::status();
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::AppBook), 2);
  if (st.sleepMin > 0) {
    char sl[16];
    snprintf(sl, sizeof(sl), "Zzz %um", (unsigned)st.sleepMin);
    g.setTextSize(1);
    uint16_t bw, bh;
    gui::textBounds(g, sl, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, HEADER_Y + 4);
    gui::print(g, sl);
  }
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  gui::printAt(g, 6, HEADER_H + 8, s_bookName, 2);

  // Kapitel x/y + Dateiname.
  bool mine = (st.owner == audio::Owner::Book && st.pos >= 0);
  char ch[80] = "(gestoppt)";
  if (mine && st.pos < s_chapCount) {
    const char* base = strrchr(s_chap[st.pos], '/');
    snprintf(ch, sizeof(ch), i18n::tr(i18n::Str::FmtChapter), st.pos + 1, s_chapCount,
             base ? base + 1 : "");
  }
  gui::printAt(g, 6, HEADER_H + 32, ch, 1);

  drawTimeAndBar(g, st);

  gui::drawButton(g, kBtnBack30, "<30", false);
  gui::drawButton(g, kBtnPlay, (st.playing && !st.paused && mine) ? "||" : ">", true);
  gui::drawButton(g, kBtnFwd30, "30>", false);
  gui::drawButton(g, kBtnPrevCh, "|<", false);
  gui::drawButton(g, kBtnSleep, "Zzz", st.sleepMin > 0);
  gui::drawButton(g, kBtnNextCh, ">|", false);
  gui::drawButton(g, kBtnList, i18n::tr(i18n::Str::BtnBookList), false);
}

// --- Interaktion ------------------------------------------------------------------
void retrySD() {
  if (board::initSD()) scanBooks();
  markDirty();
}

void moveSel(int delta) {
  if (s_bookCount <= 0) return;
  int ns = s_sel + delta;
  if (ns < 0) ns = s_bookCount - 1;
  if (ns >= s_bookCount) ns = 0;
  s_sel = ns;
  // Fenster nachziehen.
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  markDirty();
}

// Wiedergabe fortsetzen oder (falls gestoppt) Buch neu öffnen.
void togglePlay() {
  audio::Status st = audio::status();
  if (st.owner == audio::Owner::Book && st.playing) {
    audio::togglePause();
    // Bei Pause sofort sichern.
    if (!st.paused) saveBookmarkNow();
  } else if (s_bookKey) {
    // Gestoppt (z. B. evicted): Buch ab Bookmark neu starten.
    for (int i = 0; i < s_bookCount; i++) {
      if (settings::crc32(s_books[i].path) == s_bookKey) { openBook(i); return; }
    }
  }
  markDirty();
}

void cycleSleep() {
  uint16_t m = audio::status().sleepMin;
  uint16_t n = (m == 0) ? 45 : (m <= 15) ? 0 : (m <= 30) ? 15 : (m <= 45) ? 30 : 45;
  audio::setSleepTimer(n);
  markDirty();
}

void onListTouch(int x, int y) {
  if (kBack.hit(x, y)) { appmgr::goHome(); return; }
  if (!board::sdReady() || s_bookCount <= 0) {
    if (kRetrySD.hit(x, y)) retrySD();
    return;
  }
  if (kUp.hit(x, y))   { s_off = (s_off > VISIBLE) ? s_off - VISIBLE : 0; markDirty(); return; }
  if (kDown.hit(x, y)) {
    if (s_off + VISIBLE < s_bookCount) { s_off += VISIBLE; markDirty(); }
    return;
  }
  if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
    int r = (y - LIST_Y) / ROW_H;
    if (s_off + r < s_bookCount) { s_sel = s_off + r; openBook(s_sel); }
  }
}

void onPlayerTouch(int x, int y) {
  if      (kBtnPlay.hit(x, y))   { togglePlay(); return; }
  else if (kBtnBack30.hit(x, y)) audio::seekRel(-30);
  else if (kBtnFwd30.hit(x, y))  audio::seekRel(+30);
  else if (kBtnPrevCh.hit(x, y)) audio::prev();
  else if (kBtnNextCh.hit(x, y)) audio::next();
  else if (kBtnSleep.hit(x, y))  { cycleSleep(); return; }
  else if (kBtnList.hit(x, y))   { s_screen = LIST; markDirty(); return; }
  else return;
  markDirty();
}

void onListKey(char k) {
  switch (k) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(+1); break;
    case '\r':          if (s_bookCount > 0) openBook(s_sel); break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onPlayerKey(char k) {
  switch (k) {
    case ' ': case '\r': togglePlay(); break;
    case 'a': case 'A':  audio::seekRel(-30); markDirty(); break;
    case 'd': case 'D':  audio::seekRel(+30); markDirty(); break;
    case 'w': case 'W':  audio::volumeUp(); markDirty(); break;
    case 's': case 'S':  audio::volumeDown(); markDirty(); break;
    case 'b': case 'B':  audio::prev(); markDirty(); break;
    case 'n': case 'N':  audio::next(); markDirty(); break;
    case 'z': case 'Z':  cycleSleep(); break;
    case '\b':           s_screen = LIST; markDirty(); break;
    case 'q': case 'Q':  appmgr::goHome(); break;
    default: break;
  }
}

// --- App-Klasse ---------------------------------------------------------------------
class BookApp : public App {
 public:
  const char* id()   const override { return "Hörbuch"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppBook); }

  void onEnter() override {
    if (s_bookCount < 0 && board::sdReady()) scanBooks();
    // Läuft gerade ein Buch -> direkt in den Player.
    audio::Status st = audio::status();
    if (st.owner == audio::Owner::Book && st.playing && s_bookKey) s_screen = PLAYER;
  }

  void onLeave() override { saveBookmarkNow(); }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      if (s_screen == LIST) onListTouch(e.x, e.y);
      else                  onPlayerTouch(e.x, e.y);
    } else {
      if (s_screen == LIST) onListKey(e.key);
      else                  onPlayerKey(e.key);
    }
  }

  // Läuft JEDEN Loop (auch im Hintergrund): Bookmark-Pflege.
  void background() override {
    audio::Status st = audio::status();
    bool mine = (st.owner == audio::Owner::Book && st.playing);
    if (mine && s_bookKey) {
      s_lastFile = (uint16_t)((st.pos >= 0) ? st.pos : 0);
      s_lastPos  = st.posSec;
      uint32_t now = millis();
      if (now - s_lastSave >= 30000) {
        s_lastSave = now;
        settings::setBookBookmark(s_bookKey, s_lastFile, s_lastPos);
      }
    } else if (s_wasPlaying) {
      // Eviction durch Musik / Stop / Buchende: letzte Position sofort sichern.
      saveBookmarkNow();
    }
    s_wasPlaying = mine;
  }

  void tick() override {
    if (appmgr::isDirty() || s_screen != PLAYER) return;
    audio::Status st = audio::status();
    if (st.owner == audio::Owner::Book && st.playing && !st.paused &&
        st.posSec != s_lastShownSec && millis() - s_lastProg >= kProgressMs) {
      s_lastProg = millis();
      s_lastShownSec = st.posSec;
      if (++s_progCount % 10 == 0) appmgr::markDirty();
      else display::renderRegion(drawProgStrip, PROG_Y, PROG_H);
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    s_lastShownSec = 999999;
    if (s_screen == LIST) drawList(g);
    else                  drawPlayer(g);
  }
};

BookApp s_app;

}  // namespace

namespace book_app {

App* get() { return &s_app; }

}  // namespace book_app
