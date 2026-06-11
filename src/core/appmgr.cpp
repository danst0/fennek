#include "appmgr.h"
#include "config.h"
#include "core/battery.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/keyboard.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/touch.h"
#include "services/audio.h"
#include "services/library.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace {

constexpr int kMaxApps = 8;

App*     s_apps[kMaxApps];
int      s_count = 0;
App*     s_cur   = nullptr;

bool     s_dirty       = true;
uint32_t s_settleUntil = 0;    // Anti-Doppeltap nach langem Refresh

// Statuszeilen-Zustand (für Region-Refresh nur bei Änderung).
uint8_t  s_lastAudioGlyph = 0; // 0=aus, 1=spielt, 2=pausiert
uint8_t  s_battPct        = 0;
bool     s_battChg        = false;
uint32_t s_lastStatusPoll = 0;
uint32_t s_lastBattPoll   = 0;
uint32_t s_lastPersist    = 0;

uint8_t audioGlyph() {
  audio::Status st = audio::status();
  if (!st.playing) return 0;
  return st.paused ? 2 : 1;
}

// Kleines Schloss-Symbol (Tastensperre) in der Statuszeile.
void drawLock(Adafruit_GFX& g, int x, int y) {
  g.drawCircle(x + 5, y + 4, 4, GxEPD_BLACK);   // Bügel
  g.drawCircle(x + 5, y + 4, 3, GxEPD_BLACK);
  g.fillRoundRect(x, y + 4, 11, 9, 2, GxEPD_BLACK);  // Körper
  g.fillCircle(x + 5, y + 8, 1, GxEPD_WHITE);    // Schlüsselloch
}

void drawStatusBar(Adafruit_GFX& g) {
  g.fillRect(0, 0, EINK_W, appmgr::STATUS_H, GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);
  g.setCursor(6, 7);
  gui::print(g, s_cur ? s_cur->name() : "");

  // Tastensperre: Schloss + "Gesperrt" (mittig, vor Audio/Akku).
  if (power::locked()) {
    drawLock(g, 92, 4);
    g.setCursor(108, 7);
    gui::print(g, "Gesperrt");
  }

  // Audio-Indikator Mitte/rechts: CP437 0x0E = Doppelnote.
  uint8_t a = audioGlyph();
  if (a) {
    g.setCursor(EINK_W - 80, 7);
    g.write((uint8_t)0x0E);
    g.print(a == 1 ? " \x10" : " ||");   // ▶ bzw. Pause
  }

  // Akku rechts: "87%" bzw. "87%+" beim Laden.
  if (s_battPct > 0) {
    char b[8];
    snprintf(b, sizeof(b), "%d%%%s", s_battPct, s_battChg ? "+" : "");
    g.setCursor(EINK_W - 36, 7);
    g.print(b);
  }
  g.drawFastHLine(0, appmgr::STATUS_H - 1, EINK_W, GxEPD_BLACK);
}

void drawComposite(Adafruit_GFX& g) {
  drawStatusBar(g);
  if (s_cur) s_cur->draw(g);
}

// Zuletzt gespielten Musik-Track regelmäßig sichern (Resume nach Reboot).
// Hörbuch-Bookmarks verwaltet die Hörbuch-App selbst.
void persistPlayback() {
  audio::Status st = audio::status();
  settings::setVolume(st.volume);
  if (st.playing && st.owner == audio::Owner::Music) {
    char p[192];
    audio::currentPath(p, sizeof(p));
    if (p[0]) settings::setLastTrack(p, st.posSec);
  }
}

}  // namespace

