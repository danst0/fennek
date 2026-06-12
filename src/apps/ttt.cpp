// =============================================================================
// ttt.cpp — Tic-Tac-Toe-UI (siehe ttt.h). Logik in ttt_core.h (host-getestet).
// =============================================================================
#include "ttt.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <stdio.h>

#include "config.h"
#include "core/appmgr.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "apps/games_app.h"
#include "apps/ttt_core.h"

namespace {

using gui::Rect;

// --- Layout (Content y 24..319) ------------------------------------------------
constexpr int kCell = 72;
constexpr int kGridX = 12, kGridY = 64;          // 3*72 = 216 px
constexpr int kHeaderY = appmgr::CONTENT_Y + 4;  // y 28
constexpr int kFooterY = 296;

// --- Zustand ---------------------------------------------------------------------
ttt::Board s_board;
uint8_t s_mode = 0;       // 0 = gegen Fennek, 1 = 2 Spieler
uint8_t s_turn = 1;       // 1 = X am Zug (X beginnt, Mensch ist X)
bool    s_inited = false;
bool    s_counted = false;   // Ergebnis schon gezählt (NVS)

void newGame() {
  ttt::reset(s_board);
  s_turn = 1;
  s_counted = false;
  appmgr::markDirty();
}

// Spielende einmalig zählen (nur gegen das Gerät).
void noteResult() {
  int w = ttt::winner(s_board);
  if (!w || s_counted) return;
  s_counted = true;
  if (s_mode == 0) {
    settings::addTttResult(w == 1, w == 3);
    Serial.printf("[GAME] TTT: Ende (%s)\n", w == 1 ? "Sieg" : w == 2 ? "Niederlage" : "Remis");
  }
}

void place(int idx) {
  if (ttt::winner(s_board) || s_board.c[idx]) return;
  s_board.c[idx] = s_turn;
  s_turn = (uint8_t)(3 - s_turn);
  // Gegen das Gerät: Fennek (O) antwortet sofort — Minimax rechnet in <1 ms.
  if (s_mode == 0 && s_turn == 2 && !ttt::winner(s_board)) {
    int m = ttt::bestMove(s_board, 2);
    if (m >= 0) s_board.c[m] = 2;
    s_turn = 1;
  }
  noteResult();
  appmgr::markDirty();   // genau ein Refresh pro Zug (inkl. Antwort)
}

void drawMark(Adafruit_GFX& g, int idx) {
  int cx = kGridX + (idx % 3) * kCell + kCell / 2;
  int cy = kGridY + (idx / 3) * kCell + kCell / 2;
  if (s_board.c[idx] == 1) {           // X: zwei dicke Diagonalen
    for (int o = -1; o <= 1; o++) {
      g.drawLine(cx - 22 + o, cy - 22, cx + 22 + o, cy + 22, GxEPD_BLACK);
      g.drawLine(cx - 22 + o, cy + 22, cx + 22 + o, cy - 22, GxEPD_BLACK);
    }
  } else if (s_board.c[idx] == 2) {    // O: dicker Kreis
    g.drawCircle(cx, cy, 24, GxEPD_BLACK);
    g.drawCircle(cx, cy, 23, GxEPD_BLACK);
    g.drawCircle(cx, cy, 22, GxEPD_BLACK);
  }
}

}  // namespace

namespace ttt_ui {

void enter() {
  if (!s_inited) {
    s_inited = true;
    newGame();
  }
}

void handleInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (e.x >= kGridX && e.x < kGridX + 3 * kCell &&
        e.y >= kGridY && e.y < kGridY + 3 * kCell) {
      int idx = ((e.y - kGridY) / kCell) * 3 + (e.x - kGridX) / kCell;
      place(idx);
    }
    return;
  }
  switch (e.key) {
    case 'n': case 'N': newGame(); break;
    case 'm': case 'M':
      s_mode = (uint8_t)(1 - s_mode);
      newGame();
      break;
    case '\b':
    case 'q': case 'Q': games_app::showMenu(); break;
    default: break;
  }
}

void draw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);

  // Kopfzeile: Modus + Status.
  char head[48];
  int w = ttt::winner(s_board);
  if (w == 1)      snprintf(head, sizeof(head), "%s", s_mode ? i18n::tr(i18n::Str::TttXWins) : i18n::tr(i18n::Str::TttYouWin));
  else if (w == 2) snprintf(head, sizeof(head), "%s", s_mode ? i18n::tr(i18n::Str::TttOWins) : i18n::tr(i18n::Str::TttFennekWins));
  else if (w == 3) snprintf(head, sizeof(head), "%s", i18n::tr(i18n::Str::TttDraw));
  else if (s_mode) snprintf(head, sizeof(head), i18n::tr(i18n::Str::FmtTttTurn), s_turn == 1 ? "X" : "O");
  else             snprintf(head, sizeof(head), "%s", i18n::tr(i18n::Str::TttYouAreX));
  gui::printAt(g, 12, kHeaderY, head, 2);
  g.setTextSize(1);
  g.setCursor(12, kHeaderY + 22);
  gui::print(g, s_mode == 0 ? i18n::tr(i18n::Str::TttVsFennek) : i18n::tr(i18n::Str::TttTwoPlayers));

  // Gitter (3 px dick).
  for (int i = 1; i < 3; i++) {
    g.fillRect(kGridX + i * kCell - 1, kGridY, 3, 3 * kCell, GxEPD_BLACK);
    g.fillRect(kGridX, kGridY + i * kCell - 1, 3 * kCell, 3, GxEPD_BLACK);
  }
  for (int i = 0; i < 9; i++) drawMark(g, i);

  g.setTextSize(1);
  g.setCursor(12, kFooterY);
  gui::print(g, i18n::tr(i18n::Str::TttKeyHint));
}

void menuLine(char* out, size_t n) {
  snprintf(out, n, i18n::tr(i18n::Str::FmtTttWins),
           (unsigned)settings::tttWins(), (unsigned)settings::tttDraws());
}

}  // namespace ttt_ui
