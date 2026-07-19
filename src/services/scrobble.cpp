// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "scrobble.h"
#include "config.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/audio.h"
#include "services/library.h"
#include "services/timesync.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"
#include "services/wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <mbedtls/md5.h>
#include <string.h>

namespace scrobble { int pendingCount(); }   // vorwärts (in flushCore genutzt)

namespace {
using scrobble::pendingCount;

// --- RAM-Queue ----------------------------------------------------------------
constexpr int  kMax       = 64;
const char*    kFile      = "/.fennek/scrobbles.tsv";
constexpr uint32_t kSaveThrottleMs = 5000;
constexpr uint32_t kSaneEpoch      = 1500000000UL;  // ~2017; darunter Uhr ungültig

struct Entry {
  char     path[TRACK_PATH_LEN];
  uint32_t startEpoch;     // UTC, 0/ungültig = Server-Empfangszeit nutzen
  uint16_t playedSec;
  uint16_t durSec;
};

Entry*            s_ring   = nullptr;   // PSRAM, kMax Einträge (FIFO)
int               s_count  = 0;
bool              s_dirty  = false;
uint32_t          s_lastSaveMs = 0;
SemaphoreHandle_t s_mutex  = nullptr;

// Back-off für den Pre-Standby-Upload (RTC-RAM überlebt Deep Sleep), analog zu
// services/timesync.cpp.
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
constexpr uint32_t kBackoffBaseSecs = 1800;        // 30 min nach 1. Fehlschlag
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600; // Deckel 24 h
constexpr uint8_t  kBackoffMaxShift = 6;

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint16_t kHttpTimeoutMs    = 8000;

// --- kleine Helfer ------------------------------------------------------------
void md5hex(const char* in, char out[33]) {
  unsigned char d[16];
  mbedtls_md5_ret((const unsigned char*)in, strlen(in), d);
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < 16; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 0xF]; }
  out[32] = '\0';
}

// RFC-3986-Encode für Query-Werte (alles außer unreserved → %XX).
String urlEncode(const char* s) {
  String o; o.reserve(strlen(s) * 3 / 2 + 4);
  static const char* hx = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      o += c;
    } else {
      o += '%'; o += hx[(c >> 4) & 0xF]; o += hx[c & 0xF];
    }
  }
  return o;
}

// Basis-URL ohne abschließenden '/'.
void baseUrl(char* out, size_t n) {
  settings::navUrl(out, n);
  size_t l = strlen(out);
  while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
}

// Subsonic-Auth-Query (Token-Verfahren: t = md5(passwort + salt)).
String authQuery(const char* user, const char* pass) {
  // Salt aus Uhr + millis (muss nur vorhanden sein, nicht kryptografisch stark).
  char salt[24];
  snprintf(salt, sizeof(salt), "%08lx%08lx",
           (unsigned long)timesync::now(), (unsigned long)millis());
  char tok[160];   // Passwort (bis 64) + Salt (16) + Reserve; NICHT zu klein!
  snprintf(tok, sizeof(tok), "%s%s", pass, salt);
  char t[33]; md5hex(tok, t);
  String q = "u=" + urlEncode(user) + "&t=" + t + "&s=" + salt +
             "&v=1.16.1&c=fennek&f=json";
  return q;
}

bool bodyOk(const String& body) { return body.indexOf("\"status\":\"ok\"") >= 0; }

// HTTP-GET (http + https via setInsecure). true = HTTP 200, body gefüllt.
bool httpGet(const String& fullUrl, String& body) {
  bool https = fullUrl.startsWith("https");
  HTTPClient http;
  bool begun;
  WiFiClientSecure cs;
  WiFiClient       cp;
  if (https) { cs.setInsecure(); begun = http.begin(cs, fullUrl); }
  else       { begun = http.begin(cp, fullUrl); }
  if (!begun) return false;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  if (code != 200) Serial.printf("[SCROBBLE] HTTP %d\n", code);
  return code == 200;
}

// Letztes Pfadsegment ohne Endung als Titel-Fallback (wenn nicht in Library).
void basenameTitle(const char* path, char* out, size_t n) {
  const char* slash = strrchr(path, '/');
  const char* base = slash ? slash + 1 : path;
  strncpy(out, base, n - 1);
  out[n - 1] = '\0';
  char* dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}

