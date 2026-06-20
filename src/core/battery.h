// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// battery.h — BQ27220-Fuel-Gauge (I2C 0x55), Lesezugriffe.
//
// Port aus archive_legacy/variants/lilygo_tdeck_pro/TDeckBoard.cpp. Wir lesen
// nur (Spannung/SoC/Strom). Die Kalibrierung (DesignCapacity 1400 mAh) steckt im
// batteriegepufferten RAM des Gauges und stammt aus der LilyGO-Werksfirmware —
// in unserer Git-Historie gibt es keinen eigenen Kalibriercode. Die BQ27220 ist
// `sealed`; SoC = RemainingCapacity / FullChargeCapacity und resynct sich nur an
// einer sauberen, ununterbrochenen Vollladung (qualifiziertes Lade-Ende). Bricht
// das Laden ständig ab (z.B. Reboots beim Flashen), driftet die SoC-Anzeige tief.
// Diagnose read-only über die Konsole `gauge` (dumpGauge()).
// =============================================================================
#pragma once

#include <stdint.h>

namespace battery {

// Prüft, ob der Gauge antwortet (Wire muss laufen).
bool begin();

uint16_t milliVolts();   // 0 bei I2C-Fehler
uint8_t  percent();      // State of Charge 0..100
bool     charging();     // mittlerer Strom > 0 (lädt)

// Read-only-Diagnose: dumpt Kapazitäts-/Statusregister des Gauge nach Serial
// (Konsole `gauge`). Klärt, warum SoC nicht zur Spannung passt. Schreibt nichts.
void     dumpGauge();

}  // namespace battery
