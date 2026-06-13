// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// epubproc.h — EPUB → Klartext (Port aus archive_legacy .../Epubprocessor.h).
//
// Pipeline: EPUB (ZIP) → container.xml → OPF-Spine → Kapitel entpacken →
// XHTML strippen → reines UTF-8 → gecachte .txt unter /books/.epub_cache/.
//
// Anders als das Original läuft die Konvertierung INKREMENTELL (ein Kapitel
// pro convertStep()-Aufruf), damit Touch/Tastatur bedienbar bleiben. Alle
// Schritte nehmen den SPI-Mutex selbst.
//
// Output ist durchgängig UTF-8 (das Original mischte CP437-Bytes hinein);
// die Anzeige konvertiert erst beim Rendern via gui::toCp437.
// =============================================================================
#pragma once

#include <stdint.h>

namespace epubproc {

// Cache-Pfad zu einem .epub bestimmen (/books/x.epub -> /books/.epub_cache/x.txt).
// Legt das Cache-Verzeichnis bei Bedarf an.
void buildCachePath(const char* epubPath, char* cachePath, int cachePathSize);

// Konvertierung starten. true = bereit für convertStep()-Aufrufe
// (oder Cache existiert schon — dann ist done() sofort true).
bool convertBegin(const char* epubPath, const char* txtPath);

// Ein Kapitel verarbeiten. true solange weitere Schritte nötig sind.
bool convertStep();

bool convertDone();       // fertig (Erfolg)?
bool convertFailed();
int  convertPercent();    // 0..100
void convertCancel();     // abbrechen + unfertige .txt löschen

}  // namespace epubproc