// Erste Song-ID aus einer search3-Antwort herausziehen (Minimal-Parse, kein
// ArduinoJson). Nimmt das erste Ergebnis — keine Fuzzy-Artist-Prüfung.
bool extractSongId(const String& body, char* id, size_t n) {
  int sp = body.indexOf("\"song\"");
  if (sp < 0) return false;
  int idp = body.indexOf("\"id\":\"", sp);
  if (idp < 0) return false;
  idp += 6;
  int end = body.indexOf('"', idp);
  if (end < 0) return false;
  String s = body.substring(idp, end);
  strncpy(id, s.c_str(), n - 1);
  id[n - 1] = '\0';
  return id[0] != '\0';
}

// search3 → Song-ID auflösen.
bool resolveId(const char* base, const char* user, const char* pass,
               const char* title, const char* artist, char* id, size_t idN) {
  String q = String(title);
  if (artist && artist[0]) { q += " "; q += artist; }
  String url = String(base) + "/rest/search3.view?" + authQuery(user, pass) +
               "&songCount=10&artistCount=0&albumCount=0&query=" + urlEncode(q.c_str());
  String body;
  if (!httpGet(url, body) || !bodyOk(body)) return false;
  return extractSongId(body, id, idN);
}

// /rest/scrobble mit submission=true.
bool doScrobble(const char* base, const char* user, const char* pass,
                const char* id, uint32_t startEpoch) {
  String url = String(base) + "/rest/scrobble.view?" + authQuery(user, pass) +
               "&id=" + urlEncode(id) + "&submission=true";
  if (startEpoch >= kSaneEpoch) {
    // Subsonic erwartet die Abspielzeit in Millisekunden seit Epoche.
    char ms[24];
    snprintf(ms, sizeof(ms), "%llu", (unsigned long long)startEpoch * 1000ULL);
    url += "&time="; url += ms;
  }
  String body;
  return httpGet(url, body) && bodyOk(body);
}

// Einen Eintrag hochladen: Metadaten auflösen → ID suchen → scrobbeln.
bool uploadOne(const Entry& e, const char* base, const char* user, const char* pass) {
  char title[64] = "", artist[48] = "";
  int idx = library::indexOfPath(e.path);
  if (idx >= 0) {
    library::name(idx, title, sizeof(title));
    library::trackArtist(idx, artist, sizeof(artist));
  } else {
    basenameTitle(e.path, title, sizeof(title));
  }
  if (!title[0]) return false;
  char id[64];
  if (!resolveId(base, user, pass, title, artist, id, sizeof(id))) return false;
  return doScrobble(base, user, pass, id, e.startEpoch);
}

// --- SD-Persistenz (immer unter spiLock; nie aus dem Audio-Task) --------------
void saveToSd() {
  if (!board::sdReady()) return;
  // Snapshot unter Mutex, danach SD-I/O ohne den Queue-Mutex zu halten.
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  int n = s_count;
  Entry* snap = nullptr;
  if (n > 0) {
    snap = (Entry*)malloc(sizeof(Entry) * n);
    if (snap) memcpy(snap, s_ring, sizeof(Entry) * n);
    else n = 0;
  }
  s_dirty = false;
  xSemaphoreGive(s_mutex);

  spiLock();
  SD.mkdir("/.fennek");
  if (n == 0) {
    SD.remove(kFile);
  } else {
    File f = SD.open(kFile, FILE_WRITE);
    if (f) {
      for (int i = 0; i < n; i++) {
        f.printf("%lu\t%u\t%u\t%s\n", (unsigned long)snap[i].startEpoch,
                 (unsigned)snap[i].playedSec, (unsigned)snap[i].durSec, snap[i].path);
      }
      f.close();
    }
  }
  spiUnlock();
  if (snap) free(snap);
}