namespace appmgr {

void add(App* a) {
  if (s_count < kMaxApps) s_apps[s_count++] = a;
}

void begin() {
  power::begin();
  s_cur = (s_count > 0) ? s_apps[0] : nullptr;
  if (s_cur) s_cur->onEnter();

  // Zuletzt aktive App wiederherstellen.
  char last[24];
  settings::lastApp(last, sizeof(last));
  if (last[0]) {
    for (int i = 1; i < s_count; i++) {
      if (strcmp(s_apps[i]->name(), last) == 0) { launch(s_apps[i]); break; }
    }
  }
  s_dirty = true;
}

void launch(App* a) {
  if (!a || a == s_cur) return;
  if (s_cur) s_cur->onLeave();
  s_cur = a;
  s_cur->onEnter();
  settings::setLastApp(a->name());
  s_dirty = true;
}

void goHome() {
  if (s_count > 0) launch(s_apps[0]);
}

App* current() { return s_cur; }

void markDirty() { s_dirty = true; }
bool isDirty()   { return s_dirty; }

void loop() {
  // 1) Eingaben: Touch + Tastatur. Bei Tastensperre werden Events zwar
  //    abgeholt (Puffer leeren), aber nicht an die App weitergereicht.
  const bool locked = power::locked();
  int16_t tx, ty;
  if (touch::poll(tx, ty) && !locked && millis() >= s_settleUntil) {
    power::noteActivity();
    Serial.printf("[APP] Tap @ %d,%d (%s)\n", tx, ty, s_cur ? s_cur->name() : "-");
    if (ty < STATUS_H) {
      if (s_cur != s_apps[0]) goHome();
    } else if (s_cur) {
      InputEvent e;
      e.type = InputEvent::TAP;
      e.x = tx; e.y = ty; e.key = 0;
      s_cur->handleInput(e);
    }
  }
  char k = keyboard::poll();
  if (k && !locked && s_cur) {
    power::noteActivity();
    InputEvent e;
    e.type = InputEvent::KEY;
    e.x = e.y = 0; e.key = k;
    s_cur->handleInput(e);
  }

  // 1b) Ausschaltknopf (Langdruck = Standby) + Auto-Standby nach Inaktivität.
  //     Sicherer Punkt: noch kein E-Ink-Refresh angestoßen.
  power::poll();

  // 2) Hintergrund-Arbeit aller Apps (Mesh-Loop, Bookmark-Writes, ...).
  for (int i = 0; i < s_count; i++) s_apps[i]->background();

  // 2b) ID3-Tag-Scan häppchenweise abarbeiten (UI bleibt bedienbar).
  static bool s_wasTagging = false;
  if (library::tagScanPending()) {
    library::tagScanStep(2);
    s_wasTagging = true;
  } else if (s_wasTagging) {
    s_wasTagging = false;
    int d, t; library::tagScanProgress(&d, &t);
    Serial.printf("[APP] ID3-Scan fertig (%d/%d) — Indizes neu sortiert\n", d, t);
    s_dirty = true;   // Listen zeigen jetzt Tag-Daten
  }

  // 3) Vordergrund-Tick (kann markDirty() setzen oder Region-Refreshes machen).
  if (s_cur) s_cur->tick();

  uint32_t now = millis();

  // 4) Akku alle 30 s lesen; Wiedergabe-Zustand alle 30 s persistieren.
  if (now - s_lastBattPoll >= 30000 || s_lastBattPoll == 0) {
    s_lastBattPoll = now;
    s_battPct = battery::percent();
    s_battChg = battery::charging();
  }
  if (now - s_lastPersist >= 30000) {
    s_lastPersist = now;
    persistPlayback();
  }

  // 5) Statuszeile aktuell halten (nur Region-Refresh bei Änderung).
  if (!s_dirty && now - s_lastStatusPoll >= 1000) {
    s_lastStatusPoll = now;
    uint8_t a = audioGlyph();
    if (a != s_lastAudioGlyph) {
      s_lastAudioGlyph = a;
      display::renderRegion(drawStatusBar, 0, STATUS_H);
    }
  }

  // 5b) Tastensperre-Wechsel sofort in der Statuszeile spiegeln.
  static int s_lockShown = -1;
  int lk = power::locked() ? 1 : 0;
  if (lk != s_lockShown) {
    s_lockShown = lk;
    if (!s_dirty) display::renderRegion(drawStatusBar, 0, STATUS_H);
  }

  // 6) Koaleszierter Voll-Redraw.
  if (s_dirty) {
    s_dirty = false;
    s_lastAudioGlyph = audioGlyph();
    display::render(drawComposite, false);
    // Der Render blockierte ~650 ms; aufgelaufene Taps verwerfen und kurz
    // nachsperren, damit ein Tap nicht doppelt zählt (aus altem ui.cpp).
    touch::flush();
    s_settleUntil = millis() + 250;
  }
}

}  // namespace appmgr
