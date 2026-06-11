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

namespace power {

// Knopf-Pin konfigurieren, Aktivitäts-Timer starten.
void begin();

// Vom appmgr bei jeder Nutzereingabe (Tap/Taste) aufrufen — setzt den
// Inaktivitäts-Timer zurück.
void noteActivity();

// Einmal pro appmgr::loop() aufrufen: Langdruck-Erkennung + Idle-Timeout.
// Löst bei Bedarf enterStandby() aus.
void poll();

// Sofort in Deep-Sleep-Standby gehen (Audio/Radio/Peripherie aus, Wake-Quelle
// = Knopf). Kehrt nicht zurück (Aufwachen ist ein Reboot).
void enterStandby();

}  // namespace power
