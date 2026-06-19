// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// mathquiz_core.h — Kopfrechen-Aufgabengenerator, Arduino-frei (host-testbar).
//
// Erzeugt deterministisch (Zufall als Callback injiziert) eine Rechenaufgabe je
// Operation und Schwierigkeitsstufe. Invarianten der Generierung:
//  - Subtraktion liefert NIE ein negatives Ergebnis (b <= a).
//  - Division geht IMMER glatt auf (a = Quotient * b), Divisor >= 2.
// Damit kommt die UI mit reiner Ziffern-Eingabe ohne Minus/Komma aus.
// =============================================================================
#pragma once

#include <stdint.h>

namespace mathquiz {

enum Op : uint8_t { ADD = 0, SUB = 1, MUL = 2, DIV = 3, OP_COUNT = 4 };

inline uint8_t opBit(Op o) { return (uint8_t)(1u << o); }

// Liefert 0..n-1 (Gerät: esp_random()%n, Host: deterministischer Stub).
using RndFn = uint32_t (*)(uint32_t);

struct Problem {
  int32_t a, b;     // Operanden (bei DIV gilt a / b)
  uint8_t op;       // Op
  int32_t answer;
};

struct Range { int32_t lo, hi; };

namespace detail {
// Ganzzahl aus [lo, hi] (inklusiv).
inline int32_t pick(RndFn rnd, int32_t lo, int32_t hi) {
  if (hi <= lo) return lo;
  return lo + (int32_t)rnd((uint32_t)(hi - lo + 1));
}
}  // namespace detail

// Erzeugt eine Aufgabe für Operation op und Stufe level (0=Leicht..2=Schwer).
inline Problem generate(Op op, uint8_t level, RndFn rnd) {
  if (level > 2) level = 2;
  Problem p{};
  p.op = op;
  switch (op) {
    case ADD: {
      static const Range r[3] = {{1, 20}, {2, 100}, {10, 1000}};
      p.a = detail::pick(rnd, r[level].lo, r[level].hi);
      p.b = detail::pick(rnd, r[level].lo, r[level].hi);
      p.answer = p.a + p.b;
      break;
    }
    case SUB: {
      static const Range r[3] = {{1, 20}, {2, 100}, {10, 1000}};
      p.a = detail::pick(rnd, r[level].lo, r[level].hi);
      p.b = detail::pick(rnd, r[level].lo, p.a);   // b<=a -> kein Negativergebnis
      p.answer = p.a - p.b;
      break;
    }
    case MUL: {
      static const Range ra[3] = {{1, 10}, {2, 12}, {2, 20}};
      static const Range rb[3] = {{1, 10}, {2, 20}, {2, 50}};
      p.a = detail::pick(rnd, ra[level].lo, ra[level].hi);
      p.b = detail::pick(rnd, rb[level].lo, rb[level].hi);
      p.answer = p.a * p.b;
      break;
    }
    case DIV: {
      // Quotient q und Divisor b ganzzahlig ziehen, a = q*b -> exakt teilbar.
      static const Range rb[3] = {{2, 10}, {2, 12}, {2, 20}};
      static const Range rq[3] = {{1, 10}, {2, 12}, {2, 50}};
      p.b = detail::pick(rnd, rb[level].lo, rb[level].hi);
      int32_t q = detail::pick(rnd, rq[level].lo, rq[level].hi);
      p.a = q * p.b;
      p.answer = q;
      break;
    }
    default: break;
  }
  return p;
}

// Wählt aus einer Operations-Bitmaske zufällig eine erlaubte Operation.
inline Op pickOp(uint8_t mask, RndFn rnd) {
  Op ops[OP_COUNT];
  int n = 0;
  for (uint8_t i = 0; i < OP_COUNT; i++)
    if (mask & opBit((Op)i)) ops[n++] = (Op)i;
  if (n == 0) return ADD;
  return ops[rnd((uint32_t)n)];
}

// Rechenzeichen für die Anzeige (CP437-sicher: 'x' und ':' statt ×/÷).
inline char opChar(uint8_t op) {
  switch (op) {
    case ADD: return '+';
    case SUB: return '-';
    case MUL: return 'x';
    case DIV: return ':';
  }
  return '?';
}

}  // namespace mathquiz
