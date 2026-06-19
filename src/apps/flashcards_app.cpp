// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// flashcards_app.cpp — "Karteikarten": Decks von der SD lernen (Leitner).
//
// Decks liegen als Textdateien unter /flashcards/*.txt (oder .tsv), eine Karte
// pro Zeile "Vorderseite<TAB>Rückseite" (oder "... | ..."), '#' = Kommentar.
// Lernfortschritt je Deck unter /flashcards/.progress/<deck>.bin (Karten über
// flashcards::cardKey(front) referenziert -> übersteht Umsortieren des Decks).
//
// SD-Disziplin wie notes_app: jede SD-Operation gechunkt unter spiLock; geparst
// und gezeichnet wird lock-frei. Parser/Leitner-Logik sind host-getestet.
// =============================================================================
#include "flashcards_app.h"

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "core/board.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "services/timesync.h"
#include "apps/flashcards_core.h"

namespace {

using gui::Rect;
using flashcards::Card;

constexpr int W = EINK_W;

constexpr const char* kDeckDir = "/flashcards";
constexpr const char* kProgDir = "/flashcards/.progress";
constexpr int kMaxDecks = 64;
constexpr int kMaxCards = 2000;     // PSRAM-Hartgrenze (Überlauf-Schutz)
constexpr int kNameLen  = 48;
constexpr uint32_t kClockOk = 1700000000UL;   // Systemzeit gilt als gestellt

// --- Persistenz-Karte ---------------------------------------------------------
struct LCard {
  Card     c;
  uint32_t key;
  uint8_t  box;
  uint32_t lastSeen;
};

// --- Layout: Liste ------------------------------------------------------------
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_H = TOP + 26;
constexpr int ROW_H    = 32;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int VISIBLE  = 6;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;
const Rect kBack {6,   BAR_Y, 72, 44};
const Rect kUp   {84,  BAR_Y, 72, 44};
const Rect kDown {162, BAR_Y, 72, 44};
const Rect kRetrySD {20, 150, W - 40, 48};

// --- Layout: Lernen -----------------------------------------------------------
const Rect kReveal {10, 250, 220, 50};
const Rect kWrong  {10, 250, 105, 50};
const Rect kRight  {125, 250, 105, 50};

// --- Zustand ------------------------------------------------------------------
enum Screen { DECKS, STUDY, DONE };
Screen s_screen = DECKS;

char (*s_decks)[kNameLen] = nullptr;   // Deck-Dateinamen
int  s_deckCount = -1;                  // -1 = noch nicht gescannt
int  s_sel = 0, s_off = 0;

LCard*    s_cards = nullptr;
int       s_cardCount = 0;
char      s_deckName[kNameLen] = "";
bool      s_progDirty = false;

int*  s_queue = nullptr;        // Index-Warteschlange fälliger Karten
int   s_queueLen = 0, s_qpos = 0;
constexpr int kQueueCap = kMaxCards * 3;

bool s_revealed = false;
int  s_seen = 0, s_correct = 0;   // Sitzungsstatistik

// =============================================================================
// Puffer (PSRAM, lazy)
// =============================================================================
bool ensureBuffers() {
  if (!s_decks) {
    s_decks = (char(*)[kNameLen])heap_caps_malloc((size_t)kMaxDecks * kNameLen, MALLOC_CAP_SPIRAM);
    if (!s_decks) s_decks = (char(*)[kNameLen])malloc((size_t)kMaxDecks * kNameLen);
  }
  if (!s_cards) {
    s_cards = (LCard*)heap_caps_malloc(sizeof(LCard) * kMaxCards, MALLOC_CAP_SPIRAM);
    if (!s_cards) s_cards = (LCard*)malloc(sizeof(LCard) * kMaxCards);
  }
  if (!s_queue) {
    s_queue = (int*)heap_caps_malloc(sizeof(int) * kQueueCap, MALLOC_CAP_SPIRAM);
    if (!s_queue) s_queue = (int*)malloc(sizeof(int) * kQueueCap);
  }
  return s_decks && s_cards && s_queue;
}

bool hasDeckExt(const char* n) {
  size_t ln = strlen(n);
  return (ln > 4 && (strcasecmp(n + ln - 4, ".txt") == 0 ||
                     strcasecmp(n + ln - 4, ".tsv") == 0));
}

int cmpName(const void* a, const void* b) {
  return strcasecmp((const char*)a, (const char*)b);
}

// =============================================================================
// Deck-Liste scannen
// =============================================================================
void scanDecks() {
  if (!ensureBuffers() || !board::sdReady()) { s_deckCount = -1; return; }
  s_deckCount = 0;
  spiLock();
  if (!SD.exists(kDeckDir)) SD.mkdir(kDeckDir);
  File dir = SD.open(kDeckDir);
  bool ok = dir && dir.isDirectory();
  spiUnlock();
  if (!ok) { if (dir) { spiLock(); dir.close(); spiUnlock(); } s_deckCount = 0; return; }

  while (s_deckCount < kMaxDecks) {
    spiLock();
    bool isDir = false;
    String entry = dir.getNextFileName(&isDir);
    spiUnlock();
    if (entry.length() == 0) break;
    const char* full = entry.c_str();
    const char* nm = strrchr(full, '/');
    nm = nm ? nm + 1 : full;
    if (isDir || nm[0] == '.' || !hasDeckExt(nm)) continue;
    strncpy(s_decks[s_deckCount], nm, kNameLen - 1);
    s_decks[s_deckCount][kNameLen - 1] = '\0';
    s_deckCount++;
  }
  spiLock(); dir.close(); spiUnlock();
  qsort(s_decks, s_deckCount, kNameLen, cmpName);
  Serial.printf("[CARDS] %d Deck(s) unter %s\n", s_deckCount, kDeckDir);
}

// =============================================================================
// Deck + Fortschritt laden
// =============================================================================
void progPath(const char* deck, char* out, size_t n) {
  snprintf(out, n, "%s/%s.prg", kProgDir, deck);
}

// Fortschrittsdatei: uint32 count, dann count x {uint32 key, uint8 box, uint32 lastSeen}.
void loadProgress() {
  char path[96];
  progPath(s_deckName, path, sizeof(path));
  spiLock();
  File f = SD.open(path);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return;
  uint32_t count = 0;
  spiLock();
  bool ok = f.read((uint8_t*)&count, 4) == 4;
  spiUnlock();
  for (uint32_t i = 0; ok && i < count; i++) {
    uint8_t rec[9];
    spiLock();
    ok = f.read(rec, 9) == 9;
    spiUnlock();
    if (!ok) break;
    uint32_t key; uint8_t box; uint32_t seen;
    memcpy(&key, rec, 4); box = rec[4]; memcpy(&seen, rec + 5, 4);
    for (int j = 0; j < s_cardCount; j++)
      if (s_cards[j].key == key) { s_cards[j].box = box; s_cards[j].lastSeen = seen; break; }
  }
  spiLock(); f.close(); spiUnlock();
}

void saveProgress() {
  if (!s_progDirty || !board::sdReady() || !s_deckName[0]) return;
  spiLock();
  if (!SD.exists(kProgDir)) SD.mkdir(kProgDir);
  spiUnlock();
  char path[96];
  progPath(s_deckName, path, sizeof(path));
  spiLock();
  File f = SD.open(path, FILE_WRITE);
  bool ok = (bool)f;
  spiUnlock();
  if (!ok) return;
  uint32_t count = (uint32_t)s_cardCount;
  spiLock();
  ok = f.write((const uint8_t*)&count, 4) == 4;
  spiUnlock();
  for (int i = 0; ok && i < s_cardCount; i++) {
    uint8_t rec[9];
    memcpy(rec, &s_cards[i].key, 4);
    rec[4] = s_cards[i].box;
    memcpy(rec + 5, &s_cards[i].lastSeen, 4);
    spiLock();
    ok = f.write(rec, 9) == 9;
    spiUnlock();
  }
  spiLock(); f.close(); spiUnlock();
  if (ok) s_progDirty = false;
  Serial.printf("[CARDS] Fortschritt %s (%s, %d Karten)\n",
                s_deckName, ok ? "ok" : "FEHLER", s_cardCount);
}

// Deck-Datei streamen und in s_cards parsen (512-B-Chunks unter spiLock).
void parseDeckFile() {
  s_cardCount = 0;
  char path[96];
  snprintf(path, sizeof(path), "%s/%s", kDeckDir, s_deckName);
  spiLock();
  File f = SD.open(path);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return;

  // Eine angefangene Zeile bleibt zwischen den Chunks in line/ll stehen.
  char line[256];
  int  ll = 0;
  uint8_t buf[512];
  auto commit = [&]() {
    line[ll] = '\0';
    ll = 0;
    Card c;
    if (flashcards::parseLine(line, &c) && s_cardCount < kMaxCards) {
      LCard& lc = s_cards[s_cardCount];
      lc.c = c;
      lc.key = flashcards::cardKey(c.front);
      lc.box = 0;
      lc.lastSeen = 0;
      s_cardCount++;
    }
  };
  while (s_cardCount < kMaxCards) {
    spiLock();
    int rd = f.read(buf, sizeof(buf));
    spiUnlock();
    if (rd <= 0) break;
    for (int i = 0; i < rd && s_cardCount < kMaxCards; i++) {
      char ch = (char)buf[i];
      if (ch == '\r') continue;
      if (ch == '\n') commit();
      else if (ll < (int)sizeof(line) - 1) line[ll++] = ch;
    }
  }
  if (ll > 0 && s_cardCount < kMaxCards) commit();   // letzte Zeile ohne '\n'
  spiLock(); f.close(); spiUnlock();
  Serial.printf("[CARDS] Deck %s: %d Karten\n", s_deckName, s_cardCount);
}

// Warteschlange aus fälligen Karten bauen (ohne gestellte Uhr: alle).
void buildQueue() {
  uint32_t now = (uint32_t)timesync::now();
  bool clockOk = now >= kClockOk;
  s_queueLen = 0;
  for (int i = 0; i < s_cardCount && s_queueLen < kQueueCap; i++) {
    if (!clockOk || flashcards::isDue(s_cards[i].box, s_cards[i].lastSeen, now))
      s_queue[s_queueLen++] = i;
  }
  s_qpos = 0;
}

void openDeck(int idx) {
  strncpy(s_deckName, s_decks[idx], kNameLen - 1);
  s_deckName[kNameLen - 1] = '\0';
  s_progDirty = false;
  parseDeckFile();
  loadProgress();
  buildQueue();
  s_seen = 0; s_correct = 0;
  s_revealed = false;
  s_screen = (s_queueLen > 0) ? STUDY : DONE;
  appmgr::markDirty();
}

// =============================================================================
// Lernen — Bewertung
// =============================================================================
void rate(bool correct) {
  if (s_qpos >= s_queueLen) return;
  int ci = s_queue[s_qpos];
  LCard& lc = s_cards[ci];
  uint32_t now = (uint32_t)timesync::now();
  lc.box = flashcards::nextBox(lc.box, correct);
  if (now >= kClockOk) lc.lastSeen = now;   // ohne Uhr nicht terminierbar
  s_progDirty = true;
  s_seen++;
  if (correct) s_correct++;
  else if (s_queueLen < kQueueCap) s_queue[s_queueLen++] = ci;   // nochmal drannehmen
  s_qpos++;
  s_revealed = false;
  if (s_qpos >= s_queueLen) { saveProgress(); s_screen = DONE; }
  appmgr::markDirty();
}

void retrySD() {
  if (board::initSD()) scanDecks();
  appmgr::markDirty();
}

void moveSel(int delta) {
  if (s_deckCount <= 0) return;
  s_sel = (s_sel + delta + s_deckCount) % s_deckCount;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  appmgr::markDirty();
}

// =============================================================================
// Zeichnen
// =============================================================================
// Wort-Umbruch in eine Box; gibt das y nach dem letzten gezeichneten Wort zurück.
int drawWrapped(Adafruit_GFX& g, int x, int y, int boxW, const char* text, uint8_t size) {
  int cols = boxW / (6 * size);
  if (cols < 1) cols = 1;
  int lineh = 9 * size;
  char line[64];
  int ll = 0;
  const char* p = text;
  while (*p) {
    // nächstes Wort
    const char* ws = p;
    while (*ws == ' ') ws++;
    const char* we = ws;
    while (*we && *we != ' ') we++;
    int wlen = (int)(we - ws);
    if (wlen > cols) wlen = cols;   // überlanges Wort hart kappen
    if (ll > 0 && ll + 1 + wlen > cols) {
      line[ll] = '\0';
      gui::printAt(g, x, y, line, size);
      y += lineh;
      ll = 0;
    }
    if (ll > 0) line[ll++] = ' ';
    for (int i = 0; i < wlen && ll < (int)sizeof(line) - 1; i++) line[ll++] = ws[i];
    p = we;
  }
  if (ll > 0) { line[ll] = '\0'; gui::printAt(g, x, y, line, size); y += lineh; }
  return y;
}

void deckListDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppCards), 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);

  if (s_deckCount < 0 || !board::sdReady()) {
    gui::printAt(g, 20, 80, "Keine SD-Karte", 2);
    gui::drawButton(g, kRetrySD, "Erneut suchen", true);
    return;
  }
  if (s_deckCount == 0) {
    gui::printAt(g, 14, 70, "Keine Decks in", 2);
    gui::printAt(g, 14, 96, "/flashcards", 2);
    gui::printAt(g, 14, 130, "Deck = Textdatei,", 1);
    gui::printAt(g, 14, 144, "Zeile: vorn<TAB>hinten", 1);
    return;
  }
  for (int i = 0; i < VISIBLE && s_off + i < s_deckCount; i++) {
    int idx = s_off + i;
    int y = LIST_Y + i * ROW_H;
    bool seld = (idx == s_sel);
    if (seld) g.fillRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.setTextColor(seld ? GxEPD_WHITE : GxEPD_BLACK);
    // ".txt"/".tsv" für die Anzeige abschneiden.
    char nm[kNameLen];
    strncpy(nm, s_decks[idx], sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0';
    size_t ln = strlen(nm);
    if (ln > 4) nm[ln - 4] = '\0';
    gui::printAt(g, 8, y + 8, nm, 2);
  }
  g.setTextColor(GxEPD_BLACK);
  gui::drawButton(g, kBack, "Home", false);
  gui::drawButton(g, kUp,   "W",    false);
  gui::drawButton(g, kDown, "S",    false);
}

void studyDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%d / %d", s_qpos + 1, s_queueLen);
  gui::printAt(g, 8, TOP + 4, hdr, 2);
  gui::printAt(g, W - 70, TOP + 8, "Q=zurueck", 1);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);

  int ci = s_queue[s_qpos];
  const LCard& lc = s_cards[ci];

  // Vorderseite (Frage).
  drawWrapped(g, 12, LIST_Y + 6, W - 24, lc.c.front, 2);

  if (s_revealed) {
    g.drawLine(12, 168, W - 12, 168, GxEPD_BLACK);
    drawWrapped(g, 12, 180, W - 24, lc.c.back, 2);
    gui::drawButton(g, kWrong, "Falsch", false);
    gui::drawButton(g, kRight, "Gewusst", true);
  } else {
    gui::drawButton(g, kReveal, "Antwort zeigen", true);
  }
}

void doneDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppCards), 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);
  if (s_cardCount == 0) {
    gui::printAt(g, 14, 80, "Deck ist leer", 2);
  } else if (s_seen == 0) {
    gui::printAt(g, 14, 80, "Nichts faellig.", 2);
    gui::printAt(g, 14, 110, "Spaeter wieder", 1);
    gui::printAt(g, 14, 124, "vorbeischauen.", 1);
  } else {
    gui::printAt(g, 14, 80, "Fertig!", 3);
    char s[48];
    snprintf(s, sizeof(s), "Gewusst: %d / %d", s_correct, s_seen);
    gui::printAt(g, 14, 130, s, 2);
  }
  gui::drawButton(g, Rect{20, 250, 200, 50}, "Zurueck", true);
}

