// =============================================================================
// game2048.cpp — 2048-UI (siehe game2048.h). Logik in game2048_core.h
// (host-getestet). Ein markDirty pro echtem Zug; Spawn via esp_random().
// =============================================================================
#include "game2048.h"

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
#include "apps/game2048_core.h"

namespace {

// --- Layout (Content y 24..319) ------------------------------------------------
constexpr int kTile = 54, kGap = 4;
constexpr int kGridX = 6, kGridY = 64;            // 4*54 + 3*4 = 228 px
constexpr int kHeaderY = appmgr::CONTENT_Y + 4;   // y 28

// --- Zustand ---------------------------------------------------------------------
game2048::Board s_board;
uint32_t s_score = 0;
bool s_inited = false;
bool s_over = false;
bool s_wonShown = false;   // 2048-Overlay nur einmal pro Partie
bool s_winOverlay = false; // Overlay sichtbar (Enter = weiterspielen)

uint32_t rnd(uint32_t n) { return esp_random() % n; }

void newGame() {
  game2048::reset(s_board, rnd);
  s_score = 0;
  s_over = s_wonShown = s_winOverlay = false;
  appmgr::markDirty();
}

void doMove(game2048::Dir d) {
  if (s_over || s_winOverlay) return;
  uint32_t gain = 0;
  if (!game2048::slide(s_board, d, &gain)) return;   // nichts bewegt -> kein Refresh
  s_score += gain;
  game2048::spawn(s_board, rnd);
  if (!s_wonShown && game2048::hasTile(s_board, 11)) {
    s_wonShown = s_winOverlay = true;
    Serial.printf("[GAME] 2048 erreicht! Punkte=%lu\n", (unsigned long)s_score);
  }
  if (!game2048::canMove(s_board)) {
    s_over = true;
    settings::setBest2048(s_score);   // schreibt nur bei neuem Rekord
    Serial.printf("[GAME] 2048: vorbei, Punkte=%lu (Best=%lu)\n",
                  (unsigned long)s_score, (unsigned long)settings::best2048());
  }
  appmgr::markDirty();
}

// Zentriertes Overlay über dem Grid.
void drawOverlay(Adafruit_GFX& g, const char* line1, const char* line2) {
  const int w = 200, h = 80;
  const int x = (EINK_W - w) / 2, y = kGridY + (4 * kTile + 3 * kGap - h) / 2;
  g.fillRect(x, y, w, h, GxEPD_WHITE);
  g.drawRect(x, y, w, h, GxEPD_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  uint16_t tw, th;
  g.setTextSize(2);
  gui::textBounds(g, line1, &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 16, line1, 2);
  g.setTextSize(1);
  gui::textBounds(g, line2, &tw, &th);
  gui::printAt(g, x + (w - tw) / 2, y + 50, line2, 1);
}

void drawTile(Adafruit_GFX& g, int r, int c) {
  int x = kGridX + c * (kTile + kGap);
  int y = kGridY + r * (kTile + kGap);
  uint8_t e = s_board.cell[r * 4 + c];
  bool inverted = (e >= 10);   // ab 1024 invertiert (E-Ink: maximaler Kontrast)

  if (inverted) g.fillRect(x, y, kTile, kTile, GxEPD_BLACK);
  else          g.drawRect(x, y, kTile, kTile, GxEPD_BLACK);
  if (!inverted && e >= 7)     // ab 128 doppelter Rahmen
    g.drawRect(x + 2, y + 2, kTile - 4, kTile - 4, GxEPD_BLACK);
  if (!e) return;

  char num[8];
  snprintf(num, sizeof(num), "%lu", (unsigned long)1 << e);
  uint8_t size = (e >= 7) ? 1 : 2;   // 3-/4-stellig kleiner, sonst groß
  if (e >= 10) size = 2;             // invertierte Kacheln: 4-stellig passt bei 2
  g.setTextSize(size);
  g.setTextColor(inverted ? GxEPD_WHITE : GxEPD_BLACK);
  uint16_t tw, th;
  gui::textBounds(g, num, &tw, &th);
  g.setCursor(x + (kTile - tw) / 2, y + (kTile - th) / 2);
  gui::print(g, num);
  g.setTextColor(GxEPD_BLACK);
}

}  // namespace

namespace game2048_ui {

void enter() {
  if (!s_inited) {
    s_inited = true;
    newGame();
  }
}

void handleInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) return;   // 2048 ist Tastatur-gesteuert (WASD)
  if (s_winOverlay && (e.key == '\r' || e.key == ' ')) {
    s_winOverlay = false;
    appmgr::markDirty();
    return;
  }
  switch (e.key) {
    case 'a': case 'A': doMove(game2048::LEFT);  break;
    case 'd': case 'D': doMove(game2048::RIGHT); break;
    case 'w': case 'W': doMove(game2048::UP);    break;
    case 's': case 'S': doMove(game2048::DOWN);  break;
    case 'n': case 'N': newGame(); break;
    case '\b':
    case 'q': case 'Q':
      settings::setBest2048(s_score);   // laufenden Rekord nicht verlieren
      games_app::showMenu();
      break;
    default: break;
  }
}

void draw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);

  char buf[32];
  snprintf(buf, sizeof(buf), i18n::tr(i18n::Str::FmtPoints), (unsigned long)s_score);
  gui::printAt(g, 6, kHeaderY, buf, 2);
  uint32_t best = settings::best2048();
  if (s_score > best) best = s_score;
  snprintf(buf, sizeof(buf), i18n::tr(i18n::Str::FmtBest), (unsigned long)best);
  g.setTextSize(1);
  uint16_t tw, th;
  gui::textBounds(g, buf, &tw, &th);
  gui::printAt(g, EINK_W - 6 - tw, kHeaderY + 4, buf, 1);

  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) drawTile(g, r, c);

  g.setTextSize(1);
  g.setCursor(6, 300);
  gui::print(g, i18n::tr(i18n::Str::Hint2048));

  if (s_winOverlay) {
    drawOverlay(g, i18n::tr(i18n::Str::Won2048), i18n::tr(i18n::Str::ContinueEnter));
  } else if (s_over) {
    char l2[40];
    snprintf(l2, sizeof(l2), i18n::tr(i18n::Str::Fmt2048Over), (unsigned long)s_score);
    drawOverlay(g, i18n::tr(i18n::Str::GameOver), l2);
  }
}

void menuLine(char* out, size_t n) {
  uint32_t best = settings::best2048();
  if (best) snprintf(out, n, i18n::tr(i18n::Str::FmtBest), (unsigned long)best);
  else      snprintf(out, n, "%s", i18n::tr(i18n::Str::NoGameYet));
}

}  // namespace game2048_ui
