// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// maps_app.h — Karten als App (Kachel „Karten").
//
// Zeigt gerasterte Karten von der SD (/maps/{z}/{x}/{y}.bin, s. services/
// maps_tiles) mit der eigenen GPS-Position als Marker. Das GPS-Modul läuft nur,
// solange die App vorn ist (onEnter/onLeave → services/gps → board::gpsPower).
// Standard: Karte folgt der Position; A/D/W/S pannen, +/- zoomen, 0 re-zentriert.
// Bei gültigem Fix wird gedrosselt settings::setMeshPos() aktualisiert (speist
// die Mesh-Standortbake). Kacheln vorab am PC mit tools/maps_tiles.py erzeugen.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace maps_app {

App* get();

}  // namespace maps_app