// =============================================================================
// Eingabe
// =============================================================================
void deckListInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (s_deckCount <= 0) {
      if (kRetrySD.hit(e.x, e.y)) retrySD();
      return;
    }
    if (kBack.hit(e.x, e.y)) { appmgr::goHome(); return; }
    if (kUp.hit(e.x, e.y))   { moveSel(-1); return; }
    if (kDown.hit(e.x, e.y)) { moveSel(1); return; }
    for (int i = 0; i < VISIBLE && s_off + i < s_deckCount; i++) {
      int y = LIST_Y + i * ROW_H;
      if (e.y >= y && e.y < y + ROW_H) { s_sel = s_off + i; openDeck(s_sel); return; }
    }
    return;
  }
  switch (e.key) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(1);  break;
    case '\r':          if (s_deckCount > 0) openDeck(s_sel); else retrySD(); break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void studyInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (!s_revealed) { s_revealed = true; appmgr::markDirty(); return; }
    if (kWrong.hit(e.x, e.y)) { rate(false); return; }
    if (kRight.hit(e.x, e.y)) { rate(true);  return; }
    return;
  }
  switch (e.key) {
    case '\r': case ' ':
      if (!s_revealed) { s_revealed = true; appmgr::markDirty(); }
      else rate(true);
      break;
    case 'a': case 'A': if (s_revealed) rate(false); break;
    case 'd': case 'D': if (s_revealed) rate(true);  break;
    case '\b':
    case 'q': case 'Q': saveProgress(); s_screen = DECKS; appmgr::markDirty(); break;
    default: break;
  }
}

void doneInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP || e.key == '\r' || e.key == 'q' || e.key == 'Q' || e.key == '\b') {
    s_screen = DECKS;
    appmgr::markDirty();
  }
}

// =============================================================================
class FlashcardsApp : public App {
 public:
  const char* id() const override { return "Karteikarten"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppCards); }

  void onEnter() override {
    s_screen = DECKS;
    if (s_deckCount < 0) scanDecks();
    appmgr::markDirty();
  }

  void onLeave() override { saveProgress(); }

  void handleInput(const InputEvent& e) override {
    switch (s_screen) {
      case STUDY: studyInput(e);    break;
      case DONE:  doneInput(e);     break;
      default:    deckListInput(e); break;
    }
  }

  void draw(Adafruit_GFX& g) override {
    switch (s_screen) {
      case STUDY: studyDraw(g);    break;
      case DONE:  doneDraw(g);     break;
      default:    deckListDraw(g); break;
    }
  }
};

FlashcardsApp s_app;

}  // namespace

namespace flashcards_app {
App* get() { return &s_app; }
}  // namespace flashcards_app
