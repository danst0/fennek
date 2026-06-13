// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// notes_app.h — Notizen als App (Tagesnotiz-Modell).
//
// Genau eine Notiz pro Tag, benannt nach dem Datum (/notes/YYYY-MM-DD.md).
// "+ Heute" öffnet immer die heutige Notiz — legt sie an, falls noch nicht da,
// sonst schreibt man unten weiter. Liste zeigt alle Tage (neueste oben);
// Editor im Anhänge-Modus (tippen / Backspace / Enter). Ohne SD-Karte erscheint
// "Keine SD-Karte" + "Erneut suchen" wie in Musik/Lesen. Datum kommt aus der
// lokalen Systemzeit (timesync), Speicher gechunkt unter spiLock auf der SD.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace notes_app {

App* get();

// Konsolen-Diagnose (`notes`): SD-Roundtrip (schreiben/lesen) + /notes-Scan testen.
void debugSmoke();

}  // namespace notes_app
