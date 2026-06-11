// =============================================================================
// gui.h — gemeinsame Zeichen-Helfer für alle Apps.
//
// Alle Textausgaben laufen über gui::print()/drawButton()/drawRowText(), die
// UTF-8 nach CP437 konvertieren — der Adafruit-GFX-Classic-Font enthält dort
// die deutschen Umlaute (ä=0x84, ö=0x94, ü=0x81, Ä=0x8E, Ö=0x99, Ü=0x9A,
// ß=0xE1). Voraussetzung: display::begin() hat cp437(true) gesetzt.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <stddef.h>
#include <stdint.h>

namespace gui {

struct Rect {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// UTF-8 -> CP437. Nicht darstellbare Zeichen werden zu '?'; typografische
// Zeichen (Gedankenstrich, Anführungszeichen, Ellipse) zu ASCII-Äquivalenten.
void toCp437(const char* utf8, char* out, size_t outLen);

// Unicode-Codepoint -> CP437-Byte (0 = im Font nicht darstellbar).
uint8_t cp437For(uint32_t cp);

// Text (UTF-8) an der aktuellen Cursor-Position ausgeben.
void print(Adafruit_GFX& g, const char* utf8);

// Text (UTF-8) an Position ausgeben (setzt Cursor + TextSize).
void printAt(Adafruit_GFX& g, int x, int y, const char* utf8, uint8_t size = 2);

// Pixelbreite/-höhe eines UTF-8-Texts bei aktueller TextSize.
void textBounds(Adafruit_GFX& g, const char* utf8, uint16_t* w, uint16_t* h);

// Beschrifteter Button mit runder Ecke; emphasize = doppelter Rand.
void drawButton(Adafruit_GFX& g, const Rect& r, const char* utf8, bool emphasize);

// Listenzeile voller Breite mit Trennlinie; invert = schwarz hinterlegt.
void drawRowText(Adafruit_GFX& g, int y, int rowH, const char* utf8, bool invert);

}  // namespace gui
