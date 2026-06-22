// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// power.h — Standby/Deep-Sleep über den Ausschaltknopf (GPIO0).
//
// Der seitliche User-Button (PIN_USER_BTN = GPIO0) schickt das Gerät per
// Langdruck (~2 s) in Deep Sleep; ein erneuter Druck weckt es (= Voll-Reboot,
// die Resume-Logik in setup() stellt App + Track wieder her). Zusätzlich geht
// das Gerät nach einstellbarer Inaktivität automatisch in Standby
// (settings::standbyMinutes()), aber nie bei laufender Wiedergabe.
//
// E-Ink behält das Bild stromlos — Deep Sleep ist hier der natürliche
// „Aus"-Zustand und bringt die Laufzeit von Stunden auf Tage.
// =============================================================================
#pragma once

#include <stdint.h>

namespace power {

// Knopf-Pin konfigurieren, Aktivitäts-Timer starten.
void begin();

// Vom appmgr bei jeder Nutzereingabe (Tap/Taste) aufrufen — setzt den
// Inaktivitäts-Timer zurück.
void noteActivity();

// Millisekunden seit der letzten Aktivität (Eingabe oder laufende Wiedergabe
// hält den Zähler bei ~0). Dient dem Hauptloop, das Eingabe-Polling im
// Leerlauf zu drosseln (Strom sparen, ohne die Reaktionszeit spürbar zu
// verschlechtern).
uint32_t idleMs();

// Einmal pro appmgr::loop() aufrufen: Knopf (kurz = Tastensperre, lang =
// Standby) + Idle-Timeout. Löst bei Bedarf enterStandby() aus.
void poll();

// Tastensperre aktiv? Solange true ignoriert der appmgr Touch + Tastatur;
// nur der Knopf reagiert (kurz = entsperren, lang = Standby).
bool locked();

// Sofort in Deep-Sleep-Standby gehen (Audio/Radio/Peripherie aus, Wake-Quellen
// = Knopf + Stunden-Timer). Kehrt nicht zurück (Aufwachen ist ein Reboot).
void enterStandby();

// Ganz früh in setup() aufrufen (vor dem USB-Wait): War der Aufwach-Grund der
// Stunden-Timer, läuft ein Minimal-Pfad (Power + I2C + Display, KEIN SD/Audio/
// Apps), der nur die Batterie-% auf dem Schlafbild erneuert und sofort wieder
// in Deep Sleep geht — kehrt dann nicht zurück. Gibt false zurück, wenn normal
// weitergebootet werden soll (Knopf-Wake, Kaltstart oder Knopf wurde während
// des Minimal-Fensters gedrückt).
bool handleTimerWake();

}  // namespace power
