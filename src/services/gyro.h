// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// gyro.h — IMU BHI260AP (I2C 0x28) über SensorLib (Bosch BHy2).
//
// Der BHI260AP ist ein „Smart Sensor Hub" mit eigenem MCU — kein Roh-Register-
// Read, sondern Firmware-Upload (~138 KB) + virtuelle Sensoren. Der Upload ist
// langsam (I2C 100 kHz ≈ 10 s), läuft daher EINMALIG beim ersten begin();
// Folge-begin() konfiguriert nur neu. end() setzt die Sensoren auf 0 Hz
// (Leerlauf, Strom sparen), ohne neu hochladen zu müssen. Nur betreiben, solange
// die „Lage"-App vorn ist.
// =============================================================================
#pragma once

#include <stdint.h>

namespace gyro {

struct Data {
  bool  ok;                  // Sensor aktiv + initialisiert
  float ax, ay, az;          // Beschleunigung (≈ g)
  float gx, gy, gz;          // Drehrate (≈ °/s)
};

// Sensoren aktivieren. Erster Aufruf lädt die Firmware (blockt ~10 s!), spätere
// nur Re-Konfiguration. false, wenn der Sensor nicht initialisiert werden konnte.
bool begin();
// Sensoren auf 0 Hz (Leerlauf) — Firmware bleibt geladen.
void end();
// FIFO pumpen (im tick()/background der App). Aktualisiert current().
void poll();

const Data& current();

}  // namespace gyro
