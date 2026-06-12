// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// ttt.h — Tic-Tac-Toe-Screen (UI zu ttt_core.h), gehostet von games_app.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>

#include "core/input.h"

namespace ttt_ui {

void enter();
void handleInput(const InputEvent& e);
void draw(Adafruit_GFX& g);

// Unterzeile für die Menü-Kachel ("Siege: 3 · Remis: 1").
void menuLine(char* out, size_t n);

}  // namespace ttt_ui
