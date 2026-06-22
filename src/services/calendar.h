// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// calendar.h — iCal-Kalender (read-only) für die Kalender-App.
//
// Abonniert eine/mehrere .ics-Feeds (Nextcloud-Share / webcal), Liste in
// /calendar/feeds.txt (eine URL/Zeile, optional URL<TAB>Name, '#' = Kommentar).
//
// „Sparse Sync": (a) bedingter GET je Feed (If-None-Match → 304 spart den
// Download), (b) der Parser streamt zeilenweise und behält über das
// Vorwärtsfenster nur die nahen Termine — der RAM bleibt klein. Je Feed ein
// kompakter Cache /calendar/<slug>.bin; die Anzeigeliste entsteht durch Mischen
// + Sortieren aller Feed-Caches (übersteht Deep Sleep, Boot ohne WLAN).
//
// WLAN ⊥ Audio (CLAUDE.md): Sync stoppt Audio + suspendiert Mesh. Ausgelöst per
// App-Button/Konsole und opportunistisch vor dem Auto-Standby.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace calendar {

constexpr int kSummaryLen = 80;
constexpr int kLocLen     = 40;

struct CalEvent {
  uint32_t start;   // UTC-Epoche
  uint32_t end;     // UTC-Epoche (0 = unbekannt)
  uint8_t  allDay;
  char     summary[kSummaryLen];
  char     location[kLocLen];
};

// Cache (Feed-Liste + gemischte Termine) von SD laden.
void begin();

// Anstehende Termine (nach Startzeit sortiert).
int  count();
bool event(int idx, CalEvent* out);

// Feed-Verwaltung.
int  feedCount();
bool feedUrl(int idx, char* out, size_t n);
bool addFeed(const char* url);
bool removeFeed(int idx);

// Blockierender Sync aller Feeds (verwaltet WLAN selbst). log = Statuszeile.
bool sync(char* log, size_t logN);

// Vor dem Auto-Standby (RTC-RAM-Back-off; nur wenn Auto-Sync aktiv).
bool flushBeforeStandby();

}  // namespace calendar
