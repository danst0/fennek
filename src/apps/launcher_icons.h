// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// launcher_icons.h — App-Icons für den Homescreen, rein per GFX-Primitiven.
//
// Jedes Icon wird schwarz und zentriert um (cx,cy) gezeichnet, Bounding ~38x38
// px (passt in eine 108x46-Kachel mit Rand). Keine Bitmap-Blobs: alle Glyphen
// aus Linien/Kreisen/Rechtecken/Dreiecken, damit sie 1-bit-scharf bleiben und
// ohne PROGMEM-Daten auskommen. draw() liefert false, wenn es für die ID kein
// Icon gibt — der Aufrufer fällt dann auf den Text-Label zurück.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include "core/i18n.h"

namespace launcher_icons {

// 2-px-Strich (horizontal verdickt) — auf E-Ink kräftiger als drawLine.
inline void line2(Adafruit_GFX& g, int x0, int y0, int x1, int y1) {
  g.drawLine(x0, y0, x1, y1, GxEPD_BLACK);
  g.drawLine(x0 + 1, y0, x1 + 1, y1, GxEPD_BLACK);
}
inline void hbar(Adafruit_GFX& g, int x, int y, int w) { g.fillRect(x, y, w, 2, GxEPD_BLACK); }
inline void vbar(Adafruit_GFX& g, int x, int y, int h) { g.fillRect(x, y, 2, h, GxEPD_BLACK); }

// --- Musik: Achtelnote -------------------------------------------------------
inline void music(Adafruit_GFX& g, int cx, int cy) {
  g.fillCircle(cx - 6, cy + 9, 5, GxEPD_BLACK);     // Notenkopf
  g.fillRect(cx - 2, cy - 13, 2, 22, GxEPD_BLACK);  // Hals
  line2(g, cx, cy - 13, cx + 7, cy - 7);            // Fähnchen
  line2(g, cx, cy - 8, cx + 7, cy - 2);
}

// --- Hörbuch: Kopfhörer ------------------------------------------------------
inline void headphones(Adafruit_GFX& g, int cx, int cy) {
  for (int r = 13; r <= 15; r++)
    g.drawCircleHelper(cx, cy + 2, r, 0x3, GxEPD_BLACK);  // Bügel (obere Hälfte)
  g.fillRoundRect(cx - 17, cy + 1, 7, 13, 2, GxEPD_BLACK);
  g.fillRoundRect(cx + 10, cy + 1, 7, 13, 2, GxEPD_BLACK);
}

// --- Lesen: aufgeschlagenes Buch --------------------------------------------
inline void book(Adafruit_GFX& g, int cx, int cy) {
  vbar(g, cx - 1, cy - 8, 20);                                  // Buchrücken
  g.drawLine(cx - 1, cy - 8, cx - 16, cy - 11, GxEPD_BLACK);    // linke Seite
  g.drawLine(cx - 16, cy - 11, cx - 16, cy + 8, GxEPD_BLACK);
  g.drawLine(cx - 16, cy + 8, cx - 1, cy + 11, GxEPD_BLACK);
  g.drawLine(cx + 1, cy - 8, cx + 16, cy - 11, GxEPD_BLACK);    // rechte Seite
  g.drawLine(cx + 16, cy - 11, cx + 16, cy + 8, GxEPD_BLACK);
  g.drawLine(cx + 16, cy + 8, cx + 1, cy + 11, GxEPD_BLACK);
  for (int i = 0; i < 3; i++) {                                 // Textzeilen
    g.drawFastHLine(cx - 13, cy - 4 + i * 5, 9, GxEPD_BLACK);
    g.drawFastHLine(cx + 5, cy - 4 + i * 5, 9, GxEPD_BLACK);
  }
}

// --- Mesh: Knoten + Verbindungen --------------------------------------------
inline void mesh(Adafruit_GFX& g, int cx, int cy) {
  const int o[4][2] = {{-13, -10}, {13, -10}, {-13, 10}, {13, 10}};
  for (auto& p : o) g.drawLine(cx, cy, cx + p[0], cy + p[1], GxEPD_BLACK);
  for (auto& p : o) g.fillCircle(cx + p[0], cy + p[1], 3, GxEPD_BLACK);
  g.fillCircle(cx, cy, 4, GxEPD_BLACK);
}

// --- Optionen: Zahnrad -------------------------------------------------------
inline void gear(Adafruit_GFX& g, int cx, int cy) {
  const int o[8][2] = {{13, 0}, {9, 9}, {0, 13}, {-9, 9}, {-13, 0}, {-9, -9}, {0, -13}, {9, -9}};
  for (auto& p : o) g.fillRect(cx + p[0] - 2, cy + p[1] - 2, 4, 4, GxEPD_BLACK);  // Zähne
  g.fillCircle(cx, cy, 9, GxEPD_BLACK);
  g.fillCircle(cx, cy, 3, GxEPD_WHITE);   // Nabe
}

// --- Spiele: Würfel (5er-Seite) ---------------------------------------------
inline void dice(Adafruit_GFX& g, int cx, int cy) {
  g.drawRoundRect(cx - 14, cy - 14, 28, 28, 4, GxEPD_BLACK);
  const int p[5][2] = {{-7, -7}, {7, -7}, {0, 0}, {-7, 7}, {7, 7}};
  for (auto& d : p) g.fillCircle(cx + d[0], cy + d[1], 2, GxEPD_BLACK);
}

// --- Dateien: Ordner ---------------------------------------------------------
inline void folder(Adafruit_GFX& g, int cx, int cy) {
  g.drawLine(cx - 15, cy - 7, cx - 15, cy - 12, GxEPD_BLACK);   // Reiter
  g.drawLine(cx - 15, cy - 12, cx - 6, cy - 12, GxEPD_BLACK);
  g.drawLine(cx - 6, cy - 12, cx - 3, cy - 7, GxEPD_BLACK);
  g.drawRect(cx - 15, cy - 7, 30, 18, GxEPD_BLACK);             // Korpus
}

// --- Notizen: Blatt mit Stift ------------------------------------------------
inline void notes(Adafruit_GFX& g, int cx, int cy) {
  g.drawRect(cx - 13, cy - 14, 20, 28, GxEPD_BLACK);            // Blatt
  for (int i = 0; i < 3; i++) g.drawFastHLine(cx - 9, cy - 7 + i * 6, 11, GxEPD_BLACK);
  line2(g, cx + 3, cy - 16, cx + 13, cy - 3);                  // Stift
  g.fillTriangle(cx + 11, cy - 5, cx + 15, cy - 2, cx + 11, cy + 1, GxEPD_BLACK);  // Spitze
}

// --- Wecker: Uhr mit Glocken -------------------------------------------------
inline void alarm(Adafruit_GFX& g, int cx, int cy) {
  g.fillCircle(cx - 10, cy - 10, 4, GxEPD_BLACK);   // Glocken
  g.fillCircle(cx + 10, cy - 10, 4, GxEPD_BLACK);
  g.drawCircle(cx, cy + 2, 12, GxEPD_BLACK);        // Zifferblatt
  g.drawCircle(cx, cy + 2, 11, GxEPD_BLACK);
  line2(g, cx, cy + 2, cx, cy - 5);                 // Zeiger
  line2(g, cx, cy + 2, cx + 5, cy + 4);
  g.drawLine(cx - 11, cy + 13, cx - 14, cy + 16, GxEPD_BLACK);  // Füße
  g.drawLine(cx + 11, cy + 13, cx + 14, cy + 16, GxEPD_BLACK);
}

// --- Karten: Standort-Pin ----------------------------------------------------
inline void mapsPin(Adafruit_GFX& g, int cx, int cy) {
  g.fillCircle(cx, cy - 4, 9, GxEPD_BLACK);
  g.fillTriangle(cx - 8, cy + 1, cx + 8, cy + 1, cx, cy + 14, GxEPD_BLACK);
  g.fillCircle(cx, cy - 4, 4, GxEPD_WHITE);   // Loch
}

// --- Lage: Gyroskop ----------------------------------------------------------
inline void gyro(Adafruit_GFX& g, int cx, int cy) {
  g.drawCircle(cx, cy, 14, GxEPD_BLACK);
  g.drawCircle(cx, cy, 13, GxEPD_BLACK);
  g.drawCircle(cx, cy, 6, GxEPD_BLACK);
  line2(g, cx - 13, cy + 8, cx + 13, cy - 8);  // gekippte Achse
  g.fillCircle(cx, cy, 2, GxEPD_BLACK);
}

// --- Rechnen: vier Operatoren (2x2) -----------------------------------------
inline void math(Adafruit_GFX& g, int cx, int cy) {
  // + (oben links)
  hbar(g, cx - 12, cy - 8, 8); vbar(g, cx - 9, cy - 11, 8);
  // − (oben rechts)
  hbar(g, cx + 4, cy - 8, 8);
  // × (unten links)
  line2(g, cx - 12, cy + 4, cx - 5, cy + 11);
  line2(g, cx - 5, cy + 4, cx - 12, cy + 11);
  // ÷ (unten rechts)
  hbar(g, cx + 4, cy + 7, 8);
  g.fillCircle(cx + 8, cy + 4, 1, GxEPD_BLACK);
  g.fillCircle(cx + 8, cy + 11, 1, GxEPD_BLACK);
}

// --- Lernen: gestapelte Karteikarten ----------------------------------------
inline void cards(Adafruit_GFX& g, int cx, int cy) {
  g.drawRoundRect(cx - 6, cy - 13, 20, 22, 3, GxEPD_BLACK);     // hintere Karte
  g.fillRoundRect(cx - 13, cy - 7, 20, 22, 3, GxEPD_WHITE);     // vordere (verdeckt)
  g.drawRoundRect(cx - 13, cy - 7, 20, 22, 3, GxEPD_BLACK);
  for (int i = 0; i < 2; i++) g.drawFastHLine(cx - 9, cy + i * 6, 12, GxEPD_BLACK);
}

// --- Kalender ----------------------------------------------------------------
inline void calendar(Adafruit_GFX& g, int cx, int cy) {
  vbar(g, cx - 8, cy - 15, 5);                       // Ringe
  vbar(g, cx + 6, cy - 15, 5);
  g.drawRect(cx - 13, cy - 11, 26, 24, GxEPD_BLACK);
  g.fillRect(cx - 13, cy - 11, 26, 6, GxEPD_BLACK);  // Kopfleiste
  for (int r = 0; r < 2; r++)                         // Tagesraster
    for (int c = 0; c < 3; c++)
      g.fillRect(cx - 9 + c * 8, cy - 1 + r * 7, 4, 4, GxEPD_BLACK);
}

// --- Todo: Checkbox + Liste --------------------------------------------------
inline void todo(Adafruit_GFX& g, int cx, int cy) {
  g.drawRect(cx - 15, cy - 9, 13, 13, GxEPD_BLACK);  // Box
  line2(g, cx - 13, cy - 3, cx - 9, cy + 1);         // Haken
  line2(g, cx - 9, cy + 1, cx - 3, cy - 7);
  hbar(g, cx + 1, cy - 6, 14);                        // Listenzeilen
  hbar(g, cx + 1, cy + 1, 14);
}

// Dispatch nach Tile-ID. false -> kein Icon, Aufrufer zeichnet Text-Label.
inline bool draw(Adafruit_GFX& g, i18n::Str id, int cx, int cy) {
  switch (id) {
    case i18n::Str::TileMusic:    music(g, cx, cy);      return true;
    case i18n::Str::TileBook:     headphones(g, cx, cy); return true;
    case i18n::Str::TileReader:   book(g, cx, cy);       return true;
    case i18n::Str::TileMesh:     mesh(g, cx, cy);       return true;
    case i18n::Str::TileSettings: gear(g, cx, cy);       return true;
    case i18n::Str::TileGames:    dice(g, cx, cy);       return true;
    case i18n::Str::TileFiles:    folder(g, cx, cy);     return true;
    case i18n::Str::TileNotes:    notes(g, cx, cy);      return true;
    case i18n::Str::TileAlarm:    alarm(g, cx, cy);      return true;
    case i18n::Str::TileMaps:     mapsPin(g, cx, cy);    return true;
    case i18n::Str::TileGyro:     gyro(g, cx, cy);       return true;
    case i18n::Str::TileMath:     math(g, cx, cy);       return true;
    case i18n::Str::TileCards:    cards(g, cx, cy);      return true;
    case i18n::Str::TileCalendar: calendar(g, cx, cy);   return true;
    case i18n::Str::TileTodo:     todo(g, cx, cy);       return true;
    default: return false;
  }
}

}  // namespace launcher_icons
