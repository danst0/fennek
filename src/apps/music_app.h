// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// music_app.h — MP3-Player als App (Browser + Now-Playing).
//
// Port des alten ui.cpp in das App-Framework: hierarchischer Browser
// (Künstler/Album/Playlist) mit Anfangsbuchstaben-Gruppierung, Player-Screen
// mit partiellem Fortschritts-Refresh.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace music_app {

App* get();

}  // namespace music_app
