// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "maps_app.h"
#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "services/gps.h"
#include "services/maps_tiles.h"
#include "services/timesync.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <math.h>
#include <stdio.h>

namespace {

// Karten-Viewport: voller Content-Bereich, unten eine Statuszeile überlagert.
constexpr int OX  = 0;
constexpr int OY  = appmgr::CONTENT_Y;          // 24
constexpr int VW  = EINK_W;                      // 240
constexpr int VH  = EINK_H - appmgr::CONTENT_Y;  // 296
constexpr int FOOT_H = 28;                        // Statuszeilen-Band unten

// z13 ist die tiefste NRW-weit vorhandene Stufe (z14-16 nur Dortmund-Detail);
// bei z15 ueber Duesseldorf gaebe es nur leeres Punktraster.
constexpr int DEFAULT_ZOOM = 13;
// Fallback-Zentrum ohne Fix/Mesh-Pos (Duesseldorf, NRW-weit bekachelt).
constexpr double FALLBACK_LAT = 51.2277;
constexpr double FALLBACK_LON = 6.7735;

double s_lat = FALLBACK_LAT, s_lon = FALLBACK_LON;   // Karten-Zentrum
int    s_zoom = DEFAULT_ZOOM;
bool   s_follow = true;
bool   s_viewDirty = true;            // Kacheln müssen neu geladen werden

// Mesh-Pos-Drosselung.
uint32_t s_lastMeshMs = 0;
double   s_meshLat = 0, s_meshLon = 0;

bool s_lastFixValid = false;
bool s_gpsTimeSeen  = false;   // Einmal-Log: RMC lieferte in dieser Session Datum+Zeit

void markDirty() { appmgr::markDirty(); }

void clampZoom() {
  int lo = maps_tiles::minZoom(), hi = maps_tiles::maxZoom();
  if (hi < 0) return;                 // keine Kacheln — Zoom unverändert lassen
  if (s_zoom < lo) s_zoom = lo;
  if (s_zoom > hi) s_zoom = hi;
}

// Pixel-Abstand zweier WGS84-Punkte bei aktuellem Zoom (zur Redraw-Entscheidung).
double pixelDist(double aLat, double aLon, double bLat, double bLon) {
  double ax = maps_tiles::lonToTileXf(aLon, s_zoom) * maps_tiles::kTilePx;
  double ay = maps_tiles::latToTileYf(aLat, s_zoom) * maps_tiles::kTilePx;
  double bx = maps_tiles::lonToTileXf(bLon, s_zoom) * maps_tiles::kTilePx;
  double by = maps_tiles::latToTileYf(bLat, s_zoom) * maps_tiles::kTilePx;
  return sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

// Näherungs-Distanz in Metern (für die Mesh-Pos-Drossel).
double meters(double aLat, double aLon, double bLat, double bLon) {
  double dLat = (aLat - bLat) * 111320.0;
  double dLon = (aLon - bLon) * 111320.0 * cos(aLat * M_PI / 180.0);
  return sqrt(dLat * dLat + dLon * dLon);
}

void panPixels(int dxPx, int dyPx) {
  double cxf = maps_tiles::lonToTileXf(s_lon, s_zoom) + dxPx / (double)maps_tiles::kTilePx;
  double cyf = maps_tiles::latToTileYf(s_lat, s_zoom) + dyPx / (double)maps_tiles::kTilePx;
  double n = (double)(1 << s_zoom);
  if (cyf < 0.01) cyf = 0.01;
  if (cyf > n - 0.01) cyf = n - 0.01;
  s_lon = maps_tiles::tileXToLon(cxf, s_zoom);
  s_lat = maps_tiles::tileYToLat(cyf, s_zoom);
  s_follow = false;
  s_viewDirty = true;
  markDirty();
}

void recenterToFix() {
  const gps::Fix& f = gps::current();
  if (f.valid) { s_lat = f.lat; s_lon = f.lon; }
  s_follow = true;
  s_viewDirty = true;
  markDirty();
}

void zoomBy(int d) {
  int z = s_zoom + d;
  int lo = maps_tiles::minZoom(), hi = maps_tiles::maxZoom();
  if (hi >= 0) { if (z < lo) z = lo; if (z > hi) z = hi; }
  else if (z < 1 || z > 19) return;
  if (z != s_zoom) { s_zoom = z; s_viewDirty = true; markDirty(); }
}

// --- Zeichnen -------------------------------------------------------------------
void drawMarker(Adafruit_GFX& g) {
  const gps::Fix& f = gps::current();
  if (!f.valid) return;
  double cpx = maps_tiles::lonToTileXf(s_lon, s_zoom) * maps_tiles::kTilePx;
  double cpy = maps_tiles::latToTileYf(s_lat, s_zoom) * maps_tiles::kTilePx;
  double mpx = maps_tiles::lonToTileXf(f.lon, s_zoom) * maps_tiles::kTilePx;
  double mpy = maps_tiles::latToTileYf(f.lat, s_zoom) * maps_tiles::kTilePx;
  int sx = OX + VW / 2 + (int)lround(mpx - cpx);
  int sy = OY + VH / 2 + (int)lround(mpy - cpy);
  if (sx < OX || sx >= OX + VW || sy < OY || sy >= OY + VH - FOOT_H) return;
  // Weißer Hof + schwarzer Ring + Fadenkreuz, damit der Marker auf der Karte sticht.
  g.fillCircle(sx, sy, 6, GxEPD_WHITE);
  g.drawCircle(sx, sy, 6, GxEPD_BLACK);
  g.drawCircle(sx, sy, 5, GxEPD_BLACK);
  g.fillCircle(sx, sy, 2, GxEPD_BLACK);
}

void drawFooter(Adafruit_GFX& g) {
  int y = EINK_H - FOOT_H;
  g.fillRect(0, y, EINK_W, FOOT_H, GxEPD_WHITE);
  g.drawFastHLine(0, y, EINK_W, GxEPD_BLACK);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);

  const gps::Fix& f = gps::current();
  char l1[40], l2[48];
  snprintf(l1, sizeof(l1), "Z%d  %s%s", s_zoom,
           s_follow ? "Folgt GPS" : "frei",
           f.valid ? "" : "  kein Fix");
  g.setCursor(4, y + 5);
  gui::print(g, l1);

  if (f.valid)
    snprintf(l2, sizeof(l2), "%.5f, %.5f  Sat %u", f.lat, f.lon, (unsigned)f.sats);
  else
    snprintf(l2, sizeof(l2), "Warte auf Satelliten... (Sat %u)", (unsigned)f.sats);
  g.setCursor(4, y + 16);
  gui::print(g, l2);
}

void drawNoTiles(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 16, OY + 60, "Keine Karten", 2);
  gui::printAt(g, 16, OY + 84, "auf der SD-Karte", 2);
  g.setTextSize(1);
  g.setCursor(16, OY + 120);
  gui::print(g, "Kacheln am PC mit tools/maps_tiles.py");
  g.setCursor(16, OY + 134);
  gui::print(g, "erzeugen -> /maps/{z}/{x}/{y}.bin");
}

