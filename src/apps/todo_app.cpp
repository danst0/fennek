// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// todo_app.cpp — "Todo": Reinschrift-Aufgaben über Nextcloud/WebDAV.
//
// Liste (Filter offen/alle/heute), abhaken (Enter / Tap auf Zeile), Quick-Add
// (freier Text, darf +Topic/due: enthalten), auf heute/morgen schieben (A/D),
// Sync (blockierend, WLAN ⊥ Audio — wie podcast_app). Mutationen wirken sofort
// lokal und reihen eine über ^marker adressierte Op ein; der nächste Sync mergt
// sie konfliktsicher auf den frischen Remote-Stand (services/reinschrift).
// =============================================================================
#include "todo_app.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "services/reinschrift.h"
#include "services/timesync.h"

namespace {

using gui::Rect;
using reinschrift::Task;
constexpr int W = EINK_W;

constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_H = TOP + 24;
constexpr int LIST_Y   = HEADER_H + 2;
constexpr int ROW_H    = 30;
constexpr int VISIBLE  = 7;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 2;

const Rect kBack {4,   BAR_Y, 50, 44};
const Rect kNew  {58,  BAR_Y, 50, 44};
const Rect kFilt {112, BAR_Y, 56, 44};
const Rect kSync {172, BAR_Y, 64, 44};

constexpr int kMax = 384;
const char* kDefaultTopic = "Inbox";

enum Screen { LIST, ADD };
Screen s_screen = LIST;

enum Filter { FILT_OPEN, FILT_ALL, FILT_TODAY };
int s_filter = FILT_OPEN;

struct Row { int svc; uint64_t key; };
Row s_rows[kMax];
int s_rowN = 0;
int s_sel = 0, s_off = 0;

char s_add[160] = "";

// Blockierender Sync (wie podcast_app/calendar_app).
bool s_syncReq = false;
bool s_syncDrawn = false;
char s_syncLog[48] = "";

const char* filterLabel() {
  switch (s_filter) { case FILT_ALL: return "alle"; case FILT_TODAY: return "heute"; default: return "offen"; }
}

int cmpRow(const void* a, const void* b) {
  uint64_t ka = ((const Row*)a)->key, kb = ((const Row*)b)->key;
  return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

// Anzeigeliste aus dem Service neu aufbauen (Filter + Sortierung).
void rebuildView() {
  uint32_t now = timesync::now();
  int n = reinschrift_svc::count();
  s_rowN = 0;
  for (int i = 0; i < n && s_rowN < kMax; i++) {
    Task t;
    if (!reinschrift_svc::get(i, &t)) continue;
    if (s_filter == FILT_OPEN && t.done) continue;
    if (s_filter == FILT_TODAY && (t.done || !reinschrift::isDueToday(t.dueEpoch, now))) continue;
    // Sortierschlüssel: offen vor erledigt, dann nach Fälligkeit (kein/​irgendwann ans Ende).
    uint32_t rank;
    if (t.dueEpoch == 0)                       rank = 0xFFFFFFFEu;
    else if (reinschrift::isSomeday(t.dueEpoch)) rank = 0xFFFFFFFFu;
    else                                       rank = t.dueEpoch;
    s_rows[s_rowN].svc = i;
    s_rows[s_rowN].key = ((uint64_t)(t.done ? 1 : 0) << 40) | rank;
    s_rowN++;
  }
  qsort(s_rows, s_rowN, sizeof(Row), cmpRow);
  if (s_sel >= s_rowN) s_sel = s_rowN > 0 ? s_rowN - 1 : 0;
  if (s_sel < 0) s_sel = 0;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  if (s_off < 0) s_off = 0;
}

void moveSel(int delta) {
  if (s_rowN <= 0) return;
  s_sel = (s_sel + delta + s_rowN) % s_rowN;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  appmgr::markDirty();
}

void toggleSel() {
  if (s_sel < 0 || s_sel >= s_rowN) return;
  reinschrift_svc::toggle(s_rows[s_sel].svc);
  rebuildView();
  appmgr::markDirty();
}

void dueSel(int dayOffset) {
  if (s_sel < 0 || s_sel >= s_rowN) return;
  reinschrift_svc::setDueRelative(s_rows[s_sel].svc, dayOffset);
  rebuildView();
  appmgr::markDirty();
}

void cycleFilter() {
  s_filter = (s_filter + 1) % 3;
  s_off = 0; s_sel = 0;
  rebuildView();
  appmgr::markDirty();
}

void requestSync() {
  s_syncReq = true; s_syncDrawn = false; s_syncLog[0] = '\0';
  appmgr::markDirty();
}

// Kurzes Fälligkeits-Label für die Zeile.
void dueLabel(const Task& t, uint32_t now, char* out, size_t n) {
  if (t.dueEpoch == 0) { out[0] = '\0'; return; }
  if (reinschrift::isSomeday(t.dueEpoch)) { snprintf(out, n, "irg."); return; }
  uint32_t d = t.dueEpoch / 86400u, today = now / 86400u;
  if (d < today)       snprintf(out, n, "!ueberf.");
  else if (d == today) snprintf(out, n, "heute");
  else if (d == today + 1) snprintf(out, n, "morgen");
  else {
    time_t tt = (time_t)t.dueEpoch; struct tm lt; localtime_r(&tt, &lt);
    snprintf(out, n, "%02d.%02d.", lt.tm_mday, lt.tm_mon + 1);
  }
}

// =============================================================================
void drawSyncing(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppTodo), 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);
  gui::printAt(g, 10, 120, "Synchronisiere ...", 2);
  gui::printAt(g, 10, 150, "WLAN aktiv (Audio aus)", 1);
}

