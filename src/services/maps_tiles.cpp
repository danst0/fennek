// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "maps_tiles.h"
#include "config.h"
#include "core/board.h"

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace maps_tiles {

namespace {

int s_minZoom = -1;
int s_maxZoom = -1;

// --- PSRAM-Kachel-Cache ---------------------------------------------------------
struct Slot {
  int      z, x, y;
  bool     used;       // belegt
  bool     present;    // Kachel-Datei existierte
  uint32_t lru;        // Nutzungszähler (höher = jünger)
  uint8_t* buf;        // PSRAM, kTileBytes (lazy alloziert)
};
constexpr int kSlots = 16;       // 16 x 8192 B = 128 KB PSRAM
Slot     s_slots[kSlots] = {};
uint32_t s_lruCtr = 0;

Slot* findSlot(int z, int x, int y) {
  for (int i = 0; i < kSlots; i++)
    if (s_slots[i].used && s_slots[i].z == z && s_slots[i].x == x && s_slots[i].y == y)
      return &s_slots[i];
  return nullptr;
}

// Freien oder ältesten Slot beschaffen (LRU), Puffer bei Bedarf allozieren.
Slot* acquireSlot() {
  Slot* best = nullptr;
  for (int i = 0; i < kSlots; i++) {
    if (!s_slots[i].used) { best = &s_slots[i]; break; }
    if (!best || s_slots[i].lru < best->lru) best = &s_slots[i];
  }
  if (!best->buf) best->buf = (uint8_t*)ps_malloc(kTileBytes);
  return best;
}

// Sichtbare Kacheln eines Viewports: tx/ty (tx longitudinal gewrappt) und die
// Bildschirmposition der linken oberen Ecke jeder Kachel.
struct VTile { int tx, ty, sx, sy; };

int visibleTiles(double cLat, double cLon, int z, int oX, int oY, int vW, int vH,
                 VTile* out, int maxN) {
  int n = 1 << z;
  double cpx = lonToTileXf(cLon, z) * kTilePx;   // globale Pixel des Zentrums
  double cpy = latToTileYf(cLat, z) * kTilePx;
  double leftPx = cpx - vW / 2.0;
  double topPx  = cpy - vH / 2.0;
  int tx0 = (int)floor(leftPx / kTilePx);
  int ty0 = (int)floor(topPx  / kTilePx);
  int tx1 = (int)floor((leftPx + vW - 1) / kTilePx);
  int ty1 = (int)floor((topPx  + vH - 1) / kTilePx);
  int cnt = 0;
  for (int ty = ty0; ty <= ty1 && cnt < maxN; ty++) {
    if (ty < 0 || ty >= n) continue;            // außerhalb der Weltkarte
    for (int tx = tx0; tx <= tx1 && cnt < maxN; tx++) {
      int wx = ((tx % n) + n) % n;              // Längengrad wrappt rundum
      out[cnt].tx = wx;
      out[cnt].ty = ty;
      out[cnt].sx = oX + (int)lround(tx * (double)kTilePx - leftPx);
      out[cnt].sy = oY + (int)lround(ty * (double)kTilePx - topPx);
      cnt++;
    }
  }
  return cnt;
}

}  // namespace

// --- Slippy-Map-Mathematik ------------------------------------------------------
double lonToTileXf(double lon, int z) {
  return (lon + 180.0) / 360.0 * (double)(1 << z);
}
double latToTileYf(double lat, int z) {
  double r = lat * M_PI / 180.0;
  return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (double)(1 << z);
}
double tileXToLon(double xf, int z) {
  return xf / (double)(1 << z) * 360.0 - 180.0;
}
double tileYToLat(double yf, int z) {
  double a = M_PI - 2.0 * M_PI * yf / (double)(1 << z);
  return 180.0 / M_PI * atan(0.5 * (exp(a) - exp(-a)));
}

// --- Zoomstufen ermitteln -------------------------------------------------------
void scanZooms() {
  s_minZoom = s_maxZoom = -1;
  if (!board::sdReady()) return;
  spiLock();
  File dir = SD.open("/maps");
  if (dir && dir.isDirectory()) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (e.isDirectory()) {
        const char* nm = strrchr(e.name(), '/');
        nm = nm ? nm + 1 : e.name();
        int z = atoi(nm);
        if (z > 0 || (nm[0] == '0')) {
          if (s_minZoom < 0 || z < s_minZoom) s_minZoom = z;
          if (s_maxZoom < 0 || z > s_maxZoom) s_maxZoom = z;
        }
      }
      e.close();
    }
  }
  if (dir) dir.close();
  spiUnlock();
}

int minZoom() { return s_minZoom; }
int maxZoom() { return s_maxZoom; }

// --- Laden + Blitten ------------------------------------------------------------
void ensureViewport(double centerLat, double centerLon, int z,
                    int originX, int originY, int vW, int vH) {
  if (!board::sdReady()) return;
  VTile vt[12];
  int n = visibleTiles(centerLat, centerLon, z, originX, originY, vW, vH, vt, 12);

  spiLock();
  for (int i = 0; i < n; i++) {
    if (findSlot(z, vt[i].tx, vt[i].ty)) continue;   // schon im Cache
    Slot* s = acquireSlot();
    s->used = true; s->z = z; s->x = vt[i].tx; s->y = vt[i].ty;
    s->lru = ++s_lruCtr;
    s->present = false;
    if (!s->buf) continue;                           // PSRAM erschöpft
    char path[64];
    snprintf(path, sizeof(path), "/maps/%d/%d/%d.bin", z, vt[i].tx, vt[i].ty);
    File f = SD.open(path, FILE_READ);
    if (f) {
      int got = 0;
      while (got < kTileBytes) {
        int r = f.read(s->buf + got, kTileBytes - got);
        if (r <= 0) break;
        got += r;
      }
      f.close();
      s->present = (got == kTileBytes);
    }
  }
  spiUnlock();
}

int blitViewport(Adafruit_GFX& g, double centerLat, double centerLon, int z,
                 int originX, int originY, int vW, int vH) {
  VTile vt[12];
  int n = visibleTiles(centerLat, centerLon, z, originX, originY, vW, vH, vt, 12);
  int drawn = 0;
  for (int i = 0; i < n; i++) {
    Slot* s = findSlot(z, vt[i].tx, vt[i].ty);
    if (s && s->present && s->buf) {
      s->lru = ++s_lruCtr;
      g.drawBitmap(vt[i].sx, vt[i].sy, s->buf, kTilePx, kTilePx, GxEPD_BLACK);
      drawn++;
    } else {
      // Fehlende Kachel: dezentes Punktraster, damit „keine Daten" sichtbar ist.
      for (int yy = 8; yy < kTilePx; yy += 16)
        for (int xx = 8; xx < kTilePx; xx += 16)
          g.drawPixel(vt[i].sx + xx, vt[i].sy + yy, GxEPD_BLACK);
    }
  }
  return drawn;
}

}  // namespace maps_tiles
