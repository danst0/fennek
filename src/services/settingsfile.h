// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// settingsfile.h — Spiegel der Einstellungen als /fennek.ini auf der SD-Karte.
//
// Dünne SD-Schicht über settings::exportIni()/importIni(): liest/schreibt die
// Datei unter spiLock() (Disziplin wie webfm), das (De-)Serialisieren selbst
// bleibt SPI-frei in settings.cpp. Ohne SD geben beide still false zurück.
//
// Boot: main.cpp ruft importFromSd() nach dem SD-Mount (SD ist autoritativ).
// Standby: power::enterStandby() ruft exportToSd() (hält die Datei aktuell).
// Konsole: 'settings save'/'load'.
// =============================================================================
#pragma once

namespace settingsfile {

// Schreibt alle Einstellungen nach /fennek.ini. false = keine SD / Fehler.
bool exportToSd();

// Liest /fennek.ini ins NVS zurück. false = keine SD / Datei fehlt / Fehler.
bool importFromSd();

}  // namespace settingsfile
