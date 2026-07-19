// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "ota.h"
#include "config.h"
#include "services/audio.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"
#include "core/settings.h"
#include "services/wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

namespace {

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kHttpTimeoutMs    = 15000;
constexpr uint32_t kReadTimeoutMs    = 8000;

// Aktiver UI-Fortschritts-Callback des On-Device-Pfads (nur gesetzt, solange
// deviceApply() flasht). Der captureless Update.onProgress-Trampolin speist hier
// ein. nullptr im Konsolen-/Web-Pfad (dort nur Serial).
ota::ProgressFn s_uiCb = nullptr;

// --- WLAN auf/zu (zentraler Helfer: Scan → stärkstes bekanntes Netz) ------------
// ssid/pass werden ignoriert; wifi::connect wählt selbst aus den Profilen.
bool wifiUp(const char* ssid, const char* pass) {
  (void)ssid; (void)pass;
  return wifi::connect(kConnectTimeoutMs);
}

void wifiDown() { wifi::disconnect(); }

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Wert des JSON-String-Schlüssels "key" aus body extrahieren + entescapen.
// Minimal-Parser (kein ArduinoJson; gleiche Logik wie notes_ai::jsonExtractString).
bool jsonExtractString(const char* body, const char* key, char* out, size_t cap) {
  char needle[40];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(body, needle);
  if (!p) return false;
  p += strlen(needle);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return false;
  p++;                                   // hinter das öffnende "
  size_t o = 0;
  while (*p && o + 2 < cap) {
    char c = *p++;
    if (c == '"') break;                 // unescaptes " = Stringende
    if (c == '\\') {
      char e = *p++;
      switch (e) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case '/': out[o++] = '/';  break;
        case '"': out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case 'u': {
          int h0 = hexVal(p[0]), h1 = hexVal(p[1]), h2 = hexVal(p[2]), h3 = hexVal(p[3]);
          if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { out[o] = '\0'; return o > 0; }
          uint32_t cp = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3; p += 4;
          if (cp < 0x80) out[o++] = (char)cp;   // OTA-URLs sind reines ASCII
          break;
        }
        default: out[o++] = e; break;
      }
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return true;
}

// Erste "browser_download_url", die auf .bin endet (GitHub-Release-Asset).
bool findBinUrl(const char* body, char* out, size_t cap) {
  const char* key = "\"browser_download_url\"";
  for (const char* p = strstr(body, key); p; p = strstr(p + 1, key)) {
    char tmp[200];
    if (jsonExtractString(p, "browser_download_url", tmp, sizeof(tmp))) {
      size_t l = strlen(tmp);
      if (l >= 4 && strcasecmp(tmp + l - 4, ".bin") == 0) {
        strncpy(out, tmp, cap - 1);
        out[cap - 1] = '\0';
        return true;
      }
    }
  }
  return false;
}

// HTTP-GET, Body nach out (gechunkt gelesen wie notes_ai). true = HTTP 200 + Body.
bool httpGetBody(const char* url, char* out, size_t cap) {
  bool https = strncmp(url, "https", 5) == 0;
  HTTPClient http;
  WiFiClientSecure cs; WiFiClient cp;
  bool begun;
  if (https) { cs.setInsecure(); begun = http.begin(cs, url); }
  else       { begun = http.begin(cp, url); }
  if (!begun) return false;
  http.setUserAgent("fennek-ota");                       // GitHub verlangt User-Agent
  http.addHeader("Accept", "application/vnd.github+json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  int code = http.GET();
  if (code != 200) { Serial.printf("[OTA] Manifest HTTP %d\n", code); http.end(); return false; }

  WiFiClient* st = http.getStreamPtr();
  size_t total = 0; uint32_t last = millis();
  while (total < cap - 1) {
    int av = st->available();
    if (av > 0) {
      int want = (size_t)av < cap - 1 - total ? av : (int)(cap - 1 - total);
      int rd = st->readBytes((uint8_t*)out + total, want);
      if (rd > 0) { total += rd; last = millis(); }
    } else {
      if (!http.connected() && st->available() == 0) break;
      if (millis() - last > kReadTimeoutMs) break;
      delay(5);
    }
  }
  out[total] = '\0';
  http.end();
  return total > 0;
}

// Folgt EINEM Redirect manuell: liest den Location-Header, ohne den (großen)
// Body herunterzuladen. Bei direktem 200 ist die URL bereits final.
bool resolveRedirect(const char* url, char* out, size_t cap) {
  bool https = strncmp(url, "https", 5) == 0;
  HTTPClient http;
  WiFiClientSecure cs; WiFiClient cp;
  bool begun;
  if (https) { cs.setInsecure(); begun = http.begin(cs, url); }
  else       { begun = http.begin(cp, url); }
  if (!begun) return false;
  http.setUserAgent("fennek-ota");
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* hdrs[] = { "Location" };
  http.collectHeaders(hdrs, 1);
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kConnectTimeoutMs);
  int code = http.GET();
  bool ok = false;
  if (code >= 300 && code < 400 && http.hasHeader("Location")) {
    String loc = http.header("Location");
    strncpy(out, loc.c_str(), cap - 1);
    out[cap - 1] = '\0';
    ok = out[0] != '\0';
  } else if (code == 200) {
    strncpy(out, url, cap - 1);          // kein Redirect — URL ist schon final
    out[cap - 1] = '\0';
    ok = true;
  } else {
    Serial.printf("[OTA] Redirect-GET HTTP %d\n", code);
  }
  http.end();
  return ok;
}

// Semantischer Versionsvergleich "vX.Y.Z". true = a ist strikt neuer als b.
bool semverGt(const char* a, const char* b) {
  const char* pa = (*a == 'v' || *a == 'V') ? a + 1 : a;
  const char* pb = (*b == 'v' || *b == 'V') ? b + 1 : b;
  int amaj = 0, amin = 0, apch = 0, bmaj = 0, bmin = 0, bpch = 0;
  sscanf(pa, "%d.%d.%d", &amaj, &amin, &apch);
  sscanf(pb, "%d.%d.%d", &bmaj, &bmin, &bpch);
  if (amaj != bmaj) return amaj > bmaj;
  if (amin != bmin) return amin > bmin;
  return apch > bpch;
}

// Fortschritt nach Serial (captureless — als Funktionszeiger für Update.onProgress).
void otaProgress(size_t done, size_t total) {
  static int last = -1;
  int pct = total ? (int)(done * 100 / total) : 0;
  if (pct != last && (pct % 10 == 0 || pct == 100)) {
    Serial.printf("[OTA] %d%%\n", pct);
    if (s_uiCb) s_uiCb("Installiere Firmware", pct);   // E-Ink: 10%-Schritte, Ghosting-arm
    last = pct;
  }
}

}  // namespace

namespace ota {

CheckResult check(const char* manifestUrl) {
  CheckResult r;
  snprintf(r.current, sizeof(r.current), "%s", FENNEK_VERSION);
  if (!manifestUrl || !manifestUrl[0]) {
    snprintf(r.err, sizeof(r.err), "keine Update-URL konfiguriert");
    return r;
  }

  // GitHub-Release-JSON ist groß; tag_name + assets stehen vor den Release-Notes,
  // ein Abschnitt reicht. Heap-schonend einmalig holen.
  static char body[8192];
  if (!httpGetBody(manifestUrl, body, sizeof(body))) {
    snprintf(r.err, sizeof(r.err), "Manifest nicht erreichbar");
    return r;
  }
  // Version: tag_name (GitHub) oder version (eigenes Manifest).
  if (!jsonExtractString(body, "tag_name", r.latest, sizeof(r.latest)) &&
      !jsonExtractString(body, "version",  r.latest, sizeof(r.latest))) {
    snprintf(r.err, sizeof(r.err), "kein tag_name/version (Release vorhanden?)");
    return r;
  }
  // Firmware-URL: erste .bin-Asset (GitHub) oder url (eigenes Manifest).
  if (!findBinUrl(body, r.url, sizeof(r.url)) &&
      (!jsonExtractString(body, "url", r.url, sizeof(r.url)) || !r.url[0])) {
    snprintf(r.err, sizeof(r.err), "keine firmware.bin im Release");
    return r;
  }
  r.ok = true;
  r.updateAvail = semverGt(r.latest, r.current);
  return r;
}

bool apply(const char* downloadUrl, char* errOut, size_t errCap) {
  auto fail = [&](const char* m) -> bool {
    if (errOut) { strncpy(errOut, m, errCap - 1); errOut[errCap - 1] = '\0'; }
    Serial.printf("[OTA] %s\n", m);
    return false;
  };
  if (!downloadUrl || !downloadUrl[0]) return fail("keine Firmware-URL");

  // 302 selbst auflösen (GitHub -> release-assets.githubusercontent.com); umgeht
  // die wackelige Cross-Host-Redirect-Logik von httpUpdate. Die signierte
  // Asset-URL ist lang (~900 Z. mit X-Amz-/sig-Query) — Puffer großzügig, sonst
  // wird sie abgeschnitten und der Flash-GET liefert 4xx (httpUpdate-Fehler -104).
  char finalUrl[1400];
  if (!resolveRedirect(downloadUrl, finalUrl, sizeof(finalUrl)))
    return fail("Redirect-Aufloesung fehlgeschlagen");
  Serial.printf("[OTA] Lade Firmware: %s\n", finalUrl);

  bool https = strncmp(finalUrl, "https", 5) == 0;
  WiFiClientSecure cs; WiFiClient cp;
  WiFiClient* client;
  if (https) { cs.setInsecure(); client = &cs; } else client = &cp;

  httpUpdate.rebootOnUpdate(false);                       // Reboot steuert der Aufrufer
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  Update.onProgress(otaProgress);

  t_httpUpdate_return ret = httpUpdate.update(*client, finalUrl);
  if (ret == HTTP_UPDATE_OK) {
    Serial.println("[OTA] Update geschrieben — bereit zum Reboot");
    return true;
  }
  char m[80];
  snprintf(m, sizeof(m), "Update-Fehler %d: %s", (int)httpUpdate.getLastError(),
           httpUpdate.getLastErrorString().c_str());
  return fail(m);
}

// --- Headless-Pfad (Konsole) --------------------------------------------------
namespace {
void runConsole(bool doApply, bool force) {
  if (webfm::state() != webfm::State::OFF) {
    Serial.println("[OTA] WLAN/WebFM laeuft bereits — bitte erst stoppen");
    return;
  }
  char ssid[33], pass[65], url[160];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(pass, sizeof(pass));
  settings::otaUrl(url, sizeof(url));
  if (!ssid[0]) { Serial.println("[OTA] Keine WLAN-SSID konfiguriert ('wifi ssid <name>')"); return; }

  Serial.printf("[OTA] Verbinde mit '%s' ...\n", ssid);
  if (!wifiUp(ssid, pass)) { Serial.println("[OTA] WLAN-Verbindung fehlgeschlagen"); wifiDown(); return; }

  CheckResult r = check(url);
  if (!r.ok) { Serial.printf("[OTA] Pruefung fehlgeschlagen: %s\n", r.err); wifiDown(); return; }
  Serial.printf("[OTA] aktuell=%s  neu=%s  %s\n", r.current, r.latest,
                r.updateAvail ? "(Update verfuegbar)" : "(aktuell)");

  if (!doApply) { wifiDown(); return; }
  if (!r.updateAvail && !force) {
    Serial.println("[OTA] Kein Update noetig ('ota force' erzwingt Re-Flash)");
    wifiDown();
    return;
  }

  char err[80] = "";
  if (apply(r.url, err, sizeof(err))) {
    Serial.println("[OTA] Reboot in neue Version ...");
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("[OTA] Fehlgeschlagen: %s\n", err);
    wifiDown();
  }
}
}  // namespace

void consoleCheck()             { runConsole(false, false); }
void consoleUpdate(bool force)  { runConsole(true,  force); }

// --- On-Device-Pfad (Settings-App) -------------------------------------------
CheckResult deviceCheck(ProgressFn progress) {
  CheckResult r;
  snprintf(r.current, sizeof(r.current), "%s", FENNEK_VERSION);
  if (webfm::state() != webfm::State::OFF) {
    snprintf(r.err, sizeof(r.err), "WLAN belegt (Dateien-App?)");
    return r;
  }
  char ssid[33], pass[65], url[160];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(pass, sizeof(pass));
  settings::otaUrl(url, sizeof(url));
  if (!ssid[0]) { snprintf(r.err, sizeof(r.err), "Keine WLAN-SSID"); return r; }

  if (progress) progress("Verbinde WLAN", -1);
  if (!wifiUp(ssid, pass)) {
    snprintf(r.err, sizeof(r.err), "WLAN fehlgeschlagen");
    wifiDown();
    return r;
  }

  if (progress) progress("Pruefe Version", -1);
  r = check(url);          // füllt ok/err/current/latest/url/updateAvail
  wifiDown();
  return r;
}

bool deviceApply(const char* downloadUrl, ProgressFn progress, char* errOut, size_t errCap) {
  auto fail = [&](const char* m) -> bool {
    if (errOut) { strncpy(errOut, m, errCap - 1); errOut[errCap - 1] = '\0'; }
    return false;
  };
  if (!downloadUrl || !downloadUrl[0]) return fail("keine Firmware-URL");
  if (webfm::state() != webfm::State::OFF) return fail("WLAN belegt (Dateien-App?)");

  char ssid[33], pass[65];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(pass, sizeof(pass));
  if (!ssid[0]) return fail("Keine WLAN-SSID");

  if (progress) progress("Verbinde WLAN", -1);
  if (!wifiUp(ssid, pass)) { wifiDown(); return fail("WLAN fehlgeschlagen"); }

  if (progress) progress("Installiere Firmware", 0);
  s_uiCb = progress;                 // otaProgress() speist ab jetzt die UI
  char err[80] = "";
  bool ok = apply(downloadUrl, err, sizeof(err));
  s_uiCb = nullptr;
  if (!ok) { wifiDown(); return fail(err); }

  if (progress) progress("Neustart", 100);
  delay(800);
  ESP.restart();                     // kehrt nicht zurück
  return true;                       // unerreichbar
}

}  // namespace ota
