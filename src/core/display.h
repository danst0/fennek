// =============================================================================
// display.h — minimaler E-Ink-Treiber auf Basis von GxEPD2.
//
// E-Ink kann keine LCD-artige „delay-free" UI: ein Partial-Refresh dauert
// ~250-650 ms, ein Full-Refresh ~1 s mit Flackern. Strategie für „snappy":
//   - Eingaben (Touch) reagieren sofort (separater I2C-Bus, kein Refresh nötig)
//   - nur bei echter Änderung neu zeichnen (UI koalesziert Refreshes)
//   - überwiegend Partial-Refresh; periodisch Full-Refresh gegen Ghosting
//   - Refresh hält den SPI-Mutex (serialisiert gegen Audio-SD-Reads)
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>

namespace display {

// Zeichen-Callback: erhält die GFX-Fläche (240x320, Rotation 0).
typedef void (*DrawFn)(Adafruit_GFX& g);

void begin();

// Re-Init nach Deep-Sleep-Wakeup (Timer-Wake): wie begin(), aber OHNE das
// weiße Vollbild-Clear — das E-Ink hält das Schlafbild stromlos, und
// renderRegion() kann danach partiell dagegen aktualisieren.
void beginAfterSleep();

// Bildschirm neu aufbauen. full=true erzwingt Full-Refresh (Ghosting-Reset).
// Ansonsten wird ~alle N Refreshes automatisch ein Full-Refresh eingestreut.
void render(DrawFn draw, bool full = false);

// Nur einen horizontalen Streifen (volle Breite) partiell aktualisieren —
// z. B. die laufende Zeit, ohne den Rest des Schirms zu flackern.
// x ist fix 0 / w fix volle Breite (E-Ink verlangt 8er-Ausrichtung in x).
void renderRegion(DrawFn draw, int y, int h);

int width();
int height();

}  // namespace display
