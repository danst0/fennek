// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// touch.h — kapazitiver Touch (CST328) über den vendored Hynitron-Treiber.
//
// Wichtig: Touch hängt am I2C-Bus (GPIO 13/14), NICHT am geteilten SPI-Bus.
// Dadurch sind Eingaben jederzeit sofort lesbar — unabhängig von E-Ink-Refresh
// oder Audio-SD-Zugriffen. Das ist der Schlüssel für „verzögerungsfreie" Reaktion.
// =============================================================================
#pragma once

#include <stdint.h>

namespace touch {

// Initialisiert den CST328 (Wire muss vorher mit begin() laufen).
bool begin();

// Liefert true, wenn seit dem letzten Aufruf ein neuer Tap erkannt wurde.
// x/y sind dann auf das Display-Koordinatensystem (240x320) abgebildet.
bool poll(int16_t& x, int16_t& y);

// Anhängende/abgelaufene Touch-Events verwerfen (z. B. nach einem langen
// E-Ink-Refresh, damit ein nachklingender Tap nicht doppelt verarbeitet wird).
void flush();

}  // namespace touch
