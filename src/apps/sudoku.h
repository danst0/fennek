// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// sudoku.h — Sudoku-Screen (UI zu sudoku_core.h), gehostet von games_app.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>

#include "core/input.h"

namespace sudoku_ui {

void enter();
void handleInput(const InputEvent& e);
void draw(Adafruit_GFX& g);

// Unterzeile für die Menü-Kachel ("Gelöst: 3 · Best: 04:12").
void menuLine(char* out, size_t n);

}  // namespace sudoku_ui
