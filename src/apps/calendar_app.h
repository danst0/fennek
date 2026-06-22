// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// calendar_app.h — "Kalender": Agenda anstehender Termine aus abonnierten
// iCal-Feeds (read-only). Engine in services/calendar, Parser in ical_core.h.
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace calendar_app {
App* get();
}  // namespace calendar_app
