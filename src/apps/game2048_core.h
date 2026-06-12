// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// game2048_core.h — 2048-Spiellogik, Arduino-frei (host-testbar).
//
// Zellen als Exponenten: 0 = leer, 1 = "2", 2 = "4", … 11 = "2048".
// Zufall wird als Callback injiziert (Gerät: esp_random()%n, Host: Stub).
// =============================================================================
#pragma once

#include <stdint.h>

namespace game2048 {

struct Board {
  uint8_t cell[16];   // Index = row*4+col
};

enum Dir : uint8_t { LEFT, RIGHT, UP, DOWN };

using RndFn = uint32_t (*)(uint32_t);   // liefert 0..n-1

namespace detail {

// Eine Linie (4 Zellen) Richtung Index 0 schieben + mergen.
// Standard-Regel: jede Kachel merged pro Zug höchstens einmal.
inline bool slideLine(uint8_t c[4], uint32_t* gain) {
  uint8_t out[4] = {0, 0, 0, 0};
  int n = 0;
  for (int i = 0; i < 4; i++)
    if (c[i]) out[n++] = c[i];
  for (int i = 0; i + 1 < n; i++) {
    if (out[i] == out[i + 1]) {
      out[i]++;
      if (gain) *gain += (uint32_t)1 << out[i];
      for (int j = i + 1; j + 1 < n; j++) out[j] = out[j + 1];
      out[--n] = 0;
    }
  }
  bool moved = false;
  for (int i = 0; i < 4; i++) {
    if (out[i] != c[i]) moved = true;
    c[i] = out[i];
  }
  return moved;
}

}  // namespace detail

// Zug in Richtung d; true = mindestens eine Kachel hat sich bewegt.
inline bool slide(Board& b, Dir d, uint32_t* gain) {
  bool moved = false;
  for (int line = 0; line < 4; line++) {
    int idx[4];
    uint8_t tmp[4];
    for (int i = 0; i < 4; i++) {
      switch (d) {
        case LEFT:  idx[i] = line * 4 + i;       break;
        case RIGHT: idx[i] = line * 4 + (3 - i); break;
        case UP:    idx[i] = i * 4 + line;       break;
        case DOWN:  idx[i] = (3 - i) * 4 + line; break;
      }
      tmp[i] = b.cell[idx[i]];
    }
    if (detail::slideLine(tmp, gain)) moved = true;
    for (int i = 0; i < 4; i++) b.cell[idx[i]] = tmp[i];
  }
  return moved;
}

inline int countEmpty(const Board& b) {
  int n = 0;
  for (int i = 0; i < 16; i++)
    if (!b.cell[i]) n++;
  return n;
}

// Neue Kachel auf zufälliger leerer Zelle: 90 % "2", 10 % "4".
inline void spawn(Board& b, RndFn rnd) {
  int empty = countEmpty(b);
  if (!empty) return;
  int target = (int)rnd((uint32_t)empty);
  for (int i = 0; i < 16; i++) {
    if (b.cell[i]) continue;
    if (target-- == 0) {
      b.cell[i] = (rnd(10) == 0) ? 2 : 1;
      return;
    }
  }
}

inline bool canMove(const Board& b) {
  for (int i = 0; i < 16; i++)
    if (!b.cell[i]) return true;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      uint8_t v = b.cell[r * 4 + c];
      if (c < 3 && b.cell[r * 4 + c + 1] == v) return true;
      if (r < 3 && b.cell[(r + 1) * 4 + c] == v) return true;
    }
  return false;
}

inline bool hasTile(const Board& b, uint8_t exp) {
  for (int i = 0; i < 16; i++)
    if (b.cell[i] >= exp) return true;
  return false;
}

inline void reset(Board& b, RndFn rnd) {
  for (int i = 0; i < 16; i++) b.cell[i] = 0;
  spawn(b, rnd);
  spawn(b, rnd);
}

}  // namespace game2048
