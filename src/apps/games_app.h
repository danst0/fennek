// =============================================================================
// games_app.h — "Spiele": Menü-Hülle für 2048, Minensucher, Schach, Tic-Tac-Toe.
//
// Eine registrierte App mit internen Screens (Muster wie music_app/mesh_app);
// die Spiel-Module (game2048.*, mines.*, chess.*, ttt.*) liefern enter/
// handleInput/draw und werden von hier dispatcht. Spielstände leben in
// Statics und überleben App-Wechsel; Schach persistiert zusätzlich ins NVS.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace games_app {

App* get();

// Zurück ins Spiele-Menü (rufen die Spiele bei Backspace auf).
void showMenu();

}  // namespace games_app
