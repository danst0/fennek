// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// alarmclock.h — Wecker-Engine (Standortunabhängig vom UI; Konsole + App).
//
// Das Gerät hat keinen Hardware-RTC, aber die ESP32-Systemzeit übersteht den
// Deep Sleep (services/timesync). Der Wecker nutzt das: power::enterStandby()
// fragt nextDueEpoch() und stellt den Timer-Wake auf den nächsten Weckzeitpunkt
// (statt starr 1 h); power::handleTimerWake() erkennt den Wecker-Wake und fährt
// dann VOLL hoch (SD + Audio), damit poll() klingeln kann.
//
// Klingeln läuft über die normale Audio-Queue (Owner Music, Repeat One). Ohne
// gesetzten Klingelton-Pfad wird der erste Bibliothekstitel gespielt.
//
// Persistenz: eigenes NVS-Namespace "alarm" (interner Flash, nie SPI). Wecker
// sind feste Slots; dowMask = 0 bedeutet täglich, sonst Wochentage (bit0=Mo).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace alarmclock {

constexpr int kMaxAlarms = 4;

struct Alarm {
  bool    enabled;
  uint8_t hour;      // 0..23 (lokale Zeit)
  uint8_t minute;    // 0..59
  uint8_t dowMask;   // bit0=Mo .. bit6=So; 0 = täglich
};

void begin();        // lädt Wecker aus NVS
void poll();         // im loop(): Fälligkeit prüfen, klingeln, Auto-Quittung

int   count();                       // = kMaxAlarms (feste Slots)
Alarm get(int i);
void  set(int i, const Alarm& a);    // persistiert sofort
void  clear(int i);                  // Slot deaktivieren

// Nächster Auslöse-Zeitpunkt (UTC-Epoche) >= fromEpoch über alle aktiven Wecker
// (inkl. laufendem Schlummer); 0 wenn keiner aktiv. Für die Wake-Berechnung in
// power.cpp. Nutzt mktime/localtime → setzt eine gültige Zeitzone voraus.
uint32_t nextDueEpoch(uint32_t fromEpoch);

bool ringing();
void fireNow();      // sofort klingeln (Konsolentest „alarm test")
void snooze();       // schlummern (+kSnoozeMinutes), Klingeln stoppen
void dismiss();      // quittieren, Klingeln stoppen

void soundPath(char* out, size_t n);  // "" = erster Bibliothekstitel
void setSoundPath(const char* p);

}  // namespace alarmclock
