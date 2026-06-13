// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// book_app.h — Hörbuch-Player als App.
//
// Ein Hörbuch = ein Ordner unter /audiobooks (Kapitel = Audio-Dateien in
// natürlicher Sortierung; "Kapitel 2" < "Kapitel 10"). Lose Audio-Dateien
// direkt in /audiobooks gelten als Ein-Kapitel-Bücher.
//
// Resume: Bookmark (Datei-Index + Position) pro Buch im NVS, geschrieben alle
// 30 s während der Wiedergabe sowie bei Pause/Verlassen/Eviction. Öffnen eines
// Buchs setzt die Wiedergabe ~5 s vor der gemerkten Stelle fort.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace book_app {

App* get();

}  // namespace book_app
