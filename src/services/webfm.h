// =============================================================================
// webfm.h — Web-Dateiverwaltung: WLAN (Station) + HTTP-Server für die SD.
//
// Verbindet sich mit dem Heim-WLAN (SSID/Passwort aus settings::wifiSsid/
// wifiPass, Eingabe über die Serial-Konsole: "wifi ssid/pass ...") und serviert
// eine kleine HTML-Seite (webfm_html.h) plus JSON-API zum Auflisten, Hoch-/
// Runterladen und Löschen von SD-Dateien.
//
// Architektur-Regeln (CLAUDE.md):
//  - Beim Start wird Audio gestoppt und die Mesh-Pumpe pausiert: der WiFi-Task
//    (Core 0, hohe Prio) würde den Audio-Task verdrängen, und Dateitransfers
//    konkurrieren um den SPI-Bus.
//  - Alle SD-Zugriffe der HTTP-Handler laufen gechunkt unter spiLock();
//    Netz-I/O passiert grundsätzlich NACH spiUnlock(), damit E-Ink/UI nie
//    hinter einem langsamen TCP-Send verhungern.
//  - poll() im UI-Thread aufrufen (tick() der Dateien-App bzw. Konsole) —
//    nie aus einem Draw-Kontext (Handler nehmen selbst spiLock!).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace webfm {

enum class State : uint8_t {
  OFF,         // WiFi aus
  CONNECTING,  // WiFi.begin() läuft (Timeout 15 s)
  RUNNING,     // verbunden, Server nimmt Requests an
  FAILED,      // Verbindung fehlgeschlagen (WiFi wieder aus)
};

// WiFi + Server starten. false = keine SSID konfiguriert. Stoppt Audio und
// pausiert die Mesh-Pumpe. Idempotent solange CONNECTING/RUNNING.
bool start();

// Server + WiFi aus; Mesh-Pumpe läuft wieder (Audio bleibt gestoppt).
void stop();

// Statusmaschine + server.handleClient() — pro Loop-Durchlauf aufrufen.
void poll();

State state();
void  ipStr(char* out, size_t n);       // "192.168.x.y" (nur RUNNING, sonst "")
void  ssid(char* out, size_t n);        // konfigurierte SSID
uint32_t requestCount();                // bediente HTTP-Requests seit start()

}  // namespace webfm
