// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// mathquiz_app.cpp — "Kopfrechnen": Menü (Operation + Stufe) und Quiz-Screen.
// Logik/Generierung liegt Arduino-frei in mathquiz_core.h (host-getestet).
//
// Bedienung: Menü W/S = Modus, A/D = Stufe, Enter/Tap = Start. Quiz: Ziffern
// tippen, Backspace löscht, Enter prüft (Enter erneut = nächste Aufgabe),
// Q/Zurück = zurück. Ziffern-Tippen aktualisiert nur den Antwort-Streifen
// (renderRegion) — kein Vollbild-Blitzen pro Taste (vgl. Reader/Notizen).
// =============================================================================
#include "mathquiz_app.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_random.h>
#include <string.h>

#include "config.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "apps/mathquiz_core.h"

namespace {

using gui::Rect;
using namespace mathquiz;

constexpr int W = EINK_W;

uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }

// --- Modi ---------------------------------------------------------------------
struct Mode { const char* label; uint16_t mask; };
const Mode kModes[] = {
  {"Plus",     opBit(ADD)},
  {"Minus",    opBit(SUB)},
  {"Mal",      opBit(MUL)},
  {"Geteilt",  opBit(DIV)},
  {"Gemischt", (uint16_t)(opBit(ADD) | opBit(SUB) | opBit(MUL) | opBit(DIV))},
  // CFO: Anteil/Marge, Wachstum (YoY), MwSt-Aufschlag, Rabatt, %-von, Rule of 72.
  {"CFO",      (uint16_t)(opBit(SHARE) | opBit(GROWTH) | opBit(MARKUP) |
                          opBit(DISCOUNT) | opBit(PCT) | opBit(RULE72))},
};
constexpr int kNumModes = sizeof(kModes) / sizeof(kModes[0]);
const char* const kLevels[3] = {"Leicht", "Mittel", "Schwer"};

// --- Layout: Menü (Stufenzeile + 6 Modus-Knöpfe, kompakt für 320 px Höhe) -----
const Rect kLevelBtn{10, 64, 220, 30};
Rect modeBtn(int i) { return Rect{10, 100 + i * 35, 220, 30}; }

// --- Layout: Quiz -------------------------------------------------------------
constexpr int ANSW_Y = 132;   // Streifen Aufgabe-Ergebnis (Region-Refresh)
constexpr int ANSW_H = 108;

// --- Zustand ------------------------------------------------------------------
enum Screen { MENU, QUIZ };
Screen  s_screen = MENU;
int     s_sel = 4;            // Menü-Auswahl (Default: Gemischt)
uint8_t s_level = 1;          // 0..2

uint16_t s_mask = 0;          // aktive Operationsmaske im Quiz
Problem s_problem{};
char    s_input[10] = "";
int     s_inputLen = 0;
bool    s_answered = false;
bool    s_lastCorrect = false;

int s_total = 0, s_correct = 0, s_streak = 0;

// =============================================================================
// Menü
// =============================================================================
void menuDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 10, 28, i18n::tr(i18n::Str::AppMath), 2);

  char sub[40];
  snprintf(sub, sizeof(sub), "Rekord-Serie: %u", settings::mathBestStreak());
  gui::printAt(g, 10, 52, sub, 1);

  // Stufen-Zeile.
  g.drawRoundRect(kLevelBtn.x, kLevelBtn.y, kLevelBtn.w, kLevelBtn.h, 6, GxEPD_BLACK);
  char lvl[32];
  snprintf(lvl, sizeof(lvl), "Stufe: %s", kLevels[s_level]);
  gui::printAt(g, kLevelBtn.x + 10, kLevelBtn.y + 7, lvl, 2);
  gui::printAt(g, kLevelBtn.x + kLevelBtn.w - 40, kLevelBtn.y + 11, "A/D", 1);

  for (int i = 0; i < kNumModes; i++) {
    Rect r = modeBtn(i);
    g.drawRoundRect(r.x, r.y, r.w, r.h, 6, GxEPD_BLACK);
    if (i == s_sel)
      g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, GxEPD_BLACK);
    gui::printAt(g, r.x + 12, r.y + 7, kModes[i].label, 2);
  }
  gui::printAt(g, 10, 309, "W/S Modus  A/D Stufe  Enter Start", 1);
}