// Halbtransparenter Hinweis, wenn an dieser Stelle/Zoomstufe keine Kacheln liegen
// (die Zoom-Mechanik funktioniert — es fehlen nur die Detaildaten auf der SD).
void drawNoDetail(Adafruit_GFX& g) {
  int bw = 200, bh = 34;
  int bx = OX + (VW - bw) / 2;
  int by = OY + (VH - FOOT_H - bh) / 2;
  g.fillRect(bx, by, bw, bh, GxEPD_WHITE);
  g.drawRect(bx, by, bw, bh, GxEPD_BLACK);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);
  g.setCursor(bx + 8, by + 7);
  gui::print(g, "Keine Kacheln in dieser");
  g.setCursor(bx + 8, by + 19);
  gui::print(g, "Detailstufe (- = rauszoomen)");
}

class MapsApp : public App {
 public:
  const char* id()   const override { return "Karten"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppMaps); }

  void onEnter() override {
    gps::begin();
    maps_tiles::scanZooms();
    s_zoom = DEFAULT_ZOOM;
    clampZoom();
    // Zentrum aus der zuletzt bekannten Mesh-Position, falls gesetzt.
    double lat, lon;
    settings::meshPos(&lat, &lon);
    if (lat != 0.0 || lon != 0.0) { s_lat = lat; s_lon = lon; }
    s_follow = true;
    s_gpsTimeSeen = false;
    s_viewDirty = true;
    s_lastFixValid = false;
    markDirty();
  }

  void onLeave() override {
    gps::end();
  }

  void tick() override {
    gps::poll();
    const gps::Fix& f = gps::current();

    // GPS-Zeit hat höchste Priorität (vor NTP/Mesh): sobald RMC Datum+Zeit liefert
    // — auch schon vor dem Positionsfix — der Uhr melden (gpsSync drosselt selbst).
    if (f.epochUtc > 1700000000UL) {
      if (!s_gpsTimeSeen) {   // Einmal pro Session: belegt, dass RMC-Zeit ankommt
        s_gpsTimeSeen = true;
        Serial.printf("[MAPS] GPS-Zeit empfangen: %lu UTC (Fix=%s) -> timesync\n",
                      (unsigned long)f.epochUtc, f.valid ? "ja" : "nein");
      }
      timesync::gpsSync(f.epochUtc);
    }

    // Folgt die Karte dem GPS und ist der Fix spürbar gewandert: nachzentrieren.
    if (s_follow && f.valid) {
      if (pixelDist(s_lat, s_lon, f.lat, f.lon) > 24.0) {
        s_lat = f.lat; s_lon = f.lon;
        s_viewDirty = true;
        markDirty();
      }
    }

    // Fix-Status-Wechsel sichtbar machen (Footer/Marker).
    if (f.valid != s_lastFixValid) { s_lastFixValid = f.valid; markDirty(); }

    // Mesh-Position gedrosselt nachführen (>=30 s und >10 m Bewegung).
    if (f.valid) {
      uint32_t now = millis();
      if (now - s_lastMeshMs >= 30000 &&
          (s_lastMeshMs == 0 || meters(f.lat, f.lon, s_meshLat, s_meshLon) > 10.0)) {
        settings::setMeshPos(f.lat, f.lon);
        s_meshLat = f.lat; s_meshLon = f.lon;
        s_lastMeshMs = now;
      }
    }

    // Kacheln nachladen (SD-Last, außerhalb des Draw-Locks!).
    if (s_viewDirty) {
      maps_tiles::ensureViewport(s_lat, s_lon, s_zoom, OX, OY, VW, VH);
      s_viewDirty = false;
    }
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::KEY) {
      switch (e.key) {
        case 'a': case 'A': panPixels(-VW / 3, 0); break;
        case 'd': case 'D': panPixels(+VW / 3, 0); break;
        case 'w': case 'W': panPixels(0, -VH / 3); break;
        case 's': case 'S': panPixels(0, +VH / 3); break;
        case '+': case '=': zoomBy(+1); break;
        case '-': case '_': zoomBy(-1); break;
        case '0':           recenterToFix(); break;
        case '\b': case 'q': case 'Q': appmgr::goHome(); break;
        default: break;
      }
      return;
    }
    // TAP: 3x3-Raster — Mitte re-zentriert, Ränder pannen in ihre Richtung.
    int col = (e.x - OX) * 3 / VW;            // 0..2
    int row = (e.y - OY) * 3 / (VH);          // 0..2
    if (col == 1 && row == 1) { recenterToFix(); return; }
    int dx = (col == 0 ? -1 : col == 2 ? +1 : 0) * (VW / 3);
    int dy = (row == 0 ? -1 : row == 2 ? +1 : 0) * (VH / 3);
    if (dx || dy) panPixels(dx, dy);
  }

  void draw(Adafruit_GFX& g) override {
    if (maps_tiles::maxZoom() < 0) { drawNoTiles(g); return; }
    int drawn = maps_tiles::blitViewport(g, s_lat, s_lon, s_zoom, OX, OY, VW, VH);
    // Keine Kachel im Blickfeld vorhanden (z. B. Detailstufe ohne Daten an dieser
    // Stelle): das Punktraster allein wirkt wie ein Fehler — Klartext-Hinweis.
    if (drawn == 0) drawNoDetail(g);
    drawMarker(g);
    drawFooter(g);
  }
};

MapsApp s_app;

}  // namespace

namespace maps_app {

App* get() { return &s_app; }

}  // namespace maps_app
