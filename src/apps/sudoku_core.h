// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// sudoku_core.h — Sudoku-Logik (Generator + Solver), Arduino-frei (host-testbar).
//
// Generierung: erst eine vollständige Lösung per randomisiertem Backtracking,
// dann Zellen in Zufallsreihenfolge ausgraben, solange die Lösung EINDEUTIG
// bleibt (countSolutions bricht bei 2 ab). Der Backtracker arbeitet iterativ
// mit explizitem Level-Stack (rekursionsfrei, wie mines::floodOpen) — 81 Zellen
// tief würde sonst den Loop-Task-Stack sprengen.
// =============================================================================
#pragma once

#include <stdint.h>

namespace sudoku {

using RndFn = uint32_t (*)(uint32_t);   // liefert 0..n-1

enum Difficulty : uint8_t { EASY, MEDIUM, HARD, DIFF_COUNT };

// Ziel-Anzahl an Vorgaben (givens) je Schwierigkeit.
inline int cluesFor(uint8_t diff) {
  switch (diff) {
    case EASY: return 40;
    case HARD: return 26;
    default:   return 32;   // MEDIUM
  }
}

struct Puzzle {
  uint8_t cell[81];    // aktueller Spielstand, 0 = leer, sonst 1..9
  uint8_t sol[81];     // vollständige Lösung (Referenz)
  bool    given[81];   // true = Vorgabe (unveränderlich)
};

// Passt Ziffer v (1..9) konfliktfrei in Zelle idx von g? (g[idx] wird ignoriert,
// solange es 0 ist; Aufrufer leeren die Zelle vor dem Probieren.)
inline bool fits(const uint8_t* g, int idx, int v) {
  int r = idx / 9, c = idx % 9;
  for (int i = 0; i < 9; i++) {
    if (g[r * 9 + i] == v) return false;   // Zeile
    if (g[i * 9 + c] == v) return false;   // Spalte
  }
  int br = (r / 3) * 3, bc = (c / 3) * 3;
  for (int dr = 0; dr < 3; dr++)
    for (int dc = 0; dc < 3; dc++)
      if (g[(br + dr) * 9 + (bc + dc)] == v) return false;   // 3×3-Box
  return true;
}

namespace detail {

// Mischt die Ziffern 1..9 in order[9] (Fisher-Yates).
inline void shuffleDigits(uint8_t* order, RndFn rnd) {
  for (int i = 0; i < 9; i++) order[i] = (uint8_t)(i + 1);
  for (int i = 8; i > 0; i--) {
    int j = (int)rnd((uint32_t)(i + 1));
    uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
  }
}

// Iterativer Backtracker über die leeren Zellen von g. Zählt Lösungen bis
// höchstens `limit` (Abbruch danach). order[9] = Probierreihenfolge der Ziffern.
// Bei limit==1 bleibt die gefundene Lösung in g stehen.
inline int solveCount(uint8_t* g, int limit, const uint8_t* order) {
  int empties[81], ne = 0;
  for (int i = 0; i < 81; i++) if (g[i] == 0) empties[ne++] = i;
  if (ne == 0) return 1;   // schon voll = genau eine Lösung

  uint8_t pos[81];         // pos[l] = wie viele Ziffern auf Level l schon probiert
  pos[0] = 0;
  int lv = 0, solutions = 0;
  while (lv >= 0) {
    int cell = empties[lv];
    g[cell] = 0;           // Level (neu) betreten: Zelle frei, ab pos[lv] probieren
    bool placed = false;
    while (pos[lv] < 9) {
      uint8_t v = order[pos[lv]++];
      if (fits(g, cell, v)) { g[cell] = v; placed = true; break; }
    }
    if (!placed) {         // Sackgasse -> Level zurücksetzen, eins zurück
      pos[lv] = 0;
      lv--;
      continue;
    }
    if (lv == ne - 1) {    // letzte Zelle gefüllt -> eine Lösung
      solutions++;
      if (solutions >= limit) return solutions;
      continue;            // gleiche Zelle, nächste Ziffer (weitersuchen)
    }
    lv++;
    pos[lv] = 0;
  }
  return solutions;
}

}  // namespace detail

// Füllt g (leer/teilgefüllt) randomisiert zu einer gültigen Lösung; false =
// unlösbar. Die Lösung bleibt in g stehen.
inline bool solveFill(uint8_t* g, RndFn rnd) {
  uint8_t order[9];
  detail::shuffleDigits(order, rnd);
  return detail::solveCount(g, 1, order) >= 1;
}

// Zählt die Lösungen von g bis höchstens `limit` (verändert g NICHT).
inline int countSolutions(const uint8_t* g, int limit) {
  uint8_t tmp[81];
  for (int i = 0; i < 81; i++) tmp[i] = g[i];
  static const uint8_t order[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  return detail::solveCount(tmp, limit, order);
}

// Erzeugt ein Rätsel mit angestrebt `clues` Vorgaben und eindeutiger Lösung.
inline void generate(Puzzle& p, int clues, RndFn rnd) {
  // 1) Vollständige, zufällige Lösung.
  uint8_t work[81];
  for (int i = 0; i < 81; i++) work[i] = 0;
  solveFill(work, rnd);
  for (int i = 0; i < 81; i++) p.sol[i] = work[i];

  // 2) Zellen in Zufallsreihenfolge ausgraben, solange eindeutig lösbar.
  uint8_t order[81];
  for (int i = 0; i < 81; i++) order[i] = (uint8_t)i;
  for (int i = 80; i > 0; i--) {
    int j = (int)rnd((uint32_t)(i + 1));
    uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
  }

  int filled = 81;
  for (int k = 0; k < 81 && filled > clues; k++) {
    int idx = order[k];
    uint8_t saved = work[idx];
    work[idx] = 0;
    if (countSolutions(work, 2) != 1) work[idx] = saved;   // sonst mehrdeutig
    else                              filled--;
  }

  for (int i = 0; i < 81; i++) {
    p.cell[i]  = work[i];
    p.given[i] = (work[i] != 0);
  }
}

// Setzt v (0 = leer) in Zelle idx, sofern keine Vorgabe. true = verändert.
inline bool setCell(Puzzle& p, int idx, uint8_t v) {
  if (p.given[idx] || p.cell[idx] == v) return false;
  p.cell[idx] = v;
  return true;
}

// Steht in idx eine Ziffer, die mit einer anderen in Zeile/Spalte/Box kollidiert?
inline bool conflict(const Puzzle& p, int idx) {
  int v = p.cell[idx];
  if (v == 0) return false;
  int r = idx / 9, c = idx % 9;
  for (int i = 0; i < 9; i++) {
    int a = r * 9 + i, b = i * 9 + c;
    if (a != idx && p.cell[a] == v) return true;
    if (b != idx && p.cell[b] == v) return true;
  }
  int br = (r / 3) * 3, bc = (c / 3) * 3;
  for (int dr = 0; dr < 3; dr++)
    for (int dc = 0; dc < 3; dc++) {
      int a = (br + dr) * 9 + (bc + dc);
      if (a != idx && p.cell[a] == v) return true;
    }
  return false;
}

// Alle Zellen gefüllt und konfliktfrei? (== Lösung erreicht; bei eindeutigem
// Rätsel zwangsläufig identisch mit p.sol)
inline bool isSolved(const Puzzle& p) {
  for (int i = 0; i < 81; i++) {
    if (p.cell[i] == 0) return false;
    if (conflict(p, i)) return false;
  }
  return true;
}

}  // namespace sudoku
