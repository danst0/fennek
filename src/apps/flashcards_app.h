// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// flashcards_app.h — "Karteikarten": Deck-Liste + Lern-Screen (Leitner).
// Logik/Parser liegen Arduino-frei in flashcards_core.h (host-getestet).
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace flashcards_app {
App* get();
}  // namespace flashcards_app
