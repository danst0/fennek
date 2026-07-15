// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// battlog.h — Debug-Akku-/Aktivitäts-Logger (NUR mit -D BATTLOG).
//
// Zweck: den Batterieverbrauch verstehen. Schreibt eine Zeile pro Ereignis und
// alle 60 s eine periodische Akku-Probe — jeweils mit Zeitstempel, Akkustand
// (%, mV, mAh, Strom-Quelle) und dem Systemkontext (WLAN an/aus, Mesh-Chip
// an/aus) sowie einer Kategorie + Notiz (App-Wechsel, Audio-Track, Standby/Wake,
// …). Der Ring liegt im PSRAM; geschrieben wird gedrosselt und spätestens kurz
// vor dem Standby nach /.fennek/battery.log (über WebFM herunterladbar).
//
// Auswertung — zwei Regeln, an denen sich schon eine Analyse verschluckt hat:
//  1. NUR Zeilen mit „-" in der Strom-Spalte sind echte Akku-Messungen. „Kabel"
//     heißt: hängt am USB, Akku voll, Lader terminiert — sieht wie traumhaft
//     sparsamer Betrieb aus und ist in Wahrheit gar keine Messung.
//  2. Für den Verbrauch die mAh-Spalte nehmen, nicht % oder mV: das ist der
//     Coulomb-Counter des Gauge (echte Ladungsintegration), während percent()
//     nur aus der Spannungskurve geschätzt ist. mA = Δ mAh * 3600 / Δ Sekunden.
//     Den Deep-Sleep-Strom liefern die „Schlaf"-Zeilen aus power::logSleepDebug().
//
// Bewusst als abschaltbares Debug-Feature gebaut: ohne das Compile-Flag BATTLOG
// kompilieren alle BATTLOG_*-Aufrufe zu Nichts (die .cpp ist komplett gegatet).
// Zum endgültigen Ausbau: Flag entfernen, dann diese zwei Dateien + die
// BATTLOG_*-Aufrufe an den Einhängepunkten löschen.
//
// Threading: event() ist aus JEDEM Task aufrufbar (auch dem Audio-Task) — es
// liest nur gecachte Akku-Werte + Flags + Zeit und schreibt unter Mutex in den
// Ring, kein I2C/SPI. poll()/flush() laufen NUR im Arduino-loop (Core 1) und
// machen das SD-I/O unter spiLock.
// =============================================================================
#pragma once

#ifdef BATTLOG
namespace battlog {

// PSRAM-Ring anlegen, erste Akku-Probe ziehen, Boot-Zeile schreiben.
void begin();

// Im Arduino-loop() aufrufen: alle 60 s Akku sampeln, gedrosselt nach SD.
void poll();

// Ereignis mit Zeitstempel + aktuellem Akku-/System-Snapshot vormerken.
// fmt/... wie printf (z. B. event("App", "%s", id)).
void event(const char* cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Ring sofort nach SD schreiben (vor dem Standby). reason landet als Zeile.
void flush(const char* reason);

}  // namespace battlog

#define BATTLOG_BEGIN()         battlog::begin()
#define BATTLOG_POLL()          battlog::poll()
#define BATTLOG_EVENT(cat, ...) battlog::event((cat), __VA_ARGS__)
#define BATTLOG_FLUSH(reason)   battlog::flush((reason))
#else
#define BATTLOG_BEGIN()         ((void)0)
#define BATTLOG_POLL()          ((void)0)
#define BATTLOG_EVENT(cat, ...) ((void)0)
#define BATTLOG_FLUSH(reason)   ((void)0)
#endif
