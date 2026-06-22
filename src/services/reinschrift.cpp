// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "reinschrift.h"
#include "config.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/audio.h"
#include "services/timesync.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <time.h>
#include <string.h>

using reinschrift::Task;
using reinschrift::Op;

namespace {

constexpr size_t kRawCap   = 96 * 1024;   // Markdown-Datenbank (PSRAM)
constexpr int    kMaxTasks = 384;
constexpr int    kMaxOps   = 64;

const char* kCacheFile = "/.fennek/todo.md";
const char* kEtagFile  = "/.fennek/todo.etag";
const char* kOpsFile   = "/.fennek/todo_ops.tsv";

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint16_t kHttpTimeoutMs    = 12000;

char*  s_raw   = nullptr;   // zuletzt synchronisierter (gemergter) Markdown-Text
size_t s_rawLen = 0;
char   s_etag[80] = "";

Task*  s_tasks = nullptr;
int    s_taskN = 0;

Op*    s_ops   = nullptr;
int    s_opN   = 0;

// Back-off für den Pre-Standby-Sync (RTC-RAM, übersteht Deep Sleep).
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
constexpr uint32_t kBackoffBaseSecs = 1800;
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600;
constexpr uint8_t  kBackoffMaxShift = 6;

uint32_t rngN(uint32_t n) { return n ? esp_random() % n : 0; }

// --- WebDAV-URL ---------------------------------------------------------------
String pathEncode(const char* p) {
  String o; o.reserve(strlen(p) * 2 + 4);
  static const char* hx = "0123456789ABCDEF";
  for (const unsigned char* q = (const unsigned char*)p; *q; q++) {
    char c = *q;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == '/') {
      o += c;
    } else { o += '%'; o += hx[(c >> 4) & 0xF]; o += hx[c & 0xF]; }
  }
  return o;
}

// Volle WebDAV-URL aus Basis-URL + Benutzer + Pfad bauen (Nextcloud-Schema).
String webdavUrl() {
  char base[160], user[64], path[160];
  settings::todoUrl(base, sizeof(base));
  settings::todoUser(user, sizeof(user));
  settings::todoPath(path, sizeof(path));
  String b = base;
  while (b.length() && b[b.length() - 1] == '/') b.remove(b.length() - 1);
  String dav;
  if (b.indexOf("remote.php/dav") >= 0) dav = b;
  else dav = b + "/remote.php/dav/files/" + user;
  const char* pp = path;
  while (*pp == '/') pp++;
  return dav + "/" + pathEncode(pp);
}

bool makeClient(bool https, WiFiClientSecure& cs, WiFiClient& cp, HTTPClient& http, const String& url) {
  if (https) { cs.setInsecure(); return http.begin(cs, url); }
  return http.begin(cp, url);
}

// GET der Datei in s_raw, ETag merken. *notModified bei HTTP 304.
bool webdavGet(const String& url, const char* user, const char* pass,
               const char* prevEtag, bool* notModified) {
  *notModified = false;
  bool https = url.startsWith("https");
  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  if (!makeClient(https, cs, cp, http, url)) return false;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setAuthorization(user, pass);
  if (prevEtag && prevEtag[0]) http.addHeader("If-None-Match", prevEtag);
  const char* keys[] = {"ETag"};
  http.collectHeaders(keys, 1);

  int code = http.GET();
  if (code == 304) { *notModified = true; http.end(); return true; }
  if (code != 200) { Serial.printf("[TODO] GET HTTP %d\n", code); http.end(); return false; }

  String et = http.header("ETag");
  strncpy(s_etag, et.c_str(), sizeof(s_etag) - 1); s_etag[sizeof(s_etag) - 1] = '\0';

  WiFiClient* st = http.getStreamPtr();
  size_t o = 0;
  uint32_t idle = millis();
  while (http.connected() && o + 1 < kRawCap) {
    size_t avail = st->available();
    if (avail) {
      int r = st->read((uint8_t*)s_raw + o, (avail < kRawCap - 1 - o) ? avail : (kRawCap - 1 - o));
      if (r > 0) { o += r; idle = millis(); }
    } else {
      if (millis() - idle > 4000) break;
      delay(2);
    }
  }
  s_raw[o] = '\0';
  s_rawLen = o;
  http.end();
  return true;
}

