#pragma once

// =============================================================================
// WifiConfig — central /web/wifi.cfg access (read/connect/save)
//
// File format (FAT32 SD card):
//   line 1: SSID
//   line 2: password
//   line 3: optional static IPv4, e.g. "192.168.1.50". Empty or missing =
//           DHCP (the default). When set, gateway = x.x.x.1 of the /24,
//           netmask 255.255.255.0, DNS = gateway — the sensible home-network
//           defaults; for anything fancier use DHCP reservations instead.
//
// Replaces the previously duplicated read-cfg-and-connect blocks in main.cpp,
// Settingsscreen.h and Simplesettingsscreen.h.
// =============================================================================

#if defined(ESP32) && (defined(MECK_WIFI_COMPANION) || defined(MECK_FILEMGR_STA) || defined(MECK_WEB_READER))

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>

#define MECK_WIFI_CFG_PATH "/web/wifi.cfg"

// Read the config. Returns true if an SSID is present. staticIp is "" = DHCP.
inline bool meckWifiReadConfig(String& ssid, String& pass, String& staticIp) {
  ssid = ""; pass = ""; staticIp = "";
  if (!SD.exists(MECK_WIFI_CFG_PATH)) return false;
  File f = SD.open(MECK_WIFI_CFG_PATH, FILE_READ);
  if (f) {
    ssid = f.readStringUntil('\n'); ssid.trim();
    pass = f.readStringUntil('\n'); pass.trim();
    staticIp = f.readStringUntil('\n'); staticIp.trim();
    f.close();
  }
  digitalWrite(SDCARD_CS, HIGH);
  return ssid.length() > 0;
}

// Write the config (preserves whichever parts the caller didn't change by
// passing the current values).
inline void meckWifiWriteConfig(const String& ssid, const String& pass,
                                const String& staticIp) {
  File f = SD.open(MECK_WIFI_CFG_PATH, FILE_WRITE);
  if (f) {
    f.println(ssid);
    f.println(pass);
    f.println(staticIp);
    f.close();
    Serial.printf("WifiConfig: saved (ssid='%s', ip=%s)\n",
                  ssid.c_str(), staticIp.length() ? staticIp.c_str() : "DHCP");
  }
  digitalWrite(SDCARD_CS, HIGH);
}

// Save new credentials, preserving the static-IP line.
inline void meckWifiSaveCredentials(const String& ssid, const String& pass) {
  String oldSsid, oldPass, staticIp;
  meckWifiReadConfig(oldSsid, oldPass, staticIp);
  meckWifiWriteConfig(ssid, pass, staticIp);
}

// Save the static IP ("" = DHCP), preserving credentials.
inline void meckWifiSaveStaticIp(const String& staticIp) {
  String ssid, pass, oldIp;
  meckWifiReadConfig(ssid, pass, oldIp);
  meckWifiWriteConfig(ssid, pass, staticIp);
}

// Apply the static-IP config (no-op for DHCP). Call before WiFi.begin().
inline void meckWifiApplyStatic(const String& staticIp) {
  if (staticIp.length() == 0) return;  // DHCP
  IPAddress ip;
  if (!ip.fromString(staticIp)) {
    Serial.printf("WifiConfig: invalid static IP '%s' — using DHCP\n", staticIp.c_str());
    return;
  }
  IPAddress gateway(ip[0], ip[1], ip[2], 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(ip, gateway, subnet, gateway /* DNS = gateway */);
  Serial.printf("WifiConfig: static IP %s (gw %s)\n",
                staticIp.c_str(), gateway.toString().c_str());
}

// Full connect from the saved config: STA mode, static IP if configured,
// then wait up to timeoutMs. Returns true when connected.
inline bool meckWifiConnectSaved(unsigned long timeoutMs) {
  String ssid, pass, staticIp;
  if (!meckWifiReadConfig(ssid, pass, staticIp)) {
    Serial.println("WifiConfig: no /web/wifi.cfg (configure in Settings)");
    return false;
  }
  WiFi.mode(WIFI_STA);
  meckWifiApplyStatic(staticIp);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long deadline = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    yield();
    delay(100);
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.printf("WifiConfig: '%s' %s%s\n", ssid.c_str(),
                ok ? "connected, IP " : "connect failed",
                ok ? WiFi.localIP().toString().c_str() : "");
  return ok;
}

#endif // ESP32 && (companion || filemgr-sta || web-reader)
