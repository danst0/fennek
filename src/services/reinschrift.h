// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// reinschrift.h — Todo-Sync mit der Reinschrift-Markdown-Datenbank über
// Nextcloud/WebDAV (Desktop-/Web-Pendant: ../TodosExtension).
//
// Hintergrund (CLAUDE.md): WLAN und Audio schließen sich auf dieser Hardware
// aus. Sync läuft daher NICHT permanent, sondern auf Knopfdruck (App/Konsole)
// und opportunistisch vor dem Auto-Standby — wie scrobble/notes_ai/podcast.
//
// KONFLIKTAUFLÖSUNG ist der Kern (mehrere Geräte editieren dieselbe Datei).
// Wir überschreiben die Remote-Datei NIE blind:
//   1. GET frische Datei + ETag.
//   2. reinschrift_core::applyOps() legt NUR unsere lokalen Änderungen (eine
//      über ^marker adressierte Op-Queue) auf den frischen Stand.
//   3. PUT mit If-Match:<etag>. 412 (jemand war schneller) → einmal GET+Merge
//      wiederholen. So gehen fremde Edits an anderen Aufgaben nie verloren.
// Die Op-Queue liegt persistent auf SD (/.fennek/todo_ops.tsv) und übersteht
// den Deep Sleep.
//
// Die App liest die geparste Aufgabenliste (count()/get()) und löst Mutationen
// aus (toggle/setDueRelative/add) — die wirken sofort lokal und reihen eine Op
// ein; der nächste sync() schiebt sie hoch.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "apps/reinschrift_core.h"

namespace reinschrift_svc {

// Cache (Aufgabenliste + ETag) und Op-Queue von SD laden, Liste aufbauen.
void begin();

// Anzahl / Zugriff auf die aktuell bekannten Aufgaben (inkl. lokal anstehender
// Änderungen, damit die App sie sofort sieht).
int  count();
bool get(int idx, reinschrift::Task* out);

// Mutationen (idx bezieht sich auf die get()-Reihenfolge). Reihen eine Op ein,
// aktualisieren die lokale Sicht und persistieren die Queue.
void toggle(int idx);                       // erledigt umschalten
void setDueRelative(int idx, int dayOffset);// 0 = heute, 1 = morgen (12:00 lokal)
bool add(const char* title, const char* topic);  // neue Aufgabe (Quick-Add)

// Anzahl noch nicht hochgeladener Änderungen.
int pendingCount();

// Blockierender Sync (verwaltet WLAN selbst). log bekommt eine Statuszeile.
// useBackoff nur für den Pre-Standby-Pfad.
bool sync(char* log, size_t logN);

// Vor dem Auto-Standby (mit RTC-RAM-Back-off). true = nichts zu tun / Erfolg.
bool flushBeforeStandby();

}  // namespace reinschrift_svc