// PUT von s_raw mit If-Match. *conflict bei HTTP 412.
bool webdavPut(const String& url, const char* user, const char* pass,
               const char* etag, const char* body, size_t len, bool* conflict) {
  *conflict = false;
  bool https = url.startsWith("https");
  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  if (!makeClient(https, cs, cp, http, url)) return false;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setAuthorization(user, pass);
  http.addHeader("Content-Type", "text/markdown; charset=utf-8");
  if (etag && etag[0]) http.addHeader("If-Match", etag);
  int code = http.sendRequest("PUT", (uint8_t*)body, len);
  http.end();
  if (code == 412) { *conflict = true; Serial.println("[TODO] PUT 412 (Konflikt)"); return false; }
  bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.printf("[TODO] PUT HTTP %d\n", code);
  return ok;
}

// --- Op-Queue -----------------------------------------------------------------
int findOp(uint8_t kind, const char* marker) {
  for (int i = 0; i < s_opN; i++)
    if (s_ops[i].kind == kind && strcmp(s_ops[i].marker, marker) == 0) return i;
  return -1;
}
int findAddOp(const char* marker) {
  for (int i = 0; i < s_opN; i++)
    if (s_ops[i].kind == reinschrift::OP_ADD && strcmp(s_ops[i].marker, marker) == 0) return i;
  return -1;
}

void persistOps() {
  if (!board::sdReady()) return;
  spiLock();
  SD.mkdir("/.fennek");
  if (s_opN == 0) {
    SD.remove(kOpsFile);
  } else {
    File f = SD.open(kOpsFile, FILE_WRITE);
    if (f) {
      for (int i = 0; i < s_opN; i++) {
        const Op& o = s_ops[i];
        f.printf("%u\t%s\t%d\t%s\t%s\t%s\n", o.kind, o.marker, o.done ? 1 : 0,
                 o.value, o.topic, o.line);
      }
      f.close();
    }
  }
  spiUnlock();
}

