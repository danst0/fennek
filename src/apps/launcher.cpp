// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "launcher.h"
#include "launcher_icons.h"
#include "config.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"
#include "services/audio.h"
#include "services/library.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace {

using gui::Rect;

// Zwei Homescreen-Seiten à 10 Kacheln (2 Spalten x 5 Reihen). Seitenwechsel per
// Touch (Pfeile ‹/› rechts in der Hinweiszeile) oder Tastatur (A/D am Spaltenrand
// wechselt die Seite; Q/E blättern direkt).
constexpr int kPerPage = 10;
constexpr int kPages   = 2;
constexpr int kTiles   = kPerPage * kPages;

constexpr int kTileW = 108, kTileH = 46;
constexpr int kCol[2] = {8, 124};
constexpr int kRowY0  = appmgr::CONTENT_Y + 6;     // 30
constexpr int kRowGap = 52;                        // 30, 82, 134, 186, 238 -> Ende 284

constexpr int kHintY = 296;                        // Now-Playing-/Resume-Zeile
// Tap-Zonen für den Seitenwechsel (rechts in der Hinweiszeile). Bewusst groß und
// bündig bis zum rechten Display-Rand (Finger-tauglich; vorher 24x22 zu klein,
// v. a. „nach rechts" war kaum zu treffen). Die Pfeile/Indikator werden weiter
// innen gezeichnet (kPrevPage.x + 6/18 bzw. kNextPage.x + 6).
const Rect kPrevPage{168, kHintY - 8, 36, 30};
const Rect kNextPage{204, kHintY - 8, 36, 30};

struct Tile {
  i18n::Str label;   // String-ID, beim Zeichnen aufgelöst (Sprachwechsel!)
  App*      app;
};
Tile s_tiles[kTiles] = {};
int  s_page   = 0;   // aktuelle Homescreen-Seite
int  s_cursor = 0;   // Tastatur-Cursor (globaler Kachel-Index)

// Position einer Kachel-Slot-Nummer (0..kPerPage-1) im Raster der aktuellen Seite.
Rect slotRect(int slot) {
  return Rect{kCol[slot % 2], kRowY0 + (slot / 2) * kRowGap, kTileW, kTileH};
}
// Gibt es auf Seite p überhaupt eine belegte Kachel?
bool pageHasTiles(int p) {
  for (int s = 0; s < kPerPage; s++)
    if (s_tiles[p * kPerPage + s].label != i18n::Str::None) return true;
  return false;
}


