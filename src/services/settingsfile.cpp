// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "settingsfile.h"

#include "../core/board.h"
#include "../core/settings.h"

#include <Arduino.h>
#include <SD.h>
#include <stdlib.h>

namespace {

const char* kPath = "/fennek.ini";

// INI-Puffer: exportIni schreibt aktuell ~600 B; 2 KB lässt Luft für lange
// Track-/Buchpfade ([state]) und künftige Schlüssel.
constexpr size_t kBufSize = 2048;

}  // namespace

namespace settingsfile {

bool exportToSd() {
  if (!board::sdReady()) return false;

  // Serialisieren (SPI-frei), dann unter dem Lock am Stück schreiben.
  char* buf = (char*)malloc(kBufSize);
  if (!buf) return false;
  size_t len = settings::exportIni(buf, kBufSize);
  if (len == 0) { free(buf); return false; }

  spiLock();
  File f = SD.open(kPath, FILE_WRITE);
  bool ok = false;
  if (f) {
    ok = f.write((const uint8_t*)buf, len) == len;
    f.close();
  }
  spiUnlock();

  free(buf);
  Serial.printf("[FENNEK] settings.ini schreiben: %s (%u B)\n",
                ok ? "ok" : "FEHLER", (unsigned)len);
  return ok;
}

bool importFromSd() {
  if (!board::sdReady()) return false;

  // Komplett unter dem Lock in den RAM lesen; Parsen danach (SPI-frei).
  char* buf = (char*)malloc(kBufSize);
  if (!buf) return false;

  spiLock();
  // exists() vor open(): vermeidet die VFS-Fehlerzeile beim Erst-Boot ohne Datei.
  int rd = -1;
  if (SD.exists(kPath)) {
    File f = SD.open(kPath, FILE_READ);
    if (f && !f.isDirectory()) {
      rd = f.read((uint8_t*)buf, kBufSize - 1);
    }
    if (f) f.close();
  }
  spiUnlock();

  if (rd < 0) { free(buf); return false; }   // Datei fehlt
  buf[rd] = '\0';

  int applied = settings::importIni(buf);
  free(buf);
  if (applied < 0) {
    Serial.println("[FENNEK] settings.ini: Parse-Fehler");
    return false;
  }
  Serial.printf("[FENNEK] settings.ini gelesen: %d Werte uebernommen\n", applied);
  return applied > 0;
}

}  // namespace settingsfile