void loadOps() {
  s_opN = 0;
  if (!board::sdReady()) return;
  spiLock();
  File f = SD.open(kOpsFile, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return;
  char line[reinschrift::kLineLen + 128];
  for (;;) {
    spiLock();
    int len = f.available() ? f.readBytesUntil('\n', line, sizeof(line) - 1) : -1;
    spiUnlock();
    if (len < 0 || s_opN >= kMaxOps) break;
    line[len] = '\0';
    if (!line[0]) continue;
    char* sv = nullptr;
    char* a = strtok_r(line, "\t", &sv);
    char* b = strtok_r(nullptr, "\t", &sv);
    char* c = strtok_r(nullptr, "\t", &sv);
    char* d = strtok_r(nullptr, "\t", &sv);
    char* e = strtok_r(nullptr, "\t", &sv);
    char* g = strtok_r(nullptr, "\r\n", &sv);
    if (!a) continue;
    Op& o = s_ops[s_opN]; memset(&o, 0, sizeof(o));
    o.kind = (uint8_t)atoi(a);
    if (b) { strncpy(o.marker, b, sizeof(o.marker) - 1); }
    o.done = (c && atoi(c) != 0);
    if (d) { strncpy(o.value, d, sizeof(o.value) - 1); }
    if (e) { strncpy(o.topic, e, sizeof(o.topic) - 1); }
    if (g) { strncpy(o.line, g, sizeof(o.line) - 1); }
    s_opN++;
  }
  spiLock(); f.close(); spiUnlock();
  if (s_opN) Serial.printf("[TODO] %d offene Aenderung(en) geladen\n", s_opN);
}

void persistCache() {
  if (!board::sdReady()) return;
  spiLock();
  SD.mkdir("/.fennek");
  File f = SD.open(kCacheFile, FILE_WRITE);
  if (f) { f.write((const uint8_t*)s_raw, s_rawLen); f.close(); }
  File g = SD.open(kEtagFile, FILE_WRITE);
  if (g) { g.print(s_etag); g.close(); }
  spiUnlock();
}

void loadCache() {
  s_raw[0] = '\0'; s_rawLen = 0; s_etag[0] = '\0';
  if (!board::sdReady()) return;
  spiLock();
  File f = SD.open(kCacheFile, FILE_READ);
  if (f) {
    size_t n = f.read((uint8_t*)s_raw, kRawCap - 1);
    s_rawLen = n; s_raw[n] = '\0';
    f.close();
  }
  File g = SD.open(kEtagFile, FILE_READ);
  if (g) {
    int n = g.read((uint8_t*)s_etag, sizeof(s_etag) - 1);
    if (n < 0) n = 0;
    s_etag[n] = '\0';
    for (char* p = s_etag; *p; p++) if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
    g.close();
  }
  spiUnlock();
}

// --- Sicht (Aufgabenliste) aus s_raw + Op-Queue aufbauen ----------------------
int findTaskByMarker(const char* marker) {
  if (!marker[0]) return -1;
  for (int i = 0; i < s_taskN; i++)
    if (strcmp(s_tasks[i].marker, marker) == 0) return i;
  return -1;
}

void rebuildView() {
  s_taskN = 0;
  // s_raw zeilenweise parsen.
  const char* p = s_raw;
  while (*p && s_taskN < kMaxTasks) {
    const char* e = p;
    while (*e && *e != '\n') e++;
    char tmp[reinschrift::kLineLen];
    size_t len = (size_t)(e - p);
    if (len < sizeof(tmp)) {
      memcpy(tmp, p, len); tmp[len] = '\0';
      Task t;
      if (reinschrift::parseLine(tmp, &t)) s_tasks[s_taskN++] = t;
    }
    p = (*e == '\n') ? e + 1 : e;
  }
  // Lokale Ops auf die Sicht legen.
  for (int i = 0; i < s_opN; i++) {
    const Op& o = s_ops[i];
    if (o.kind == reinschrift::OP_ADD) {
      if (s_taskN >= kMaxTasks) continue;
      Task t;
      if (reinschrift::parseLine(o.line, &t)) {
        if (!t.topic[0] && o.topic[0]) { strncpy(t.topic, o.topic, sizeof(t.topic) - 1); t.topic[sizeof(t.topic)-1]='\0'; }
        s_tasks[s_taskN++] = t;
      }
    } else {
      int idx = findTaskByMarker(o.marker);
      if (idx < 0) continue;
      if (o.kind == reinschrift::OP_TOGGLE) s_tasks[idx].done = o.done;
      else if (o.kind == reinschrift::OP_DUE) s_tasks[idx].dueEpoch = reinschrift::detail::parseDue(o.value);
    }
  }
}

// --- WLAN-gestützter Sync-Kern ------------------------------------------------
bool syncCore(bool useBackoff, char* log, size_t logN) {
  auto setLog = [&](const char* m) { if (log && logN) { strncpy(log, m, logN - 1); log[logN - 1] = '\0'; } };

  if (!settings::todoEnabled()) { setLog("Todo-Sync aus"); return true; }

  char user[64], pass[65], ssid[33], wpass[65], base[160];
  settings::todoUrl(base, sizeof(base));
  settings::todoUser(user, sizeof(user));
  settings::todoPass(pass, sizeof(pass));
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(wpass, sizeof(wpass));
  if (!base[0] || !user[0]) { setLog("WebDAV nicht konfiguriert"); return false; }
  if (!ssid[0])             { setLog("kein WLAN konfiguriert"); return false; }
  if (webfm::state() != webfm::State::OFF) { setLog("WLAN belegt"); return false; }

  if (useBackoff) {
    uint32_t cur = timesync::now();
    if (s_failCount > 0 && s_lastAttemptEpoch != 0 && cur >= s_lastAttemptEpoch) {
      uint8_t shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
      uint32_t wait = kBackoffBaseSecs << shift;
      if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
      if (cur - s_lastAttemptEpoch < wait) { setLog("Back-off aktiv"); return false; }
    }
    s_lastAttemptEpoch = cur;
  }

  audio::stop();
  mesh_client::setSuspended(true);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wpass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);
  bool connected = (WiFi.status() == WL_CONNECTED);

  bool ok = false;
  int pushed = 0;
  if (connected) {
    String url = webdavUrl();
    // Bei anstehenden Ops: GET → Merge → PUT (mit einem 412-Retry). Ohne Ops:
    // nur frisch ziehen (bedingt, 304 spart Bandbreite).
    bool notMod = false;
    if (webdavGet(url, user, pass, s_opN ? "" : s_etag, &notMod)) {
      if (notMod) {
        ok = true;   // Cache aktuell, nichts zu tun
      } else if (s_opN == 0) {
        persistCache(); rebuildView(); ok = true;
      } else {
        // Merge + PUT, bei Konflikt einmal neu ziehen + mergen.
        char* merged = (char*)heap_caps_malloc(kRawCap, MALLOC_CAP_SPIRAM);
        if (!merged) merged = (char*)malloc(kRawCap);
        for (int attempt = 0; attempt < 2 && merged; attempt++) {
          if (!reinschrift::applyOps(s_raw, s_ops, s_opN, merged, kRawCap)) { setLog("Merge zu gross"); break; }
          bool conflict = false;
          if (webdavPut(url, user, pass, s_etag, merged, strlen(merged), &conflict)) {
            // Erfolg: gemergten Stand übernehmen, Queue leeren.
            strncpy(s_raw, merged, kRawCap - 1); s_raw[kRawCap - 1] = '\0';
            s_rawLen = strlen(s_raw);
            pushed = s_opN; s_opN = 0;
            persistOps(); persistCache(); rebuildView();
            ok = true; break;
          }
          if (!conflict) break;                  // echter Fehler, nicht 412
          if (!webdavGet(url, user, pass, "", &notMod)) break;   // frisch ziehen, dann erneut
        }
        if (merged) free(merged);
      }
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mesh_client::setSuspended(false);

  if (useBackoff) {
    if (ok) s_failCount = 0;
    else if (s_failCount < 0xFF) s_failCount++;
  }

  char m[48];
  if (!connected)   snprintf(m, sizeof(m), "WLAN-Fehler");
  else if (ok && pushed) snprintf(m, sizeof(m), "%d Aenderung(en) gesendet", pushed);
  else if (ok)      snprintf(m, sizeof(m), "Aktuell, %d Aufgaben", s_taskN);
  else              snprintf(m, sizeof(m), "Sync fehlgeschlagen");
  setLog(m);
  Serial.printf("[TODO] %s\n", m);
  return ok;
}

// Lokales Datum (heute + offset) als "YYYY-MM-DDT12:00" (Reinschrift-Konvention).
void relativeDueStr(int dayOffset, char* out, size_t cap) {
  time_t t = (time_t)timesync::now() + (time_t)dayOffset * 86400;
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, cap, "%04d-%02d-%02dT12:00", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

}  // namespace

namespace reinschrift_svc {

void begin() {
  s_raw   = (char*)heap_caps_malloc(kRawCap, MALLOC_CAP_SPIRAM);
  if (!s_raw) s_raw = (char*)malloc(kRawCap);
  s_tasks = (Task*)heap_caps_malloc(sizeof(Task) * kMaxTasks, MALLOC_CAP_SPIRAM);
  if (!s_tasks) s_tasks = (Task*)malloc(sizeof(Task) * kMaxTasks);
  s_ops   = (Op*)heap_caps_malloc(sizeof(Op) * kMaxOps, MALLOC_CAP_SPIRAM);
  if (!s_ops) s_ops = (Op*)malloc(sizeof(Op) * kMaxOps);
  if (!s_raw || !s_tasks || !s_ops) { Serial.println("[TODO] PSRAM-Allokation fehlgeschlagen"); return; }
  s_raw[0] = '\0';
  loadCache();
  loadOps();
  rebuildView();
}

int  count() { return s_taskN; }

bool get(int idx, Task* out) {
  if (idx < 0 || idx >= s_taskN || !out) return false;
  *out = s_tasks[idx];
  return true;
}

void toggle(int idx) {
  if (idx < 0 || idx >= s_taskN) return;
  Task& t = s_tasks[idx];
  bool nd = !t.done;
  t.done = nd;
  if (!t.marker[0]) return;
  int add = findAddOp(t.marker);
  if (add >= 0) {
    // Noch nicht synchronisierte neue Aufgabe → Checkbox in ihrer Zeile setzen.
    char buf[reinschrift::kLineLen];
    reinschrift::detail::applyToggle(s_ops[add].line, s_ops[add].line + strlen(s_ops[add].line), nd, buf, sizeof(buf));
    strncpy(s_ops[add].line, buf, sizeof(s_ops[add].line) - 1);
    s_ops[add].line[sizeof(s_ops[add].line) - 1] = '\0';
  } else {
    int op = findOp(reinschrift::OP_TOGGLE, t.marker);
    if (op < 0 && s_opN < kMaxOps) { op = s_opN++; memset(&s_ops[op], 0, sizeof(Op)); s_ops[op].kind = reinschrift::OP_TOGGLE; strncpy(s_ops[op].marker, t.marker, sizeof(s_ops[op].marker) - 1); }
    if (op >= 0) s_ops[op].done = nd;
  }
  persistOps();
}

void setDueRelative(int idx, int dayOffset) {
  if (idx < 0 || idx >= s_taskN) return;
  Task& t = s_tasks[idx];
  char val[24]; relativeDueStr(dayOffset, val, sizeof(val));
  t.dueEpoch = reinschrift::detail::parseDue(val);
  if (!t.marker[0]) return;
  int add = findAddOp(t.marker);
  if (add >= 0) {
    char buf[reinschrift::kLineLen];
    reinschrift::detail::applyDue(s_ops[add].line, s_ops[add].line + strlen(s_ops[add].line), val, buf, sizeof(buf));
    strncpy(s_ops[add].line, buf, sizeof(s_ops[add].line) - 1);
    s_ops[add].line[sizeof(s_ops[add].line) - 1] = '\0';
  } else {
    int op = findOp(reinschrift::OP_DUE, t.marker);
    if (op < 0 && s_opN < kMaxOps) { op = s_opN++; memset(&s_ops[op], 0, sizeof(Op)); s_ops[op].kind = reinschrift::OP_DUE; strncpy(s_ops[op].marker, t.marker, sizeof(s_ops[op].marker) - 1); }
    if (op >= 0) { strncpy(s_ops[op].value, val, sizeof(s_ops[op].value) - 1); s_ops[op].value[sizeof(s_ops[op].value)-1]='\0'; }
  }
  persistOps();
}

bool add(const char* title, const char* topic) {
  if (!title || !title[0] || s_opN >= kMaxOps || s_taskN >= kMaxTasks) return false;
  char marker[reinschrift::kMarkerLen];
  reinschrift::genMarker(marker, sizeof(marker), rngN);
  // Titel ggf. um +Topic ergänzen, wenn der Nutzer keins getippt hat.
  char full[reinschrift::kTitleLen + reinschrift::kTopicLen + 4];
  if (topic && topic[0] && !strchr(title, '+'))
    snprintf(full, sizeof(full), "%s +%s", title, topic);
  else
    snprintf(full, sizeof(full), "%s", title);
  Op& o = s_ops[s_opN]; memset(&o, 0, sizeof(o));
  o.kind = reinschrift::OP_ADD;
  strncpy(o.marker, marker, sizeof(o.marker) - 1);
  reinschrift::buildAddLine(full, marker, o.line, sizeof(o.line));
  // Thema für die Einsortierung: aus dem getippten +Topic oder dem Default.
  Task probe;
  reinschrift::parseLine(o.line, &probe);
  strncpy(o.topic, probe.topic[0] ? probe.topic : (topic ? topic : "Inbox"), sizeof(o.topic) - 1);
  s_opN++;
  // Sicht aktualisieren.
  if (!probe.topic[0]) { strncpy(probe.topic, o.topic, sizeof(probe.topic) - 1); probe.topic[sizeof(probe.topic)-1]='\0'; }
  s_tasks[s_taskN++] = probe;
  persistOps();
  return true;
}

int pendingCount() { return s_opN; }

bool sync(char* log, size_t logN) { return syncCore(false, log, logN); }

bool flushBeforeStandby() {
  if (!settings::todoEnabled()) return true;
  // Nur Funk hochfahren, wenn etwas ansteht oder Auto-Sync gewünscht ist.
  if (s_opN == 0 && !settings::todoAutoSync()) return true;
  char dummy[8];
  return syncCore(true, dummy, sizeof(dummy));
}

}  // namespace reinschrift_svc
