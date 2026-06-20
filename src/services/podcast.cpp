// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "podcast.h"
#include "config.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/audio.h"
#include "services/timesync.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"
#include "apps/podcast_core.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr const char* kDir   = "/podcasts";
constexpr const char* kFeeds = "/podcasts/feeds.txt";

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kHttpTimeoutMs    = 15000;
constexpr uint32_t kStallMs          = 20000;   // Download/Feed-Stillstand → Abbruch
constexpr int      kRssCap           = 96 * 1024;  // Anfang des Feeds (erstes <item>)
constexpr int      kDlBuf            = 4096;

// Back-off für den Pre-Standby-Sync (RTC-RAM überlebt Deep Sleep), analog zu
// services/scrobble.cpp / notes_ai.cpp.
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
RTC_DATA_ATTR uint16_t s_rrIndex          = 0;   // Round-Robin-Feed-Zeiger
constexpr uint32_t kBackoffBaseSecs = 1800;        // 30 min nach 1. Fehlschlag
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600; // Deckel 24 h
constexpr uint8_t  kBackoffMaxShift = 6;

void copyStr(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = '\0';
}

void rstrip(char* s) {
  for (int i = (int)strlen(s) - 1; i >= 0 && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'); --i)
    s[i] = '\0';
}

bool isAudioName(const char* n) {
  auto ext = [&](const char* e) {
    size_t ln = strlen(n), le = strlen(e);
    return ln > le && strcasecmp(n + ln - le, e) == 0;
  };
  return ext(".mp3") || ext(".m4a") || ext(".aac") || ext(".ogg") || ext(".opus") || ext(".wav");
}

// --- feeds.txt ----------------------------------------------------------------
int readFeeds(podcast::Feed* out, int cap) {
  int n = 0;
  if (!board::sdReady()) return 0;
  spiLock();
  File f = SD.open(kFeeds, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return 0;
  char line[320];
  for (;;) {
    spiLock();
    int len = f.available() ? f.readBytesUntil('\n', line, sizeof(line) - 1) : -1;
    spiUnlock();
    if (len < 0) break;
    line[len] = '\0';
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#') continue;
    if (n >= cap) break;
    podcast::Feed& fd = out[n];
    char* tab = strchr(s, '\t');
    if (tab) { *tab = '\0'; copyStr(fd.name, sizeof(fd.name), tab + 1); rstrip(fd.name); }
    else     { fd.name[0] = '\0'; }
    copyStr(fd.url, sizeof(fd.url), s);
    rstrip(fd.url);
    if (!fd.url[0]) continue;
    podcast::feedSlug(fd.url, fd.slug, sizeof(fd.slug));
    if (!fd.name[0]) copyStr(fd.name, sizeof(fd.name), fd.slug);
    n++;
  }
  spiLock(); f.close(); spiUnlock();
  return n;
}

// --- state.txt ----------------------------------------------------------------
void statePath(const char* slug, char* out, size_t n) {
  snprintf(out, n, "%s/%s/state.txt", kDir, slug);
}

void writeState(const char* slug, const char* title, const char* guid, const char* file) {
  char dir[80]; snprintf(dir, sizeof(dir), "%s/%s", kDir, slug);
  char path[100]; statePath(slug, path, sizeof(path));
  spiLock();
  if (!SD.exists(dir)) SD.mkdir(dir);
  File f = SD.open(path, FILE_WRITE);
  if (f) {
    f.printf("title=%s\nguid=%s\nfile=%s\n", title, guid, file);
    f.close();
  }
  spiUnlock();
}

// --- WLAN auf/zu (WLAN-Regel: Audio stop + Mesh suspend) ----------------------
bool wifiUp(const char* ssid, const char* pass) {
  audio::stop();
  mesh_client::setSuspended(true);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

void wifiDown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mesh_client::setSuspended(false);
}

// HTTP-GET mit manueller Redirect-Auflösung (max 6 Hops; deckt http→https und
// CDN-Umleitungen ab, die der eingebaute Follow je nach Schema-Wechsel verschluckt).
// Bei HTTP 200 bleibt http offen und der Stream wird zurückgegeben (Aufrufer
// ruft http.end()); sonst nullptr.
WiFiClient* httpOpenFollow(HTTPClient& http, WiFiClientSecure& cs, WiFiClient& cp,
                           const char* url, int* code) {
  String cur = url;
  for (int hop = 0; hop < 6; ++hop) {
    bool https = cur.startsWith("https");
    bool begun = https ? (cs.setInsecure(), http.begin(cs, cur)) : http.begin(cp, cur);
    if (!begun) { *code = -1; return nullptr; }
    http.setUserAgent("fennek-podcast");
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    const char* hdr[] = { "Location" };
    http.collectHeaders(hdr, 1);
    int c = http.GET();
    *code = c;
    if (c == 301 || c == 302 || c == 303 || c == 307 || c == 308) {
      String loc = http.header("Location");
      http.end();
      if (loc.length() == 0) return nullptr;
      cur = loc;
      continue;
    }
    if (c == 200) return http.getStreamPtr();
    http.end();
    return nullptr;
  }
  return nullptr;
}

// Feed laden, bis das erste <item> alle Felder hergibt (oder kRssCap/EOF). Spart
// es, den oft riesigen Feed komplett zu ziehen. WLAN muss bereits an sein.
bool fetchLatest(const char* feedUrl, podcast::EpisodeMeta& meta) {
  char* buf = (char*)heap_caps_malloc(kRssCap, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char*)malloc(kRssCap);
  if (!buf) return false;

  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  int code = 0;
  WiFiClient* st = httpOpenFollow(http, cs, cp, feedUrl, &code);
  if (!st) { Serial.printf("[PODCAST] Feed HTTP %d\n", code); free(buf); return false; }

  size_t total = 0; bool got = false; uint32_t last = millis();
  buf[0] = '\0';
  while (total < (size_t)kRssCap - 1) {
    int av = st->available();
    if (av > 0) {
      int want = av > 2048 ? 2048 : av;
      if ((size_t)want > (size_t)kRssCap - 1 - total) want = (int)(kRssCap - 1 - total);
      int r = st->readBytes((uint8_t*)buf + total, want);
      if (r > 0) {
        total += r; buf[total] = '\0'; last = millis();
        meta = podcast::parseLatest(buf);
        if (podcast::isComplete(meta)) { got = true; break; }
      }
    } else {
      if (!http.connected() && st->available() == 0) break;
      if (millis() - last > kStallMs) break;
      delay(10);
    }
  }
  http.end();
  if (!got) { meta = podcast::parseLatest(buf); got = meta.valid; }
  free(buf);
  return got;
}

// Episode streamend auf die SD laden (Netz-I/O lock-frei, SD-Writes unter
// spiLock). Erst in <dest>.part, bei Erfolg umbenannt. WLAN muss an sein.
bool downloadEpisode(const char* url, const char* dest, podcast::Progress prog, const char* feedName) {
  HTTPClient http; WiFiClientSecure cs; WiFiClient cp;
  int code = 0;
  WiFiClient* st = httpOpenFollow(http, cs, cp, url, &code);
  if (!st) { Serial.printf("[PODCAST] Episode HTTP %d\n", code); return false; }
  int contentLen = http.getSize();

  char tmp[170]; snprintf(tmp, sizeof(tmp), "%s.part", dest);
  spiLock();
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  bool open = (bool)f;
  spiUnlock();
  if (!open) { http.end(); Serial.println("[PODCAST] SD-Datei nicht offen"); return false; }

  uint8_t* buf = (uint8_t*)malloc(kDlBuf);
  if (!buf) { spiLock(); f.close(); SD.remove(tmp); spiUnlock(); http.end(); return false; }

  size_t written = 0; uint32_t last = millis(); bool ok = true; int lastPct = -2;
  for (;;) {
    int av = st->available();
    if (av > 0) {
      int want = av > kDlBuf ? kDlBuf : av;
      int r = st->readBytes(buf, want);            // Netz: ohne Lock
      if (r > 0) {
        spiLock();
        size_t w = f.write(buf, r);                // SD: unter Lock
        spiUnlock();
        if (w != (size_t)r) { ok = false; break; }
        written += r; last = millis();
        if (prog && contentLen > 0) {
          int pct = (int)((uint64_t)written * 100 / contentLen);
          if (pct != lastPct) { lastPct = pct; prog(feedName, pct); }
        }
      }
    } else {
      if (!http.connected() && st->available() == 0) break;
      if (millis() - last > kStallMs) { ok = false; Serial.println("[PODCAST] Download-Timeout"); break; }
      delay(5);
    }
  }
  free(buf);
  spiLock(); f.close(); spiUnlock();
  http.end();

  if (ok && contentLen > 0 && written < (size_t)contentLen) {
    Serial.printf("[PODCAST] unvollstaendig %u/%d\n", (unsigned)written, contentLen);
    ok = false;
  }
  if (ok && written == 0) ok = false;
  if (!ok) { spiLock(); SD.remove(tmp); spiUnlock(); return false; }

  spiLock();
  SD.remove(dest);
  bool ren = SD.rename(tmp, dest);
  if (!ren) SD.remove(tmp);
  spiUnlock();
  if (!ren) { Serial.println("[PODCAST] rename fehlgeschlagen"); return false; }
  Serial.printf("[PODCAST] %s: %u Bytes -> %s\n", feedName, (unsigned)written, dest);
  return true;
}

// "Nur letzte Folge behalten": alle Audiodateien im Feed-Ordner außer keepBase
// löschen. Namen erst sammeln, dann löschen (nicht während der Iteration).
void retain(const char* slug, const char* keepBase) {
  char dir[80]; snprintf(dir, sizeof(dir), "%s/%s", kDir, slug);
  char victims[8][64]; int nv = 0;
  spiLock();
  File d = SD.open(dir);
  if (d && d.isDirectory()) {
    File e;
    while ((e = d.openNextFile()) && nv < 8) {
      const char* nm = e.name();
      const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
      bool isDir = e.isDirectory();
      if (!isDir && isAudioName(base) && strcmp(base, keepBase) != 0) {
        copyStr(victims[nv], sizeof(victims[nv]), base);
        nv++;
      }
      e.close();
    }
  }
  if (d) d.close();
  for (int i = 0; i < nv; ++i) {
    char full[160]; snprintf(full, sizeof(full), "%s/%s", dir, victims[i]);
    SD.remove(full);
    Serial.printf("[PODCAST] alte Folge entfernt: %s\n", victims[i]);
  }
  spiUnlock();
}

// Einen Feed synchronisieren (WLAN muss an sein). 1=neu geladen, 0=aktuell, -1=Fehler.
int syncFeedWifiUp(const podcast::Feed& f, podcast::Progress prog) {
  if (prog) prog(f.name, -1);
  podcast::EpisodeMeta meta;
  if (!fetchLatest(f.url, meta)) { Serial.printf("[PODCAST] %s: Feed-Fehler\n", f.name); return -1; }

  podcast::Local loc;
  bool have = podcast::localEpisode(f, &loc);
  if (have && strcmp(loc.guid, meta.guid) == 0) {
    Serial.printf("[PODCAST] %s: aktuell (%s)\n", f.name, meta.title);
    return 0;
  }

  char dir[80]; snprintf(dir, sizeof(dir), "%s/%s", kDir, f.slug);
  spiLock(); if (!SD.exists(dir)) SD.mkdir(dir); spiUnlock();

  char ext[8]; podcast::urlExt(meta.url, ext, sizeof(ext));
  char dest[160]; snprintf(dest, sizeof(dest), "%s/episode%s", dir, ext);
  Serial.printf("[PODCAST] %s: lade '%s'\n", f.name, meta.title);
  if (!downloadEpisode(meta.url, dest, prog, f.name)) return -1;

  char keep[24]; snprintf(keep, sizeof(keep), "episode%s", ext);
  retain(f.slug, keep);
  writeState(f.slug, meta.title, meta.guid, dest);
  return 1;
}

// Gemeinsamer Kern. onlyIdx>=0 = nur dieser Feed; roundRobin = genau ein Feed
// (Round-Robin) — für den Pre-Standby-Lauf, damit das WLAN kurz bleibt.
bool syncCore(int onlyIdx, bool roundRobin, bool useBackoff, char* log, size_t logN,
              podcast::Progress prog) {
  auto setLog = [&](const char* m) { if (log && logN) { strncpy(log, m, logN - 1); log[logN - 1] = '\0'; } };

  if (useBackoff && !settings::podcastAutoSync()) { setLog("Auto-Sync aus"); return true; }
  if (!board::sdReady()) { setLog("keine SD-Karte"); return false; }

  podcast::Feed feeds[podcast::kMaxFeeds];
  int n = readFeeds(feeds, podcast::kMaxFeeds);
  if (n == 0) { setLog("keine Feeds"); return true; }

  char ssid[33], wpass[65];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(wpass, sizeof(wpass));
  if (!ssid[0]) { setLog("kein WLAN konfiguriert"); return false; }
  if (webfm::state() != webfm::State::OFF) { setLog("WLAN belegt"); return false; }

  if (useBackoff) {
    uint32_t cur = timesync::now();
    if (s_failCount > 0 && s_lastAttemptEpoch != 0 && cur >= s_lastAttemptEpoch) {
      uint8_t  shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
      uint32_t wait  = kBackoffBaseSecs << shift;
      if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
      if (cur - s_lastAttemptEpoch < wait) {
        Serial.printf("[PODCAST] Pre-Standby-Sync uebersprungen (Back-off: %u Fehlschlaege, "
                      "warte %lus)\n", (unsigned)s_failCount, (unsigned long)wait);
        setLog("Back-off aktiv");
        return false;
      }
    }
    s_lastAttemptEpoch = cur;
  }

  if (!wifiUp(ssid, wpass)) {
    wifiDown();
    if (useBackoff && s_failCount < 0xFF) s_failCount++;
    setLog("WLAN-Fehler");
    return false;
  }

  int neu = 0, aktuell = 0, fehler = 0;
  auto acc = [&](int r) { if (r == 1) neu++; else if (r == 0) aktuell++; else fehler++; };

  Serial.printf("[PODCAST] WLAN verbunden, synchronisiere ...\n");
  if (roundRobin) {
    int idx = s_rrIndex % n;
    s_rrIndex = (uint16_t)((idx + 1) % n);
    acc(syncFeedWifiUp(feeds[idx], prog));
  } else if (onlyIdx >= 0) {
    if (onlyIdx < n) acc(syncFeedWifiUp(feeds[onlyIdx], prog));
  } else {
    for (int i = 0; i < n; ++i) acc(syncFeedWifiUp(feeds[i], prog));
  }

  wifiDown();

  if (useBackoff) {
    if (fehler == 0) s_failCount = 0;
    else if (s_failCount < 0xFF) s_failCount++;
  }

  char m[64];
  snprintf(m, sizeof(m), "%d neu, %d aktuell, %d Fehler", neu, aktuell, fehler);
  setLog(m);
  Serial.printf("[PODCAST] %s\n", m);
  return fehler == 0;
}

}  // namespace

namespace podcast {

void begin() {
  if (!board::sdReady()) return;
  spiLock();
  if (!SD.exists(kDir)) SD.mkdir(kDir);
  bool haveFeeds = SD.exists(kFeeds);
  spiUnlock();
  if (haveFeeds) return;
  spiLock();
  File f = SD.open(kFeeds, FILE_WRITE);
  if (f) {
    f.printf("# Podcast-Feeds — eine RSS-URL pro Zeile, optional <TAB>Name\n");
    f.printf("%s\t%s\n", kDefaultFeedUrl, kDefaultFeedName);
    f.close();
    Serial.printf("[PODCAST] feeds.txt angelegt (Default: %s)\n", kDefaultFeedName);
  }
  spiUnlock();
}

int feedCount() {
  Feed feeds[kMaxFeeds];
  return readFeeds(feeds, kMaxFeeds);
}

bool feed(int idx, Feed* out) {
  if (idx < 0 || !out) return false;
  Feed feeds[kMaxFeeds];
  int n = readFeeds(feeds, kMaxFeeds);
  if (idx >= n) return false;
  *out = feeds[idx];
  return true;
}

bool addFeed(const char* url, const char* name) {
  if (!url || !url[0] || !board::sdReady()) return false;
  spiLock();
  if (!SD.exists(kDir)) SD.mkdir(kDir);
  File f = SD.open(kFeeds, FILE_APPEND);
  bool ok = (bool)f;
  if (ok) {
    if (name && name[0]) f.printf("%s\t%s\n", url, name);
    else                 f.printf("%s\n", url);
    f.close();
  }
  spiUnlock();
  return ok;
}

bool removeFeed(int idx) {
  Feed feeds[kMaxFeeds];
  int n = readFeeds(feeds, kMaxFeeds);
  if (idx < 0 || idx >= n || !board::sdReady()) return false;
  spiLock();
  File f = SD.open(kFeeds, FILE_WRITE);   // FILE_WRITE trunkiert
  bool ok = (bool)f;
  if (ok) {
    f.printf("# Podcast-Feeds — eine RSS-URL pro Zeile, optional <TAB>Name\n");
    for (int i = 0; i < n; ++i) {
      if (i == idx) continue;
      f.printf("%s\t%s\n", feeds[i].url, feeds[i].name);
    }
    f.close();
  }
  spiUnlock();
  return ok;
}

bool localEpisode(const Feed& f, Local* out) {
  memset(out, 0, sizeof(*out));
  if (!board::sdReady()) return false;
  char path[100]; statePath(f.slug, path, sizeof(path));
  spiLock();
  File fp = SD.open(path, FILE_READ);
  bool open = (bool)fp;
  spiUnlock();
  if (!open) return false;
  char line[256];
  for (;;) {
    spiLock();
    int len = fp.available() ? fp.readBytesUntil('\n', line, sizeof(line) - 1) : -1;
    spiUnlock();
    if (len < 0) break;
    line[len] = '\0';
    rstrip(line);
    if (!strncmp(line, "title=", 6))     copyStr(out->title, sizeof(out->title), line + 6);
    else if (!strncmp(line, "guid=", 5)) copyStr(out->guid,  sizeof(out->guid),  line + 5);
    else if (!strncmp(line, "file=", 5)) copyStr(out->file,  sizeof(out->file),  line + 5);
  }
  spiLock(); fp.close(); spiUnlock();
  if (out->file[0]) {
    spiLock(); bool ex = SD.exists(out->file); spiUnlock();
    if (!ex) out->file[0] = '\0';
  }
  return out->file[0] != '\0';
}

bool syncAll(char* log, size_t logN, Progress prog) {
  return syncCore(-1, false, false, log, logN, prog);
}

bool syncOne(int idx, char* log, size_t logN, Progress prog) {
  return syncCore(idx, false, false, log, logN, prog);
}

bool flushBeforeStandby() {
  char dummy[8];
  return syncCore(-1, true, true, dummy, sizeof(dummy), nullptr);
}

}  // namespace podcast