void startQuiz() {
  s_mask = kModes[s_sel].mask;
  s_total = s_correct = s_streak = 0;
  s_input[0] = '\0';
  s_inputLen = 0;
  s_answered = false;
  s_problem = generate(pickOp(s_mask, rnd), s_level, rnd);
  s_screen = QUIZ;
  Serial.printf("[MATH] Start: %s, Stufe %s\n", kModes[s_sel].label, kLevels[s_level]);
  appmgr::markDirty();
}

void menuInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (kLevelBtn.hit(e.x, e.y)) { s_level = (s_level + 1) % 3; appmgr::markDirty(); return; }
    for (int i = 0; i < kNumModes; i++)
      if (modeBtn(i).hit(e.x, e.y)) { s_sel = i; startQuiz(); return; }
    return;
  }
  switch (e.key) {
    case 'w': case 'W': s_sel = (s_sel + kNumModes - 1) % kNumModes; appmgr::markDirty(); break;
    case 's': case 'S': s_sel = (s_sel + 1) % kNumModes;            appmgr::markDirty(); break;
    case 'a': case 'A': s_level = (s_level + 2) % 3;                appmgr::markDirty(); break;
    case 'd': case 'D': s_level = (s_level + 1) % 3;                appmgr::markDirty(); break;
    case '\r':          startQuiz(); break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

// =============================================================================
// Quiz
// =============================================================================
// Aufgabe + Antwort + Feedback (mittlerer Streifen) — auch als Region gezeichnet.
void quizMid(Adafruit_GFX& g) {
  g.fillRect(0, ANSW_Y, W, ANSW_H, GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);

  char q[48];
  switch (s_problem.op) {
    case PCT:
      snprintf(q, sizeof(q), "%ld%% von %ld =", (long)s_problem.b, (long)s_problem.a);
      break;
    case MARKUP:
      snprintf(q, sizeof(q), "%ld + %ld%% =", (long)s_problem.a, (long)s_problem.b);
      break;
    case DISCOUNT:
      snprintf(q, sizeof(q), "%ld - %ld%% =", (long)s_problem.a, (long)s_problem.b);
      break;
    case SHARE:   // a = Anteil, b = Basis; gefragt ist der Prozentsatz
      snprintf(q, sizeof(q), "%ld/%ld = ?%%", (long)s_problem.a, (long)s_problem.b);
      break;
    case GROWTH: {   // a -> end; gefragt ist das Wachstum in %
      long end = (long)s_problem.a + (long)s_problem.a / 100 * (long)s_problem.b;
      snprintf(q, sizeof(q), "%ld>%ld = ?%%", (long)s_problem.a, end);
      break;
    }
    case RULE72:   // Verdopplungszeit in Jahren bei b% (Rule of 72)
      snprintf(q, sizeof(q), "2x bei %ld%% = ?J", (long)s_problem.b);
      break;
    default:
      snprintf(q, sizeof(q), "%ld %c %ld =", (long)s_problem.a,
               opChar(s_problem.op), (long)s_problem.b);
      break;
  }
  constexpr int kMargin = 6;
  const int maxW = W - 2 * kMargin;
  uint16_t bw, bh;

  // Größte Schriftgröße (maxSize..1), bei der `text` in maxW passt. textBounds
  // misst bei der aktuell gesetzten TextSize -> vor jeder Messung setTextSize.
  auto fitSize = [&](const char* text, uint8_t maxSize) -> uint8_t {
    for (uint8_t s = maxSize; s > 1; s--) {
      g.setTextSize(s);
      gui::textBounds(g, text, &bw, &bh);
      if ((int)bw <= maxW) return s;
    }
    return 1;
  };
  // Eine zentrierte Zeile bei Größe `size` setzen (misst neu bei dieser Größe).
  auto drawCentered = [&](int y, const char* text, uint8_t size) {
    g.setTextSize(size);
    gui::textBounds(g, text, &bw, &bh);
    int x = (W - (int)bw) / 2;
    if (x < kMargin) x = kMargin;
    gui::printAt(g, x, y, text, size);
  };

  // Aufgabe und Antwort werden auto-skaliert (lange CFO-Aufgaben/Eingaben
  // schrumpfen 3->2->1, statt über den Rand zu laufen).
  uint8_t qsize = fitSize(q, 3);

  char ans[16];
  snprintf(ans, sizeof(ans), "%s%s", s_input, s_answered ? "" : "_");
  const char* ansShown = ans[0] ? ans : " ";
  uint8_t asize = fitSize(ansShown, 3);

  char fb[40];
  if (s_answered) {
    if (s_lastCorrect) snprintf(fb, sizeof(fb), "Richtig!");
    else snprintf(fb, sizeof(fb), "Falsch: %ld", (long)s_problem.answer);
  }
  uint8_t fsize = s_answered ? fitSize(fb, 2) : 0;

  // Vertikal mittig im Streifen: Blockhöhe aus den gewählten Größen rechnen
  // (9 px/Zeile pro Größe, vgl. drawWrapped), gap zwischen den Zeilen.
  constexpr int gap = 12;
  int hQ = 9 * qsize, hA = 9 * asize, hF = s_answered ? 9 * fsize : 0;
  int blockH = hQ + gap + hA + (s_answered ? gap + hF : 0);
  int y = ANSW_Y + (ANSW_H - blockH) / 2;
  if (y < ANSW_Y + 2) y = ANSW_Y + 2;

  drawCentered(y, q, qsize);
  y += hQ + gap;
  drawCentered(y, ans, asize);   // ans ohne Cursor ist nie leer-zentriert nötig
  if (s_answered) {
    y += hA + gap;
    drawCentered(y, fb, fsize);
  }
}

void quizDraw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 10, 26, kModes[s_sel].label, 2);
  gui::printAt(g, 150, 32, kLevels[s_level], 1);
  g.drawLine(0, 52, W, 52, GxEPD_BLACK);

  quizMid(g);

  char stat[48];
  snprintf(stat, sizeof(stat), "Serie: %d   Richtig: %d/%d", s_streak, s_correct, s_total);
  gui::printAt(g, 10, 286, stat, 1);
  gui::printAt(g, 10, 304, "Ziffern + Enter   Q = zurueck", 1);
}

