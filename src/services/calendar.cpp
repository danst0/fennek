// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "calendar.h"
#include "config.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/audio.h"
#include "services/timesync.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"
#include "apps/ical_core.h"
#include "apps/podcast_core.h"   // feedSlug() wiederverwenden

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int      kMaxEvents   = 256;
constexpr int      kMaxPerFeed  = 200;
constexpr int      kMaxFeeds    = 16;
constexpr uint32_t kWindowDays  = 56;          // ~8 Wochen Vorschau
constexpr uint32_t kSaneEpoch   = 1500000000UL;
const char*        kFeedsFile   = "/calendar/feeds.txt";

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint16_t kHttpTimeoutMs    = 12000;

calendar::CalEvent* s_events = nullptr;
int                 s_eventN = 0;

RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
constexpr uint32_t kBackoffBaseSecs = 1800;
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600;
constexpr uint8_t  kBackoffMaxShift = 6;

// --- Fenster ------------------------------------------------------------------
void window(uint32_t* ws, uint32_t* we) {
  uint32_t now = timesync::now();
  if (now < kSaneEpoch) { *ws = 0; *we = 0xFFFFFFFFu; }   // Uhr unbekannt → alles
  else { *ws = now - 86400u; *we = now + kWindowDays * 86400u; }
}

// --- Feed-Liste ---------------------------------------------------------------
int readFeeds(char urls[][192], int maxN) {
  int n = 0;
  if (!board::sdReady()) return 0;
  spiLock();
  File f = SD.open(kFeedsFile, FILE_READ);
  if (f) {
    char line[256];
    while (f.available() && n < maxN) {
      int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[len] = '\0';
      char* p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (!*p || *p == '#') continue;
      // URL bis Tab/Whitespace/Zeilenende.
      char* e = p;
      while (*e && *e != '\t' && *e != '\r' && *e != '\n' && *e != ' ') e++;
      *e = '\0';
      strncpy(urls[n], p, 191); urls[n][191] = '\0';
      n++;
    }
    f.close();
  }
  spiUnlock();
  return n;
}

void writeFeeds(char urls[][192], int n) {
  if (!board::sdReady()) return;
  spiLock();
  SD.mkdir("/calendar");
  File f = SD.open(kFeedsFile, FILE_WRITE);
  if (f) { for (int i = 0; i < n; i++) f.printf("%s\n", urls[i]); f.close(); }
  spiUnlock();
}

// --- Per-Feed-Cache (.bin) ----------------------------------------------------
void slugPaths(const char* url, char* bin, size_t bn, char* etag, size_t en) {
  char slug[48];
  podcast::feedSlug(url, slug, sizeof(slug));
  snprintf(bin,  bn, "/calendar/%s.bin",  slug);
  snprintf(etag, en, "/calendar/%s.etag", slug);
}

void readEtag(const char* path, char* out, size_t cap) {
  out[0] = '\0';
  spiLock();
  File f = SD.open(path, FILE_READ);
  if (f) { int n = f.read((uint8_t*)out, cap - 1); if (n < 0) n = 0; out[n] = '\0'; f.close(); }
  spiUnlock();
  for (char* p = out; *p; p++) if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
}

void writeEtag(const char* path, const char* etag) {
  spiLock();
  SD.mkdir("/calendar");
  File f = SD.open(path, FILE_WRITE);
  if (f) { f.print(etag); f.close(); }
  spiUnlock();
}

// CalEvents eines Feeds in s_events anhängen (aus .bin laden).
void appendFromBin(const char* bin) {
  spiLock();
  File f = SD.open(bin, FILE_READ);
  if (f) {
    uint16_t cnt = 0;
    if (f.read((uint8_t*)&cnt, sizeof(cnt)) == sizeof(cnt)) {
      for (uint16_t i = 0; i < cnt && s_eventN < kMaxEvents; i++) {
        calendar::CalEvent ev;
        if (f.read((uint8_t*)&ev, sizeof(ev)) != (int)sizeof(ev)) break;
        s_events[s_eventN++] = ev;
      }
    }
    f.close();
  }
  spiUnlock();
}

