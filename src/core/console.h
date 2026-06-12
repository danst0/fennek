// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// console.h — Serial-Debug-Konsole (USB-CDC, 115200).
//
// Non-blocking Zeilenpuffer, gepollt aus loop(). Primär zum Testen des Mesh
// ohne Display-Interaktion; Antworten mit "[CON]"-Präfix. `help` listet die
// Befehle. Senden-Befehle initialisieren das Mesh bei Bedarf automatisch.
// =============================================================================
#pragma once

namespace console {

// Pro Loop-Durchlauf aufrufen; verarbeitet vollständige Zeilen.
void poll();

}  // namespace console
