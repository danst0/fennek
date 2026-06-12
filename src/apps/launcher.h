// =============================================================================
// launcher.h — Homescreen mit App-Kacheln.
//
// Die erste beim appmgr registrierte App; Tap auf eine Kachel startet die
// zugehörige App. Kacheln ohne App (nullptr) sind Platzhalter ("bald").
// =============================================================================
#pragma once

#include "core/appmgr.h"
#include "core/i18n.h"

namespace launcher {

App* get();

// Kachel idx (0..5) belegen. app == nullptr -> deaktivierter Platzhalter.
// Label als String-ID, damit Kacheln bei Sprachwechsel sofort umschalten.
void setTile(int idx, i18n::Str label, App* app);

}  // namespace launcher
