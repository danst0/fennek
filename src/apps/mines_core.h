// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// mines_core.h — Minesweeper-Spiellogik, Arduino-frei (host-testbar).
//
// Minen werden erst beim ERSTEN Aufdecken platziert (Erstzelle + Nachbarn
// ausgeschlossen) — der erste Tap ist nie eine Mine und öffnet eine Fläche.
// Flood-Fill läuft iterativ mit explizitem Stack (rekursionsfrei).
// =============================================================================
#pragma once

#include <stdint.h>

namespace mines {

constexpr int MAX_W = 12, MAX_H = 12, MAX_CELLS = MAX_W * MAX_H;

// Zell-Bits; untere 4 Bit = Nachbar-Minenzahl (0..8).
enum : uint8_t { NUM_MASK = 0x0F, MINE = 0x10, OPEN = 0x20, FLAG = 0x40 };

enum State : uint8_t { RUNNING, WON, LOST };

using RndFn = uint32_t (*)(uint32_t);   // liefert 0..n-1

struct Field {
  uint8_t cell[MAX_CELLS];
  uint8_t w, h, mines;
  uint8_t state;
  uint8_t revealed, flags;
  bool    placed;        // Minen liegen erst nach dem ersten Aufdecken
};

inline void init(Field& f, uint8_t w, uint8_t h, uint8_t m) {
  f.w = w; f.h = h; f.mines = m;
  f.state = RUNNING;
  f.revealed = 0; f.flags = 0;
  f.placed = false;
  for (int i = 0; i < w * h; i++) f.cell[i] = 0;
}

// k-ter Nachbar (0..7) von idx; false = außerhalb des Felds.
inline bool neighbor(const Field& f, int idx, int k, int* out) {
  static const int8_t D[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
  int r = idx / f.w + D[k][0];
  int c = idx % f.w + D[k][1];
  if (r < 0 || r >= f.h || c < 0 || c >= f.w) return false;
  *out = r * f.w + c;
  return true;
}

namespace detail {

// Zusammenhängende Nullfläche samt Zahlenrand öffnen. Jede Zelle wird beim
// Push als OPEN markiert -> jede landet höchstens einmal auf dem Stack.
inline void floodOpen(Field& f, int idx) {
  int stack[MAX_CELLS];
  int sp = 0;
  f.cell[idx] |= OPEN;
  f.revealed++;
  stack[sp++] = idx;
  while (sp) {
    int i = stack[--sp];
    if (f.cell[i] & NUM_MASK) continue;   // Zahlzelle = Rand der Fläche
    for (int k = 0; k < 8; k++) {
      int nb;
      if (!neighbor(f, i, k, &nb)) continue;
      if (f.cell[nb] & (OPEN | FLAG | MINE)) continue;
      f.cell[nb] |= OPEN;
      f.revealed++;
      stack[sp++] = nb;
    }
  }
}

}  // namespace detail

// Zelle aufdecken; false = Mine erwischt (state -> LOST).
inline bool reveal(Field& f, int idx) {
  if (f.state != RUNNING || (f.cell[idx] & (OPEN | FLAG))) return true;
  if (f.cell[idx] & MINE) {
    f.cell[idx] |= OPEN;
    f.state = LOST;
    return false;
  }
  detail::floodOpen(f, idx);
  if (f.revealed == f.w * f.h - f.mines) f.state = WON;
  return true;
}

// Erstes Aufdecken: platziert vorher die Minen (idx + Nachbarn sicher).
inline bool firstReveal(Field& f, int idx, RndFn rnd) {
  if (!f.placed) {
    f.placed = true;
    uint8_t allowed[MAX_CELLS];
    int na = 0;
    for (int i = 0; i < f.w * f.h; i++) {
      bool excluded = (i == idx);
      for (int k = 0; k < 8 && !excluded; k++) {
        int nb;
        if (neighbor(f, idx, k, &nb) && nb == i) excluded = true;
      }
      if (!excluded) allowed[na++] = (uint8_t)i;
    }
    for (int m = 0; m < f.mines && na > 0; m++) {
      int pick = (int)rnd((uint32_t)na);
      f.cell[allowed[pick]] |= MINE;
      allowed[pick] = allowed[--na];
    }
    for (int i = 0; i < f.w * f.h; i++) {
      if (f.cell[i] & MINE) continue;
      int cnt = 0;
      for (int k = 0; k < 8; k++) {
        int nb;
        if (neighbor(f, i, k, &nb) && (f.cell[nb] & MINE)) cnt++;
      }
      f.cell[i] |= (uint8_t)cnt;
    }
  }
  return reveal(f, idx);
}

inline void toggleFlag(Field& f, int idx) {
  if (f.state != RUNNING || (f.cell[idx] & OPEN)) return;
  if (f.cell[idx] & FLAG) {
    f.cell[idx] &= (uint8_t)~FLAG;
    f.flags--;
  } else {
    f.cell[idx] |= FLAG;
    f.flags++;
  }
}

}  // namespace mines
