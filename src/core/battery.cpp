// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kAddr          = 0x55;
constexpr uint8_t REG_CONTROL    = 0x00;   // Control() — Subkommandos
constexpr uint8_t REG_VOLTAGE    = 0x08;
constexpr uint8_t REG_BATSTATUS  = 0x0A;   // BatteryStatus() — Flags
constexpr uint8_t REG_CURRENT    = 0x0C;   // mA, signed (Momentanstrom)
constexpr uint8_t REG_REMAINCAP  = 0x10;   // RemainingCapacity, mAh
constexpr uint8_t REG_FULLCAP    = 0x12;   // FullChargeCapacity, mAh
constexpr uint8_t REG_AVG_CURRENT= 0x14;   // mA, signed
constexpr uint8_t REG_SOC        = 0x2C;
constexpr uint8_t REG_SOH        = 0x2E;   // StateOfHealth (low byte = %)
constexpr uint8_t REG_DESIGNCAP  = 0x3C;   // DesignCapacity, mAh

bool s_ready = false;

uint16_t read16(uint8_t reg) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(kAddr, (uint8_t)2) != 2) return 0;
  uint16_t v = Wire.read();
  v |= (Wire.read() << 8);
  return v;
}

// LiPo-Klemmenspannung (mV) -> Ladestand (%). Wir vertrauen dem Coulomb-Counter
// (REG_SOC) auf diesem Gerät NICHT: die BQ27220 ist `sealed` und resynct ihren
// FullChargeCapacity nur an einer sauberen, ununterbrochenen Vollladung. Auf dem
// Dev-Gerät bricht das Laden staendig ab (Reboots beim Flashen), FCC driftet tief
// (gemessen 1272 statt 1400 mAh) -> SoC haengt sichtbar fest (Symptom „immer 72%"
// bei 4136 mV, obwohl die Spannung ~90% bedeutet). Die Spannung folgt dagegen dem
// echten Ladestand. Unter der leichten, stetigen Last des E-Ink-Handhelds (WLAN
// aus, ~100 mA) sackt die Klemmenspannung kaum unter die Ruhespannung; beim Laden
// liest sie etwas optimistisch — fuer eine grobe %-Anzeige akzeptabel und in jedem
// Fall besser als ein eingefrorener Zaehler. Stuetzstellen = typische Einzelzell-
// Entladekurve, linear interpoliert.
uint8_t percentFromMilliVolts(uint16_t mv) {
  static const struct { uint16_t mv; uint8_t pct; } kCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75},
    {3950, 70},  {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45},
    {3800, 40},  {3790, 35}, {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15},
    {3690, 10},  {3610, 5},  {3500, 0},
  };
  if (mv >= kCurve[0].mv) return 100;
  const uint8_t n = (uint8_t)(sizeof(kCurve) / sizeof(kCurve[0]));
  if (mv <= kCurve[n - 1].mv) return 0;
  for (uint8_t i = 1; i < n; ++i) {
    if (mv >= kCurve[i].mv) {
      // Zwischen kCurve[i] (niedriger) und kCurve[i-1] (hoeher) interpolieren.
      uint16_t loMv = kCurve[i].mv,     hiMv = kCurve[i - 1].mv;
      uint8_t  loPc = kCurve[i].pct,    hiPc = kCurve[i - 1].pct;
      return (uint8_t)(loPc + (uint32_t)(mv - loMv) * (hiPc - loPc) / (hiMv - loMv));
    }
  }
  return 0;
}

}  // namespace

