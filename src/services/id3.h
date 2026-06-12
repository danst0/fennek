// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// id3.h — minimaler ID3-Tag-Reader (Titel/Künstler/Album).
//
// Unterstützt ID3v2.2/2.3/2.4 (Frames TIT2/TPE1/TALB bzw. TT2/TP1/TAL) mit
// Latin-1-, UTF-16- und UTF-8-Encodings sowie ID3v1 als Fallback. Ergebnisse
// werden als UTF-8 geliefert (Anzeige konvertiert via gui::toCp437).
//
// Frame-Payloads werden übersprungen (seek), nicht gelesen — auch Dateien mit
// großem Cover-Art-Frame (APIC) scannen schnell.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace id3 {

struct Tags {
  char title[64];
  char artist[48];
  char album[48];
  uint32_t fsize;   // Dateigröße (0 = Datei ließ sich nicht öffnen) — der
                    // readdir-Scan der Bibliothek kennt keine Größen, sie
                    // werden hier beim ohnehin nötigen Öffnen mitgeliefert.
};

// Tags aus einer Audio-Datei lesen. Nimmt spiLock() selbst.
// true, wenn mindestens ein Feld gefunden wurde (leere Felder = "").
bool read(const char* path, Tags& out);

}  // namespace id3
