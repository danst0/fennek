// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// scrobble.h — Navidrome/Subsonic-Scrobbling gespielter Musik-Tracks.
//
// Hintergrund (CLAUDE.md): WLAN und Audio schließen sich auf dieser Hardware
// aus (der WiFi-Task auf Core 0 verdrängt den Audio-Task, beide teilen den
// SPI-Bus). Echtzeit-Scrobbling während der Wiedergabe ist daher unmöglich.
//
// Lösung: Gespielte Tracks (>50 % oder >4 min, nur Owner::Music) landen in einer
// RAM-Queue (enqueue() aus dem Audio-Task — leichtgewichtig), die der loop()
// gedrosselt nach SD persistiert (übersteht Deep Sleep). Hochgeladen wird
// gesammelt VOR dem Auto-Standby (Gerät idle, Audio gestoppt, WLAN frei) —
// analog zu timesync::syncBeforeStandby().
//
// Subsonic braucht eine Server-Song-ID: pro Eintrag wird sie beim Upload per
// /rest/search3 (Titel+Artist) aufgelöst, dann /rest/scrobble gerufen. Setzt
// voraus, dass dieselbe Sammlung auch in Navidrome liegt.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace scrobble {

// Beim Boot offene Scrobbles von SD (/.fennek/scrobbles.tsv) laden.
void begin();

// Im Arduino-loop() aufrufen: persistiert die RAM-Queue gedrosselt nach SD.
void poll();

// Einen abgespielten Track vormerken. AUS DEM AUDIO-TASK aufrufbar: nur Mutex +
// memcpy, keine SD-/Netz-/Library-Zugriffe. Ignoriert wird der Aufruf, wenn
// Scrobbling deaktiviert ist (settings::navEnabled()).
void enqueue(const char* path, uint32_t startEpoch, uint16_t playedSec, uint16_t durSec);

// Anzahl noch nicht hochgeladener Scrobbles.
int pendingCount();

// Blockierender Upload vor dem Auto-Standby (verwaltet WLAN selbst, mit
// RTC-RAM-Back-off nach Fehlschlägen). true = nichts zu tun oder ≥1 hochgeladen.
bool flushBeforeStandby();

// Wie flushBeforeStandby(), aber ohne Back-off — für die Konsole ("scrobble
// flush") zum sofortigen Test. logN bekommt eine kurze Statuszeile.
bool flushNow(char* log, size_t logN);

// Subsonic-Ping (Konsole "nav test"). msg erhält einen Status-Text.
bool ping(char* msg, size_t msgN);

}  // namespace scrobble
