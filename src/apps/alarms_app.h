// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// alarms_app.h — Wecker als App (Kachel „Wecker").
//
// UI über die Wecker-Engine (services/alarmclock): Liste der vier Wecker-Slots
// mit Zeit + Wiederholungsmuster, ein einfacher Editor (Stunde/Minute/Tage/Aktiv)
// und der Klingel-Bildschirm. Klingelt ein Wecker, holt background() die App in
// den Vordergrund (auch aus einer anderen App), damit Schlummer/Stop greifbar
// sind. Die eigentliche Auslösung + Deep-Sleep-Weckung liegt in der Engine bzw.
// in core/power; diese App ist nur die Bedienoberfläche.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace alarms_app {

App* get();

}  // namespace alarms_app
