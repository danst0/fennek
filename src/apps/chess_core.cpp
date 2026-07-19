// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// chess_core.cpp — Implementierung (siehe chess_core.h). Arduino-frei.
// =============================================================================
#include "chess_core.h"

namespace chess {

namespace {

inline int fileOf(int sq) { return sq & 7; }
inline int rankOf(int sq) { return sq >> 3; }
inline int sqAt(int r, int f) { return r * 8 + f; }
inline bool onBoard(int r, int f) { return r >= 0 && r < 8 && f >= 0 && f < 8; }

// Richtungs-Deltas als (dRank, dFile). kKing: gerade Indizes = orthogonal,
// ungerade = diagonal — Schieber nutzen dieselbe Tabelle mit Schrittweite.
const int8_t kKnightD[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
const int8_t kKingD[8][2]   = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};

int findKing(const Pos& p, int8_t side) {
  for (int sq = 0; sq < 64; sq++)
    if (p.board[sq] == side * KING) return sq;
  return -1;   // kommt in legalen Stellungen nicht vor
}

// Bauernzug eintragen; auf der Grundreihe des Gegners als 4 Umwandlungen
// (Dame zuerst — gut für die Zugsortierung der Suche).
void addPawnMove(Move* out, int& n, int from, int to, bool promo) {
  if (promo) {
    out[n++] = {(uint8_t)from, (uint8_t)to, QUEEN};
    out[n++] = {(uint8_t)from, (uint8_t)to, ROOK};
    out[n++] = {(uint8_t)from, (uint8_t)to, BISHOP};
    out[n++] = {(uint8_t)from, (uint8_t)to, KNIGHT};
  } else {
    out[n++] = {(uint8_t)from, (uint8_t)to, 0};
  }
}

// Pseudo-legale Züge (Königssicherheit prüft genMoves per Probezug).
int genPseudo(const Pos& p, Move* out) {
  int n = 0;
  const int8_t s = p.stm;
  for (int sq = 0; sq < 64; sq++) {
    int8_t pc = p.board[sq];
    if (!pc || (pc > 0) != (s > 0)) continue;
    int t = pc * s;
    int r = rankOf(sq), f = fileOf(sq);

    if (t == PAWN) {
      int r1 = r + s;
      int promoRank = (s > 0) ? 7 : 0;
      int startRank = (s > 0) ? 1 : 6;
      if (r1 >= 0 && r1 < 8 && !p.board[sqAt(r1, f)]) {
        addPawnMove(out, n, sq, sqAt(r1, f), r1 == promoRank);
        if (r == startRank && !p.board[sqAt(r + 2 * s, f)])
          out[n++] = {(uint8_t)sq, (uint8_t)sqAt(r + 2 * s, f), 0};
      }
      for (int df = -1; df <= 1; df += 2) {
        int ff = f + df;
        if (ff < 0 || ff > 7 || r1 < 0 || r1 > 7) continue;
        int to = sqAt(r1, ff);
        int8_t cap = p.board[to];
        if ((cap && (cap > 0) != (s > 0)) || to == p.ep)
          addPawnMove(out, n, sq, to, r1 == promoRank);
      }
    } else if (t == KNIGHT || t == KING) {
      const int8_t (*dl)[2] = (t == KNIGHT) ? kKnightD : kKingD;
      for (int i = 0; i < 8; i++) {
        int rr = r + dl[i][0], ff = f + dl[i][1];
        if (!onBoard(rr, ff)) continue;
        int8_t cap = p.board[sqAt(rr, ff)];
        if (!cap || (cap > 0) != (s > 0))
          out[n++] = {(uint8_t)sq, (uint8_t)sqAt(rr, ff), 0};
      }
    } else {
      // Schieber: Läufer = diagonale Richtungen, Turm = orthogonale, Dame = alle.
      int step = (t == QUEEN) ? 1 : 2;
      int first = (t == BISHOP) ? 1 : 0;
      for (int i = first; i < 8; i += step) {
        int rr = r + kKingD[i][0], ff = f + kKingD[i][1];
        while (onBoard(rr, ff)) {
          int8_t cap = p.board[sqAt(rr, ff)];
          if (!cap) {
            out[n++] = {(uint8_t)sq, (uint8_t)sqAt(rr, ff), 0};
          } else {
            if ((cap > 0) != (s > 0))
              out[n++] = {(uint8_t)sq, (uint8_t)sqAt(rr, ff), 0};
            break;
          }
          rr += kKingD[i][0]; ff += kKingD[i][1];
        }
      }
    }
  }

  // Rochade: Rechte-Bits garantieren, dass König/Turm noch nie gezogen haben
  // (makeMove löscht sie bei jedem Zug von/auf die relevanten Felder).
  const int8_t e = -s;   // Gegner
  if (s > 0) {
    if ((p.castle & CASTLE_WK) && !p.board[5] && !p.board[6] &&
        !attacked(p, 4, e) && !attacked(p, 5, e) && !attacked(p, 6, e))
      out[n++] = {4, 6, 0};
    if ((p.castle & CASTLE_WQ) && !p.board[1] && !p.board[2] && !p.board[3] &&
        !attacked(p, 4, e) && !attacked(p, 3, e) && !attacked(p, 2, e))
      out[n++] = {4, 2, 0};
  } else {
    if ((p.castle & CASTLE_BK) && !p.board[61] && !p.board[62] &&
        !attacked(p, 60, e) && !attacked(p, 61, e) && !attacked(p, 62, e))
      out[n++] = {60, 62, 0};
    if ((p.castle & CASTLE_BQ) && !p.board[57] && !p.board[58] && !p.board[59] &&
        !attacked(p, 60, e) && !attacked(p, 59, e) && !attacked(p, 58, e))
      out[n++] = {60, 58, 0};
  }
  return n;
}

}  // namespace

void startPos(Pos& p) {
  static const int8_t back[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
  for (int i = 0; i < 64; i++) p.board[i] = EMPTY;
  for (int f = 0; f < 8; f++) {
    p.board[sqAt(0, f)] = back[f];
    p.board[sqAt(1, f)] = PAWN;
    p.board[sqAt(6, f)] = -PAWN;
    p.board[sqAt(7, f)] = (int8_t)-back[f];
  }
  p.stm = 1;
  p.castle = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
  p.ep = -1;
  p.halfmove = 0;
}

bool attacked(const Pos& p, int sq, int8_t by) {
  int r = rankOf(sq), f = fileOf(sq);
  // Bauer: steht eine Reihe "hinter" dem Feld (aus Sicht von `by`).
  int pr = r - by;
  if (pr >= 0 && pr < 8) {
    if (f > 0 && p.board[sqAt(pr, f - 1)] == by * PAWN) return true;
    if (f < 7 && p.board[sqAt(pr, f + 1)] == by * PAWN) return true;
  }
  for (int i = 0; i < 8; i++) {
    int rr = r + kKnightD[i][0], ff = f + kKnightD[i][1];
    if (onBoard(rr, ff) && p.board[sqAt(rr, ff)] == by * KNIGHT) return true;
  }
  for (int i = 0; i < 8; i++) {
    int rr = r + kKingD[i][0], ff = f + kKingD[i][1];
    if (onBoard(rr, ff) && p.board[sqAt(rr, ff)] == by * KING) return true;
  }
  // Schieber-Strahlen: erste Figur auf dem Strahl entscheidet.
  for (int i = 0; i < 8; i++) {
    bool diag = (i & 1) != 0;
    int rr = r + kKingD[i][0], ff = f + kKingD[i][1];
    while (onBoard(rr, ff)) {
      int8_t pc = p.board[sqAt(rr, ff)];
      if (pc) {
        if (pc == by * QUEEN || pc == by * (diag ? BISHOP : ROOK)) return true;
        break;
      }
      rr += kKingD[i][0]; ff += kKingD[i][1];
    }
  }
  return false;
}

bool inCheck(const Pos& p, int8_t side) {
  int k = findKing(p, side);
  return k >= 0 && attacked(p, k, (int8_t)-side);
}

int genMoves(const Pos& p, Move* out) {
  Move tmp[MAX_MOVES];
  int n = genPseudo(p, tmp), m = 0;
  for (int i = 0; i < n; i++) {
    Pos c = p;
    makeMove(c, tmp[i]);
    if (!inCheck(c, p.stm)) out[m++] = tmp[i];
  }
  return m;
}

void makeMove(Pos& p, const Move& m) {
  const int8_t s = p.stm;
  const int8_t pc = p.board[m.from];
  const int t = pc * s;
  bool capture = p.board[m.to] != EMPTY;

  // En passant: Bauer zieht diagonal auf ein leeres Feld.
  if (t == PAWN && (int)m.to == p.ep && fileOf(m.from) != fileOf(m.to) && !p.board[m.to]) {
    p.board[sqAt(rankOf(m.from), fileOf(m.to))] = EMPTY;
    capture = true;
  }
  // Rochade: König springt zwei Spalten -> Turm zieht mit.
  if (t == KING && fileOf(m.to) - fileOf(m.from) == 2) {        // kurz
    p.board[m.to - 1] = p.board[m.to + 1];
    p.board[m.to + 1] = EMPTY;
  } else if (t == KING && fileOf(m.from) - fileOf(m.to) == 2) {  // lang
    p.board[m.to + 1] = p.board[m.to - 2];
    p.board[m.to - 2] = EMPTY;
  }

  p.board[m.to] = m.promo ? (int8_t)(m.promo * s) : pc;
  p.board[m.from] = EMPTY;

  // En-passant-Feld nur direkt nach einem Doppelschritt.
  p.ep = -1;
  if (t == PAWN && (m.to > m.from ? m.to - m.from : m.from - m.to) == 16)
    p.ep = (int8_t)((m.from + m.to) / 2);

  // Rochade-Rechte verfallen, wenn König/Turm zieht oder der Turm fällt.
  auto clearRights = [&p](int sq) {
    switch (sq) {
      case 0:  p.castle &= (uint8_t)~CASTLE_WQ; break;
      case 7:  p.castle &= (uint8_t)~CASTLE_WK; break;
      case 4:  p.castle &= (uint8_t)~(CASTLE_WK | CASTLE_WQ); break;
      case 56: p.castle &= (uint8_t)~CASTLE_BQ; break;
      case 63: p.castle &= (uint8_t)~CASTLE_BK; break;
      case 60: p.castle &= (uint8_t)~(CASTLE_BK | CASTLE_BQ); break;
    }
  };
  clearRights(m.from);
  clearRights(m.to);

  if (t == PAWN || capture) p.halfmove = 0;
  else if (p.halfmove < 255) p.halfmove++;
  p.stm = (int8_t)-s;
}

Result result(const Pos& p) {
  Move mv[MAX_MOVES];
  if (genMoves(p, mv) == 0) return inCheck(p, p.stm) ? MATE : STALEMATE;
  if (p.halfmove >= 100) return DRAW50;
  return ONGOING;
}

uint32_t hashPos(const Pos& p) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
  for (int i = 0; i < 64; i++) mix((uint8_t)p.board[i]);
  mix((uint8_t)p.stm);
  mix(p.castle);
  mix((uint8_t)p.ep);
  return h;
}

uint64_t perft(const Pos& p, int depth) {
  if (depth == 0) return 1;
  Move mv[MAX_MOVES];
  int n = genMoves(p, mv);
  if (depth == 1) return (uint64_t)n;
  uint64_t sum = 0;
  for (int i = 0; i < n; i++) {
    Pos c = p;
    makeMove(c, mv[i]);
    sum += perft(c, depth - 1);
  }
  return sum;
}

// --- KI ----------------------------------------------------------------------
namespace {

constexpr int INF = 1000000;
constexpr int MATE_SCORE = 100000;

const int kPieceVal[7] = {0, 100, 320, 330, 500, 900, 20000};

// Piece-Square-Tables ("Simplified Evaluation Function", chessprogramming.org).
// Zeile 0 = 8. Reihe; für Weiß wird gespiegelt indiziert.
const int8_t kPstPawn[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
  50, 50, 50, 50, 50, 50, 50, 50,
  10, 10, 20, 30, 30, 20, 10, 10,
   5,  5, 10, 25, 25, 10,  5,  5,
   0,  0,  0, 20, 20,  0,  0,  0,
   5, -5,-10,  0,  0,-10, -5,  5,
   5, 10, 10,-20,-20, 10, 10,  5,
   0,  0,  0,  0,  0,  0,  0,  0
};
const int8_t kPstKnight[64] = {
 -50,-40,-30,-30,-30,-30,-40,-50,
 -40,-20,  0,  0,  0,  0,-20,-40,
 -30,  0, 10, 15, 15, 10,  0,-30,
 -30,  5, 15, 20, 20, 15,  5,-30,
 -30,  0, 15, 20, 20, 15,  0,-30,
 -30,  5, 10, 15, 15, 10,  5,-30,
 -40,-20,  0,  5,  5,  0,-20,-40,
 -50,-40,-30,-30,-30,-30,-40,-50
};
const int8_t kPstBishop[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5, 10, 10,  5,  0,-10,
 -10,  5,  5, 10, 10,  5,  5,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -20,-10,-10,-10,-10,-10,-10,-20
};
const int8_t kPstRook[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
   5, 10, 10, 10, 10, 10, 10,  5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
   0,  0,  0,  5,  5,  0,  0,  0
};
const int8_t kPstQueen[64] = {
 -20,-10,-10, -5, -5,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5,  5,  5,  5,  0,-10,
  -5,  0,  5,  5,  5,  5,  0, -5,
   0,  0,  5,  5,  5,  5,  0, -5,
 -10,  5,  5,  5,  5,  5,  0,-10,
 -10,  0,  5,  0,  0,  0,  0,-10,
 -20,-10,-10, -5, -5,-10,-10,-20
};
const int8_t kPstKing[64] = {
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -20,-30,-30,-40,-40,-30,-30,-20,
 -10,-20,-20,-20,-20,-20,-20,-10,
  20, 20,  0,  0,  0,  0,  0, 20,
  20, 30, 10,  0,  0, 10, 30, 20
};
const int8_t* kPst[7] = {nullptr, kPstPawn, kPstKnight, kPstBishop,
                         kPstRook, kPstQueen, kPstKing};

// Statische Bewertung aus Sicht der Seite am Zug.
int evaluate(const Pos& p) {
  int score = 0;   // aus Weiß-Sicht
  for (int sq = 0; sq < 64; sq++) {
    int8_t pc = p.board[sq];
    if (!pc) continue;
    int t = pc > 0 ? pc : -pc;
    // PST-Zeile 0 = 8. Reihe -> Weiß spiegelt den Rang.
    int idx = (pc > 0) ? (7 - rankOf(sq)) * 8 + fileOf(sq) : sq;
    int v = kPieceVal[t] + kPst[t][idx];
    score += (pc > 0) ? v : -v;
  }
  return p.stm > 0 ? score : -score;
}

struct SearchCtx {
  uint32_t nodes;
  uint32_t nodeLimit;   // 0 = unbegrenzt
  bool     aborted;
};

// Zugsortierung: Schlagzüge nach Opferwert zuerst, Umwandlungen hoch.
int moveScore(const Pos& p, const Move& m) {
  int sc = 0;
  int8_t cap = p.board[m.to];
  if (cap) {
    int8_t att = p.board[m.from];
    sc = 16 * kPieceVal[cap > 0 ? cap : -cap] - kPieceVal[att > 0 ? att : -att];
  }
  if (m.promo) sc += 8 * kPieceVal[(int)m.promo];
  return sc;
}

void sortMoves(const Pos& p, Move* mv, int n, const Move* pv) {
  int sc[MAX_MOVES];
  for (int i = 0; i < n; i++) {
    sc[i] = moveScore(p, mv[i]);
    if (pv && mv[i].from == pv->from && mv[i].to == pv->to && mv[i].promo == pv->promo)
      sc[i] = INF;   // Hauptvariante der letzten Iteration zuerst
  }
  for (int i = 1; i < n; i++) {   // Insertion-Sort, absteigend
    Move m = mv[i];
    int s = sc[i], j = i - 1;
    while (j >= 0 && sc[j] < s) { mv[j + 1] = mv[j]; sc[j + 1] = sc[j]; j--; }
    mv[j + 1] = m;
    sc[j + 1] = s;
  }
}

// Capture-Quiescence: nur Schlagzüge weiterrechnen (gegen Horizont-Effekt).
int qsearch(const Pos& p, int alpha, int beta, SearchCtx& ctx) {
  ctx.nodes++;
  if (ctx.nodeLimit && ctx.nodes >= ctx.nodeLimit) ctx.aborted = true;
  int stand = evaluate(p);
  if (stand >= beta || ctx.aborted) return stand >= beta ? beta : alpha;
  if (stand > alpha) alpha = stand;

  Move mv[MAX_MOVES];
  int n = genMoves(p, mv);
  sortMoves(p, mv, n, nullptr);
  for (int i = 0; i < n; i++) {
    bool isEp = (p.board[mv[i].from] * p.stm == PAWN) && (int)mv[i].to == p.ep;
    if (!p.board[mv[i].to] && !isEp) continue;   // nur Schlagzüge
    Pos c = p;
    makeMove(c, mv[i]);
    int sc = -qsearch(c, -beta, -alpha, ctx);
    if (ctx.aborted) break;
    if (sc >= beta) return beta;
    if (sc > alpha) alpha = sc;
  }
  return alpha;
}

int negamax(const Pos& p, int depth, int alpha, int beta, int ply, SearchCtx& ctx) {
  if (ctx.nodeLimit && ctx.nodes >= ctx.nodeLimit) { ctx.aborted = true; return alpha; }
  if (p.halfmove >= 100) return 0;
  if (depth == 0) return qsearch(p, alpha, beta, ctx);
  ctx.nodes++;

  Move mv[MAX_MOVES];
  int n = genMoves(p, mv);
  if (n == 0) return inCheck(p, p.stm) ? -(MATE_SCORE - ply) : 0;
  sortMoves(p, mv, n, nullptr);

  int best = -INF;
  for (int i = 0; i < n; i++) {
    Pos c = p;
    makeMove(c, mv[i]);
    int sc = -negamax(c, depth - 1, -beta, -alpha, ply + 1, ctx);
    if (ctx.aborted) break;
    if (sc > best) best = sc;
    if (sc > alpha) alpha = sc;
    if (alpha >= beta) break;
  }
  return best;
}

}  // namespace

bool search(const Pos& p, int maxDepth, uint32_t nodeLimit, Move* best,
            int* score, uint32_t* nodesOut) {
  Move mv[MAX_MOVES];
  int n = genMoves(p, mv);
  if (n == 0) return false;

  SearchCtx ctx{0, nodeLimit, false};
  Move bestMove = mv[0];
  int bestScore = 0;
  Move pv = bestMove;

  for (int depth = 1; depth <= maxDepth && !ctx.aborted; depth++) {
    sortMoves(p, mv, n, &pv);
    Move iterBest = mv[0];
    int iterScore = -INF;
    int alpha = -INF;
    bool complete = true;
    for (int i = 0; i < n; i++) {
      Pos c = p;
      makeMove(c, mv[i]);
      int sc = -negamax(c, depth - 1, -INF, -alpha, 1, ctx);
      if (ctx.aborted) { complete = false; break; }
      if (sc > iterScore) { iterScore = sc; iterBest = mv[i]; }
      if (sc > alpha) alpha = sc;
    }
    // Abgebrochene Iterationen zählen nur, wenn noch gar keine fertig ist.
    if (complete || depth == 1) {
      bestMove = iterBest;
      bestScore = iterScore;
      pv = iterBest;
    }
  }

  *best = bestMove;
  if (score) *score = bestScore;
  if (nodesOut) *nodesOut = ctx.nodes;
  return true;
}

bool analyzeMove(const Pos& before, const Move& played, int maxDepth,
                 uint32_t nodeLimit, MoveEval* out) {
  // 1) Bester Zug + Bewertung in der Ausgangsstellung (Schülersicht).
  Move best;
  int sBest = 0;
  if (!search(before, maxDepth, nodeLimit, &best, &sBest)) return false;
  const bool isBest = (played.from == best.from && played.to == best.to &&
                       played.promo == best.promo);

  // War der Schülerzug ein Schlag? (auch en passant: Bauer diagonal auf leer)
  const bool isCapture =
      before.board[played.to] != EMPTY ||
      (before.board[played.from] == (int8_t)(PAWN * before.stm) &&
       fileOf(played.from) != fileOf(played.to) && before.board[played.to] == EMPTY);

  // 2) Folgestellung: liefert Bewertung UND den Antwortzug des Trainers.
  Pos after = before;
  makeMove(after, played);
  const bool givesCheck = inCheck(after, after.stm);   // Gegner am Zug im Schach?

  Move reply;
  bool hasReply = false;
  int scoreAfter;
  int sOpp = 0;
  if (search(after, maxDepth, nodeLimit, &reply, &sOpp)) {
    scoreAfter = -sOpp;          // Gegnersicht -> Schülersicht
    hasReply = true;
  } else {
    // Partie endete mit dem Schülerzug: Gegner matt (Schüler gab Matt) oder Patt.
    scoreAfter = inCheck(after, after.stm) ? MATE_SCORE : 0;
  }

  int loss = sBest - scoreAfter;
  if (loss < 0) loss = 0;

  const bool getsMated  = scoreAfter <= -(MATE_SCORE - 1000);
  const bool missedMate = (sBest >= MATE_SCORE - 1000) && !isBest;

  MoveClass cls;
  if (isBest || loss <= 15)   cls = MC_BEST;
  else if (loss <= 60)        cls = MC_GOOD;
  else if (loss <= 150)       cls = MC_INACCURACY;
  else if (loss <= 350)       cls = MC_MISTAKE;
  else                        cls = MC_BLUNDER;
  if (getsMated)              cls = MC_BLUNDER;

  // Hängt die gezogene Figur? (vom Gegner angegriffen, von uns ungedeckt)
  const int8_t opp = after.stm;
  const bool hangs = after.board[played.to] != EMPTY &&
                     attacked(after, played.to, opp) &&
                     !attacked(after, played.to, (int8_t)-opp);
  // Empfehlung war ein Schlag, den der Schüler ausließ?
  const bool missedCapture =
      !isBest && before.board[best.to] != EMPTY && before.board[played.to] == EMPTY;

  out->cls           = cls;
  out->lossCp        = loss;
  out->scoreAfter    = scoreAfter;
  out->best          = best;
  out->playedIsBest  = isBest;
  out->isCapture     = isCapture;
  out->givesCheck    = givesCheck;
  out->hangsPiece    = hangs;
  out->missedCapture = missedCapture;
  out->getsMated     = getsMated;
  out->missedMate    = missedMate;
  out->reply         = reply;
  out->hasReply      = hasReply;
  return true;
}

}  // namespace chess