void loadFromSd() {
  if (!board::sdReady()) return;
  spiLock();
  File f = SD.open(kFile, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_count = 0;
  char line[TRACK_PATH_LEN + 48];
  for (;;) {
    spiLock();
    int len = f.available() ? f.readBytesUntil('\n', line, sizeof(line) - 1) : -1;
    spiUnlock();
    if (len < 0) break;
    line[len] = '\0';
    if (!line[0] || s_count >= kMax) { if (s_count >= kMax) break; else continue; }
    // startEpoch \t playedSec \t durSec \t path
    char* save = nullptr;
    char* a = strtok_r(line, "\t", &save);
    char* b = strtok_r(nullptr, "\t", &save);
    char* c = strtok_r(nullptr, "\t", &save);
    char* p = strtok_r(nullptr, "\t\r\n", &save);
    if (!a || !b || !c || !p) continue;
    Entry& e = s_ring[s_count];
    e.startEpoch = (uint32_t)strtoul(a, nullptr, 10);
    e.playedSec  = (uint16_t)atoi(b);
    e.durSec     = (uint16_t)atoi(c);
    strncpy(e.path, p, TRACK_PATH_LEN - 1);
    e.path[TRACK_PATH_LEN - 1] = '\0';
    s_count++;
  }
  xSemaphoreGive(s_mutex);

  spiLock();
  f.close();
  spiUnlock();
  if (s_count) Serial.printf("[SCROBBLE] %d offene Eintraege geladen\n", s_count);
}

// Kern beider Flush-Wege. useBackoff nur für den Pre-Standby-Pfad.
bool flushCore(bool useBackoff, char* logMsg, size_t logN) {
  auto setLog = [&](const char* m) { if (logMsg && logN) { strncpy(logMsg, m, logN - 1); logMsg[logN - 1] = '\0'; } };

  if (!settings::navEnabled()) { setLog("Scrobbeln aus"); return true; }
  if (pendingCount() == 0)     { setLog("nichts offen"); return true; }

  char base[128], user[64], pass[65];
  baseUrl(base, sizeof(base));
  settings::navUser(user, sizeof(user));
  settings::navPass(pass, sizeof(pass));
  if (!base[0] || !user[0]) { setLog("Navidrome nicht konfiguriert"); return false; }
  if (settings::wifiCount() == 0) { setLog("kein WLAN konfiguriert"); return false; }
  if (webfm::state() != webfm::State::OFF) { setLog("WLAN belegt"); return false; }

  if (useBackoff) {
    uint32_t cur = timesync::now();
    if (s_failCount > 0 && s_lastAttemptEpoch != 0 && cur >= s_lastAttemptEpoch) {
      uint8_t  shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
      uint32_t wait  = kBackoffBaseSecs << shift;
      if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
      if (cur - s_lastAttemptEpoch < wait) {
        Serial.printf("[SCROBBLE] Pre-Standby-Upload uebersprungen (Back-off: %u "
                      "Fehlschlaege, warte %lus)\n", (unsigned)s_failCount, (unsigned long)wait);
        setLog("Back-off aktiv");
        return false;
      }
    }
    s_lastAttemptEpoch = cur;
  }

  // WLAN-Regel (CLAUDE.md): Audio stoppen + Mesh-Pumpe pausieren (zentral in wifi).
  bool connected = wifi::connect(kConnectTimeoutMs);

  int uploaded = 0, failed = 0;
  if (connected) {
    Serial.printf("[SCROBBLE] WLAN verbunden, lade %d Eintraege hoch ...\n", pendingCount());
    // Snapshot (während WLAN an ist, ist Audio gestoppt → keine neuen enqueues).
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_count;
    Entry* snap = (n > 0) ? (Entry*)malloc(sizeof(Entry) * n) : nullptr;
    if (snap) memcpy(snap, s_ring, sizeof(Entry) * n); else n = 0;
    xSemaphoreGive(s_mutex);

    bool* ok = (n > 0) ? (bool*)calloc(n, sizeof(bool)) : nullptr;
    for (int i = 0; i < n; i++) {
      ok[i] = uploadOne(snap[i], base, user, pass);
      if (ok[i]) uploaded++; else failed++;
      Serial.printf("[SCROBBLE] %s: %s\n", snap[i].path, ok[i] ? "OK" : "Fehler");
    }

    // Ring neu aufbauen = gescheiterte Einträge + evtl. währenddessen neu
    // hinzugekommene (Indizes >= n; nur bei Konsolen-Flush mit laufender Musik).
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int extra = (s_count > n) ? (s_count - n) : 0;
    Entry* tmp = (extra > 0) ? (Entry*)malloc(sizeof(Entry) * extra) : nullptr;
    if (tmp) memcpy(tmp, &s_ring[n], sizeof(Entry) * extra);
    int w = 0;
    for (int i = 0; i < n; i++) if (!ok[i]) s_ring[w++] = snap[i];
    for (int i = 0; i < extra && w < kMax; i++) s_ring[w++] = tmp[i];
    s_count = w;
    s_dirty = true;
    xSemaphoreGive(s_mutex);
    if (tmp)  free(tmp);
    if (snap) free(snap);
    if (ok)   free(ok);
  }

  wifi::disconnect();

  saveToSd();

  if (useBackoff) {
    if (connected && failed == 0) s_failCount = 0;
    else if (s_failCount < 0xFF)  s_failCount++;
  }

  char m[48];
  if (!connected) snprintf(m, sizeof(m), "WLAN-Fehler");
  else            snprintf(m, sizeof(m), "%d hochgeladen, %d offen", uploaded, failed);
  setLog(m);
  Serial.printf("[SCROBBLE] %s\n", m);
  return connected && uploaded > 0;
}

}  // namespace

