// =============================================================================
// game2048.h — 2048-Screen (UI zu game2048_core.h), gehostet von games_app.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>

#include "core/input.h"

namespace game2048_ui {

void enter();
void handleInput(const InputEvent& e);
void draw(Adafruit_GFX& g);

// Unterzeile für die Menü-Kachel ("Best: 4096").
void menuLine(char* out, size_t n);

}  // namespace game2048_ui
