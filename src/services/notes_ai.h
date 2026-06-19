// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// notes_ai.h — Notizen automatisch "schoenschreiben" via Ollama (lokales LLM).
//
// Hintergrund (CLAUDE.md): WLAN und Audio schliessen sich auf dieser Hardware
// aus (WiFi-Task auf Core 0 verdraengt den Audio-Task, beide teilen den SPI-
// Bus). Es gibt also kein dauerhaftes WLAN — das etablierte "WLAN ist frei"-
// Fenster ist der Pre-Standby-Lauf (analog scrobble::flushBeforeStandby() und
// timesync::syncBeforeStandby()).
//
// Ablauf: vor dem Auto-Standby werden alle Tagesnotizen (/notes/*.md) gesucht,
// die seit dem letzten Polieren geaendert wurden (Hash-Vergleich gegen
// /.fennek/notes_ai.state). Die HEUTIGE Notiz wird ausgelassen (der gerade
// wachsende Tagestext soll nicht mitten am Tag umformuliert werden). Jede
// Notiz geht per HTTP-POST an Ollamas /api/generate (stream:false); die
// Antwort ersetzt die Notiz direkt auf der SD. Danach wird der neue Hash
// gespeichert, damit unveraenderte Notizen nicht erneut durchlaufen.
//
// SD-Disziplin wie scrobble: Lesen/Schreiben gechunkt unter spiLock, der HTTP-
// Verkehr ausschliesslich NACH spiUnlock (Netz ⊥ SPI-Bus). Laeuft nur aus dem
// Arduino-loop()/der Konsole (Core 1) — kein Audio-Task, daher kein Mutex.
// =============================================================================
#pragma once

#include <stddef.h>

namespace notes_ai {

// Anzahl Notizen, die seit dem letzten Polieren geaendert wurden (ohne die
// heutige). Scannt die SD — nur fuer Status/Konsole gedacht.
int pendingCount();

// Blockierender Lauf vor dem Auto-Standby (verwaltet WLAN selbst, mit RTC-RAM-
// Back-off nach Fehlschlaegen). No-op ohne offene Notizen bzw. ohne
// Konfiguration. true = nichts zu tun oder >=1 Notiz poliert.
bool flushBeforeStandby();

// Wie flushBeforeStandby(), aber ohne Back-off — fuer die Konsole ("ollama
// flush") zum sofortigen Test. logN bekommt eine kurze Statuszeile.
bool flushNow(char* log, size_t logN);

// Verbindungstest (Konsole "ollama test"): WLAN auf, GET /api/tags, wieder aus.
// msg erhaelt einen Status-Text.
bool ping(char* msg, size_t msgN);

}  // namespace notes_ai