void nextProblem() {
  s_input[0] = '\0';
  s_inputLen = 0;
  s_answered = false;
  s_problem = generate(pickOp(s_mask, rnd), s_level, rnd);
  appmgr::markDirty();
}

void submit() {
  if (s_inputLen == 0) return;
  long given = atol(s_input);
  s_lastCorrect = (given == s_problem.answer);
  s_total++;
  if (s_lastCorrect) {
    s_correct++;
    s_streak++;
    settings::setMathBestStreak((uint16_t)s_streak);
  } else {
    s_streak = 0;
  }
  s_answered = true;
  appmgr::markDirty();   // Feedback + Statistik (Voll-Refresh, 1x pro Aufgabe)
}

char mapKeyToDigit(char k) {
  switch (k) {
    case 'w': case 'W': return '1';
    case 'e': case 'E': return '2';
    case 'r': case 'R': return '3';
    case 's': case 'S': return '4';
    case 'd': case 'D': return '5';
    case 'f': case 'F': return '6';
    case 'z': case 'Z': return '7';
    case 'x': case 'X': return '8';
    case 'c': case 'C': return '9';
    case 0x02:          return '0'; // Mikrofontaste ohne Alt/Sym
    default:            return k;
  }
}

void quizInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (s_answered) nextProblem();   // Tap nach Bewertung = weiter
    return;
  }
  char key = mapKeyToDigit(e.key);
  if (key == 'q' || key == 'Q') { s_screen = MENU; appmgr::markDirty(); return; }
  if (key == '\b') {
    if (s_answered || s_inputLen == 0) { s_screen = MENU; appmgr::markDirty(); return; }
    s_input[--s_inputLen] = '\0';   // letzte Ziffer löschen
    display::renderRegion(quizMid, ANSW_Y, ANSW_H);
    return;
  }
  if (key == '\r') {
    if (s_answered) nextProblem();
    else submit();
    return;
  }
  if (key >= '0' && key <= '9' && !s_answered) {
    if (s_inputLen < (int)sizeof(s_input) - 2) {
      s_input[s_inputLen++] = key;
      s_input[s_inputLen] = '\0';
      display::renderRegion(quizMid, ANSW_Y, ANSW_H);
    }
  }
}

// =============================================================================
class MathQuizApp : public App {
 public:
  const char* id() const override { return "Kopfrechnen"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppMath); }

  void onEnter() override { s_screen = MENU; }

  void handleInput(const InputEvent& e) override {
    if (s_screen == QUIZ) quizInput(e);
    else menuInput(e);
  }

  void draw(Adafruit_GFX& g) override {
    if (s_screen == QUIZ) quizDraw(g);
    else menuDraw(g);
  }
};

MathQuizApp s_app;

}  // namespace

namespace mathquiz_app {
App* get() { return &s_app; }
}  // namespace mathquiz_app
