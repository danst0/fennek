// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// games_app.cpp — Spiele-Menü + Screen-Dispatch (siehe games_app.h).
// =============================================================================
#include "games_app.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE

#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "apps/game2048.h"
#include "apps/mines.h"
#include "apps/chess.h"
#include "apps/ttt.h"
#include "apps/sudoku.h"

namespace {

using gui::Rect;

enum Screen : uint8_t { MENU, G2048, GMINES, GCHESS, GTTT, GSUDOKU };

Screen s_screen = MENU;
int    s_sel = 0;

constexpr int kEntries = 5;

struct Entry {
  i18n::Str   label;
  Screen      sc;
  void (*menuLine)(char*, size_t);
};

const Entry kMenu[kEntries] = {
  {i18n::Str::Game2048,   G2048,   game2048_ui::menuLine},
  {i18n::Str::GameMines,  GMINES,  mines_ui::menuLine},
  {i18n::Str::GameChess,  GCHESS,  chess_ui::menuLine},
  {i18n::Str::GameTtt,    GTTT,    ttt_ui::menuLine},
  {i18n::Str::GameSudoku, GSUDOKU, sudoku_ui::menuLine},
};

// 5 Buttons à 46 px (y 56..310) unter der Überschrift.
const Rect kBtn[kEntries] = {
  {10,  56, 220, 46},
  {10, 108, 220, 46},
  {10, 160, 220, 46},
  {10, 212, 220, 46},
  {10, 264, 220, 46},
};

void openGame(int i) {
  s_sel = i;
  s_screen = kMenu[i].sc;
  switch (s_screen) {
    case G2048:  game2048_ui::enter(); break;
    case GMINES: mines_ui::enter();    break;
    case GCHESS: chess_ui::enter();    break;
    case GTTT:   ttt_ui::enter();      break;
    case GSUDOKU: sudoku_ui::enter();  break;
    default: break;
  }
  Serial.printf("[GAME] öffne %s\n", i18n::tr(kMenu[i].label));
  appmgr::markDirty();
}

void menuInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    for (int i = 0; i < kEntries; i++)
      if (kBtn[i].hit(e.x, e.y)) { openGame(i); return; }
    return;
  }
  switch (e.key) {
    case 'w': case 'W': case 'i': case 'I': s_sel = (s_sel + kEntries - 1) % kEntries; appmgr::markDirty(); break;
    case 's': case 'S': case 'k': case 'K': s_sel = (s_sel + 1) % kEntries;            appmgr::markDirty(); break;
    case '\r':          openGame(s_sel); break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void menuDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 10, 30, i18n::tr(i18n::Str::AppGames), 2);

  for (int i = 0; i < kEntries; i++) {
    const Rect& r = kBtn[i];
    g.drawRoundRect(r.x, r.y, r.w, r.h, 6, GxEPD_BLACK);
    if (i == s_sel)
      g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, GxEPD_BLACK);
    gui::printAt(g, r.x + 14, r.y + 6, i18n::tr(kMenu[i].label), 2);
    char sub[48];
    kMenu[i].menuLine(sub, sizeof(sub));
    gui::printAt(g, r.x + 14, r.y + 28, sub, 1);
  }
}

class GamesApp : public App {
 public:
  const char* id() const override { return "Spiele"; }

  const char* name() const override {
    switch (s_screen) {
      case G2048:  return "2048";
      case GMINES: return i18n::tr(i18n::Str::GameMines);
      case GCHESS: return i18n::tr(i18n::Str::GameChess);
      case GTTT:   return i18n::tr(i18n::Str::GameTtt);
      case GSUDOKU: return i18n::tr(i18n::Str::GameSudoku);
      default:     return i18n::tr(i18n::Str::AppGames);
    }
  }

  void onEnter() override {
    // Immer im Menü starten (auch beim lastApp-Restore nach Reboot);
    // die Spielstände selbst leben in den Modulen weiter.
    s_screen = MENU;
  }

  void onLeave() override {
    chess_ui::leave();   // sichert eine laufende Partie ins NVS
  }

  void tick() override {
    if (s_screen == GCHESS) chess_ui::tick();
  }

  void handleInput(const InputEvent& e) override {
    switch (s_screen) {
      case G2048:  game2048_ui::handleInput(e); break;
      case GMINES: mines_ui::handleInput(e);    break;
      case GCHESS: chess_ui::handleInput(e);    break;
      case GTTT:   ttt_ui::handleInput(e);      break;
      case GSUDOKU: sudoku_ui::handleInput(e);  break;
      default:     menuInput(e);                break;
    }
  }

  void draw(Adafruit_GFX& g) override {
    switch (s_screen) {
      case G2048:  game2048_ui::draw(g); break;
      case GMINES: mines_ui::draw(g);    break;
      case GCHESS: chess_ui::draw(g);    break;
      case GTTT:   ttt_ui::draw(g);      break;
      case GSUDOKU: sudoku_ui::draw(g);  break;
      default:     menuDraw(g);          break;
    }
  }
};

GamesApp s_app;

}  // namespace

namespace games_app {

App* get() { return &s_app; }

void showMenu() {
  if (s_screen == GCHESS) chess_ui::leave();
  s_screen = MENU;
  appmgr::markDirty();
}

}  // namespace games_app
