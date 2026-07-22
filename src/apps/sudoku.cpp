// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// sudoku.cpp — Sudoku-UI (siehe sudoku.h). Logik in sudoku_core.h (host-getestet).
// 9×9-Gitter à 26 px; Touch-Tap/IJKL bewegen den Cursor. Ziffern ohne Alt über
// das Tastenaufdruck-Numpad W E R / S D F / Z X C = 1..9 (Alt+1..9 zusätzlich).
// 0/Backspace/Mikrofon leeren. Refresh nur pro Zug (E-Ink-Invariante).
// =============================================================================
#include "sudoku.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <esp_random.h>
#include <stdio.h>

#include "config.h"
#include "core/appmgr.h"
#include "core/gui.h"
#include "core/settings.h"
#include "apps/games_app.h"
#include "apps/sudoku_core.h"

namespace {

using gui::Rect;

// --- Layout (Content y 24..319) ------------------------------------------------
constexpr int kCell  = 26;
constexpr int kGridX = 3;                          // (240 - 9*26) / 2 = 3
constexpr int kGridY = 54;                         // 9*26 = 234 px
constexpr int kHeaderY = appmgr::CONTENT_Y + 4;    // y 28
constexpr int kFooterY = 300;

const Rect kDiffBtn{132, appmgr::CONTENT_Y, 102, 26};

const char* const kDiffName[sudoku::DIFF_COUNT] = {"Leicht", "Mittel", "Schwer"};

// --- Zustand ---------------------------------------------------------------------
sudoku::Puzzle s_p;
uint8_t s_diff   = sudoku::EASY;
int     s_cursor = 40;            // Mitte (Zeile 4, Spalte 4)
bool    s_inited = false;
bool    s_counted = false;        // Sieg schon gezählt (NVS)
uint32_t s_startMs = 0;           // millis() bei erster Eingabe
uint32_t s_elapsedSec = 0;        // eingefroren bei Lösung

// Anti-Dauer-Tap (gehaltener Finger liefert Wiederhol-Taps).
int      s_lastTapIdx = -1;
uint32_t s_lastTapMs = 0;

uint32_t rnd(uint32_t n) { return esp_random() % n; }

void newGame() {
  uint32_t t0 = millis();
  sudoku::generate(s_p, sudoku::cluesFor(s_diff), rnd);
  s_counted = false;
  s_startMs = 0;
  s_elapsedSec = 0;
  Serial.printf("[GAME] Sudoku: neu (%s) in %lu ms\n",
                kDiffName[s_diff], (unsigned long)(millis() - t0));
  appmgr::markDirty();
}

void noteResult() {
  if (s_counted || !sudoku::isSolved(s_p)) return;
  s_counted = true;
  s_elapsedSec = s_startMs ? (millis() - s_startMs) / 1000 : 0;
  settings::setSudokuResult(true, (uint16_t)s_elapsedSec);
  Serial.printf("[GAME] Sudoku: gelöst in %lu s\n", (unsigned long)s_elapsedSec);
}

// Ziffer v (0 = leeren) in die Cursor-Zelle setzen.
void enterDigit(uint8_t v) {
  if (s_counted) return;                       // schon gelöst
  if (!sudoku::setCell(s_p, s_cursor, v)) return;
  if (!s_startMs) s_startMs = millis();
  noteResult();
  appmgr::markDirty();
}

void drawCell(Adafruit_GFX& g, int idx) {
  int x = kGridX + (idx % 9) * kCell;
  int y = kGridY + (idx / 9) * kCell;
  uint8_t v = s_p.cell[idx];

  bool bad = sudoku::conflict(s_p, idx);
  if (bad) g.fillRect(x + 1, y + 1, kCell - 1, kCell - 1, GxEPD_BLACK);

  if (v) {
    g.setTextColor(bad ? GxEPD_WHITE : GxEPD_BLACK);
    g.setTextSize(2);
    g.setCursor(x + 7, y + 6);
    g.print((char)('0' + v));
    if (s_p.given[idx]) {            // Vorgaben fett (Doppelstrich)
      g.setCursor(x + 8, y + 6);
      g.print((char)('0' + v));
    }
    g.setTextColor(GxEPD_BLACK);
  }

  if (idx == s_cursor) {            // Cursor: Innenrahmen in Gegenfarbe
    g.drawRect(x + 2, y + 2, kCell - 3, kCell - 3, bad ? GxEPD_WHITE : GxEPD_BLACK);
  }
}

void drawGrid(Adafruit_GFX& g) {
  const int span = 9 * kCell;
  for (int i = 0; i <= 9; i++) {
    int t = (i % 3 == 0) ? 3 : 1;            // Box-Linien dick
    int x = kGridX + i * kCell; if (i == 9) x -= (t - 1);
    g.fillRect(x, kGridY, t, span + 1, GxEPD_BLACK);
    int y = kGridY + i * kCell; if (i == 9) y -= (t - 1);
    g.fillRect(kGridX, y, span + 1, t, GxEPD_BLACK);
  }
}

void drawOverlay(Adafruit_GFX& g) {
  const int w = 204, h = 70;
  const int x = (EINK_W - w) / 2, y = kGridY + 80;
  g.fillRect(x, y, w, h, GxEPD_WHITE);
  g.drawRect(x, y, w, h, GxEPD_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  char l2[40];
  snprintf(l2, sizeof(l2), "Zeit %02lu:%02lu  ·  N = neu",
           (unsigned long)(s_elapsedSec / 60), (unsigned long)(s_elapsedSec % 60));
  uint16_t tw, th;
  g.setTextSize(2);
  gui::textBounds(g, "Gelöst!", &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 12, "Gelöst!", 2);
  g.setTextSize(1);
  gui::textBounds(g, l2, &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 44, l2, 1);
}

}  // namespace

namespace sudoku_ui {

void enter() {
  if (!s_inited) {
    s_inited = true;
    newGame();
  }
}

void handleInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (kDiffBtn.hit(e.x, e.y)) {
      s_diff = (uint8_t)((s_diff + 1) % sudoku::DIFF_COUNT);
      newGame();
      return;
    }
    if (e.x >= kGridX && e.x < kGridX + 9 * kCell &&
        e.y >= kGridY && e.y < kGridY + 9 * kCell) {
      int idx = ((e.y - kGridY) / kCell) * 9 + (e.x - kGridX) / kCell;
      uint32_t now = millis();
      if (idx == s_lastTapIdx && now - s_lastTapMs < 500) return;
      s_lastTapIdx = idx;
      s_lastTapMs = now;
      s_cursor = idx;
      appmgr::markDirty();
    }
    return;
  }

