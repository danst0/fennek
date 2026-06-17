// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "alarms_app.h"
#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "services/alarmclock.h"
#include "services/timesync.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>
#include <time.h>

namespace {

using gui::Rect;

constexpr int W      = EINK_W;
constexpr int TOP    = appmgr::CONTENT_Y;     // 24
constexpr int ROW_H  = 26;
constexpr int kLastRow  = alarmclock::kMaxAlarms - 1;   // nur die Wecker-Zeilen
// Klingelton bleibt per Konsole `alarm sound <pfad>` setzbar; Default = Piepton.

const Rect kBack    {6, 270, 104, 42};
const Rect kSnooze  {12, 232, 100, 56};       // Klingel-Bildschirm
const Rect kStop    {128, 232, 100, 56};

// Wiederholungs-Presets; eigene (per Konsole gesetzte) Masken zeigen "eigen".
struct DayPreset { const char* name; uint8_t mask; };
const DayPreset kDays[] = {
  {"Taeglich",   0x00},
  {"Werktags",   0x1F},   // Mo-Fr
  {"Wochenende", 0x60},   // Sa+So
};
constexpr int kNumDays = (int)(sizeof(kDays) / sizeof(kDays[0]));

enum Field { F_HOUR, F_MIN, F_DAYS, F_SIGNAL, F_ENABLED, F_COUNT };

enum Screen { LIST, EDIT };
int  s_screen  = LIST;
int  s_sel     = 0;              // LIST: 0..kSoundRow ; EDIT: 0..F_COUNT-1
int  s_editIdx = -1;             // welcher Wecker editiert wird
alarmclock::Alarm s_buf = {};    // Arbeitskopie im Editor

int  s_typeCount = 0;            // getippte Ziffern im aktuellen Zeit-Feld (0..2)

bool s_wasRinging = false;       // Flankenerkennung fürs Auto-Aufpoppen

void markDirty() { appmgr::markDirty(); }

void daysLabel(uint8_t mask, char* out, size_t n) {
  for (int i = 0; i < kNumDays; i++)
    if (kDays[i].mask == mask) { snprintf(out, n, "%s", kDays[i].name); return; }
  // Eigene Maske (Konsole): Kürzel auflisten.
  static const char* dn[7] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
  out[0] = '\0';
  for (int d = 0; d < 7; d++)
    if (mask & (1 << d)) { strncat(out, dn[d], n - strlen(out) - 1);
                           strncat(out, " ", n - strlen(out) - 1); }
}

void cycleDays(int dir) {
  int idx = 0;
  for (int i = 0; i < kNumDays; i++) if (kDays[i].mask == s_buf.dowMask) idx = i;
  idx = (idx + dir + kNumDays) % kNumDays;
  s_buf.dowMask = kDays[idx].mask;
}

const char* signalLabel(uint8_t sig) {
  switch (sig) {
    case alarmclock::SIG_TONE:  return "Ton";
    case alarmclock::SIG_BLINK: return "Blinken";
    default:                    return "Beides";
  }
}

// Ziffer ins fokussierte Zeit-Feld tippen (Telefon-Uhr-Logik): zwei Ziffern pro
// Feld; eine zu große erste Ziffer (Stunde >2, Minute >5) ist einstellig und
// springt sofort weiter. Stunde→Minute automatisch.
void typeDigit(int d) {
  if (s_sel == F_HOUR) {
    if (s_typeCount == 0) {
      s_buf.hour = d;
      if (d >= 3) { s_typeCount = 0; s_sel = F_MIN; } else s_typeCount = 1;
    } else {
      int v = s_buf.hour * 10 + d;
      if (v <= 23) s_buf.hour = v;
      s_typeCount = 0; s_sel = F_MIN;
    }
  } else if (s_sel == F_MIN) {
    if (s_typeCount == 0) {
      s_buf.minute = d;
      s_typeCount = (d >= 6) ? 0 : 1;
    } else {
      int v = s_buf.minute * 10 + d;
      if (v <= 59) s_buf.minute = v;
      s_typeCount = 0;
    }
  } else {
    return;
  }
  markDirty();
}

void openEdit(int i) {
  s_editIdx = i;
  s_typeCount = 0;
  s_buf = alarmclock::get(i);
  if (!s_buf.enabled && s_buf.hour == 0 && s_buf.minute == 0) {
    s_buf.hour = 7;            // sinnvolle Vorbelegung für einen neuen Wecker
    s_buf.minute = 0;
  }
  s_screen = EDIT;
  s_sel = 0;
  markDirty();
}

void saveEdit() {
  if (s_editIdx >= 0) alarmclock::set(s_editIdx, s_buf);
  s_screen = LIST;
  s_sel = s_editIdx < 0 ? 0 : s_editIdx;
  s_editIdx = -1;
  markDirty();
}

// --- Klingel-Bildschirm ---------------------------------------------------------
void drawRinging(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 30, TOP + 24, i18n::tr(i18n::Str::AppAlarm), 4);