class LauncherApp : public App {
 public:
  const char* id()   const override { return "Start"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppLauncher); }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      // Seitenpfeile (nur wenn es mehr als eine belegte Seite gibt).
      if (kPages > 1) {
        if (kPrevPage.hit(e.x, e.y)) { flipPage(-1); return; }
        if (kNextPage.hit(e.x, e.y)) { flipPage(+1); return; }
      }
      // Hinweis-Zeile: Now-Playing -> Musik-App; Resume-Angebot -> abspielen.
      if (e.y >= kHintY - 8) { onHintActivate(); return; }
      for (int slot = 0; slot < kPerPage; slot++) {
        int i = s_page * kPerPage + slot;
        if (s_tiles[i].label != i18n::Str::None && slotRect(slot).hit(e.x, e.y)) {
          if (s_tiles[i].app) appmgr::launch(s_tiles[i].app);
          return;
        }
      }
      return;
    }

    switch (e.key) {
      case 'w': case 'W': case 'i': case 'I': moveCursor(-2); break;
      case 's': case 'S': case 'k': case 'K': moveCursor(+2); break;
      case 'a': case 'A': case 'j': case 'J': moveCursor(-1); break;
      case 'd': case 'D': case 'l': case 'L': moveCursor(+1); break;
      case 'q': case 'Q': flipPage(-1); break;
      case 'e': case 'E': flipPage(+1); break;
      case '\r': {
        int i = s_page * kPerPage + s_cursor;
        if (s_tiles[i].label != i18n::Str::None && s_tiles[i].app)
          appmgr::launch(s_tiles[i].app);
        break;
      }
      case ' ': onHintActivate(); break;
      default: break;
    }
  }

  void tick() override {
    // Now-Playing-Zeile aktuell halten (Track-Wechsel im Hintergrund).
    if (appmgr::isDirty()) return;
    audio::Status st = audio::status();
    int cur = st.playing ? st.pos : -1;
    if (cur != s_shownIndex) appmgr::markDirty();
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    for (int slot = 0; slot < kPerPage; slot++) {
      int i = s_page * kPerPage + slot;
      if (s_tiles[i].label == i18n::Str::None) continue;
      Rect r = slotRect(slot);
      g.drawRoundRect(r.x, r.y, r.w, r.h, 8, GxEPD_BLACK);
      if (slot == s_cursor)
        g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, GxEPD_BLACK);
      // Belegte Kacheln zeigen ein Icon links + kleines Label rechts daneben;
      // ohne Icon/ohne App fällt es auf den großen Text-Label zurück.
      if (s_tiles[i].app &&
          launcher_icons::draw(g, s_tiles[i].label, r.x + 22, r.y + r.h / 2)) {
        const char* label = i18n::tr(s_tiles[i].label);
        g.setTextSize(1);
        uint16_t bw, bh;
        gui::textBounds(g, label, &bw, &bh);
        g.setCursor(r.x + 44, r.y + (r.h - (int)bh) / 2);
        gui::print(g, label);
        continue;
      }
      g.setTextSize(2);
      uint16_t bw, bh;
      const char* label = i18n::tr(s_tiles[i].label);
      gui::textBounds(g, label, &bw, &bh);
      int ty = s_tiles[i].app ? r.y + (r.h - (int)bh) / 2 : r.y + r.h / 2 - 16;
      g.setCursor(r.x + (r.w - (int)bw) / 2, ty);
      gui::print(g, label);
      if (!s_tiles[i].app) {
        g.setTextSize(1);
        gui::textBounds(g, i18n::tr(i18n::Str::TileSoon), &bw, &bh);
        g.setCursor(r.x + (r.w - (int)bw) / 2, r.y + r.h / 2 + 8);
        gui::print(g, i18n::tr(i18n::Str::TileSoon));
      }
    }

    // Hinweis-Zeile: Now-Playing oder Resume-Angebot.
    audio::Status st = audio::status();
    s_shownIndex = st.playing ? st.pos : -1;
    g.setTextSize(1);
    g.setCursor(8, kHintY);
    if (st.playing) {
      char p[TRACK_PATH_LEN], nm[64] = "";
      audio::currentPath(p, sizeof(p));
      int flat = p[0] ? library::indexOfPath(p) : -1;
      if (flat >= 0) library::name(flat, nm, sizeof(nm));
      else if (p[0]) {
        const char* base = strrchr(p, '/');
        strncpy(nm, base ? base + 1 : p, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0';
      }
      char line[80];
      snprintf(line, sizeof(line), "%c %s", 0x0E, nm);   // ♫ Titel
      gui::print(g, line);
    } else {
      char p[TRACK_PATH_LEN]; uint32_t pos;
      settings::lastTrack(p, sizeof(p), &pos);
      if (p[0] && library::indexOfPath(p) >= 0) {
        const char* base = strrchr(p, '/');
        char line[80];
        snprintf(line, sizeof(line), i18n::tr(i18n::Str::FmtResume), base ? base + 1 : p);
        gui::print(g, line);
      }
    }

    // Seiten-Indikator + Pfeile (rechts in der Hinweiszeile).
    if (kPages > 1) {
      g.setTextSize(1);
      char pg[8];
      snprintf(pg, sizeof(pg), "%d/%d", s_page + 1, kPages);
      g.setCursor(kPrevPage.x + 6, kHintY); gui::print(g, "\x11");   // ◄
      g.setCursor(kPrevPage.x + 18, kHintY); gui::print(g, pg);
      g.setCursor(kNextPage.x + 6, kHintY); gui::print(g, "\x10");   // ►
    }
  }

 private:
  int s_shownIndex = -1;

  // Erster belegter Slot einer Seite (für die Cursor-Position nach Seitenwechsel).
  int firstSlot(int p) {
    for (int s = 0; s < kPerPage; s++)
      if (s_tiles[p * kPerPage + s].label != i18n::Str::None) return s;
    return 0;
  }

  bool flipPage(int dir) {
    int p = s_page + dir;
    if (p < 0 || p >= kPages || !pageHasTiles(p)) return false;
    s_page = p;
    s_cursor = firstSlot(p);
    appmgr::markDirty();
    return true;
  }

  void moveCursor(int delta) {
    int n = s_cursor + delta;
    // A/D (±1) am Spaltenrand wechselt die Seite.
    if ((delta == 1 || delta == -1) && (n < 0 || n >= kPerPage)) { flipPage(delta); return; }
    while (n >= 0 && n < kPerPage &&
           s_tiles[s_page * kPerPage + n].label == i18n::Str::None) n += (delta > 0 ? 1 : -1);
    if (n < 0 || n >= kPerPage || s_tiles[s_page * kPerPage + n].label == i18n::Str::None) return;
    if (n != s_cursor) { s_cursor = n; appmgr::markDirty(); }
  }

  // Hinweis-Zeile aktivieren: läuft Musik -> Musik-App; sonst Resume starten.
  void onHintActivate() {
    audio::Status st = audio::status();
    if (st.playing) {
      if (s_tiles[0].app) appmgr::launch(s_tiles[0].app);
      return;
    }
    char p[TRACK_PATH_LEN]; uint32_t pos;
    settings::lastTrack(p, sizeof(p), &pos);
    if (p[0] && library::indexOfPath(p) >= 0) {
      // Ab gespeicherter Position fortsetzen (Seek zuverlässig bei CBR-MP3).
      audio::queueBegin(audio::Owner::Music);
      audio::queueAdd(p);
      audio::queueCommit(0, pos > 5 ? pos - 5 : 0);   // 5 s Überlappung
      if (s_tiles[0].app) appmgr::launch(s_tiles[0].app);
    }
  }
};

LauncherApp s_app;

}  // namespace

namespace launcher {

App* get() { return &s_app; }

void setTile(int idx, i18n::Str label, App* app) {
  if (idx < 0 || idx >= kTiles) return;
  s_tiles[idx] = Tile{label, app};
}

}  // namespace launcher
