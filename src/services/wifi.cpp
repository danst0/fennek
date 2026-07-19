// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "wifi.h"

#include "core/settings.h"
#include "services/audio.h"
#include "apps/mesh_client.h"

#include <Arduino.h>
#include <WiFi.h>

namespace wifi {

bool pickBest(char* ssid, size_t ns, char* pass, size_t np) {
  if (ns) ssid[0] = '\0';
  if (np) pass[0] = '\0';
  int cnt = settings::wifiCount();
  if (cnt == 0) return false;

  WiFi.persistent(false);          // WiFi-Lib nicht selbst ins NVS schreiben lassen
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();  // blockierend (~1-3 s); Audio/Mesh ruhen bereits

  int best = -1, bestRssi = -1000;
  for (int i = 0; i < found; i++) {
    String s = WiFi.SSID(i);
    for (int p = 0; p < cnt; p++) {
      char ps[33];
      settings::wifiSsidAt(p, ps, sizeof(ps));
      if (ps[0] && s == ps) {
        int r = WiFi.RSSI(i);
        if (r > bestRssi) { bestRssi = r; best = p; }
        break;
      }
    }
  }
  if (found > 0) WiFi.scanDelete();

  if (best < 0) {
    // Kein Scan-Treffer (Netz gerade nicht sichtbar, versteckte SSID oder
    // Scan-Fehler) → Slot 0 als Rückfall, damit ein einzelnes bekanntes Netz
    // weiter funktioniert.
    best = 0;
    Serial.println("[WIFI] Kein Scan-Treffer, nutze Slot 0");
  } else {
    char sel[33];
    settings::wifiSsidAt(best, sel, sizeof(sel));
    Serial.printf("[WIFI] Bekanntes Netz gewählt: %s (%d dBm)\n", sel, bestRssi);
  }

  settings::wifiSsidAt(best, ssid, ns);
  settings::wifiPassAt(best, pass, np);
  return ssid[0] != '\0';
}

bool connect(uint32_t timeoutMs) {
  audio::stop();
  mesh_client::setSuspended(true);

  char ssid[33], pass[65];
  if (!pickBest(ssid, sizeof(ssid), pass, sizeof(pass))) {
    Serial.println("[WIFI] Kein WLAN konfiguriert");
    WiFi.mode(WIFI_OFF);
    mesh_client::setSuspended(false);
    return false;
  }

  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connect-Timeout");
    disconnect();
    return false;
  }
  // Modem-Sleep aus: sonst schläft das WiFi zwischen den DTIM-Beacons und
  // Downloads (Calibre/Podcast) brechen auf ~15 KB/s ein. Fenster ist kurz und
  // Audio ruht bereits.
  WiFi.setSleep(false);
  return true;
}

void disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mesh_client::setSuspended(false);
}

}  // namespace wifi
