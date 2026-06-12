// =============================================================================
// mines.cpp — Minensucher-UI (siehe mines.h). Logik in mines_core.h
// (host-getestet). 10×11-Feld à 24 px; Touch deckt auf, 'f' togglet den
// Flaggen-Modus; kein Live-Timer (E-Ink-Invariante: Refresh nur bei Zug).
// =============================================================================
#include "mines.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <esp_random.h>
#include <stdio.h>

#include "config.h"
#include "core/appmgr.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "apps/games_app.h"
#include "apps/mines_core.h"

namespace {

using gui::Rect;

// --- Layout (Content y 24..319) ------------------------------------------------
constexpr int kW = 10, kH = 11, kMines = 15;
constexpr int kCell = 24;
constexpr int kGridX = 0, kGridY = 52;            // 10*24 = 240 px (volle Breite)
constexpr int kHeaderY = appmgr::CONTENT_Y + 4;   // y 28

const Rect kModeBtn{132, appmgr::CONTENT_Y, 102, 26};

// --- Zustand ---------------------------------------------------------------------
mines::Field s_field;
bool s_inited = false;
bool s_flagMode = false;
bool s_counted = false;
int  s_cursor = 0;
uint32_t s_startMs = 0;      // millis() beim ersten Aufdecken
uint32_t s_elapsedSec = 0;   // eingefroren bei Spielende

// Anti-Dauer-Tap: gehaltener Finger liefert wiederholte Taps (bekanntes
// Touch-Verhalten) — dieselbe Zelle innerhalb 500 ms ignorieren.
int      s_lastTapIdx = -1;
uint32_t s_lastTapMs = 0;

uint32_t rnd(uint32_t n) { return esp_random() % n; }

void newGame() {
  mines::init(s_field, kW, kH, kMines);
  s_flagMode = false;
  s_counted = false;
  s_cursor = kW * kH / 2;
  s_startMs = 0;
  s_elapsedSec = 0;
  appmgr::markDirty();
}

void noteResult() {
  if (s_field.state == mines::RUNNING || s_counted) return;
  s_counted = true;
  s_elapsedSec = s_startMs ? (millis() - s_startMs) / 1000 : 0;
  if (s_field.state == mines::WON) {
    settings::setMinesResult(true, (uint16_t)s_elapsedSec);
    Serial.printf("[GAME] Minensucher: gewonnen in %lu s\n", (unsigned long)s_elapsedSec);
  } else {
    Serial.println("[GAME] Minensucher: Mine erwischt");
  }
}

void revealAt(int idx) {
  if (s_field.state != mines::RUNNING) return;
  if (!s_field.placed) {
    s_startMs = millis();
    mines::firstReveal(s_field, idx, rnd);
  } else {
    mines::reveal(s_field, idx);
  }
  Serial.printf("[GAME] Minensucher: Zelle %d, offen=%u\n", idx, s_field.revealed);
  noteResult();
  appmgr::markDirty();
}

void flagAt(int idx) {
  if (s_field.state != mines::RUNNING || !s_field.placed) return;   // erst graben
  mines::toggleFlag(s_field, idx);
  appmgr::markDirty();
}

void cellAction(int idx) {
  if (s_flagMode) flagAt(idx);
  else            revealAt(idx);
}

void drawCell(Adafruit_GFX& g, int idx) {
  int x = kGridX + (idx % kW) * kCell;
  int y = kGridY + (idx / kW) * kCell;
  uint8_t c = s_field.cell[idx];
  bool open = (c & mines::OPEN) != 0;

  if (!open) {
    // Verdeckt = invertiert (schwarz) — maximaler Kontrast zu offenen Zellen.
    g.fillRect(x, y, kCell, kCell, GxEPD_BLACK);
    g.drawRect(x, y, kCell, kCell, GxEPD_WHITE);
    if (c & mines::FLAG) {
      g.setTextSize(2);
      g.setTextColor(GxEPD_WHITE);
      g.setCursor(x + 7, y + 5);
      g.write((uint8_t)0x10);   // CP437: kleines Dreieck als Fähnchen
      g.setTextColor(GxEPD_BLACK);
    }
    // Bei Niederlage alle Minen zeigen.
    if (s_field.state == mines::LOST && (c & mines::MINE) && !(c & mines::FLAG)) {
      g.setTextSize(2);
      g.setTextColor(GxEPD_WHITE);
      g.setCursor(x + 7, y + 5);
      g.print('*');
      g.setTextColor(GxEPD_BLACK);
    }
  } else {
    g.drawRect(x, y, kCell, kCell, GxEPD_BLACK);
    if (c & mines::MINE) {       // die getroffene Mine
      g.setTextSize(2);
      g.setCursor(x + 7, y + 5);
      g.print('*');
    } else if (c & mines::NUM_MASK) {
      g.setTextSize(2);
      g.setCursor(x + 7, y + 5);
      g.print((char)('0' + (c & mines::NUM_MASK)));
    }
  }

  if (idx == s_cursor && s_field.state == mines::RUNNING) {
    // Tastatur-Cursor: Innenrahmen in Gegenfarbe.
    g.drawRect(x + 3, y + 3, kCell - 6, kCell - 6, open ? GxEPD_BLACK : GxEPD_WHITE);
  }
}

void drawOverlay(Adafruit_GFX& g) {
  const int w = 204, h = 70;
  const int x = (EINK_W - w) / 2, y = kGridY + 90;
  g.fillRect(x, y, w, h, GxEPD_WHITE);
  g.drawRect(x, y, w, h, GxEPD_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  char l1[24], l2[40];
  if (s_field.state == mines::WON) {
    snprintf(l1, sizeof(l1), "%s", i18n::tr(i18n::Str::MinesWin));
    snprintf(l2, sizeof(l2), i18n::tr(i18n::Str::FmtMinesTime), (unsigned long)s_elapsedSec);
  } else {
    snprintf(l1, sizeof(l1), "%s", i18n::tr(i18n::Str::MinesBoom));
    snprintf(l2, sizeof(l2), "%s", i18n::tr(i18n::Str::MinesNewGame));
  }
  uint16_t tw, th;
  g.setTextSize(2);
  gui::textBounds(g, l1, &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 12, l1, 2);
  g.setTextSize(1);
  gui::textBounds(g, l2, &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 44, l2, 1);
}

}  // namespace

namespace mines_ui {

void enter() {
  if (!s_inited) {
    s_inited = true;
    newGame();
  }
}

void handleInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (kModeBtn.hit(e.x, e.y)) {
      s_flagMode = !s_flagMode;
      appmgr::markDirty();
      return;
    }
    if (e.x >= kGridX && e.x < kGridX + kW * kCell &&
        e.y >= kGridY && e.y < kGridY + kH * kCell) {
      int idx = ((e.y - kGridY) / kCell) * kW + (e.x - kGridX) / kCell;
      uint32_t now = millis();
      if (idx == s_lastTapIdx && now - s_lastTapMs < 500) return;
      s_lastTapIdx = idx;
      s_lastTapMs = now;
      s_cursor = idx;
      cellAction(idx);
    }
    return;
  }
  int r = s_cursor / kW, c = s_cursor % kW;
  switch (e.key) {
    case 'w': case 'W': r = (r + kH - 1) % kH; break;
    case 's': case 'S': r = (r + 1) % kH;      break;
    case 'a': case 'A': c = (c + kW - 1) % kW; break;
    case 'd': case 'D': c = (c + 1) % kW;      break;
    case 'f': case 'F':
      s_flagMode = !s_flagMode;
      appmgr::markDirty();
      return;
    case '\r': cellAction(s_cursor); return;
    case ' ':  flagAt(s_cursor);     return;
    case 'n': case 'N': newGame();   return;
    case '\b':
    case 'q': case 'Q': games_app::showMenu(); return;
    default: return;
  }
  s_cursor = r * kW + c;
  appmgr::markDirty();
}

void draw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);

  char buf[24];
  int left = (int)s_field.mines - (int)s_field.flags;
  snprintf(buf, sizeof(buf), i18n::tr(i18n::Str::FmtMinesLeft), left);
  gui::printAt(g, 6, kHeaderY, buf, 2);

  gui::drawButton(g, kModeBtn, s_flagMode ? i18n::tr(i18n::Str::BtnFlag) : i18n::tr(i18n::Str::BtnDig), s_flagMode);

  for (int i = 0; i < kW * kH; i++) drawCell(g, i);

  if (s_field.state != mines::RUNNING) drawOverlay(g);
}

void menuLine(char* out, size_t n) {
  uint16_t wins = settings::minesWins();
  if (wins) snprintf(out, n, i18n::tr(i18n::Str::FmtMinesWins),
                     (unsigned)wins, (unsigned)settings::minesBestSec());
  else      snprintf(out, n, "%s", i18n::tr(i18n::Str::NoWinYet));
}

}  // namespace mines_ui
