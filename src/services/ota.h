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

}  // namespace ota