void drawList(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppTodo), 2);
  char hdr[28];
  int pend = reinschrift_svc::pendingCount();
  snprintf(hdr, sizeof(hdr), "[%s]%s", filterLabel(), pend ? " *" : "");
  gui::printAt(g, W - 88, TOP + 8, hdr, 1);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);

  uint32_t now = timesync::now();
  if (s_rowN == 0) {
    gui::printAt(g, 14, 80, "Keine Aufgaben", 2);
    gui::printAt(g, 14, 110, "Sync druecken oder", 1);
    gui::printAt(g, 14, 124, "Neu (+) anlegen.", 1);
  } else {
    for (int i = 0; i < VISIBLE && s_off + i < s_rowN; i++) {
      int r = s_off + i;
      Task t;
      if (!reinschrift_svc::get(s_rows[r].svc, &t)) continue;
      int y = LIST_Y + i * ROW_H;
      bool seld = (r == s_sel);
      if (seld) g.fillRect(0, y, W, ROW_H, GxEPD_BLACK);
      g.setTextColor(seld ? GxEPD_WHITE : GxEPD_BLACK);
      gui::printAt(g, 4, y + 8, t.done ? "[x]" : "[ ]", 2);
      char dl[12]; dueLabel(t, now, dl, sizeof(dl));
      int titleW = W - 46 - (dl[0] ? 52 : 0);
      char title[40];
      strncpy(title, t.title, sizeof(title) - 1); title[sizeof(title) - 1] = '\0';
      int maxCh = titleW / 12;                  // size-2 Glyphe ~12 px
      if (maxCh > 0 && (int)strlen(title) > maxCh) title[maxCh] = '\0';
      gui::printAt(g, 44, y + 8, title, 2);
      if (dl[0]) gui::printAt(g, W - 50, y + 11, dl, 1);
    }
  }
  g.setTextColor(GxEPD_BLACK);
  if (s_syncLog[0]) gui::printAt(g, 6, BAR_Y - 13, s_syncLog, 1);
  gui::drawButton(g, kBack, "Home", false);
  gui::drawButton(g, kNew,  "+",    false);
  gui::drawButton(g, kFilt, "Filt", false);
  gui::drawButton(g, kSync, "Sync", true);
}