  time_t tt = (time_t)timesync::now();
  struct tm lt;
  localtime_r(&tt, &lt);
  char hhmm[8];
  snprintf(hhmm, sizeof(hhmm), "%02u:%02u", (unsigned)lt.tm_hour, (unsigned)lt.tm_min);
  uint16_t tw, th;
  g.setTextSize(5);
  gui::textBounds(g, hhmm, &tw, &th);
  gui::printAt(g, (W - (int)tw) / 2, TOP + 100, hhmm, 5);

  char snz[12];
  snprintf(snz, sizeof(snz), "+%u min", (unsigned)alarmclock::snoozeMinutes());
  gui::drawButton(g, kSnooze, snz, false);
  gui::drawButton(g, kStop, "Stop", true);
}

// --- Liste ----------------------------------------------------------------------
void drawListRow(Adafruit_GFX& g, int i, int y) {
  bool sel = (s_sel == i);
  g.setTextSize(2);
  char line[48];
  alarmclock::Alarm a = alarmclock::get(i);
  if (a.enabled) {
    char days[32];
    daysLabel(a.dowMask, days, sizeof(days));
    snprintf(line, sizeof(line), "%02u:%02u", (unsigned)a.hour, (unsigned)a.minute);
    g.setCursor(10, y + 6);
    gui::print(g, line);
    g.setTextSize(1);
    g.setCursor(96, y + 11);
    gui::print(g, days);
  } else {
    g.setCursor(10, y + 6);
    gui::print(g, "--:--");
    g.setTextSize(1);
    g.setCursor(96, y + 11);
    gui::print(g, i18n::tr(i18n::Str::StandbyOff));   // "Aus"
  }
  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);
  if (sel) {
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
}

// --- Editor ---------------------------------------------------------------------
void fieldValue(int f, char* v, size_t n) {
  switch (f) {
    case F_HOUR:    snprintf(v, n, "%02u", (unsigned)s_buf.hour); break;
    case F_MIN:     snprintf(v, n, "%02u", (unsigned)s_buf.minute); break;
    case F_DAYS:    daysLabel(s_buf.dowMask, v, n); break;
    case F_SIGNAL:  snprintf(v, n, "%s", signalLabel(s_buf.signal)); break;
    case F_ENABLED: snprintf(v, n, "%s", s_buf.enabled ? "An" : "Aus"); break;
  }
}
const char* fieldName(int f) {
  switch (f) {
    case F_HOUR:    return "Stunde";
    case F_MIN:     return "Minute";
    case F_DAYS:    return "Tage";
    case F_SIGNAL:  return "Signal";
    case F_ENABLED: return "Aktiv";
  }
  return "";
}
void changeField(int f, int dir) {
  switch (f) {
    case F_HOUR:    s_buf.hour = (uint8_t)((s_buf.hour + dir + 24) % 24); break;
    case F_MIN:     s_buf.minute = (uint8_t)((s_buf.minute + dir + 60) % 60); break;
    case F_DAYS:    cycleDays(dir); break;
    case F_SIGNAL: {
      int m = (s_buf.signal >= 1 && s_buf.signal <= 3) ? s_buf.signal : alarmclock::SIG_BOTH;
      s_buf.signal = (uint8_t)(((m - 1 + dir + 3) % 3) + 1);
      break;
    }
    case F_ENABLED: s_buf.enabled = !s_buf.enabled; break;
  }
  markDirty();
}

void drawEditRow(Adafruit_GFX& g, int f, int y) {
  bool sel = (s_sel == f);
  g.setTextSize(1);
  g.setCursor(10, y + 9);
  gui::print(g, fieldName(f));
  char v[32];
  fieldValue(f, v, sizeof(v));
  g.setTextSize(2);
  uint16_t vw, vh;
  gui::textBounds(g, v, &vw, &vh);
  int vx = W - 24 - (int)vw;
  g.setCursor(vx, y + 4);
  gui::print(g, v);
  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);
  if (sel) {
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
    g.setTextSize(1);
    g.setCursor(vx - 14, y + 9); g.write((uint8_t)0x11);   // ◄
    g.setCursor(W - 12, y + 9);  g.write((uint8_t)0x10);   // ►
  }
}

