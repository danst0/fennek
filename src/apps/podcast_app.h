// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// podcast_app.h — "Podcast" (Launcher-Kachel, Seite 2): Feed-Liste + Player.
//
// Abos liegen in /podcasts/feeds.txt (per WebFM editierbar), je Feed wird die
// neueste Folge geladen und behalten (services/podcast). Wiedergabe über die
// Audio-Queue (Owner::Podcast), Resume-Position via NVS-Bookmark wie book_app.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace podcast_app {
App* get();
}
