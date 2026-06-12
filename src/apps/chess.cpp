// =============================================================================
// chess.cpp — Schach-UI (siehe chess.h). Logik/KI in chess_core.* (perft-
// und suchgetestet auf dem Host).
//
// KI-Task: chess::search ist reine CPU-Arbeit (keine SPI-/Audio-Berührung)
// und läuft als eigener FreeRTOS-Task (Prio 1, Core 1) — loop() und die
// Mesh-Pumpe bleiben über das Round-Robin der gleichen Prio bedienbar,
// Audio (Core 0, Prio 4) ist gar nicht betroffen. tick() pollt das Ergebnis;
// ein Generationszähler entwertet Resultate nach "Neues Spiel".
// =============================================================================
#include "chess.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <stdio.h>

#include "config.h"
#include "core/appmgr.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "apps/games_app.h"
#include "apps/chess_core.h"

namespace {

using gui::Rect;

// --- Layout (Content y 24..319) ------------------------------------------------
constexpr int kCell = 28;
constexpr int kBoardX = 8, kBoardY = 52;          // 8*28 = 224 px, y 52..275
constexpr int kHeaderY = appmgr::CONTENT_Y + 4;   // y 28
constexpr int kFooterY = 284;

// --- Schwierigkeitsstufen --------------------------------------------------------
const i18n::Str kLevelName[3] = {i18n::Str::LevelEasy, i18n::Str::LevelMid, i18n::Str::LevelHard};
const int      kDepth[3]     = {2, 3, 4};
const uint32_t kNodeLimit[3] = {15000, 60000, 250000};

// Figurbuchstaben (deutsch): Bauer Springer Läufer Turm Dame König.
const char kLetter[7] = {' ', 'B', 'S', 'L', 'T', 'D', 'K'};

// --- Spielzustand ------------------------------------------------------------------
enum Sub : uint8_t { SETUP, BOARD };
Sub s_sub = SETUP;

chess::Pos  s_pos;
chess::Move s_moves[chess::MAX_MOVES];   // legale Züge der aktuellen Stellung
int         s_nMoves = 0;

bool        s_active = false;     // Partie vorhanden (auch beendete Stellung)
uint8_t     s_result = 0;         // 0 läuft, 1 Weiß gewinnt, 2 Schwarz, 3 Remis
const char* s_resultText = "";

uint8_t s_mode = 0;       // 0 = gegen Fennek, 1 = 2 Spieler
int8_t  s_human = 1;      // Farbe des Menschen (Modus 0)
uint8_t s_level = 1;

int s_sel = -1;           // gewähltes Ausgangsfeld (-1 = keins)
int s_cursor = 12;        // Tastatur-Cursor (e2)
int s_promoTo = -1, s_promoFrom = -1;   // Umwandlungs-Dialog aktiv wenn >= 0

// Stellungs-Historie seit letztem irreversiblen Zug (Remis durch Wiederholung).
uint32_t s_hist[120];
int      s_nHist = 0;

bool s_dirtySave = false;   // Partie seit letztem NVS-Write verändert

// --- KI-Task ------------------------------------------------------------------------
volatile bool s_thinking = false;
volatile bool s_aiDone = false;
volatile uint32_t s_aiGen = 0;    // entwertet Alt-Ergebnisse nach Neustart
chess::Move s_aiMove;
bool        s_aiOk = false;
chess::Pos  s_aiPos;
int         s_aiDepth;
uint32_t    s_aiLimit;

void aiTaskFn(void*) {
  uint32_t gen = s_aiGen;
  uint32_t t0 = millis();
  chess::Move mv;
  int score = 0;
  uint32_t nodes = 0;
  bool ok = chess::search(s_aiPos, s_aiDepth, s_aiLimit, &mv, &score, &nodes);
  Serial.printf("[GAME] Schach-KI: Tiefe %d, %lu Knoten, %lu ms, Score %d\n",
                s_aiDepth, (unsigned long)nodes, (unsigned long)(millis() - t0), score);
  if (gen == s_aiGen) {
    s_aiMove = mv;
    s_aiOk = ok;
    s_aiDone = true;
  }
  vTaskDelete(nullptr);
}

void startThinking() {
  s_aiPos = s_pos;
  s_aiDepth = kDepth[s_level];
  s_aiLimit = kNodeLimit[s_level];
  s_aiOk = false;
  s_aiDone = false;
  s_thinking = true;
  // 24 KB Stack: Negamax kopiert pro Ebene Stellung + Zugliste (~1 KB/Ebene).
  if (xTaskCreatePinnedToCore(aiTaskFn, "chess_ai", 24576, nullptr, 1, nullptr, 1) != pdPASS) {
    s_thinking = false;
    Serial.println("[GAME] Schach-KI: Task-Start FEHLGESCHLAGEN");
  }
  appmgr::markDirty();   // "Fennek denkt…" anzeigen
}

// --- Partie-Verwaltung ----------------------------------------------------------------
struct SaveBlob {
  uint32_t   magic;
  chess::Pos pos;
  uint8_t    mode, level;
  int8_t     human;
  uint8_t    pad;
};
constexpr uint32_t kSaveMagic = 0x46434831;   // "FCH1"

void refreshLegal() { s_nMoves = chess::genMoves(s_pos, s_moves); }

void updateResult() {
  s_result = 0;
  s_resultText = "";
  chess::Result r = chess::result(s_pos);
  if (r == chess::MATE) {
    s_result = (s_pos.stm > 0) ? 2 : 1;   // Seite am Zug ist matt
    s_resultText = (s_result == 1) ? i18n::tr(i18n::Str::ChessMateW) : i18n::tr(i18n::Str::ChessMateB);
  } else if (r == chess::STALEMATE) {
    s_result = 3;
    s_resultText = i18n::tr(i18n::Str::ChessStale);
  } else if (r == chess::DRAW50) {
    s_result = 3;
    s_resultText = i18n::tr(i18n::Str::ChessDraw50);
  } else if (s_nHist > 0) {
    int rep = 0;
    uint32_t h = s_hist[s_nHist - 1];
    for (int i = 0; i < s_nHist; i++)
      if (s_hist[i] == h) rep++;
    if (rep >= 3) {
      s_result = 3;
      s_resultText = i18n::tr(i18n::Str::ChessDrawRep);
    }
  }
  if (s_result) {
    Serial.printf("[GAME] Schach: %s\n", s_resultText);
    if (s_mode == 0 &&
        ((s_result == 1 && s_human > 0) || (s_result == 2 && s_human < 0)))
      settings::addChessWin();
    settings::setChessGame(nullptr, 0);   // beendete Partien nicht wiederherstellen
    s_dirtySave = false;
  }
}

void applyMove(const chess::Move& m) {
  chess::makeMove(s_pos, m);
  if (s_pos.halfmove == 0) s_nHist = 0;   // irreversibel: Historie neu
  if (s_nHist < (int)(sizeof(s_hist) / sizeof(s_hist[0])))
    s_hist[s_nHist++] = chess::hashPos(s_pos);
  s_dirtySave = true;
  s_sel = -1;
  refreshLegal();
  updateResult();
  appmgr::markDirty();
}

void maybeStartAi() {
  if (!s_result && s_mode == 0 && s_pos.stm != s_human && !s_thinking)
    startThinking();
}

void newGame() {
  s_aiGen++;             // laufende Suche entwerten
  s_thinking = false;
  s_aiDone = false;
  chess::startPos(s_pos);
  s_active = true;
  s_result = 0;
  s_resultText = "";
  s_nHist = 0;
  s_hist[s_nHist++] = chess::hashPos(s_pos);
  s_sel = -1;
  s_promoFrom = s_promoTo = -1;
  s_cursor = (s_human > 0 || s_mode == 1) ? 12 : 52;   // e2 bzw. e7
  refreshLegal();
  settings::setChessGame(nullptr, 0);
  s_dirtySave = true;
  s_sub = BOARD;
  Serial.printf("[GAME] Schach: neue Partie (%s, Stufe %s)\n",
                s_mode ? "2 Spieler" : "gegen Fennek", kLevelName[s_level]);
  maybeStartAi();
  appmgr::markDirty();
}

bool loadGame() {
  SaveBlob b;
  if (!settings::chessGame(&b, sizeof(b)) || b.magic != kSaveMagic) return false;
  s_pos = b.pos;
  s_mode = b.mode <= 1 ? b.mode : 0;
  s_level = b.level <= 2 ? b.level : 1;
  s_human = (b.human < 0) ? -1 : 1;
  s_active = true;
  s_result = 0;
  s_nHist = 0;
  s_hist[s_nHist++] = chess::hashPos(s_pos);   // Historie vor dem Save entfällt bewusst
  s_sel = -1;
  s_promoFrom = s_promoTo = -1;
  refreshLegal();
  updateResult();
  Serial.println("[GAME] Schach: Partie aus NVS geladen");
  return true;
}

// --- Zugeingabe ------------------------------------------------------------------------
void afterHumanMove() {
  maybeStartAi();
}

bool humanMayMove() {
  if (s_result || s_thinking || s_promoFrom >= 0) return false;
  if (s_mode == 0 && s_pos.stm != s_human) return false;
  return true;
}

void choosePromo(int8_t type) {
  for (int i = 0; i < s_nMoves; i++) {
    if (s_moves[i].from == s_promoFrom && s_moves[i].to == s_promoTo &&
        s_moves[i].promo == type) {
      s_promoFrom = s_promoTo = -1;
      applyMove(s_moves[i]);
      afterHumanMove();
      return;
    }
  }
}

void tapSquare(int sq) {
  if (!humanMayMove()) return;
  int8_t pc = s_pos.board[sq];
  bool own = pc && ((pc > 0) == (s_pos.stm > 0));
  if (s_sel < 0) {
    if (own) { s_sel = sq; appmgr::markDirty(); }
    return;
  }
  if (sq == s_sel) { s_sel = -1; appmgr::markDirty(); return; }
  if (own)         { s_sel = sq; appmgr::markDirty(); return; }

  for (int i = 0; i < s_nMoves; i++) {
    if (s_moves[i].from != s_sel || s_moves[i].to != sq) continue;
    if (s_moves[i].promo) {   // Umwandlung: Figur wählen lassen
      s_promoFrom = s_sel;
      s_promoTo = sq;
      appmgr::markDirty();
      return;
    }
    applyMove(s_moves[i]);
    afterHumanMove();
    return;
  }
  // illegales Ziel: Auswahl stehen lassen, kein Refresh
}

// --- Setup-Dialog ------------------------------------------------------------------------
int s_setupSel = 0;   // 0 Gegner, 1 Farbe, 2 Stufe, 3 Start

const Rect kStartBtn{10, 236, 220, 44};
constexpr int kSetupRowY = 70, kSetupRowH = 30;

void setupChange(int row) {
  switch (row) {
    case 0: s_mode = (uint8_t)(1 - s_mode); break;
    case 1: if (s_mode == 0) s_human = (int8_t)-s_human; break;
    case 2: s_level = (uint8_t)((s_level + 1) % 3); break;
  }
  appmgr::markDirty();
}

void setupInput(const InputEvent& e) {
  if (e.type == InputEvent::TAP) {
    if (kStartBtn.hit(e.x, e.y)) { newGame(); return; }
    if (e.y >= kSetupRowY && e.y < kSetupRowY + 3 * kSetupRowH) {
      int row = (e.y - kSetupRowY) / kSetupRowH;
      s_setupSel = row;
      setupChange(row);
    }
    return;
  }
  switch (e.key) {
    case 'w': case 'W': s_setupSel = (s_setupSel + 3) % 4; appmgr::markDirty(); break;
    case 's': case 'S': s_setupSel = (s_setupSel + 1) % 4; appmgr::markDirty(); break;
    case 'a': case 'A':
    case 'd': case 'D': if (s_setupSel < 3) setupChange(s_setupSel); break;
    case '\r':
      if (s_setupSel == 3) newGame();
      else setupChange(s_setupSel);
      break;
    case '\b':
    case 'q': case 'Q':
      if (s_active) { s_sub = BOARD; appmgr::markDirty(); }
      else games_app::showMenu();
      break;
    default: break;
  }
}

void setupDraw(Adafruit_GFX& g) {
  gui::printAt(g, 10, kHeaderY, i18n::tr(i18n::Str::ChessNewGame), 2);

  char lbl[40];
  for (int row = 0; row < 3; row++) {
    switch (row) {
      case 0: snprintf(lbl, sizeof(lbl), i18n::tr(i18n::Str::FmtOpponent), s_mode ? i18n::tr(i18n::Str::TwoPlayers) : "Fennek"); break;
      case 1:
        if (s_mode == 0)
          snprintf(lbl, sizeof(lbl), i18n::tr(i18n::Str::FmtYourColor), s_human > 0 ? i18n::tr(i18n::Str::ColorWhite) : i18n::tr(i18n::Str::ColorBlack));
        else
          snprintf(lbl, sizeof(lbl), i18n::tr(i18n::Str::FmtYourColor), "-");
        break;
      case 2: snprintf(lbl, sizeof(lbl), i18n::tr(i18n::Str::FmtLevel), i18n::tr(kLevelName[s_level])); break;
    }
    gui::drawRowText(g, kSetupRowY + row * kSetupRowH, kSetupRowH, lbl, false);
  }
  int y = kSetupRowY + s_setupSel * kSetupRowH;
  if (s_setupSel < 3) {
    g.drawRect(0, y, EINK_W, kSetupRowH, GxEPD_BLACK);
    g.drawRect(1, y + 1, EINK_W - 2, kSetupRowH - 2, GxEPD_BLACK);
  }
  gui::drawButton(g, kStartBtn, i18n::tr(i18n::Str::BtnStart), s_setupSel == 3);

  g.setTextSize(1);
  g.setCursor(10, 294);
  gui::print(g, s_active ? i18n::tr(i18n::Str::ChessBackHint) : i18n::tr(i18n::Str::HintChange));
}

// --- Brett zeichnen ------------------------------------------------------------------------
void drawSquare(Adafruit_GFX& g, int sq) {
  int f = sq & 7, r = sq >> 3;
  int x = kBoardX + f * kCell;
  int y = kBoardY + (7 - r) * kCell;

  // Dunkle Felder (a1 dunkel): lichtes Punktraster, Figuren bleiben lesbar.
  if (((r + f) & 1) == 0) {
    for (int yy = 2; yy < kCell; yy += 4)
      for (int xx = 2 + (((yy >> 2) & 1) << 1); xx < kCell; xx += 4)
        g.drawPixel(x + xx, y + yy, GxEPD_BLACK);
  }

  // Auswahl: dicker Rahmen. Tastatur-Cursor: dünner Innenrahmen.
  if (sq == s_sel) {
    for (int i = 0; i < 3; i++)
      g.drawRect(x + i, y + i, kCell - 2 * i, kCell - 2 * i, GxEPD_BLACK);
  } else if (sq == s_cursor && !s_result) {
    g.drawRect(x + 2, y + 2, kCell - 4, kCell - 4, GxEPD_BLACK);
  }

  // Legale Ziele des gewählten Felds markieren.
  if (s_sel >= 0 && sq != s_sel) {
    for (int i = 0; i < s_nMoves; i++) {
      if (s_moves[i].from == s_sel && s_moves[i].to == sq) {
        if (s_pos.board[sq])
          g.drawRect(x + 1, y + 1, kCell - 2, kCell - 2, GxEPD_BLACK);   // Schlag
        else
          g.fillRect(x + kCell / 2 - 2, y + kCell / 2 - 2, 5, 5, GxEPD_BLACK);
        break;
      }
    }
  }

  int8_t pc = s_pos.board[sq];
  if (!pc) return;
  int t = pc > 0 ? pc : -pc;
  int cx = x + kCell / 2, cy = y + kCell / 2;
  if (pc > 0) {
    g.fillCircle(cx, cy, 11, GxEPD_WHITE);
    g.drawCircle(cx, cy, 11, GxEPD_BLACK);
    g.setTextColor(GxEPD_BLACK);
  } else {
    g.fillCircle(cx, cy, 11, GxEPD_BLACK);
    g.setTextColor(GxEPD_WHITE);
  }
  g.setTextSize(2);
  g.setCursor(cx - 5, cy - 7);
  g.print(kLetter[t]);
  g.setTextColor(GxEPD_BLACK);
}

void drawPromoDialog(Adafruit_GFX& g) {
  const int w = 200, h = 76;
  const int x = (EINK_W - w) / 2, y = 130;
  g.fillRect(x, y, w, h, GxEPD_WHITE);
  g.drawRect(x, y, w, h, GxEPD_BLACK);
  g.drawRect(x + 1, y + 1, w - 2, h - 2, GxEPD_BLACK);
  gui::printAt(g, x + 12, y + 8, i18n::tr(i18n::Str::ChessPromo), 1);
  const char* names[4] = {"D", "T", "L", "S"};
  for (int i = 0; i < 4; i++) {
    Rect r{x + 12 + i * 46, y + 26, 40, 38};
    gui::drawButton(g, r, names[i], i == 0);
  }
}

void boardInput(const InputEvent& e) {
  // Umwandlungs-Dialog fängt alles ab.
  if (s_promoFrom >= 0) {
    const int x = (EINK_W - 200) / 2, y = 130;
    if (e.type == InputEvent::TAP) {
      const int8_t types[4] = {chess::QUEEN, chess::ROOK, chess::BISHOP, chess::KNIGHT};
      for (int i = 0; i < 4; i++) {
        Rect r{x + 12 + i * 46, y + 26, 40, 38};
        if (r.hit(e.x, e.y)) { choosePromo(types[i]); return; }
      }
      return;
    }
    switch (e.key) {
      case 'd': case 'D': case '\r': choosePromo(chess::QUEEN);  break;
      case 't': case 'T':            choosePromo(chess::ROOK);   break;
      case 'l': case 'L':            choosePromo(chess::BISHOP); break;
      case 's': case 'S':            choosePromo(chess::KNIGHT); break;
      case '\b': s_promoFrom = s_promoTo = -1; appmgr::markDirty(); break;
      default: break;
    }
    return;
  }

  if (e.type == InputEvent::TAP) {
    if (e.x >= kBoardX && e.x < kBoardX + 8 * kCell &&
        e.y >= kBoardY && e.y < kBoardY + 8 * kCell) {
      int f = (e.x - kBoardX) / kCell;
      int r = 7 - (e.y - kBoardY) / kCell;
      s_cursor = r * 8 + f;
      tapSquare(s_cursor);
    }
    return;
  }
  int f = s_cursor & 7, r = s_cursor >> 3;
  switch (e.key) {
    case 'w': case 'W': r = (r + 1) % 8; break;
    case 's': case 'S': r = (r + 7) % 8; break;
    case 'a': case 'A': f = (f + 7) % 8; break;
    case 'd': case 'D': f = (f + 1) % 8; break;
    case '\r': tapSquare(s_cursor); return;
    case 'n': case 'N': s_sub = SETUP; appmgr::markDirty(); return;
    case '\b':
    case 'q': case 'Q': games_app::showMenu(); return;
    default: return;
  }
  s_cursor = r * 8 + f;
  appmgr::markDirty();
}

void boardDraw(Adafruit_GFX& g) {
  // Kopfzeile: Ergebnis > Denken > Zugrecht (+ Schach-Hinweis).
  char head[40];
  if (s_result) {
    snprintf(head, sizeof(head), "%s", s_resultText);
  } else if (s_thinking) {
    snprintf(head, sizeof(head), "%s", i18n::tr(i18n::Str::ChessThinking));
  } else {
    bool chk = chess::inCheck(s_pos, s_pos.stm);
    snprintf(head, sizeof(head), i18n::tr(i18n::Str::FmtChessTurn),
             s_pos.stm > 0 ? i18n::tr(i18n::Str::ColorWhite) : i18n::tr(i18n::Str::ColorBlack), chk ? i18n::tr(i18n::Str::ChessCheck) : "");
  }
  gui::printAt(g, 8, kHeaderY, head, s_result ? 1 : 2);
  if (s_result) {
    g.setCursor(8, kHeaderY + 12);
    g.setTextSize(1);
    gui::print(g, i18n::tr(i18n::Str::ChessNewHint));
  }

  g.drawRect(kBoardX - 1, kBoardY - 1, 8 * kCell + 2, 8 * kCell + 2, GxEPD_BLACK);
  for (int sq = 0; sq < 64; sq++) drawSquare(g, sq);

  g.setTextSize(1);
  g.setCursor(8, kFooterY);
  if (s_mode == 0)
    gui::print(g, s_human > 0 ? i18n::tr(i18n::Str::YouWhite) : i18n::tr(i18n::Str::YouBlack));
  else
    gui::print(g, i18n::tr(i18n::Str::TwoPlayers));
  g.setCursor(8, kFooterY + 14);
  gui::print(g, i18n::tr(i18n::Str::ChessKeyHint));

  if (s_promoFrom >= 0) drawPromoDialog(g);
}

}  // namespace