  int r = s_cursor / 9, c = s_cursor % 9;
  char k = e.key;
  if (k >= 'A' && k <= 'Z') k = k - 'A' + 'a';   // Groß-/Kleinschreibung egal
  switch (k) {
    // Cursor: IJKL (rechte Hand) — WSD sind jetzt Ziffern.
    case 'i': r = (r + 8) % 9; break;
    case 'k': r = (r + 1) % 9; break;
    case 'j': c = (c + 8) % 9; break;
    case 'l': c = (c + 1) % 9; break;
    // Ziffern ohne Alt: Tastenaufdruck-Numpad W E R / S D F / Z X C = 1..9.
    case 'w': enterDigit(1); return;
    case 'e': enterDigit(2); return;
    case 'r': enterDigit(3); return;
    case 's': enterDigit(4); return;
    case 'd': enterDigit(5); return;
    case 'f': enterDigit(6); return;
    case 'z': enterDigit(7); return;
    case 'x': enterDigit(8); return;
    case 'c': enterDigit(9); return;
    // Alt-Ebene weiterhin möglich.
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
      enterDigit((uint8_t)(k - '0')); return;
    case '0': case '\b': case 0x02:        // 0 / Backspace / Mikrofon = leeren
      enterDigit(0); return;
    case 'n': newGame(); return;
    case 'm': case '+':
      s_diff = (uint8_t)((s_diff + 1) % sudoku::DIFF_COUNT);
      newGame(); return;
    case 'q': games_app::showMenu(); return;
    default: return;
  }
  s_cursor = r * 9 + c;
  appmgr::markDirty();
}

void draw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 6, kHeaderY, "Sudoku", 2);
  gui::drawButton(g, kDiffBtn, kDiffName[s_diff], false);

  drawGrid(g);
  for (int i = 0; i < 81; i++) drawCell(g, i);

  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);
  g.setCursor(6, kFooterY);
  gui::print(g, "IJKL bewegen · 1-9 setzen (ohne Alt)");
  g.setCursor(6, kFooterY + 11);
  gui::print(g, "0 leeren · N neu · Q Menü");

  if (s_counted) drawOverlay(g);
}

void menuLine(char* out, size_t n) {
  uint16_t solved = settings::sudokuSolved();
  if (solved) {
    uint16_t best = settings::sudokuBestSec();
    snprintf(out, n, "Gelöst: %u  ·  Best: %02u:%02u",
             (unsigned)solved, (unsigned)(best / 60), (unsigned)(best % 60));
  } else {
    snprintf(out, n, "Noch nicht gelöst");
  }
}

}  // namespace sudoku_ui