void writeBin(const char* bin, calendar::CalEvent* evs, int n) {
  spiLock();
  SD.mkdir("/calendar");
  File f = SD.open(bin, FILE_WRITE);
  if (f) {
    uint16_t cnt = (uint16_t)n;
    f.write((const uint8_t*)&cnt, sizeof(cnt));
    f.write((const uint8_t*)evs, sizeof(calendar::CalEvent) * n);
    f.close();
  }
  spiUnlock();
}

int cmpEvent(const void* a, const void* b) {
  uint32_t sa = ((const calendar::CalEvent*)a)->start;
  uint32_t sb = ((const calendar::CalEvent*)b)->start;
  return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}

void sortEvents() {
  if (s_eventN > 1) qsort(s_events, s_eventN, sizeof(calendar::CalEvent), cmpEvent);
}

// --- HTTP-GET mit Streaming-Parse in einen Feed-Puffer ------------------------
// true = HTTP 200 + geparst; *notMod bei 304.
bool fetchFeed(const char* url, const char* prevEtag, char* newEtag, size_t etagCap,
               calendar::CalEvent* feedEv, int* feedN, uint32_t ws, uint32_t we) {
  *feedN = 0;
  bool https = String(url).startsWith("https");
  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  bool begun = https ? (cs.setInsecure(), http.begin(cs, url)) : http.begin(cp, url);
  if (!begun) return false;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setUserAgent("fennek");
  if (prevEtag && prevEtag[0]) http.addHeader("If-None-Match", prevEtag);
  const char* keys[] = {"ETag"};
  http.collectHeaders(keys, 1);

  int code = http.GET();
  if (code == 304) { http.end(); return false; }   // notMod über Rückgabe unten geregelt
  if (code != 200) { Serial.printf("[CAL] GET HTTP %d\n", code); http.end(); return false; }

  String et = http.header("ETag");
  strncpy(newEtag, et.c_str(), etagCap - 1); newEtag[etagCap - 1] = '\0';

  ical::Parser parser;
  WiFiClient* st = http.getStreamPtr();
  char line[300]; size_t li = 0;
  uint32_t idle = millis();
  int n = 0;

  auto handle = [&](ical::Event& ev) {
    ical::expand(ev, ws, we, [&](uint32_t s, uint32_t e) {
      if (n >= kMaxPerFeed) return;
      calendar::CalEvent& c = feedEv[n++];
      c.start = s; c.end = e; c.allDay = ev.allDay ? 1 : 0;
      strncpy(c.summary, ev.summary, sizeof(c.summary) - 1); c.summary[sizeof(c.summary) - 1] = '\0';
      strncpy(c.location, ev.location, sizeof(c.location) - 1); c.location[sizeof(c.location) - 1] = '\0';
    });
  };

  while (http.connected() || st->available()) {
    int avail = st->available();
    if (avail <= 0) { if (millis() - idle > 5000) break; delay(2); continue; }
    while (st->available()) {
      int ch = st->read();
      if (ch < 0) break;
      if (ch == '\n') {
        while (li > 0 && line[li - 1] == '\r') li--;
        line[li] = '\0'; li = 0;
        ical::Event ev;
        if (parser.feedLine(line, &ev)) handle(ev);
      } else if (li < sizeof(line) - 1) {
        line[li++] = (char)ch;
      }
      // überlange Zeile: Rest bis '\n' verwerfen (li bleibt gekappt)
    }
    idle = millis();
  }
  line[li] = '\0';
  { ical::Event ev; if (parser.feedLine(line, &ev)) handle(ev); }
  { ical::Event ev; if (parser.finish(&ev)) handle(ev); }
  http.end();
  *feedN = n;
  return true;
}

