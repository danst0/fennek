// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// ical_core.h — Arduino-freier iCalendar-Parser (RFC-5545-Subset) für die
// Kalender-App.
//
// Read-only: wir abonnieren eine/mehrere .ics-Feeds (Nextcloud-Share / webcal)
// und zeigen anstehende Termine. „Sparse": der Service streamt die (oft große)
// Datei zeilenweise herein und behält über den onEvent-Callback nur Termine im
// Vorwärtsfenster — der Parser hält immer nur EIN Event im RAM.
//
// Unterstützt: Line-Unfolding (Fortsetzung mit führendem Space/Tab), VEVENT mit
// UID/SUMMARY/LOCATION/DTSTART/DTEND (Datum + Datum-Zeit, all-day VALUE=DATE)
// und eine einfache RRULE-Expansion (FREQ=DAILY/WEEKLY/MONTHLY mit INTERVAL/
// COUNT/UNTIL) — bewusst innerhalb des Fensters (expand()).
//
// Vereinfachung: Zeitzonen (TZID) werden ignoriert, Zeiten als UTC gelesen
// (für eine Agenda-Übersicht ausreichend; Z-suffixierte Zeiten sind ohnehin UTC).
//
// Header-only, host-testbar (tools/host_test_apps.cpp).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

namespace ical {

constexpr int kSummaryLen = 96;
constexpr int kLocLen     = 64;
constexpr int kUidLen     = 64;
constexpr int kRRuleLen   = 96;
constexpr int kLogicalLen = 256;   // entfaltete logische Zeile

struct Event {
  char     uid[kUidLen];
  char     summary[kSummaryLen];
  char     location[kLocLen];
  uint32_t start;        // UTC-Epoche
  uint32_t end;          // UTC-Epoche (0 = unbekannt)
  bool     allDay;
  char     rrule[kRRuleLen];   // "" = einmalig
  bool     valid;        // DTSTART + SUMMARY vorhanden?
};

namespace detail {

inline long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// Epoche → (Jahr, Monat, Tag) (UTC).
inline void civilFromEpoch(uint32_t epoch, int* y, unsigned* m, unsigned* d) {
  long z = (long)(epoch / 86400) + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (*m <= 2));
}

// "YYYYMMDD" oder "YYYYMMDDTHHMMSS[Z]" → UTC-Epoche. *isDate = reines Datum.
inline uint32_t parseDateTime(const char* s, bool* isDate) {
  int y = 0, mo = 0, d = 0, hh = 0, mi = 0, se = 0;
  if (strlen(s) < 8) { *isDate = true; return 0; }
  char ybuf[5] = {s[0], s[1], s[2], s[3], 0};
  char mbuf[3] = {s[4], s[5], 0};
  char dbuf[3] = {s[6], s[7], 0};
  y = atoi(ybuf); mo = atoi(mbuf); d = atoi(dbuf);
  *isDate = (s[8] != 'T');
  if (!*isDate) {
    const char* t = s + 9;
    if (strlen(t) >= 6) {
      char hb[3] = {t[0], t[1], 0}, mib[3] = {t[2], t[3], 0}, sb[3] = {t[4], t[5], 0};
      hh = atoi(hb); mi = atoi(mib); se = atoi(sb);
    }
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  long e = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400L + hh * 3600L + mi * 60L + se;
  return e < 0 ? 0 : (uint32_t)e;
}

// Property-Wert nach ':' (überspringt ;PARAM=…). Gibt Zeiger auf Wert; *name
// bekommt den Namensteil ohne Parameter (z. B. "DTSTART").
inline const char* propValue(const char* line, char* name, size_t nameCap) {
  const char* colon = line;
  const char* semi = nullptr;
  while (*colon && *colon != ':') { if (*colon == ';' && !semi) semi = colon; colon++; }
  const char* nameEnd = semi ? semi : colon;
  size_t nl = (size_t)(nameEnd - line);
  if (nl > nameCap - 1) nl = nameCap - 1;
  memcpy(name, line, nl); name[nl] = '\0';
  return (*colon == ':') ? colon + 1 : colon;
}

// iCal-TEXT entescapen (\n \, \; \\) → out.
inline void unescapeText(const char* s, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = s; *p && *p != '\r' && *p != '\n' && o + 1 < cap; p++) {
    if (*p == '\\' && p[1]) {
      char c = p[1];
      if (c == 'n' || c == 'N') out[o++] = ' ';
      else out[o++] = c;
      p++;
    } else out[o++] = *p;
  }
  out[o] = '\0';
}

}  // namespace detail

// Inkrementeller Parser: rohe (physische) Zeilen füttern; bei jedem komplett
// gelesenen VEVENT liefert feedLine()/finish() true und füllt *out.
class Parser {
 public:
  Parser() { reset(); }
  void reset() {
    s_logical[0] = '\0'; s_have = false; s_inEvent = false;
    memset(&s_ev, 0, sizeof(s_ev));
  }

  // Eine physische Zeile (ohne Zeilenende) übergeben. true = Event fertig.
  bool feedLine(const char* line, Event* out) {
    // Fortsetzungszeile (Folding): führendes Space/Tab → an logische anhängen.
    if (line[0] == ' ' || line[0] == '\t') {
      append(line + 1);
      return false;
    }
    bool done = flush(out);          // bisher gepufferte logische Zeile abschließen
    // neue logische Zeile beginnen
    strncpy(s_logical, line, sizeof(s_logical) - 1);
    s_logical[sizeof(s_logical) - 1] = '\0';
    s_have = true;
    return done;
  }

  // Letzte gepufferte Zeile am Stream-Ende abschließen.
  bool finish(Event* out) { return flush(out); }

