// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// todo_app.h — "Todo": Reinschrift-Aufgaben (Nextcloud/WebDAV). Liste, abhaken,
// Quick-Add, heute/morgen, Sync. Engine in services/reinschrift, Parser +
// konfliktsicherer Merge in reinschrift_core.h (host-getestet).
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace todo_app {
App* get();
}  // namespace todo_app
