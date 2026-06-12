// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// input.h — gemeinsame Eingabe-Abstraktion für alle Apps.
//
// Quellen: Touch (CST328, I2C) und ab M2 die physische Tastatur (TCA8418).
// Beide werden vom appmgr gepollt und als InputEvent an die aktive App geleitet.
// =============================================================================
#pragma once

#include <stdint.h>

struct InputEvent {
  enum Type : uint8_t { TAP, KEY };
  Type    type;
  int16_t x, y;   // TAP: Display-Koordinaten (240x320)
  char    key;    // KEY: ASCII; Sondertasten siehe Konstanten unten
};

// Sondertasten der TCA8418-Tastatur (Werte wie im Legacy-Treiber).
constexpr char KEY_ENTER     = '\r';
constexpr char KEY_BACKSPACE = '\b';
