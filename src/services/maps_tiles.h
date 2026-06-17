// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// maps_tiles.h — SD-Raster-Kacheln + Slippy-Map-Mathematik für die Maps-App.
//
// Kacheln liegen als 1-bit-Bitmaps unter /maps/{z}/{x}/{y}.bin (256x256,
// row-major, MSB zuerst, 32 Byte/Zeile = 8192 Byte; Bit gesetzt = schwarz).
// Ein Host-Skript (tools/maps_tiles.py) dithert OSM-PNG-Kacheln in dieses
// Format. Auf dem Gerät werden sie nur gelesen und geblittet — kein Decoder.
//
// WICHTIG (Anti-Stotter/Anti-Deadlock): SD-Zugriffe (scanZooms/ensureViewport)
// nehmen selbst spiLock und dürfen NICHT aus App::draw() laufen (das hält den
// nicht-rekursiven SPI-Mutex bereits → Self-Deadlock). draw() ruft nur
// blitViewport(), das ausschließlich aus dem PSRAM-Cache ins GFX kopiert.
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>

namespace maps_tiles {

constexpr int kTilePx    = 256;
constexpr int kTileBytes = kTilePx * kTilePx / 8;   // 8192

// Vorhandene Zoomstufen aus /maps ermitteln (Verzeichnisnamen). Nimmt spiLock.
void scanZooms();
int  minZoom();   // -1 = keine Kacheln / keine SD
int  maxZoom();   // -1 = keine Kacheln / keine SD

// Slippy-Map-Umrechnung (Web-Mercator).
double lonToTileXf(double lon, int z);
double latToTileYf(double lat, int z);
double tileXToLon(double xf, int z);
double tileYToLat(double yf, int z);

// Alle Kacheln, die das Viewport (Bildschirm-Ursprung originX/Y, Größe vW x vH,
// Zentrum centerLat/Lon, Zoom z) überdecken, in den PSRAM-Cache laden. Nimmt
// spiLock; NUR aus tick()/handleInput() aufrufen, nie aus draw().
void ensureViewport(double centerLat, double centerLon, int z,
                    int originX, int originY, int vW, int vH);

// Die gecachten Kacheln ins GFX blitten (reiner RAM-Zugriff, lock-frei → aus
// draw()). Fehlende Kacheln werden dezent schraffiert. Rückgabe = Zahl der
// tatsächlich vorhandenen (geblitteten) Kacheln im Viewport.
int blitViewport(Adafruit_GFX& g, double centerLat, double centerLon, int z,
                 int originX, int originY, int vW, int vH);

}  // namespace maps_tiles