namespace battery {

bool begin() {
  Wire.beginTransmission(kAddr);
  s_ready = (Wire.endTransmission() == 0);
  return s_ready;
}

uint16_t milliVolts() { return s_ready ? read16(REG_VOLTAGE) : 0; }

uint8_t percent() {
  if (!s_ready) return 0;
  // Spannungsbasiert statt REG_SOC (driftender Coulomb-Counter, s.
  // percentFromMilliVolts). milliVolts()==0 = I2C-Fehler -> 0.
  return percentFromMilliVolts(read16(REG_VOLTAGE));
}

bool charging() {
  if (!s_ready) return false;
  int16_t mA = (int16_t)read16(REG_AVG_CURRENT);
  return mA > 0;
}

bool external() {
  // BatteryStatus-Bit 0 (DSG) = 1, solange der Akku entlädt. Hängt das Gerät am
  // Strom — egal ob es noch lädt oder voll am Kabel ruht (Strom ~0) — ist DSG=0.
  // Damit verlässlicher als charging() (Mittelstrom>0), das bei vollem Akku auf
  // false fällt. Bei I2C-Fehler liefert read16() 0 → external() meldet "am Strom";
  // bewusst der sichere Default (kein Auto-Standby aus Fehlmessung mitten im
  // Web-Transfer). Aufrufer prüfen ohnehin s_ready indirekt über percent() o.ä.
  if (!s_ready) return false;
  uint16_t bst = read16(REG_BATSTATUS);
  return (bst & 0x0001) == 0;
}

// Read-only-Diagnose des BQ27220 (Konsole `gauge`). Liest die Kapazitäts- und
// Status-Register, um zu prüfen, warum StateOfCharge nicht zur Spannung passt.
// SoC = RemainingCapacity / FullChargeCapacity — ist FCC/DesignCapacity zu hoch
// programmiert (Werks-Default für eine größere Zelle), liest die %-Anzeige bei
// vollem 4,2-V-Akku zu niedrig. Schreibt NICHTS.
void dumpGauge() {
  if (!s_ready) { Serial.println("[CON] Gauge: kein I2C-Ack (0x55)"); return; }

  uint16_t mv   = read16(REG_VOLTAGE);
  int16_t  cur  = (int16_t)read16(REG_CURRENT);
  int16_t  avg  = (int16_t)read16(REG_AVG_CURRENT);
  uint16_t rm   = read16(REG_REMAINCAP);
  uint16_t fcc  = read16(REG_FULLCAP);
  uint16_t dcap = read16(REG_DESIGNCAP);
  uint16_t soc  = read16(REG_SOC);
  uint16_t soh  = read16(REG_SOH);
  uint16_t bst  = read16(REG_BATSTATUS);

  // CONTROL_STATUS via Subkommando 0x0000 (nur Lesen).
  Wire.beginTransmission(kAddr);
  Wire.write(REG_CONTROL); Wire.write(0x00); Wire.write(0x00);
  Wire.endTransmission();
  uint16_t ctrl = read16(REG_CONTROL);

  Serial.printf("[CON] Gauge BQ27220:\n");
  Serial.printf("[CON]   Spannung      = %u mV\n", mv);
  Serial.printf("[CON]   Strom         = %d mA (avg %d)\n", cur, avg);
  Serial.printf("[CON]   RemainingCap  = %u mAh\n", rm);
  Serial.printf("[CON]   FullChargeCap = %u mAh   <- SoC = RM/FCC\n", fcc);
  Serial.printf("[CON]   DesignCap     = %u mAh\n", dcap);
  Serial.printf("[CON]   StateOfCharge = %u %% (Gauge, ungenutzt)\n", soc);
  Serial.printf("[CON]   -> Anzeige    = %u %% (spannungsbasiert)\n",
                percentFromMilliVolts(mv));
  Serial.printf("[CON]   StateOfHealth = %u %% (0x%04X)\n", soh & 0xFF, soh);
  Serial.printf("[CON]   BatteryStatus = 0x%04X\n", bst);
  Serial.printf("[CON]   ControlStatus = 0x%04X (SEC=%u %s)\n", ctrl,
                (unsigned)((ctrl >> 13) & 0x3),
                ((ctrl >> 13) & 0x3) == 0x3 ? "sealed" : "unsealed/full");
  if (fcc > 0)
    Serial.printf("[CON]   -> rechnerisch SoC = %u%%\n",
                  (unsigned)((uint32_t)rm * 100 / fcc));
}

}  // namespace battery
