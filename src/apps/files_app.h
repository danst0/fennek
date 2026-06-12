// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// files_app.h — App „Dateien": Web-Dateiverwaltung der SD-Karte über WLAN.
//
// Dünner UI-Mantel um services/webfm: WiFi startet erst auf expliziten
// Nutzerwunsch (Enter/Tap, nicht beim Betreten — WiFi kostet Strom) und endet
// beim Verlassen der App. Während der Server läuft, ist Audio gestoppt und
// die Mesh-Pumpe pausiert (SPI-/Core-Konkurrenz, siehe webfm.h).
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace files_app {

App* get();

}  // namespace files_app
