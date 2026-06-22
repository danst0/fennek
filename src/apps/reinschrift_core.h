// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// reinschrift_core.h — Arduino-freier Parser + konfliktsicherer Merge für die
// Todo-App (Reinschrift, ../TodosExtension).
//
// Reinschrift speichert alle Aufgaben in EINER Markdown-Datei, die über
// Nextcloud/WebDAV zwischen Geräten synchronisiert wird. Themen sind Header,
// jede Aufgabe eine Zeile:
//
//   # Zentrale Aufgabenübersicht
//   ### +Chores
//   - [ ] Titel +Topic @Context due:2026-06-21T12:00 ~note:"…" [[link]] ^marker
//   - [x] erledigte Aufgabe …
//
// Felder: [ ]/[x] (erledigt), +Topic, @Context, due:ISO (Jahr ≥ 9999 =
// „irgendwann"), ~note:"…", [[wikilink]] und ^marker (stabile ID).
//
// KONFLIKTAUFLÖSUNG (der eigentliche Kern): mehrere Geräte editieren dieselbe
// Datei. Wir überschreiben sie NIE blind. Lokale Änderungen sind eine Op-Queue,
// jede Op über die stabile ^marker-ID adressiert. applyOps() lädt den frischen
// Remote-Stand und legt NUR unsere Änderungen darüber — fremde Edits an anderen
// Aufgaben bleiben unangetastet. Gleichzeitige Edits derselben Aufgabe folgen
// „last writer wins", verschiedene Aufgaben gehen nie verloren.
//
// Header-only und ohne Arduino/SD/WiFi → host-testbar (tools/host_test_apps.cpp;
// Präzedenz podcast_core/flashcards_core).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