namespace scrobble {

void begin() {
  s_mutex = xSemaphoreCreateMutex();
  s_ring  = (Entry*)heap_caps_malloc(sizeof(Entry) * kMax, MALLOC_CAP_SPIRAM);
  if (!s_ring) s_ring = (Entry*)malloc(sizeof(Entry) * kMax);
  s_count = 0;
  if (s_ring) loadFromSd();
}

void enqueue(const char* path, uint32_t startEpoch, uint16_t playedSec, uint16_t durSec) {
  if (!s_ring || !path || !path[0]) return;
  if (!settings::navEnabled()) return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_count >= kMax) {            // voll: ältesten verwerfen
    memmove(&s_ring[0], &s_ring[1], sizeof(Entry) * (kMax - 1));
    s_count = kMax - 1;
  }
  Entry& e = s_ring[s_count++];
  strncpy(e.path, path, TRACK_PATH_LEN - 1);
  e.path[TRACK_PATH_LEN - 1] = '\0';
  e.startEpoch = startEpoch;
  e.playedSec  = playedSec;
  e.durSec     = durSec;
  s_dirty = true;
  xSemaphoreGive(s_mutex);
  Serial.printf("[SCROBBLE] enqueue %s (%us/%us)\n", path, playedSec, durSec);
}

int pendingCount() {
  if (!s_ring) return 0;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  int n = s_count;
  xSemaphoreGive(s_mutex);
  return n;
}

void poll() {
  if (!s_dirty) return;
  if (millis() - s_lastSaveMs < kSaveThrottleMs) return;
  s_lastSaveMs = millis();
  saveToSd();
}

bool flushBeforeStandby() {
  char dummy[8];
  return flushCore(true, dummy, sizeof(dummy));
}

bool flushNow(char* log, size_t logN) {
  return flushCore(false, log, logN);
}

bool ping(char* msg, size_t msgN) {
  auto setMsg = [&](const char* m) { if (msg && msgN) { strncpy(msg, m, msgN - 1); msg[msgN - 1] = '\0'; } };
  char base[128], user[64], pass[65];
  baseUrl(base, sizeof(base));
  settings::navUser(user, sizeof(user));
  settings::navPass(pass, sizeof(pass));
  if (!base[0] || !user[0]) { setMsg("Navidrome nicht konfiguriert"); return false; }
  if (settings::wifiCount() == 0) { setMsg("kein WLAN konfiguriert"); return false; }
  if (webfm::state() != webfm::State::OFF) { setMsg("WLAN belegt"); return false; }

  if (!wifi::connect(kConnectTimeoutMs)) { setMsg("WLAN-Fehler"); return false; }
  String url = String(base) + "/rest/ping.view?" + authQuery(user, pass);
  String body;
  bool ok = httpGet(url, body) && bodyOk(body);
  setMsg(ok ? "Verbindung OK" : "Server-Fehler (Auth/URL?)");
  wifi::disconnect();
  return ok;
}

}  // namespace scrobble
