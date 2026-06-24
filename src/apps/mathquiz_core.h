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
//
// Zeit-Ziel: jede Aufgabe soll in ~20 s lösbar sein. Dafür sind die Zahlen-
// bereiche bewusst gedeckelt UND Aufgaben, die man im Kopf nur näherungsweise
// rechnet (große Produkte, Geld-Prozente), tragen ein Schätzfenster `tol`:
// eine Antwort innerhalb von ±tol zählt als „gut geschätzt" (s. accept()).
// Schnelle, exakte Aufgaben (Plus/Minus, kleines 1x1, glatte Division,
// Rule of 72) behalten tol=0 und verlangen weiter den genauen Wert.
// =============================================================================
#pragma once

#include <stdint.h>

namespace mathquiz {

// Grundrechenarten + CFO-/Finanz-Operationen. Bei den Geld-Ops ist der
// Grundbetrag stets ein Vielfaches von 100 -> ganzzahlige Ergebnisse; die
// Felder a/b sind je Op unterschiedlich belegt (s. Kommentare), die UI
// formatiert den Aufgabentext entsprechend.
enum Op : uint8_t {
  ADD = 0, SUB = 1, MUL = 2, DIV = 3,
  PCT = 4,        // "b% von a"        -> a*b/100
  MARKUP = 5,     // "a + b%"          -> a*(100+b)/100   (Netto + MwSt)
  DISCOUNT = 6,   // "a - b%"          -> a*(100-b)/100   (Rabatt)
  SHARE = 7,      // "a/b = ?%"        -> a*100/b          (Anteil/Marge in %)
  GROWTH = 8,     // "a > end = ?%"    -> Wachstum a->end in %
  RULE72 = 9,     // "2x bei b% = ?J"  -> 72/b  (Verdopplungszeit, Rule of 72)
  OP_COUNT = 10
};

// Operations-Bitmaske (16 Bit, da >8 Operationen).
inline uint16_t opBit(Op o) { return (uint16_t)(1u << o); }

// Liefert 0..n-1 (Gerät: esp_random()%n, Host: deterministischer Stub).
using RndFn = uint32_t (*)(uint32_t);

struct Problem {
  int32_t a, b;     // Operanden (bei DIV gilt a / b)
  uint8_t op;       // Op
  int32_t answer;
  int32_t tol;      // erlaubtes Schätzfenster (absolute Abweichung; 0 = exakt)
};

// Bewertung einer Antwort.
enum Verdict : uint8_t { WRONG = 0, EXACT = 1, CLOSE = 2 };

struct Range { int32_t lo, hi; };

namespace detail {
// Ganzzahl aus [lo, hi] (inklusiv).
inline int32_t pick(RndFn rnd, int32_t lo, int32_t hi) {
  if (hi <= lo) return lo;
  return lo + (int32_t)rnd((uint32_t)(hi - lo + 1));
}
// pct % des Betrags, aber mindestens 1 (sonst hätte ein kleiner Wert kein Fenster).
inline int32_t pctOf(int32_t value, int pct) {
  int32_t v = value < 0 ? -value : value;
  int32_t t = v * pct / 100;
  return t < 1 ? 1 : t;
}
}  // namespace detail

// Schätzfenster (absolute Abweichung) für eine Aufgabe. Aufgaben, die man im
// Kopf nur näherungsweise löst, bekommen ein prozentuales Fenster (das mit der
// Stufe wächst); reine Kopf-Rechen-Aufgaben mit exakter Erwartung bleiben bei 0.
inline int32_t toleranceFor(uint8_t op, uint8_t level, int32_t answer) {
  switch (op) {
    case MUL:                                  // großes 1x1: schätzbar ab Mittel
      return level == 0 ? 0 : detail::pctOf(answer, level == 1 ? 5 : 10);
    case PCT: case MARKUP: case DISCOUNT:      // Geldbeträge: immer schätzbar
      return detail::pctOf(answer, level == 0 ? 2 : (level == 1 ? 4 : 7));
    case SHARE: case GROWTH:                    // Prozent-Ergebnis: ±1..2 Punkte
      return level == 2 ? 2 : 1;
    default:                                     // ADD/SUB/DIV/RULE72: exakt
      return 0;
  }
}

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
      // Bereiche gedeckelt (20-s-Ziel); große Produkte ab Mittel sind schätzbar.
      static const Range ra[3] = {{1, 10}, {2, 12}, {2, 19}};
      static const Range rb[3] = {{1, 10}, {2, 15}, {2, 30}};
      p.a = detail::pick(rnd, ra[level].lo, ra[level].hi);
      p.b = detail::pick(rnd, rb[level].lo, rb[level].hi);
      p.answer = p.a * p.b;
      break;
    }
    case DIV: {
      // Quotient q und Divisor b ganzzahlig ziehen, a = q*b -> exakt teilbar.
      // Divisor bleibt klein (<=12), damit die Division im Kopf rasch aufgeht.
      static const Range rb[3] = {{2, 9}, {2, 10}, {2, 12}};
      static const Range rq[3] = {{1, 10}, {2, 12}, {2, 30}};
      p.b = detail::pick(rnd, rb[level].lo, rb[level].hi);
      int32_t q = detail::pick(rnd, rq[level].lo, rq[level].hi);
      p.a = q * p.b;
      p.answer = q;
      break;
    }
    case PCT:
    case MARKUP:
    case DISCOUNT:
    case SHARE:
    case GROWTH: {
      // Grundbetrag G = 100*K (größere "Geld"-Zahlen) und Prozentsatz aus einem
      // realistischen Satz. Da G ein Vielfaches von 100 ist, bleibt der Anteil
      // G*pct/100 = K*pct stets ganzzahlig.
      static const Range rk[3] = {{1, 20}, {1, 100}, {2, 500}};   // -> G = 100..50000
      static const int8_t pEasy[] = {10, 20, 25, 50};
      static const int8_t pMed[]  = {5, 10, 15, 20, 25, 50};
      static const int8_t pHard[] = {3, 5, 7, 15, 19, 25, 30, 40};
      const int8_t* ps = pEasy; int pn = 4;
      if (level == 1) { ps = pMed;  pn = 6; }
      else if (level == 2) { ps = pHard; pn = 8; }
      int32_t K = detail::pick(rnd, rk[level].lo, rk[level].hi);
      int32_t pct = ps[rnd((uint32_t)pn)];
      int32_t G = 100 * K;
      int32_t part = K * pct;          // = G * pct / 100
      switch (op) {
        case PCT:      p.a = G;    p.b = pct; p.answer = part;       break;
        case MARKUP:   p.a = G;    p.b = pct; p.answer = G + part;   break;
        case DISCOUNT: p.a = G;    p.b = pct; p.answer = G - part;   break;  // pct<100 -> >=0
        case SHARE:    p.a = part; p.b = G;   p.answer = pct;        break;  // "part/G = ?%"
        default:       p.a = G;    p.b = pct; p.answer = pct;        break;  // GROWTH: a -> a+part
      }
      break;
    }
    case RULE72: {
      // Verdopplungszeit nach der "Rule of 72": Jahre ~= 72 / Zinssatz. Nur
      // Teiler von 72 -> ganzzahliges Jahresergebnis.
      static const int8_t rates[] = {2, 3, 4, 6, 8, 9, 12};
      int32_t r = rates[rnd(7)];
      p.a = 0;
      p.b = r;
      p.answer = 72 / r;
      break;
    }
    default: break;
  }
  p.tol = toleranceFor(p.op, level, p.answer);
  return p;
}

// Bewertet eine eingegebene Antwort: exakt, gut geschätzt (innerhalb ±tol) oder
// falsch. Eine gute Schätzung zählt für die Statistik wie eine richtige Antwort.
inline Verdict accept(const Problem& p, int32_t given) {
  if (given == p.answer) return EXACT;
  int32_t d = given - p.answer;
  if (d < 0) d = -d;
  return (p.tol > 0 && d <= p.tol) ? CLOSE : WRONG;
}

// Wählt aus einer Operations-Bitmaske zufällig eine erlaubte Operation.
inline Op pickOp(uint16_t mask, RndFn rnd) {
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
    case PCT: case MARKUP: case DISCOUNT:
    case SHARE: case GROWTH: case RULE72: return '%';
  }
  return '?';
}

}  // namespace mathquiz
