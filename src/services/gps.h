// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// gps.h — minimaler NMEA-0183-Empfänger für das T-Deck-Pro-GPS (Maps-App).
//
// Liest die NMEA-Sätze des Moduls über UART1 (board::gpsPower richtet RX44/TX43
// ein) und wertet $..GGA (Fix-Qualität, Sats, HDOP, Höhe, Position) sowie
// $..RMC (Position, Geschwindigkeit, UTC-Datum/-Zeit, Gültigkeit) aus. Kein
// TinyGPS++ — bewusst schlank, im Stil von services/id3. Der reine Parser
// (parseLine) ist Arduino-frei und host-testbar.
// =============================================================================
#pragma once

#include <stdint.h>

namespace gps {

struct Fix {
  bool     valid;       // gültiger Fix (RMC-Status 'A' bzw. GGA-Qualität > 0)
  double   lat, lon;    // Dezimalgrad (positiv = N/E)
  float    hdop;        // horizontale Streuung (kleiner = besser)
  float    altM;        // Höhe über Meeresspiegel (m)
  float    speedKmh;    // Geschwindigkeit über Grund
  uint8_t  sats;        // Satelliten im Fix
  uint8_t  quality;     // GGA-Fix-Qualität (0 = keiner, 1 = GPS, 2 = DGPS)
  uint32_t epochUtc;    // UTC aus RMC (Unix-Sekunden; 0 = unbekannt)
};

// GPS-Modul einschalten (board::gpsPower(true)) und Parser-Zustand zurücksetzen.
void begin();
// GPS-Modul ausschalten (board::gpsPower(false)).
void end();
// Verfügbare UART1-Bytes einlesen und vollständige NMEA-Zeilen parsen.
void poll();

// Letzter zusammengeführter Stand.
const Fix& current();
// millis() der zuletzt korrekt empfangenen NMEA-Zeile (0 = noch keine).
uint32_t lastSentenceMs();

// Eine NMEA-Zeile (mit oder ohne CR/LF) in fix einarbeiten. Prüft die
// Checksumme; gibt true zurück, wenn die Zeile gültig und bekannt war.
// Arduino-frei — host-testbar.
bool parseLine(const char* line, Fix& fix);

}  // namespace gps
