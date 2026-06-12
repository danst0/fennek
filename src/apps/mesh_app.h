// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// mesh_app.h — Mesh-UI (Public-Channel, Direktnachrichten, Kontakte).
//
// Schlanke E-Ink-Oberfläche über mesh_client. Das Radio wird beim ersten
// Betreten der App initialisiert (spart Strom, solange Mesh ungenutzt ist);
// danach läuft der Mesh-Loop dauerhaft im Hintergrund weiter — Nachrichten
// kommen auch an, während man Musik hört oder liest.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace mesh_app {

App* get();

}  // namespace mesh_app
