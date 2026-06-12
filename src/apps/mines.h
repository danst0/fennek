// =============================================================================
// mines.h — Minensucher-Screen (UI zu mines_core.h), gehostet von games_app.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>

#include "core/input.h"

namespace mines_ui {

void enter();
void handleInput(const InputEvent& e);
void draw(Adafruit_GFX& g);

// Unterzeile für die Menü-Kachel ("Siege: 2 · Bestzeit: 87 s").
void menuLine(char* out, size_t n);

}  // namespace mines_ui
