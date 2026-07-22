// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// calendar_app.cpp — "Kalender": read-only Agenda anstehender Termine.
//
// Die Termine liefert services/calendar (gemischt + nach Startzeit sortiert).
// Sync läuft blockierend auf Knopfdruck (WLAN ⊥ Audio) — wie podcast_app: erst
// „Synchronisiere…"-Frame, dann der blockierende calendar::sync().
// =============================================================================
#include "calendar_app.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "core/board.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "services/calendar.h"
#include "services/timesync.h"

namespace {

using gui::Rect;
constexpr int W = EINK_W;

constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_H = TOP + 26;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int ROW_H    = 38;
constexpr int VISIBLE  = 6;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;

const Rect kBack {4,   BAR_Y, 52, 44};
const Rect kUp   {60,  BAR_Y, 52, 44};
const Rect kDown {116, BAR_Y, 52, 44};
const Rect kSync {172, BAR_Y, 64, 44};

int  s_off = 0;

// Blockierender Sync (wie podcast_app): erst Frame zeichnen, dann sync().
bool s_syncReq   = false;
bool s_syncDrawn = false;
char s_syncLog[48] = "";

const char* kWd[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

void fmtDay(uint32_t epoch, char* out, size_t n) {
  time_t t = (time_t)epoch; struct tm lt; localtime_r(&t, &lt);
  snprintf(out, n, "%s %02d.%02d.", kWd[lt.tm_wday % 7], lt.tm_mday, lt.tm_mon + 1);
}
void fmtTime(uint32_t epoch, char* out, size_t n) {
  time_t t = (time_t)epoch; struct tm lt; localtime_r(&t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void requestSync() {
  s_syncReq = true; s_syncDrawn = false; s_syncLog[0] = '\0';
  appmgr::markDirty();
}

void moveScroll(int delta) {
  int n = calendar::count();
  s_off += delta;
  if (s_off > n - 1) s_off = n - 1;
  if (s_off < 0) s_off = 0;
  appmgr::markDirty();
}

// =============================================================================
void drawSyncing(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppCalendar), 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);
  gui::printAt(g, 10, 120, "Synchronisiere ...", 2);
  gui::printAt(g, 10, 150, "WLAN aktiv (Audio aus)", 1);
}

void drawAgenda(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 8, TOP + 4, i18n::tr(i18n::Str::AppCalendar), 2);
  g.drawLine(0, HEADER_H, W, HEADER_H, GxEPD_BLACK);

  int n = calendar::count();
  if (n == 0) {
    gui::printAt(g, 14, 76, "Keine Termine", 2);
    gui::printAt(g, 14, 110, "Feed eintragen unter", 1);
    gui::printAt(g, 14, 124, "/calendar/feeds.txt", 1);
    gui::printAt(g, 14, 142, "dann Sync druecken.", 1);
    if (s_syncLog[0]) gui::printAt(g, 6, BAR_Y - 14, s_syncLog, 1);
    gui::drawButton(g, kBack, "Home", false);
    gui::drawButton(g, kSync, "Sync", true);
    return;
  }

  uint32_t prevDay = 0xFFFFFFFFu;
  for (int i = 0; i < VISIBLE && s_off + i < n; i++) {
    calendar::CalEvent ev;
    if (!calendar::event(s_off + i, &ev)) continue;
    int y = LIST_Y + i * ROW_H;
    uint32_t day = ev.start / 86400u;
    char dbuf[16]; fmtDay(ev.start, dbuf, sizeof(dbuf));
    // Datum nur beim ersten Eintrag des Tages prominent; sonst Zeit-Spalte.
    char tbuf[16];
    if (ev.allDay) snprintf(tbuf, sizeof(tbuf), "ganzt.");
    else           fmtTime(ev.start, tbuf, sizeof(tbuf));

    g.setTextColor(GxEPD_BLACK);
    char head[28];
    if (day != prevDay) snprintf(head, sizeof(head), "%s %s", dbuf, tbuf);
    else                snprintf(head, sizeof(head), "    %s", tbuf);
    gui::printAt(g, 6, y, head, 1);
    prevDay = day;

    // Titel (gekürzt) eine Zeile darunter, größer.
    char title[40];
    strncpy(title, ev.summary, sizeof(title) - 1); title[sizeof(title) - 1] = '\0';
    gui::printAt(g, 6, y + 12, title, 2);
    g.drawLine(0, y + ROW_H - 2, W, y + ROW_H - 2, GxEPD_BLACK);
  }

  // Scroll-/Status-Hinweis.
  char pos[24]; snprintf(pos, sizeof(pos), "%d-%d/%d",
                         s_off + 1, (s_off + VISIBLE < n ? s_off + VISIBLE : n), n);
  gui::printAt(g, W - 76, TOP + 8, pos, 1);
  if (s_syncLog[0]) gui::printAt(g, 6, BAR_Y - 14, s_syncLog, 1);

  gui::drawButton(g, kBack, "Home", false);
  gui::drawButton(g, kUp,   "W",    false);
  gui::drawButton(g, kDown, "S",    false);
  gui::drawButton(g, kSync, "Sync", true);
}

// =============================================================================
class CalendarApp : public App {
 public:
  const char* id()   const override { return "Kalender"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppCalendar); }

  void onEnter() override { s_off = 0; appmgr::markDirty(); }

  void handleInput(const InputEvent& e) override {
    if (s_syncReq) return;   // während des Syncs keine Eingaben
    if (e.type == InputEvent::TAP) {
      if (kBack.hit(e.x, e.y)) { appmgr::goHome(); return; }
      if (kSync.hit(e.x, e.y)) { requestSync(); return; }
      if (kUp.hit(e.x, e.y))   { moveScroll(-VISIBLE); return; }
      if (kDown.hit(e.x, e.y)) { moveScroll(VISIBLE);  return; }
      return;
    }
    switch (e.key) {
      case 'w': case 'W': case 'i': case 'I': moveScroll(-1); break;
      case 's': case 'S': case 'k': case 'K': moveScroll(1);  break;
      case 'y': case 'Y': requestSync();  break;
      case '\b': case 'q': case 'Q': appmgr::goHome(); break;
      default: break;
    }
  }

  void tick() override {
    if (!s_syncReq) return;
    if (!s_syncDrawn) { if (!appmgr::isDirty()) s_syncDrawn = true; return; }
    calendar::sync(s_syncLog, sizeof(s_syncLog));
    s_syncReq = false;
    s_off = 0;
    appmgr::markDirty();
  }

  void draw(Adafruit_GFX& g) override {
    if (s_syncReq) drawSyncing(g);
    else           drawAgenda(g);
  }
};

CalendarApp s_app;

}  // namespace

namespace calendar_app {
App* get() { return &s_app; }
}  // namespace calendar_app
