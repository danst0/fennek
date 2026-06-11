#include "launcher.h"
#include "config.h"
#include "core/gui.h"
#include "core/settings.h"
#include "services/audio.h"
#include "services/library.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace {

using gui::Rect;

constexpr int kTiles = 6;

// 2 Spalten x 3 Reihen im Content-Bereich; darunter eine Now-Playing-Zeile.
constexpr int kTileW = 108, kTileH = 78;
constexpr int kCol[2] = {8, 124};
constexpr int kRowY0  = appmgr::CONTENT_Y + 6;     // 30
constexpr int kRowGap = 88;                        // 30, 118, 206 -> Ende 284

constexpr int kHintY = 296;                        // Now-Playing-/Resume-Zeile

struct Tile {
  const char* label;
  App*        app;
};
Tile s_tiles[kTiles] = {};
int  s_cursor = 0;   // Tastatur-Cursor (Kachel-Index)

Rect tileRect(int i) {
  return Rect{kCol[i % 2], kRowY0 + (i / 2) * kRowGap, kTileW, kTileH};
}


class LauncherApp : public App {
 public:
  const char* name() const override { return "Start"; }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      // Hinweis-Zeile: Now-Playing -> Musik-App; Resume-Angebot -> abspielen.
      if (e.y >= kHintY - 8) { onHintActivate(); return; }
      for (int i = 0; i < kTiles; i++) {
        if (s_tiles[i].label && tileRect(i).hit(e.x, e.y)) {
          if (s_tiles[i].app) appmgr::launch(s_tiles[i].app);
          return;
        }
      }
      return;
    }

    switch (e.key) {
      case 'w': case 'W': moveCursor(-2); break;
      case 's': case 'S': moveCursor(+2); break;
      case 'a': case 'A': moveCursor(-1); break;
      case 'd': case 'D': moveCursor(+1); break;
      case '\r':
        if (s_tiles[s_cursor].label && s_tiles[s_cursor].app)
          appmgr::launch(s_tiles[s_cursor].app);
        break;
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
    for (int i = 0; i < kTiles; i++) {
      if (!s_tiles[i].label) continue;
      Rect r = tileRect(i);
      g.drawRoundRect(r.x, r.y, r.w, r.h, 8, GxEPD_BLACK);
      if (i == s_cursor)
        g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, GxEPD_BLACK);
      g.setTextSize(2);
      uint16_t bw, bh;
      gui::textBounds(g, s_tiles[i].label, &bw, &bh);
      int ty = s_tiles[i].app ? r.y + (r.h - (int)bh) / 2 : r.y + r.h / 2 - 16;
      g.setCursor(r.x + (r.w - (int)bw) / 2, ty);
      gui::print(g, s_tiles[i].label);
      if (!s_tiles[i].app) {
        g.setTextSize(1);
        gui::textBounds(g, "(bald)", &bw, &bh);
        g.setCursor(r.x + (r.w - (int)bw) / 2, r.y + r.h / 2 + 8);
        gui::print(g, "(bald)");
      }
    }

    // Hinweis-Zeile: Now-Playing oder Resume-Angebot.
    audio::Status st = audio::status();
    s_shownIndex = st.playing ? st.pos : -1;
    g.setTextSize(1);
    g.setCursor(8, kHintY);
    if (st.playing) {
      char p[192], nm[64] = "";
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
      char p[192]; uint32_t pos;
      settings::lastTrack(p, sizeof(p), &pos);
      if (p[0] && library::indexOfPath(p) >= 0) {
        const char* base = strrchr(p, '/');
        char line[80];
        snprintf(line, sizeof(line), "Fortsetzen: %s", base ? base + 1 : p);
        gui::print(g, line);
      }
    }
  }

 private:
  int s_shownIndex = -1;

  void moveCursor(int delta) {
    int n = s_cursor + delta;
    while (n >= 0 && n < kTiles && !s_tiles[n].label) n += (delta > 0 ? 1 : -1);
    if (n < 0 || n >= kTiles || !s_tiles[n].label) return;
    if (n != s_cursor) { s_cursor = n; appmgr::markDirty(); }
  }

  // Hinweis-Zeile aktivieren: läuft Musik -> Musik-App; sonst Resume starten.
  void onHintActivate() {
    audio::Status st = audio::status();
    if (st.playing) {
      if (s_tiles[0].app) appmgr::launch(s_tiles[0].app);
      return;
    }
    char p[192]; uint32_t pos;
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

void setTile(int idx, const char* label, App* app) {
  if (idx < 0 || idx >= kTiles) return;
  s_tiles[idx] = Tile{label, app};
}

}  // namespace launcher
