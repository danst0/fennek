// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// ttt_core.h — Tic-Tac-Toe-Spiellogik, Arduino-frei (host-testbar).
//
// Minimax über den vollen Suchraum (max. 9 Halbzüge) — das Gerät spielt
// perfekt; schnellere Siege werden bevorzugt (Score sinkt mit der Tiefe).
// =============================================================================
#pragma once

#include <stdint.h>

namespace ttt {

// 0 = leer, 1 = X, 2 = O.
struct Board {
  uint8_t c[9];   // Index = row*3+col
};

inline void reset(Board& b) {
  for (int i = 0; i < 9; i++) b.c[i] = 0;
}

// 0 = läuft, 1/2 = Gewinner, 3 = Remis (Brett voll).
inline int winner(const Board& b) {
  static const uint8_t L[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
  };
  for (int i = 0; i < 8; i++)
    if (b.c[L[i][0]] && b.c[L[i][0]] == b.c[L[i][1]] && b.c[L[i][1]] == b.c[L[i][2]])
      return b.c[L[i][0]];
  for (int i = 0; i < 9; i++)
    if (!b.c[i]) return 0;
  return 3;
}

namespace detail {

// Score aus Sicht von `player`; `turn` ist am Zug.
inline int minimax(Board& b, uint8_t player, uint8_t turn, int depth) {
  int w = winner(b);
  if (w == 3) return 0;
  if (w) return (w == player) ? 10 - depth : depth - 10;
  int best = (turn == player) ? -100 : 100;
  for (int i = 0; i < 9; i++) {
    if (b.c[i]) continue;
    b.c[i] = turn;
    int s = minimax(b, player, (uint8_t)(3 - turn), depth + 1);
    b.c[i] = 0;
    if (turn == player) { if (s > best) best = s; }
    else                { if (s < best) best = s; }
  }
  return best;
}

}  // namespace detail

// Bester Zug (Index 0..8) für `player`; -1 = kein Feld frei.
inline int bestMove(const Board& b, uint8_t player) {
  Board t = b;
  int best = -1, bestScore = -1000;
  for (int i = 0; i < 9; i++) {
    if (t.c[i]) continue;
    t.c[i] = player;
    int s = detail::minimax(t, player, (uint8_t)(3 - player), 1);
    t.c[i] = 0;
    if (s > bestScore) { bestScore = s; best = i; }
  }
  return best;
}

}  // namespace ttt
