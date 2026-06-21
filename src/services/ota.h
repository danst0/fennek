// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// ota.h — OTA-Firmware-Update (Pull von URL).
//
// Quelle = GitHub-Releases-API von danst0/fennek (dient direkt als Manifest)
// oder ein selbst gehostetes JSON-Manifest. Geschrieben wird in den inaktiven
// OTA-Slot (app0/app1) im internen QSPI-Flash — der geteilte HSPI-Bus (SD/
// E-Ink/LoRa) ist NICHT betroffen. Beim WLAN-Betrieb ist Audio ohnehin
// gestoppt und die Mesh-Pumpe suspendiert (webfm-Regel).
//
// check()/apply() setzen verbundenes WLAN voraus (z. B. webfm RUNNING). Die
// console*-Helfer bringen WLAN selbst hoch (headless, ohne Display).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ota {

struct CheckResult {
  bool ok          = false;   // Manifest geladen + geparst
  bool updateAvail = false;   // latest != current
  char current[16] = "";      // FENNEK_VERSION
  char latest[16]  = "";      // tag_name aus dem Release
  char url[200]    = "";      // browser_download_url der firmware.bin
  char err[80]     = "";      // Fehlertext (gültig wenn !ok)
};

// Liest das Manifest unter manifestUrl (GitHub-API: tag_name + erste .bin-Asset;
// Fallback: "version"/"url"). WLAN muss verbunden sein.
CheckResult check(const char* manifestUrl);

// Lädt die Firmware von downloadUrl (302 wird selbst aufgelöst) und flasht sie
// in den inaktiven OTA-Slot. true = Erfolg — der Aufrufer rebootet danach
// selbst (apply rebootet bewusst NICHT). WLAN muss verbunden sein.
bool apply(const char* downloadUrl, char* errOut, size_t errCap);

// Headless für die Konsole: WLAN hoch (Audio stop + Mesh suspend), prüfen,
// Status nach Serial, WLAN wieder runter. Kein Flashen.
void consoleCheck();

// Headless für die Konsole: prüfen und ggf. flashen + Reboot. force = ohne
// Versionsvergleich flashen (Re-Flash/Downgrade).
void consoleUpdate(bool force);

// --- On-Device-Pfad (Settings-App, blockierend mit UI-Fortschritt) -----------
// UI-Fortschritts-Callback: phase = Kurztext ("Verbinde WLAN" …), pct = 0..100
// oder <0 für „unbestimmt" (keine Prozentzahl). Wird synchron/blockierend aus
// deviceCheck()/deviceApply() heraus aufgerufen — der Handler darf das E-Ink
// aktualisieren (während des WLAN-Betriebs ruhen Audio + Mesh, der SPI-Bus ist
// frei). Beide Funktionen sind blockierend und laufen nur aus loop()-Kontext
// (Core 1), nie aus dem Audio-Task — wie der Konsolen-Pfad.
typedef void (*ProgressFn)(const char* phase, int pct);

// Bringt WLAN hoch, prüft das Manifest und schaltet WLAN wieder ab. Setzt
// WLAN-Zugangsdaten (NVS) voraus und scheitert, wenn webfm bereits läuft.
// progress (optional) bekommt die Phasen ("Verbinde WLAN"/"Pruefe Version").
CheckResult deviceCheck(ProgressFn progress);

// Bringt WLAN hoch und flasht die Firmware unter downloadUrl (Fortschritt via
// progress, 0..100 %). Bei Erfolg Reboot (kehrt NICHT zurück); bei Fehler WLAN
// runter, false und errOut trägt den Grund.
bool deviceApply(const char* downloadUrl, ProgressFn progress, char* errOut, size_t errCap);

}  // namespace ota