// --- Eingabe --------------------------------------------------------------------
void onRingKey(char k) {
  if (k == 'z' || k == 'Z' || k == ' ') alarmclock::snooze();
  else                                  alarmclock::dismiss();
  markDirty();
}

void onListKey(char k) {
  switch (k) {
    case 'w': case 'W': s_sel = (s_sel + kLastRow) % (kLastRow + 1); markDirty(); break;
    case 's': case 'S': s_sel = (s_sel + 1) % (kLastRow + 1); markDirty(); break;
    case '\r':          openEdit(s_sel); break;
    case '\b': case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onEditKey(char k) {
  if (k >= '0' && k <= '9') { typeDigit(k - '0'); return; }   // Zeit direkt tippen
  switch (k) {
    case 'w': case 'W': s_typeCount = 0; s_sel = (s_sel + F_COUNT - 1) % F_COUNT; markDirty(); break;
    case 's': case 'S': s_typeCount = 0; s_sel = (s_sel + 1) % F_COUNT; markDirty(); break;
    case 'a': case 'A': s_typeCount = 0; changeField(s_sel, -1); break;
    case 'd': case 'D': s_typeCount = 0; changeField(s_sel, +1); break;
    case '\r':          s_typeCount = 0; changeField(s_sel, +1); break;
    case '\b': case 'q': case 'Q': saveEdit(); break;   // zurück = speichern
    default: break;
  }
}

void onRingTouch(int x, int y) {
  if (kSnooze.hit(x, y)) alarmclock::snooze();
  else if (kStop.hit(x, y)) alarmclock::dismiss();
  markDirty();
}

void onListTouch(int x, int y) {
  if (kBack.hit(x, y)) { appmgr::goHome(); return; }
  int row = (y - TOP) / ROW_H;
  if (row < 0 || row > kLastRow) return;
  if (row != s_sel) { s_sel = row; markDirty(); return; }   // erster Tap wählt
  openEdit(row);
}

void onEditTouch(int x, int y) {
  if (kBack.hit(x, y)) { saveEdit(); return; }
  int row = (y - TOP) / ROW_H;
  if (row < 0 || row >= F_COUNT) return;
  if (row != s_sel) { s_sel = row; markDirty(); return; }
  changeField(row, (x >= W / 2) ? +1 : -1);
}

class AlarmsApp : public App {
 public:
  const char* id()   const override { return "Wecker"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppAlarm); }

  void onEnter() override {
    if (!alarmclock::ringing()) { s_screen = LIST; s_sel = 0; }
  }

  void onLeave() override {
    if (s_screen == EDIT) saveEdit();
  }

  // Klingelt ein Wecker, während eine andere App vorn ist: diese App holen.
  void background() override {
    bool r = alarmclock::ringing();
    if (r && appmgr::current() != this) appmgr::launch(this);
    if (r != s_wasRinging) { s_wasRinging = r; markDirty(); }
  }

  void handleInput(const InputEvent& e) override {
    if (alarmclock::ringing()) {
      if (e.type == InputEvent::TAP) onRingTouch(e.x, e.y);
      else                           onRingKey(e.key);
      return;
    }
    if (e.type == InputEvent::TAP) (s_screen == EDIT ? onEditTouch : onListTouch)(e.x, e.y);
    else                           (s_screen == EDIT ? onEditKey   : onListKey)(e.key);
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    if (alarmclock::ringing()) { drawRinging(g); return; }

    if (s_screen == EDIT) {
      g.setTextSize(1);
      char hdr[24];
      snprintf(hdr, sizeof(hdr), "\x11 Wecker %d", s_editIdx + 1);   // 0x11 = ◄
      g.setCursor(8, TOP + 2);
      gui::print(g, hdr);
      int y = TOP + 12;
      for (int f = 0; f < F_COUNT; f++) { drawEditRow(g, f, y); y += ROW_H; }
      gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
      g.setTextSize(1);
      g.setCursor(120, 286);
      gui::print(g, "A/D o. Ziffern tippen");
      g.setCursor(120, 300);
      gui::print(g, "Zurueck speichert");
      return;
    }

    // Liste.
    int y = TOP;
    for (int i = 0; i <= kLastRow; i++) { drawListRow(g, i, y); y += ROW_H; }
    gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
    g.setTextSize(1);
    g.setCursor(120, 286);
    gui::print(g, "Enter: bearbeiten");
    g.setCursor(120, 300);
    gui::print(g, "Fennek " FENNEK_VERSION);
  }
};

AlarmsApp s_app;

}  // namespace

namespace alarms_app {

App* get() { return &s_app; }

}  // namespace alarms_app
