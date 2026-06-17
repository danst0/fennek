// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// gyro_app.h — Sensor-/Lage-Anzeige (Kachel „Gyro", zweiter Homescreen).
//
// Zeigt den IMU-Sensor BHI260AP (I2C 0x28). Stufe 1: Sensor-Präsenz erkennen
// (I2C-Probe). Der vollständige Lese-Pfad braucht den Bosch-BHy2-Firmware-Upload
// (smarter Sensor-Hub, kein Roh-Register-Read) und wird am Gerät verifiziert.
// Der Sensor wird nur betrieben, solange die App vorn ist (Strom sparen).
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace gyro_app {

App* get();

}  // namespace gyro_app
