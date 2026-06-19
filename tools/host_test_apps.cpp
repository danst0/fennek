// =============================================================================
// host_test_apps.cpp — Host-Tests für die Arduino-freien App-Cores
// (mathquiz_core, flashcards_core). Nicht Teil des Firmware-Builds.
//
//   g++ -std=c++17 -O2 -I src tools/host_test_apps.cpp -o /tmp/apps_test && /tmp/apps_test
//
// Präzedenz: tools/host_test_games.cpp testet die Spiele-Cores genauso.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "apps/mathquiz_core.h"
#include "apps/flashcards_core.h"

// --- Deterministischer Zufall (LCG) ------------------------------------------
static uint32_t s_seed = 2025;
static uint32_t testRnd(uint32_t n) {
  s_seed = s_seed * 1664525u + 1013904223u;
  return n ? (s_seed >> 16) % n : 0;
}

static int s_checks = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
  s_checks++; \
} while (0)

// =============================================================================
// mathquiz
// =============================================================================
static int testMathquiz() {
  using namespace mathquiz;
  // Grundrechenarten x jede Stufe oft genug erzeugen und Invarianten prüfen.
  for (int op = ADD; op <= DIV; op++) {
    for (uint8_t lvl = 0; lvl < 3; lvl++) {
      for (int i = 0; i < 5000; i++) {
        Problem p = generate((Op)op, lvl, testRnd);
        CHECK(p.op == op);
        switch (op) {
          case ADD: CHECK(p.answer == p.a + p.b); break;
          case SUB:
            CHECK(p.b <= p.a);          // nie negativ
            CHECK(p.answer == p.a - p.b);
            CHECK(p.answer >= 0);
            break;
          case MUL: CHECK(p.answer == p.a * p.b); break;
          case DIV:
            CHECK(p.b >= 2);            // Divisor sinnvoll
            CHECK(p.a % p.b == 0);      // geht glatt auf
            CHECK(p.answer == p.a / p.b);
            CHECK(p.answer >= 1);
            break;
        }
      }
    }
  }

  // Finanz-Ops (Prozent): Grundbetrag durch 100 teilbar -> ganzzahliges Ergebnis.
  for (int op = PCT; op <= DISCOUNT; op++) {
    for (uint8_t lvl = 0; lvl < 3; lvl++) {
      for (int i = 0; i < 5000; i++) {
        Problem p = generate((Op)op, lvl, testRnd);
        CHECK(p.op == op);
        CHECK(p.a % 100 == 0);              // Grundbetrag = Vielfaches von 100
        CHECK(p.b >= 1 && p.b <= 99);       // sinnvoller Prozentsatz
        int32_t part = p.a / 100 * p.b;     // exakt, da a % 100 == 0
        if (op == PCT)         CHECK(p.answer == part);
        else if (op == MARKUP) CHECK(p.answer == p.a + part);
        else                   CHECK(p.answer == p.a - part);
        CHECK(p.answer >= 0);               // Rabatt bleibt nicht-negativ
      }
    }
  }
  // pickOp respektiert die Maske.
  for (int i = 0; i < 1000; i++) {
    Op o = pickOp(opBit(MUL) | opBit(DIV), testRnd);
    CHECK(o == MUL || o == DIV);
  }
  CHECK(pickOp(opBit(SUB), testRnd) == SUB);
  CHECK(pickOp(0, testRnd) == ADD);   // leere Maske -> Fallback
  for (int i = 0; i < 1000; i++) {    // Finanz-Maske
    Op o = pickOp(opBit(PCT) | opBit(MARKUP) | opBit(DISCOUNT), testRnd);
    CHECK(o == PCT || o == MARKUP || o == DISCOUNT);
  }
  CHECK(opChar(ADD) == '+' && opChar(SUB) == '-');
  CHECK(opChar(MUL) == 'x' && opChar(DIV) == ':');
  CHECK(opChar(PCT) == '%' && opChar(MARKUP) == '%' && opChar(DISCOUNT) == '%');
  printf("  mathquiz ok\n");
  return 0;
}

// =============================================================================
// flashcards
// =============================================================================
static int testFlashcards() {
  using namespace flashcards;
  Card c;

  // TAB-Trenner.
  CHECK(parseLine("hund\tder Hund", &c));
  CHECK(strcmp(c.front, "hund") == 0);
  CHECK(strcmp(c.back, "der Hund") == 0);

  // Pipe-Trenner + Trimmen.
  CHECK(parseLine("  katt   |   die Katze  ", &c));
  CHECK(strcmp(c.front, "katt") == 0);
  CHECK(strcmp(c.back, "die Katze") == 0);

  // Mehrere Worte je Seite, TAB hat Vorrang vor späterem '|'.
  CHECK(parseLine("god morgon\tguten Morgen | wörtlich", &c));
  CHECK(strcmp(c.front, "god morgon") == 0);
  CHECK(strcmp(c.back, "guten Morgen | wörtlich") == 0);

  // Kommentare / leer / kein Trenner / leere Seite -> abgelehnt.
  CHECK(!parseLine("# Schwedisch Vokabeln", &c));
  CHECK(!parseLine("", &c));
  CHECK(!parseLine("   ", &c));
  CHECK(!parseLine("ohne trenner", &c));
  CHECK(!parseLine("\tnur rueckseite", &c));
  CHECK(!parseLine("nur vorderseite\t", &c));

  // Folgezeile darf nicht hereinrutschen (eol-Grenze).
  CHECK(parseLine("a\tb\n# c | d", &c));
  CHECK(strcmp(c.front, "a") == 0);
  CHECK(strcmp(c.back, "b") == 0);

  // Leitner: richtig schiebt hoch (max 4), falsch zurück auf 0.
  CHECK(nextBox(0, true) == 1);
  CHECK(nextBox(3, true) == 4);
  CHECK(nextBox(4, true) == 4);       // Decke
  CHECK(nextBox(4, false) == 0);
  CHECK(nextBox(2, false) == 0);

  // Intervalle monoton steigend, Box 0 sofort fällig.
  CHECK(boxInterval(0) == 0);
  CHECK(boxInterval(1) < boxInterval(2));
  CHECK(boxInterval(2) < boxInterval(3));
  CHECK(boxInterval(3) < boxInterval(4));

  // Fälligkeit.
  uint32_t now = 1700000000u;
  CHECK(isDue(0, 0, now));                          // neue Karte
  CHECK(isDue(0, now - 10, now));                   // Box 0: sofort wieder
  CHECK(!isDue(1, now - 100, now));                 // Box 1: noch nicht
  CHECK(isDue(1, now - 2 * 86400u, now));           // Box 1: nach 2 Tagen fällig
  CHECK(!isDue(4, now - 10 * 86400u, now));         // Box 4: 16 Tage nötig
  CHECK(isDue(4, now - 20 * 86400u, now));

  // cardKey stabil + verschieden für verschiedene Fronten.
  CHECK(cardKey("hund") == cardKey("hund"));
  CHECK(cardKey("hund") != cardKey("katt"));
  printf("  flashcards ok\n");
  return 0;
}

int main() {
  if (testMathquiz()) return 1;
  if (testFlashcards()) return 1;
  printf("ALLE TESTS GRÜN (%d Checks)\n", s_checks);
  return 0;
}