void drawAdd(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, "Neue Aufgabe", 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);
  gui::printAt(g, 8, LIST_Y + 8, "Titel (+Thema moeglich):", 1);
  // Eingabefeld mit Rahmen + Cursor.
  g.drawRect(8, LIST_Y + 24, W - 16, 60, GxEPD_BLACK);
  char shown[160];
  strncpy(shown, s_add, sizeof(shown) - 2); shown[sizeof(shown) - 2] = '\0';
  size_t l = strlen(shown); shown[l] = '_'; shown[l + 1] = '\0';
  gui::printAt(g, 12, LIST_Y + 32, shown, 2);
  gui::printAt(g, 8, LIST_Y + 100, "Enter = anlegen, Esc/Q = ab", 1);
  gui::printAt(g, 8, LIST_Y + 116, "Thema sonst: Inbox", 1);
  gui::drawButton(g, Rect{20, 256, 90, 48}, "Abbruch", false);
  gui::drawButton(g, Rect{130, 256, 90, 48}, "Anlegen", true);
}

void commitAdd() {
  if (s_add[0]) {
    reinschrift_svc::add(s_add, kDefaultTopic);
    rebuildView();
  }
  s_add[0] = '\0';
  s_screen = LIST;
  appmgr::markDirty();
}

// =============================================================================
void listInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (kBack.hit(e.x, e.y)) { appmgr::goHome(); return; }
    if (kNew.hit(e.x, e.y))  { s_add[0] = '\0'; s_screen = ADD; appmgr::markDirty(); return; }
    if (kFilt.hit(e.x, e.y)) { cycleFilter(); return; }
    if (kSync.hit(e.x, e.y)) { requestSync(); return; }
    for (int i = 0; i < VISIBLE && s_off + i < s_rowN; i++) {
      int y = LIST_Y + i * ROW_H;
      if (e.y >= y && e.y < y + ROW_H) { s_sel = s_off + i; toggleSel(); return; }
    }
    return;
  }
  switch (e.key) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(1);  break;
    case '\r': case ' ': toggleSel(); break;
    case 'a': case 'A': dueSel(0); break;   // heute
    case 'd': case 'D': dueSel(1); break;   // morgen
    case 'n': case 'N': case '+': s_add[0] = '\0'; s_screen = ADD; appmgr::markDirty(); break;
    case 'f': case 'F': cycleFilter(); break;
    case 'y': case 'Y': requestSync(); break;
    case '\b': case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void addInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (Rect{20, 256, 90, 48}.hit(e.x, e.y))  { s_add[0] = '\0'; s_screen = LIST; appmgr::markDirty(); return; }
    if (Rect{130, 256, 90, 48}.hit(e.x, e.y)) { commitAdd(); return; }
    return;
  }
  char k = e.key;
  if (k == '\r') { commitAdd(); return; }
  if (k == '\b') { size_t l = strlen(s_add); if (l) s_add[l - 1] = '\0'; appmgr::markDirty(); return; }
  if (k == 27 || k == 'Q') { s_add[0] = '\0'; s_screen = LIST; appmgr::markDirty(); return; }
  if ((unsigned char)k >= 0x20 && (unsigned char)k < 0x7f) {
    size_t l = strlen(s_add);
    if (l < sizeof(s_add) - 1) { s_add[l] = k; s_add[l + 1] = '\0'; appmgr::markDirty(); }
  }
}

// =============================================================================
class TodoApp : public App {
 public:
  const char* id()   const override { return "Todo"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppTodo); }

  void onEnter() override { s_screen = LIST; rebuildView(); appmgr::markDirty(); }

  void handleInput(const InputEvent& e) override {
    if (s_syncReq) return;
    if (s_screen == ADD) addInput(e);
    else                 listInput(e);
  }

  void tick() override {
    if (!s_syncReq) return;
    if (!s_syncDrawn) { if (!appmgr::isDirty()) s_syncDrawn = true; return; }
    reinschrift_svc::sync(s_syncLog, sizeof(s_syncLog));
    s_syncReq = false;
    rebuildView();
    appmgr::markDirty();
  }

  void draw(Adafruit_GFX& g) override {
    if (s_syncReq)            drawSyncing(g);
    else if (s_screen == ADD) drawAdd(g);
    else                      drawList(g);
  }
};

TodoApp s_app;

}  // namespace

namespace todo_app {
App* get() { return &s_app; }
}  // namespace todo_app