namespace reinschrift {

constexpr int kTitleLen   = 160;
constexpr int kTopicLen   = 48;
constexpr int kContextLen = 40;
constexpr int kMarkerLen  = 16;
constexpr int kLineLen    = 320;

// Jahr ≥ 9999 = „irgendwann" (kein echtes Fälligkeitsdatum). Als Epoche-Sentinel.
constexpr uint32_t kSomeday = 0xFFFFFFFFu;

struct Task {
  bool     done;
  char     title[kTitleLen];
  char     topic[kTopicLen];      // ohne führendes '+'
  char     context[kContextLen];  // ohne führendes '@'
  uint32_t dueEpoch;              // UTC; 0 = kein due, kSomeday = „irgendwann"
  char     marker[kMarkerLen];    // ohne führendes '^'; "" = kein Marker
};

// --- kleine Helfer ------------------------------------------------------------
namespace detail {

// Tage seit 1970-01-01 für ein gregorianisches Datum (Howard Hinnant).
inline long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// "YYYY-MM-DD[THH:MM]" → UTC-Epoche. Jahr ≥ 9999 → kSomeday. 0 bei Unsinn.
inline uint32_t parseDue(const char* s) {
  int y = 0, mo = 0, d = 0, hh = 0, mi = 0;
  if (sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3) return 0;
  if (y >= 9999) return kSomeday;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  const char* t = strchr(s, 'T');
  if (t) sscanf(t + 1, "%d:%d", &hh, &mi);
  long days = daysFromCivil(y, (unsigned)mo, (unsigned)d);
  long e = days * 86400L + hh * 3600L + mi * 60L;
  if (e < 0) return 0;
  return (uint32_t)e;
}

// Wort an [w, we) ist ein Attribut-Token (markiert das Ende des Titels)?
inline bool isAttrWord(const char* w, const char* we) {
  size_t n = (size_t)(we - w);
  if (n == 0) return false;
  if (w[0] == '+' || w[0] == '@' || w[0] == '^') return true;
  if (w[0] == '[' && n >= 2 && w[1] == '[') return true;
  if (w[0] == '~' && n >= 5 && memcmp(w, "~note", 5) == 0) return true;
  if (n >= 4 && memcmp(w, "due:", 4) == 0) return true;
  if (n >= 6 && memcmp(w, "myday:", 6) == 0) return true;
  return false;
}

inline void copyTrim(const char* s, const char* e, char* out, size_t cap) {
  while (s < e && (*s == ' ' || *s == '\t')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
  size_t n = (size_t)(e - s);
  if (n > cap - 1) n = cap - 1;
  memcpy(out, s, n);
  out[n] = '\0';
}

}  // namespace detail

// Eine Markdown-Zeile parsen. true = es ist eine Aufgaben-Zeile ("- [ ] …").
// Themen-Header (### +Topic) und sonstige Zeilen geben false zurück.
inline bool parseLine(const char* raw, Task* out) {
  memset(out, 0, sizeof(*out));
  const char* p = raw;
  // bis zum Zeilenende (Tests dürfen mehrzeilige Strings übergeben).
  const char* eol = p;
  while (*eol && *eol != '\n' && *eol != '\r') eol++;

  while (p < eol && (*p == ' ' || *p == '\t')) p++;
  // "- [ ] " / "- [x] "
  if (eol - p < 5 || p[0] != '-' || p[1] != ' ' || p[2] != '[') return false;
  char box = p[3];
  if (p[4] != ']') return false;
  out->done = (box == 'x' || box == 'X');
  p += 5;
  while (p < eol && *p == ' ') p++;
  const char* rest = p;

  // Titel = Text bis zum ersten Attribut-Wort.
  const char* titleEnd = eol;
  {
    const char* w = rest;
    while (w < eol) {
      while (w < eol && *w == ' ') w++;
      const char* we = w;
      while (we < eol && *we != ' ') we++;
      if (detail::isAttrWord(w, we)) { titleEnd = w; break; }
      w = we;
    }
  }
  detail::copyTrim(rest, titleEnd, out->title, sizeof(out->title));

  // Attribute aus dem ganzen Rest ziehen (Wort-weise; ~note:"…" mit Leerzeichen
  // stört die space-freien Tokens nicht).
  const char* w = rest;
  while (w < eol) {
    while (w < eol && *w == ' ') w++;
    const char* we = w;
    while (we < eol && *we != ' ') we++;
    size_t n = (size_t)(we - w);
    if (n >= 1 && w[0] == '+')      detail::copyTrim(w + 1, we, out->topic, sizeof(out->topic));
    else if (n >= 1 && w[0] == '@') detail::copyTrim(w + 1, we, out->context, sizeof(out->context));
    else if (n >= 1 && w[0] == '^') detail::copyTrim(w + 1, we, out->marker, sizeof(out->marker));
    else if (n >= 4 && memcmp(w, "due:", 4) == 0) {
      char buf[24]; detail::copyTrim(w + 4, we, buf, sizeof(buf));
      out->dueEpoch = detail::parseDue(buf);
    }
    w = we;
  }
  return true;
}

// Themen-Header "### +Topic" erkennen; topic ohne '+' nach out. false = keiner.
inline bool parseTopicHeader(const char* raw, char* out, size_t cap) {
  out[0] = '\0';
  const char* p = raw;
  while (*p == '#') p++;
  if (p == raw) return false;            // muss mit '#' beginnen
  while (*p == ' ') p++;
  if (*p != '+') return false;
  p++;
  const char* e = p;
  while (*e && *e != '\n' && *e != '\r') e++;
  detail::copyTrim(p, e, out, cap);
  return out[0] != '\0';
}

// --- Fälligkeits-Helfer (Logik aus ReinschriftCli.js) -------------------------
inline bool isSomeday(uint32_t due)            { return due == kSomeday; }
inline bool isOverdue(uint32_t due, uint32_t now) {
  return due != 0 && due != kSomeday && (due / 86400u) < (now / 86400u);
}
inline bool isDueToday(uint32_t due, uint32_t now) {
  return due != 0 && due != kSomeday && (due / 86400u) <= (now / 86400u);
}

// --- Marker-Generierung -------------------------------------------------------
// 8-stellige Base62-ID (wie die Reinschrift-Marker, ^xxxxxxxx). rng(n) liefert
// 0..n-1 (im Test deterministisch, am Gerät esp_random()).
template <typename Rng>
inline void genMarker(char* out, size_t cap, Rng rng) {
  static const char* a = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  size_t want = 8;
  if (want > cap - 1) want = cap - 1;
  for (size_t i = 0; i < want; i++) out[i] = a[rng(62)];
  out[want] = '\0';
}

// =============================================================================
// Konfliktsicherer Merge
// =============================================================================
enum OpKind : uint8_t { OP_TOGGLE = 0, OP_DUE = 1, OP_ADD = 2 };

struct Op {
  uint8_t kind;
  char    marker[kMarkerLen];   // Ziel (TOGGLE/DUE); ADD: Marker der neuen Zeile
  bool    done;                 // TOGGLE: gewünschter Zustand
  char    value[24];            // DUE: neuer due-Wert, z. B. "2026-06-21T12:00"
  char    topic[kTopicLen];     // ADD: Thema (ohne '+'), unter dem eingefügt wird
  char    line[kLineLen];       // ADD: fertige Markdown-Zeile inkl. ^marker
};

namespace detail {

// ^marker einer Zeile [s, e) lesen (ohne '^'). "" = keiner.
inline void lineMarker(const char* s, const char* e, char* out, size_t cap) {
  out[0] = '\0';
  const char* w = s;
  while (w < e) {
    while (w < e && *w == ' ') w++;
    const char* we = w;
    while (we < e && *we != ' ') we++;
    if (we > w && w[0] == '^') { copyTrim(w + 1, we, out, cap); return; }
    w = we;
  }
}

// Checkbox in [s,e) auf gewünschten Zustand setzen → nach out (mit '\0').
inline void applyToggle(const char* s, const char* e, bool done, char* out, size_t cap) {
  size_t o = 0;
  bool patched = false;
  for (const char* p = s; p < e && o + 1 < cap; ) {
    if (!patched && p + 3 <= e && p[0] == '[' &&
        (p[1] == ' ' || p[1] == 'x' || p[1] == 'X') && p[2] == ']') {
      out[o++] = '[';
      if (o + 1 < cap) out[o++] = done ? 'x' : ' ';
      if (o + 1 < cap) out[o++] = ']';
      p += 3;
      patched = true;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = '\0';
}

// due:-Token in [s,e) auf value setzen (oder anhängen) → nach out.
inline void applyDue(const char* s, const char* e, const char* value, char* out, size_t cap) {
  size_t o = 0;
  bool replaced = false;
  const char* w = s;
  while (w < e) {
    const char* ws = w;
    while (w < e && *w == ' ') w++;          // führende Spaces mitnehmen
    const char* tok = w;
    while (w < e && *w != ' ') w++;          // Tokenende
    bool isDue = (w - tok >= 4) && memcmp(tok, "due:", 4) == 0;
    const char* emitS = ws;
    const char* emitE = w;
    if (isDue) {
      // Spaces vor dem Token + "due:" + neuer Wert
      for (const char* q = ws; q < tok && o + 1 < cap; q++) out[o++] = *q;
      const char* repl = "due:";
      for (int i = 0; repl[i] && o + 1 < cap; i++) out[o++] = repl[i];
      for (int i = 0; value[i] && o + 1 < cap; i++) out[o++] = value[i];
      replaced = true;
      continue;
    }
    for (const char* q = emitS; q < emitE && o + 1 < cap; q++) out[o++] = *q;
  }
  if (!replaced) {
    // Vor dem ^marker einfügen wäre schöner; ans Zeilenende reicht (Parser
    // liest Tokens positionsunabhängig).
    if (o + 1 < cap) out[o++] = ' ';
    const char* repl = "due:";
    for (int i = 0; repl[i] && o + 1 < cap; i++) out[o++] = repl[i];
    for (int i = 0; value[i] && o + 1 < cap; i++) out[o++] = value[i];
  }
  out[o] = '\0';
}

}  // namespace detail

// Frischen Remote-Text + Op-Queue zu mergen. Schreibt das Ergebnis nach out.
// Rückgabe: true wenn alles in cap passte. Nicht passende Ops werden still
// verworfen (Marker remote nicht (mehr) vorhanden = anderswo gelöscht).
inline bool applyOps(const char* in, const Op* ops, int nOps, char* out, size_t cap) {
  bool used[64] = {false};
  if (nOps > 64) nOps = 64;
  size_t o = 0;
  auto emit = [&](const char* s, size_t n) {
    for (size_t i = 0; i < n && o + 1 < cap; i++) out[o++] = s[i];
  };
  auto emitStr = [&](const char* s) { emit(s, strlen(s)); };

  const char* p = in;
  bool lastHadNL = true;
  while (*p) {
    const char* e = p;
    while (*e && *e != '\n') e++;
    bool hasNL = (*e == '\n');

    // Themen-Header?
    char topic[kTopicLen];
    if (parseTopicHeader(p, topic, sizeof(topic))) {
      emit(p, (size_t)(e - p));
      if (hasNL && o + 1 < cap) out[o++] = '\n';
      // ADD-Ops dieses Themas direkt hinter den Header.
      for (int i = 0; i < nOps; i++) {
        if (used[i] || ops[i].kind != OP_ADD) continue;
        if (strcmp(ops[i].topic, topic) != 0) continue;
        emitStr(ops[i].line);
        if (o + 1 < cap) out[o++] = '\n';
        used[i] = true;
      }
      p = hasNL ? e + 1 : e;
      lastHadNL = hasNL;
      continue;
    }

    // Aufgaben-Zeile mit passendem Marker?
    char mk[kMarkerLen];
    detail::lineMarker(p, e, mk, sizeof(mk));
    bool transformed = false;
    if (mk[0]) {
      for (int i = 0; i < nOps; i++) {
        if (used[i]) continue;
        if (ops[i].kind == OP_ADD) continue;
        if (strcmp(ops[i].marker, mk) != 0) continue;
        char buf[kLineLen];
        if (ops[i].kind == OP_TOGGLE)
          detail::applyToggle(p, e, ops[i].done, buf, sizeof(buf));
        else  // OP_DUE
          detail::applyDue(p, e, ops[i].value, buf, sizeof(buf));
        emitStr(buf);
        used[i] = true;
        transformed = true;
        // weitere Ops auf denselben Marker (z. B. Toggle + Due) anwenden:
        // erneut über buf laufen lassen wäre nötig — selten; wir lassen je
        // Sync max. eine Op pro Marker zu (Queue dedupliziert beim Einreihen).
        break;
      }
    }
    if (!transformed) emit(p, (size_t)(e - p));
    if (hasNL && o + 1 < cap) out[o++] = '\n';
    p = hasNL ? e + 1 : e;
    lastHadNL = hasNL;
  }

  // ADD-Ops ohne passendes Thema → neue Header am Dateiende.
  for (int i = 0; i < nOps; i++) {
    if (used[i] || ops[i].kind != OP_ADD) continue;
    if (!lastHadNL && o + 1 < cap) out[o++] = '\n';
    emitStr("\n### +");
    emitStr(ops[i].topic[0] ? ops[i].topic : "Inbox");
    if (o + 1 < cap) out[o++] = '\n';
    emitStr(ops[i].line);
    if (o + 1 < cap) out[o++] = '\n';
    used[i] = true;
    lastHadNL = true;
  }

  out[o] = '\0';
  return o + 1 < cap;
}

// Eine Roh-Markdown-Zeile für eine neue Aufgabe bauen (für OP_ADD.line).
// title darf bereits +Topic/@Context/due: enthalten (Quick-Add tippt frei);
// fehlt ein ^marker, wird marker angehängt. topic wird NUR für die Einsortierung
// (Op.topic) gebraucht, nicht hier dupliziert.
inline void buildAddLine(const char* title, const char* marker, char* out, size_t cap) {
  snprintf(out, cap, "- [ ] %s ^%s", title, marker);
}

}  // namespace reinschrift
