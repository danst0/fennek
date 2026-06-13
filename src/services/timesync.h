// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// timesync.h — Zeit-Koordinator (das Gerät hat keinen gepufferten Hardware-RTC).
//
// Kanonische Uhr = ESP32-Systemzeit (settimeofday/gettimeofday). Diese überlebt
// den Deep Sleep (der RTC-Zähler läuft im Schlaf weiter) und wird aus zwei
// Quellen frisch gehalten:
//   1. NTP über WLAN — opportunistisch, wenn WLAN (webfm) ohnehin läuft, und
//      einmalig kurz vor dem Auto-Standby, falls die geschätzte Qualität schlecht
//      ist und WLAN-Zugangsdaten gesetzt sind.
//   2. Mesh-Adverts — gemeldet von mesh_client über onExternalSync().
//
// Die Sync-Häufigkeit ist an die geschätzte Uhr-Qualität geknüpft (geschätzter
// Fehler = gelernte Oszillator-Drift × Zeit seit letztem Sync), nicht an einen
// festen Takt. NVS speichert die letzte Uhrzeit + gelernte Drift nur als
// Kaltstart-Fallback (Stromausfall/Reset/Reflash).
// =============================================================================
#pragma once

#include <stdint.h>

namespace timesync {

// Beim Boot (nach settings::begin()): Uhr aus NVS wiederherstellen, falls die
// Systemzeit noch im Kaltstart-Zustand ist; gelernte Drift laden.
void begin();

// Pro Loop-Durchlauf (neben webfm::poll()): opportunistische NTP-Sync, wenn WLAN
// läuft; gedrosselte NVS-Sicherung.
void poll();

// Blockierend, nur aus dem Auto-Standby-Pfad (power::poll), bevor das Gerät
// schlafen geht: bei schlechter Qualität + vorhandenen WLAN-Daten + ohne aktives
// webfm kurz selbst WLAN hochfahren, NTP holen, wieder aus. No-op sonst.
void syncBeforeStandby();

// Von mesh_client gemeldet, wenn ein Advert die Uhr gesetzt hat (Freshness).
void onExternalSync(uint32_t epoch, const char* src);

// Zeitzone (settings::tzString) auf die libc anwenden (setenv TZ + tzset), damit
// localtime_r() lokale Zeit liefert. In begin() und nach Settings-Änderung rufen.
void applyTimezone();

// Uhr manuell setzen (UTC-Epoche) — bestätigter Sync, markiert die Uhr als gut.
// Für die Settings-App / Konsole, wenn nie WLAN/Mesh erreichbar ist.
void setManualTime(uint32_t epochUtc);

// Sofort einen NTP-Sync erzwingen (blockierend, eigenes WLAN) — Konsole `time sync`.
// Ignoriert die Qualitäts-/Back-off-Logik. false = kein WLAN/keine Daten/Fehler.
bool forceSyncNow();

// --- Abfragen ----------------------------------------------------------------
uint32_t    now();              // aktuelle Uhrzeit (Unix-Epoche, UTC)
uint32_t    estErrSeconds();    // geschätzter Fehler in Sekunden
bool        isStale();          // estErrSeconds() über der Schwelle (~5 min)?
const char* source();           // "NTP" / "Mesh" / "NVS" / "—"
const char* qualityStr();       // "gut" / "mäßig" / "schlecht"
uint16_t    driftPpm();         // gelernte Oszillator-Drift

}  // namespace timesync
