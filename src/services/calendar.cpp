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
#include "services/wifi.h"
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

// CalDAV-Feed? Erkennung über das Pseudo-Schema-Präfix "caldav:" in feeds.txt
// (z. B. "caldav:https://p##-caldav.icloud.com/123/calendars/home/"). Dahinter
// steht die echte URL, gegen die mit Basic-Auth ein REPORT läuft.
bool isCaldav(const char* url) { return strncmp(url, "caldav:", 7) == 0; }
const char* realUrl(const char* url) { return isCaldav(url) ? url + 7 : url; }

// --- Per-Feed-Cache (.bin) ----------------------------------------------------
void slugPaths(const char* url, char* bin, size_t bn, char* etag, size_t en) {
  char slug[48];
  podcast::feedSlug(realUrl(url), slug, sizeof(slug));
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

// --- CalDAV (iCloud u. a.): REPORT calendar-query mit Basic-Auth --------------
constexpr size_t kXmlCap = 256 * 1024;       // REPORT-Antwort (zeitfenster-begrenzt)
char* s_xml = nullptr;

// UTC-Epoche → "YYYYMMDDTHHMMSSZ" (CalDAV time-range-Grenzen).
void fmtUtcZ(uint32_t e, char* out) {
  int y; unsigned m, d; ical::detail::civilFromEpoch(e, &y, &m, &d);
  uint32_t tod = e % 86400u;
  snprintf(out, 18, "%04d%02u%02uT%02u%02u%02uZ", y, m, d,
           (unsigned)(tod / 3600), (unsigned)((tod % 3600) / 60), (unsigned)(tod % 60));
}

// Eine (XML-escapte) iCalendar-Zeile entescapen → out.
void xmlUnescapeLine(const char* s, const char* e, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = s; p < e && o + 1 < cap; ) {
    if (*p == '&') {
      const char* sc = p + 1; const char* semi = sc;
      while (semi < e && *semi != ';' && (size_t)(semi - sc) < 10) semi++;
      if (semi < e && *semi == ';') {
        size_t nlen = (size_t)(semi - sc);
        if (nlen >= 2 && sc[0] == '#') {
          uint32_t cp = (sc[1] == 'x' || sc[1] == 'X') ? (uint32_t)strtoul(sc + 2, nullptr, 16)
                                                       : (uint32_t)strtoul(sc + 1, nullptr, 10);
          if (cp == 13 || cp == 10) { p = semi + 1; continue; }   // CR/LF verwerfen
          if (cp && cp < 128) { out[o++] = (char)cp; p = semi + 1; continue; }
        } else if (nlen == 3 && memcmp(sc, "amp", 3) == 0)  { out[o++] = '&';  p = semi + 1; continue; }
        else if (nlen == 2 && memcmp(sc, "lt", 2) == 0)     { out[o++] = '<';  p = semi + 1; continue; }
        else if (nlen == 2 && memcmp(sc, "gt", 2) == 0)     { out[o++] = '>';  p = semi + 1; continue; }
        else if (nlen == 4 && memcmp(sc, "quot", 4) == 0)   { out[o++] = '"';  p = semi + 1; continue; }
        else if (nlen == 4 && memcmp(sc, "apos", 4) == 0)   { out[o++] = '\''; p = semi + 1; continue; }
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}

bool fetchCaldav(const char* url, const char* user, const char* pass,
                 calendar::CalEvent* feedEv, int* feedN, uint32_t ws, uint32_t we) {
  *feedN = 0;
  if (!s_xml) {
    s_xml = (char*)heap_caps_malloc(kXmlCap, MALLOC_CAP_SPIRAM);
    if (!s_xml) { Serial.println("[CAL] kein PSRAM fuer CalDAV"); return false; }
  }
  // Server-seitige Zeitfilterung hält die Antwort klein.
  char zs[18], ze[18];
  fmtUtcZ(ws == 0 ? 0 : ws, zs);
  fmtUtcZ(we == 0xFFFFFFFFu ? (uint32_t)(timesync::now() + 365u * 86400u) : we, ze);
  String body =
    String("<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
           "<d:prop><c:calendar-data/></d:prop>"
           "<c:filter><c:comp-filter name=\"VCALENDAR\"><c:comp-filter name=\"VEVENT\">"
           "<c:time-range start=\"") + zs + "\" end=\"" + ze + "\"/>"
           "</c:comp-filter></c:comp-filter></c:filter></c:calendar-query>";

  bool https = String(url).startsWith("https");
  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  bool begun = https ? (cs.setInsecure(), http.begin(cs, url)) : http.begin(cp, url);
  if (!begun) return false;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setUserAgent("fennek");
  if (user && user[0]) http.setAuthorization(user, pass);
  http.addHeader("Depth", "1");
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.sendRequest("REPORT", (uint8_t*)body.c_str(), body.length());
  if (code != 207 && code != 200) { Serial.printf("[CAL] REPORT HTTP %d\n", code); http.end(); return false; }

  WiFiClient* st = http.getStreamPtr();
  size_t o = 0; uint32_t idle = millis();
  while ((http.connected() || st->available()) && o + 1 < kXmlCap) {
    int avail = st->available();
    if (avail <= 0) { if (millis() - idle > 6000) break; delay(2); continue; }
    int want = (avail < (int)(kXmlCap - 1 - o)) ? avail : (int)(kXmlCap - 1 - o);
    int r = st->read((uint8_t*)s_xml + o, want);
    if (r > 0) { o += r; idle = millis(); }
  }
  s_xml[o] = '\0';
  http.end();

  // <calendar-data>…</calendar-data>-Bloecke aus dem multistatus ziehen, jeden
  // (XML-escapt) als VCALENDAR an den iCal-Parser fuettern.
  const char* end = s_xml + o;
  ical::Parser parser;
  int n = 0;
  auto emit = [&](ical::Event& ev) {
    ical::expand(ev, ws, we, [&](uint32_t s, uint32_t e) {
      if (n >= kMaxPerFeed) return;
      calendar::CalEvent& c = feedEv[n++];
      c.start = s; c.end = e; c.allDay = ev.allDay ? 1 : 0;
      strncpy(c.summary, ev.summary, sizeof(c.summary) - 1); c.summary[sizeof(c.summary) - 1] = '\0';
      strncpy(c.location, ev.location, sizeof(c.location) - 1); c.location[sizeof(c.location) - 1] = '\0';
    });
  };

  const char* p = s_xml;
  char lbuf[320];
  while (n < kMaxPerFeed) {
    const char* cd = podcast::detail::ciFind(p, end, "calendar-data");
    if (!cd) break;
    const char* gt = (const char*)memchr(cd, '>', (size_t)(end - cd));
    if (!gt) break;
    if (gt > cd && gt[-1] == '/') { p = gt + 1; continue; }   // <calendar-data/>
    const char* content = gt + 1;
    const char* close = podcast::detail::ciFind(content, end, "calendar-data>");
    const char* be = close ? close : end;
    if (close) { while (be > content && *be != '<') be--; }    // bis zum "</" zurück

    parser.reset();
    for (const char* s = content; s < be; ) {
      const char* nl = s;
      while (nl < be && *nl != '\n') nl++;
      const char* le = nl;
      while (le > s && le[-1] == '\r') le--;
      xmlUnescapeLine(s, le, lbuf, sizeof(lbuf));
      ical::Event ev;
      if (parser.feedLine(lbuf, &ev)) emit(ev);
      s = (nl < be) ? nl + 1 : nl;
    }
    { ical::Event ev; if (parser.finish(&ev)) emit(ev); }
    p = close ? close + 14 : end;
  }

  *feedN = n;
  return true;
}

bool syncCore(bool useBackoff, char* log, size_t logN) {
  auto setLog = [&](const char* m) { if (log && logN) { strncpy(log, m, logN - 1); log[logN - 1] = '\0'; } };

  static char urls[kMaxFeeds][192];
  int feeds = readFeeds(urls, kMaxFeeds);
  if (feeds == 0) { setLog("keine Feeds"); return false; }
  if (settings::wifiCount() == 0) { setLog("kein WLAN konfiguriert"); return false; }
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

  bool connected = wifi::connect(kConnectTimeoutMs);

  int fetched = 0;
  if (connected) {
    char cdUser[64], cdPass[65];
    settings::calDavUser(cdUser, sizeof(cdUser));
    settings::calDavPass(cdPass, sizeof(cdPass));
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
      bool ok = isCaldav(urls[i])
          ? fetchCaldav(realUrl(urls[i]), cdUser, cdPass, feedEv, &fn, ws, we)
          : fetchFeed(urls[i], prevEtag, newEtag, sizeof(newEtag), feedEv, &fn, ws, we);
      if (ok) {
        // frisch geladen → Cache schreiben, in Liste aufnehmen.
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

  wifi::disconnect();

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
