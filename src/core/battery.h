// =============================================================================
// battery.h — BQ27220-Fuel-Gauge (I2C 0x55), Lesezugriffe.
//
// Port aus archive_legacy/variants/lilygo_tdeck_pro/TDeckBoard.cpp. Die
// Design-Capacity-Kalibrierung aus dem Archiv wird NICHT wiederholt — sie
// persistiert im batteriegepufferten RAM des Gauges und wurde von der
// Legacy-Firmware auf diesem Gerät bereits geschrieben.
// =============================================================================
#pragma once

#include <stdint.h>

namespace battery {

// Prüft, ob der Gauge antwortet (Wire muss laufen).
bool begin();

uint16_t milliVolts();   // 0 bei I2C-Fehler
uint8_t  percent();      // State of Charge 0..100
bool     charging();     // mittlerer Strom > 0 (lädt)

}  // namespace battery
