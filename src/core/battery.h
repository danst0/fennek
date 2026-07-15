// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// battery.h — BQ27220-Fuel-Gauge (I2C 0x55), Lesezugriffe.
//
// Port aus archive_legacy/variants/lilygo_tdeck_pro/TDeckBoard.cpp. Wir lesen
// nur (Spannung/SoC/Strom). Die BQ27220 ist `sealed`; ihr Coulomb-Counter (SoC =
// RemainingCapacity / FullChargeCapacity) resynct sich nur an einer sauberen,
// ununterbrochenen Vollladung. Bricht das Laden ständig ab (z.B. Reboots beim
// Flashen), driftet FullChargeCapacity tief und die SoC-Anzeige bleibt sichtbar
// hängen (Symptom „immer 72%"). Deshalb liefert percent() den Ladestand
// SPANNUNGSBASIERT (LiPo-Entladekurve, s. percentFromMilliVolts in battery.cpp) —
// die Klemmenspannung folgt dem echten Ladestand, der Gauge-SoC nicht. Das rohe
// SoC-Register bleibt nur in der Diagnose `gauge` (dumpGauge()) sichtbar.
// =============================================================================
#pragma once

#include <stdint.h>

namespace battery {

// Prüft, ob der Gauge antwortet (Wire muss laufen).
bool begin();

uint16_t milliVolts();   // 0 bei I2C-Fehler
uint8_t  percent();      // State of Charge 0..100
bool     charging();     // mittlerer Strom > 0 (lädt aktiv)
bool     external();     // am Strom (lädt ODER voll am Kabel); DSG-Bit aus BatteryStatus

// RemainingCapacity (mAh) — der rohe Coulomb-Counter. Der ABSOLUTWERT ist auf
// diesem Gerät unbrauchbar (FCC driftet, s.o.), die DIFFERENZ zweier Messungen
// ist dagegen eine echte Ladungsintegration: (mAh_alt - mAh_neu) * 3600 / Sekunden
// = mittlerer Strom in mA. Läuft im Deep Sleep weiter (der Gauge hängt direkt an
// der Zelle) und ist damit das einzige Instrument, das den Standby-Verbrauch
// misst, statt ihn aus der Spannungskurve zu raten. 0 bei I2C-Fehler.
// Vorsicht: der Gauge korrigiert RM gelegentlich sprunghaft (Impedance Track) —
// deshalb werden die Rohwerte mitgeloggt, damit ein Sprung sichtbar bleibt.
uint16_t remainingCapacity();

// Read-only-Diagnose: dumpt Kapazitäts-/Statusregister des Gauge nach Serial
// (Konsole `gauge`). Klärt, warum SoC nicht zur Spannung passt. Schreibt nichts.
void     dumpGauge();

}  // namespace battery