 private:
  char  s_logical[kLogicalLen];
  bool  s_have;
  bool  s_inEvent;
  Event s_ev;

  void append(const char* s) {
    size_t l = strlen(s_logical);
    strncpy(s_logical + l, s, sizeof(s_logical) - 1 - l);
    s_logical[sizeof(s_logical) - 1] = '\0';
  }

  // Gepufferte logische Zeile auswerten. true = ein Event wurde abgeschlossen.
  bool flush(Event* out) {
    if (!s_have) return false;
    s_have = false;
    const char* line = s_logical;

    if (strncmp(line, "BEGIN:VEVENT", 12) == 0) {
      s_inEvent = true;
      memset(&s_ev, 0, sizeof(s_ev));
      return false;
    }
    if (strncmp(line, "END:VEVENT", 10) == 0) {
      s_inEvent = false;
      s_ev.valid = (s_ev.start != 0 || s_ev.allDay) && s_ev.summary[0];
      if (s_ev.valid) { *out = s_ev; return true; }
      return false;
    }
    if (!s_inEvent) return false;

    char name[32];
    const char* val = detail::propValue(line, name, sizeof(name));
    if (strcmp(name, "SUMMARY") == 0)        detail::unescapeText(val, s_ev.summary, sizeof(s_ev.summary));
    else if (strcmp(name, "LOCATION") == 0)  detail::unescapeText(val, s_ev.location, sizeof(s_ev.location));
    else if (strcmp(name, "UID") == 0)       detail::unescapeText(val, s_ev.uid, sizeof(s_ev.uid));
    else if (strcmp(name, "DTSTART") == 0) {
      bool isDate; s_ev.start = detail::parseDateTime(val, &isDate);
      s_ev.allDay = isDate;
    } else if (strcmp(name, "DTEND") == 0) {
      bool isDate; s_ev.end = detail::parseDateTime(val, &isDate);
    } else if (strcmp(name, "RRULE") == 0) {
      strncpy(s_ev.rrule, val, sizeof(s_ev.rrule) - 1);
      s_ev.rrule[sizeof(s_ev.rrule) - 1] = '\0';
      // evtl. \r am Ende kappen
      for (char* q = s_ev.rrule; *q; q++) if (*q == '\r') { *q = '\0'; break; }
    }
    return false;
  }
};

// RRULE-Feld auslesen (z. B. "FREQ", "INTERVAL"). false = nicht vorhanden.
inline bool rruleField(const char* rr, const char* key, char* out, size_t cap) {
  out[0] = '\0';
  size_t kl = strlen(key);
  for (const char* p = rr; *p; ) {
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
      const char* v = p + kl + 1;
      size_t o = 0;
      while (*v && *v != ';' && o + 1 < cap) out[o++] = *v++;
      out[o] = '\0';
      return true;
    }
    while (*p && *p != ';') p++;
    if (*p == ';') p++;
  }
  return false;
}

// Vorkommen eines (ggf. wiederkehrenden) Events im Fenster [winStart, winEnd)
// aufzählen. cb(occStart, occEnd) je Vorkommen. Einmalige Events: max. eines.
template <typename CB>
inline void expand(const Event& ev, uint32_t winStart, uint32_t winEnd, CB cb) {
  uint32_t dur = (ev.end > ev.start) ? (ev.end - ev.start) : (ev.allDay ? 86400u : 0u);
  auto overlaps = [&](uint32_t s) {
    // Zeitpunkt-Events (Dauer 0): zählen, wenn der Start im Fenster liegt.
    if (dur == 0) return s >= winStart && s < winEnd;
    return (s + dur) > winStart && s < winEnd;
  };

  if (!ev.rrule[0]) {
    if (overlaps(ev.start)) cb(ev.start, ev.start + dur);
    return;
  }

  char freq[12], buf[16];
  if (!rruleField(ev.rrule, "FREQ", freq, sizeof(freq))) {
    if (overlaps(ev.start)) cb(ev.start, ev.start + dur);
    return;
  }
  int interval = 1;
  if (rruleField(ev.rrule, "INTERVAL", buf, sizeof(buf))) { interval = atoi(buf); if (interval < 1) interval = 1; }
  long count = 100000;   // praktisch unbegrenzt
  if (rruleField(ev.rrule, "COUNT", buf, sizeof(buf))) count = atol(buf);
  uint32_t until = 0;
  if (rruleField(ev.rrule, "UNTIL", buf, sizeof(buf))) { bool d; until = detail::parseDateTime(buf, &d); }

  bool monthly = (strcmp(freq, "MONTHLY") == 0);
  uint32_t stepSecs = 0;
  if (strcmp(freq, "DAILY") == 0)       stepSecs = (uint32_t)interval * 86400u;
  else if (strcmp(freq, "WEEKLY") == 0) stepSecs = (uint32_t)interval * 7u * 86400u;
  else if (!monthly) { if (overlaps(ev.start)) cb(ev.start, ev.start + dur); return; }

  uint32_t s = ev.start;
  for (long i = 0; i < count && i < 4096; i++) {
    if (s >= winEnd) break;
    if (until && s > until) break;
    if (overlaps(s)) cb(s, s + dur);
    if (monthly) {
      int y; unsigned m, d;
      detail::civilFromEpoch(s, &y, &m, &d);
      uint32_t tod = s % 86400u;
      m += (unsigned)interval;
      while (m > 12) { m -= 12; y++; }
      s = (uint32_t)(detail::daysFromCivil(y, m, d) * 86400L) + tod;
    } else {
      s += stepSecs;
    }
  }
}

}  // namespace ical
