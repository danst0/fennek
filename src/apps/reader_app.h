// =============================================================================
// reader_app.h — TXT/EPUB-Reader als App.
//
// /books listet .txt und .epub — rekursiv bis Tiefe 2, damit Calibre-
// Bibliotheken (/books/Autor/Titel/Buch.epub) gefunden werden. EPUBs werden
// beim ersten Öffnen einmalig nach /books/.epub_cache/<crc>_<name>.txt
// konvertiert (inkrementell, Fortschritt auf dem Screen, abbrechbar), danach
// wie TXT gelesen. Die Leseposition pro Buch liegt im NVS; Blättern per Touch
// (rechte/linke Hälfte) und Tastatur.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace reader_app {

App* get();

// Konsolen-Diagnose: /books neu scannen und Treffer auf Serial dumpen.
void debugScan();

}  // namespace reader_app
