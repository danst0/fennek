// =============================================================================
// chess.h — Schach-Screen (UI zu chess_core.h), gehostet von games_app.
//
// Die KI rechnet in einem eigenen FreeRTOS-Task (reine CPU-Arbeit, kein
// SPI/Audio); tick() pollt das Ergebnis. Laufende Partien werden als
// NVS-Blob gesichert (Reboot-fest; Wiederholungs-Historie geht dabei
// bewusst verloren — nur die Stellung zählt).
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>

#include "core/input.h"

namespace chess_ui {

void enter();
void leave();    // Partie ins NVS sichern (von games_app bei onLeave/Menü)
void tick();     // KI-Ergebnis abholen (nur wenn Schach-Screen aktiv)
void handleInput(const InputEvent& e);
void draw(Adafruit_GFX& g);

// Unterzeile für die Menü-Kachel ("Siege gegen Fennek: 1").
void menuLine(char* out, size_t n);

}  // namespace chess_ui