namespace chess_ui {

void enter() {
  if (s_active) {
    s_sub = BOARD;
    maybeStartAi();   // z.B. nach Reboot mitten im KI-Zug
    return;
  }
  s_sub = loadGame() ? BOARD : SETUP;
  if (s_sub == BOARD) maybeStartAi();
}

void leave() {
  if (!s_active || s_result || !s_dirtySave) return;
  SaveBlob b{};
  b.magic = kSaveMagic;
  b.pos = s_pos;
  b.mode = s_mode;
  b.level = s_level;
  b.human = s_human;
  settings::setChessGame(&b, sizeof(b));
  s_dirtySave = false;
  Serial.println("[GAME] Schach: Partie ins NVS gesichert");
}

void tick() {
  if (s_thinking && s_aiDone) {
    s_thinking = false;
    s_aiDone = false;
    if (s_aiOk) applyMove(s_aiMove);
    else appmgr::markDirty();
  }
}

void handleInput(const InputEvent& e) {
  if (s_sub == SETUP) setupInput(e);
  else                boardInput(e);
}

void draw(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  if (s_sub == SETUP) setupDraw(g);
  else                boardDraw(g);
}

void menuLine(char* out, size_t n) {
  uint16_t wins = settings::chessWins();
  SaveBlob b;
  bool saved = s_active && !s_result;
  if (!saved) saved = settings::chessGame(&b, sizeof(b)) && b.magic == kSaveMagic;
  if (saved) snprintf(out, n, i18n::tr(i18n::Str::FmtChessSaved), (unsigned)wins);
  else       snprintf(out, n, i18n::tr(i18n::Str::FmtChessWins), (unsigned)wins);
}

}  // namespace chess_ui
