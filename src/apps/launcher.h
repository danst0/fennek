// =============================================================================
// launcher.h — Homescreen mit App-Kacheln.
//
// Die erste beim appmgr registrierte App; Tap auf eine Kachel startet die
// zugehörige App. Kacheln ohne App (nullptr) sind Platzhalter ("bald").
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace launcher {

App* get();

// Kachel idx (0..5) belegen. app == nullptr -> deaktivierter Platzhalter.
void setTile(int idx, const char* label, App* app);

}  // namespace launcher
