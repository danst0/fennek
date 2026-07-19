// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#pragma once
#include <stddef.h>
#include <stdint.h>

// Zentrale WLAN-Anbindung. Kennt mehrere Profile (settings::wifi*) und wählt
// beim Verbinden per Scan das stärkste in Reichweite befindliche bekannte Netz.
// Ersetzt die früher in jedem Dienst duplizierte WiFi.begin-Sequenz und setzt
// dabei die WLAN-Regel durch (Audio stoppen, Mesh suspendieren).
namespace wifi {

// Scannt (setzt STA-Mode) und liefert das stärkste erreichbare bekannte Netz.
// Findet der Scan kein bekanntes Netz, fällt sie auf Slot 0 zurück (deckt u. a.
// versteckte SSIDs ab). Lässt den STA-Mode aktiv; der Aufrufer verbindet selbst.
// false = kein Profil konfiguriert. Für Dienste mit eigener Verbindungslogik
// (webfm).
bool pickBest(char* ssid, size_t ns, char* pass, size_t np);

// Voller, blockierender Connect nach WLAN-Regel: Audio stoppen, Mesh
// suspendieren, bestes bekanntes Netz wählen und verbinden. true = verbunden.
// Bei Misserfolg wird WLAN wieder ausgeschaltet (disconnect()).
bool connect(uint32_t timeoutMs);

// Gegenstück zu connect(): WLAN aus, Mesh fortsetzen.
void disconnect();

}  // namespace wifi
