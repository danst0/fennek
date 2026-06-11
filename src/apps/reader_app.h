// =============================================================================
// reader_app.h — TXT/EPUB-Reader als App.
//
// /books listet .txt und .epub. EPUBs werden beim ersten Öffnen einmalig in
// /books/.epub_cache/<name>.txt konvertiert (inkrementell, Fortschritt auf
// dem Screen, abbrechbar), danach wie TXT gelesen. Die Leseposition pro Buch
// liegt im NVS; Blättern per Touch (rechte/linke Hälfte) und Tastatur.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace reader_app {

App* get();

}  // namespace reader_app
