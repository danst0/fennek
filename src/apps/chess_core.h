// =============================================================================
// chess_core.h — Schach-Spiellogik + KI, Arduino-frei (host-testbar).
//
// Vollständige Regeln: Rochade, en passant, Umwandlung, Schach/Matt/Patt,
// 50-Züge-Regel. Stellungswiederholung prüft die UI über hashPos().
// Zuggenerator ist perft-verifiziert (tools/host_test_games.cpp).
//
// KI: Negamax mit Alpha-Beta, Capture-Quiescence und iterativer Vertiefung;
// Bewertung = Material + Piece-Square-Tables. Reine CPU-Arbeit ohne jede
// Hardware-Berührung — läuft auf dem Gerät in einem eigenen Task.
// =============================================================================
#pragma once

#include <stdint.h>

namespace chess {

// Figuren: positiver Wert = Weiß, negativer = Schwarz; Betrag = Typ.
enum : int8_t { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

// Rochade-Rechte als Bits.
enum : uint8_t { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };

// Feld-Index: a1 = 0 … h8 = 63 (rank*8 + file).
struct Pos {
  int8_t  board[64];
  int8_t  stm;        // Seite am Zug: 1 = Weiß, -1 = Schwarz
  uint8_t castle;
  int8_t  ep;         // En-passant-Zielfeld, -1 = keins
  uint8_t halfmove;   // Halbzüge seit Bauernzug/Schlag (50-Züge-Regel)
};

struct Move {
  uint8_t from, to;
  int8_t  promo;      // 0 oder Zieltyp (KNIGHT..QUEEN) bei Umwandlung
};

constexpr int MAX_MOVES = 218;

void startPos(Pos& p);

// Wird `sq` von Seite `by` (1/-1) angegriffen?
bool attacked(const Pos& p, int sq, int8_t by);
bool inCheck(const Pos& p, int8_t side);

// Nur legale Züge; Rückgabe = Anzahl (out braucht MAX_MOVES Plätze).
int genMoves(const Pos& p, Move* out);

void makeMove(Pos& p, const Move& m);

enum Result : uint8_t { ONGOING, MATE, STALEMATE, DRAW50 };
Result result(const Pos& p);   // MATE/STALEMATE betrifft die Seite am Zug

// FNV-1a über Brett + Zugrecht + Rochade + ep (für Stellungswiederholung).
uint32_t hashPos(const Pos& p);

// Knotenzähler für den Zuggenerator-Test.
uint64_t perft(const Pos& p, int depth);

// Besten Zug suchen: iterative Vertiefung bis maxDepth; bricht weich ab,
// sobald nodeLimit überschritten ist (letzte fertige Tiefe zählt).
// false = kein legaler Zug (Matt/Patt). score optional (Sicht: Seite am Zug).
bool search(const Pos& p, int maxDepth, uint32_t nodeLimit, Move* best,
            int* score = nullptr, uint32_t* nodesOut = nullptr);

}  // namespace chess
