// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// flashcards_core.h — Karteikarten-Logik (Leitner) + Zeilen-Parser,
// Arduino-frei (host-testbar). Die SD-I/O und der PSRAM-Kartenspeicher liegen
// in apps/flashcards_app.cpp; hier nur die reine, testbare Logik.
//
// Deck-Format (eine Karte pro Zeile):
//   Vorderseite<TAB>Rückseite       (TAB bevorzugt)
//   Vorderseite | Rückseite         (Pipe als Fallback)
// Zeilen ab '#' und Leerzeilen sind Kommentare/Trenner.
//
// Lernfortschritt = Leitner-Boxen 0..4. Eine Karte ist fällig, wenn seit dem
// letzten Sehen ihr Box-Intervall verstrichen ist (Gerät hat eine deep-sleep-
// feste Systemuhr, s. timesync). Richtige Antwort schiebt eine Box hoch, eine
// falsche zurück auf Box 0.
// =============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

namespace flashcards {

constexpr int kBoxCount = 5;

// Intervall (Sekunden) bis eine Karte in Box `box` wieder fällig wird.
inline uint32_t boxInterval(uint8_t box) {
  constexpr uint32_t kDay = 86400u;
  switch (box) {
    case 0:  return 0;          // sofort (gleiche Lernsitzung)
    case 1:  return 1 * kDay;
    case 2:  return 3 * kDay;
    case 3:  return 7 * kDay;
    default: return 16 * kDay;  // Box 4
  }
}

// Box-Übergang nach einer Bewertung.
inline uint8_t nextBox(uint8_t box, bool correct) {
  if (!correct) return 0;
  if (box + 1 >= kBoxCount) return (uint8_t)(kBoxCount - 1);
  return (uint8_t)(box + 1);
}

// Fälligkeit. Neue Karte (lastSeen==0) ist immer fällig.
inline bool isDue(uint8_t box, uint32_t lastSeen, uint32_t nowEpoch) {
  if (lastSeen == 0) return true;
  return nowEpoch >= lastSeen + boxInterval(box);
}

// --- Zeilen-Parser -----------------------------------------------------------
struct Card {
  char front[96];
  char back[96];
};

namespace detail {
inline const char* skipWs(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}
// Bereich [s, end) getrimmt nach dst kopieren (max cap inkl. NUL).
inline void copyTrim(char* dst, size_t cap, const char* s, const char* end) {
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                     end[-1] == '\r' || end[-1] == '\n'))
    end--;
  s = skipWs(s);
  size_t n = 0;
  while (s < end && n + 1 < cap) dst[n++] = *s++;
  dst[n] = 0;
}
}  // namespace detail

// Eine Zeile parsen. false bei Kommentar/leer/ohne Trenner/leerer Seite.
inline bool parseLine(const char* line, Card* out) {
  const char* p = detail::skipWs(line);
  if (*p == 0 || *p == '#' || *p == '\r' || *p == '\n') return false;

  // Zeilenende bestimmen, damit ein '|' der Folgezeile nicht hereinrutscht.
  const char* eol = line;
  while (*eol && *eol != '\n') eol++;

  const char* sep = nullptr;
  for (const char* q = line; q < eol; q++)
    if (*q == '\t') { sep = q; break; }
  if (!sep)
    for (const char* q = line; q < eol; q++)
      if (*q == '|') { sep = q; break; }
  if (!sep) return false;

  detail::copyTrim(out->front, sizeof(out->front), line, sep);
  detail::copyTrim(out->back, sizeof(out->back), sep + 1, eol);
  return out->front[0] != 0 && out->back[0] != 0;
}

// Stabiler Karten-Schlüssel über die Vorderseite (überlebt Umsortieren des
// Decks; gleiche Polynom-Variante wie settings::crc32).
inline uint32_t cardKey(const char* front) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const char* s = front; *s; ++s) {
    crc ^= (uint8_t)*s;
    for (int i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}

}  // namespace flashcards
