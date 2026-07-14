// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// calibre_books.h — E-Book-Pull-Sync von Calibre-Web (OPDS).
//
// Fennek holt sich Bücher selbst (Pull statt Calibre-Plugin): das konfigurierte
// Bücherregal (Shelf, Default "Fennek") wird über /opds/shelfindex aufgelöst,
// der Regal-Feed liefert pro Buch den /opds/download/<id>/epub/-Link, neue IDs
// werden nach /books geladen. Nur-hinzufügend — Löschen bleibt manuell
// (webfm/Dateien-App), Lesestände sind nie in Gefahr. Bereits geladene IDs
// stehen im Manifest /.fennek/calibre.tsv (ID<TAB>Datei).
//
// Zielserver ist Calibre-Web (calibre.dumke.me auf cassius), NICHT der native
// Calibre Content Server (der hätte /ajax + /get). OPDS in Calibre-Web nutzt
// immer HTTP-Basic-Auth — kein Digest-Problem.
//
// WLAN ⊥ Audio (CLAUDE.md): Sync stoppt Audio + suspendiert Mesh. Auto-Sync
// vor dem Auto-Standby höchstens alle 6 h (Podcast-Lektion v2.5.9: nicht bei
// jedem Standby das WLAN hochfahren) + RTC-RAM-Back-off nach Fehlern.
// =============================================================================
#pragma once

#include <stddef.h>

namespace calibre_books {

// Blockierender Sync (verwaltet WLAN selbst). log = Statuszeile.
bool sync(char* log, size_t logN);

// Vor dem Auto-Standby (Mindestintervall + Back-off; nur wenn cbauto aktiv).
bool flushBeforeStandby();

// Anzahl Einträge im Manifest (bereits gesyncte Bücher).
int syncedCount();

}  // namespace calibre_books