bool syncCore(bool useBackoff, char* log, size_t logN) {
  auto setLog = [&](const char* m) { if (log && logN) { strncpy(log, m, logN - 1); log[logN - 1] = '\0'; } };

  char ssid[33], wpass[65];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(wpass, sizeof(wpass));
  static char urls[kMaxFeeds][192];
  int feeds = readFeeds(urls, kMaxFeeds);
  if (feeds == 0) { setLog("keine Feeds"); return false; }
  if (!ssid[0])   { setLog("kein WLAN konfiguriert"); return false; }
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

  int fetched = 0;
  if (connected) {
    uint32_t ws, we; window(&ws, &we);
    s_eventN = 0;
    calendar::CalEvent* feedEv = (calendar::CalEvent*)heap_caps_malloc(
        sizeof(calendar::CalEvent) * kMaxPerFeed, MALLOC_CAP_SPIRAM);
    if (!feedEv) feedEv = (calendar::CalEvent*)malloc(sizeof(calendar::CalEvent) * kMaxPerFeed);
    for (int i = 0; i < feeds && feedEv; i++) {
      char bin[80], etagPath[80], prevEtag[80], newEtag[80] = "";
      slugPaths(urls[i], bin, sizeof(bin), etagPath, sizeof(etagPath));
      readEtag(etagPath, prevEtag, sizeof(prevEtag));
      int fn = 0;
      if (fetchFeed(urls[i], prevEtag, newEtag, sizeof(newEtag), feedEv, &fn, ws, we)) {
        // 200: frisch geparst → Cache schreiben, in Liste aufnehmen.
        writeBin(bin, feedEv, fn);
        if (newEtag[0]) writeEtag(etagPath, newEtag);
        for (int k = 0; k < fn && s_eventN < kMaxEvents; k++) s_events[s_eventN++] = feedEv[k];
        fetched++;
        Serial.printf("[CAL] %s: %d Termine\n", urls[i], fn);
      } else {
        // 304 oder Fehler → vorhandenen Cache behalten.
        appendFromBin(bin);
      }
    }
    if (feedEv) free(feedEv);
    sortEvents();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mesh_client::setSuspended(false);

  if (useBackoff) {
    if (connected) s_failCount = 0;
    else if (s_failCount < 0xFF) s_failCount++;
  }

  char m[48];
  if (!connected) snprintf(m, sizeof(m), "WLAN-Fehler");
  else            snprintf(m, sizeof(m), "%d Feeds, %d Termine", feeds, s_eventN);
  setLog(m);
  Serial.printf("[CAL] %s\n", m);
  return connected;
}

}  // namespace

namespace calendar {

void begin() {
  s_events = (CalEvent*)heap_caps_malloc(sizeof(CalEvent) * kMaxEvents, MALLOC_CAP_SPIRAM);
  if (!s_events) s_events = (CalEvent*)malloc(sizeof(CalEvent) * kMaxEvents);
  s_eventN = 0;
  if (!s_events) { Serial.println("[CAL] PSRAM-Allokation fehlgeschlagen"); return; }
  // Per-Feed-Caches mischen (Boot ohne WLAN).
  static char urls[kMaxFeeds][192];
  int feeds = readFeeds(urls, kMaxFeeds);
  for (int i = 0; i < feeds; i++) {
    char bin[80], etagPath[80];
    slugPaths(urls[i], bin, sizeof(bin), etagPath, sizeof(etagPath));
    appendFromBin(bin);
  }
  sortEvents();
}

int count() { return s_eventN; }

bool event(int idx, CalEvent* out) {
  if (idx < 0 || idx >= s_eventN || !out) return false;
  *out = s_events[idx];
  return true;
}

int feedCount() {
  static char urls[kMaxFeeds][192];
  return readFeeds(urls, kMaxFeeds);
}

bool feedUrl(int idx, char* out, size_t n) {
  static char urls[kMaxFeeds][192];
  int f = readFeeds(urls, kMaxFeeds);
  if (idx < 0 || idx >= f) return false;
  strncpy(out, urls[idx], n - 1); out[n - 1] = '\0';
  return true;
}

bool addFeed(const char* url) {
  if (!url || !url[0]) return false;
  static char urls[kMaxFeeds][192];
  int f = readFeeds(urls, kMaxFeeds);
  if (f >= kMaxFeeds) return false;
  strncpy(urls[f], url, 191); urls[f][191] = '\0';
  writeFeeds(urls, f + 1);
  return true;
}

bool removeFeed(int idx) {
  static char urls[kMaxFeeds][192];
  int f = readFeeds(urls, kMaxFeeds);
  if (idx < 0 || idx >= f) return false;
  for (int i = idx; i < f - 1; i++) memcpy(urls[i], urls[i + 1], 192);
  writeFeeds(urls, f - 1);
  return true;
}

bool sync(char* log, size_t logN) { return syncCore(false, log, logN); }

bool flushBeforeStandby() {
  if (!settings::calAutoSync()) return true;
  char dummy[8];
  return syncCore(true, dummy, sizeof(dummy));
}

}  // namespace calendar
