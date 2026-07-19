// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "calibre_books.h"
#include "config.h"
#include "core/board.h"
#include "core/power.h"
#include "core/settings.h"
#include "services/audio.h"
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
#include <stdlib.h>
#include <string.h>

namespace {

const char* kManifest = "/.fennek/calibre.tsv";
const char* kBooksDir = "/books";
const char* kTmpFile  = "/books/.calibre.part";

constexpr int      kMaxIds           = 512;    // Regal-Einträge/Manifest (RAM-Deckel)
constexpr int      kMaxNewPerSync    = 8;      // Downloads pro Lauf (Zeit-Deckel)
constexpr int      kMaxFeedPages     = 8;      // OPDS-Pagination (rel="next")
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint16_t kHttpTimeoutMs    = 15000;
constexpr uint32_t kStallMs          = 10000;
constexpr size_t   kFeedCap          = 256 * 1024;   // eine OPDS-Feed-Seite (PSRAM)

// Auto-Sync-Zügelung: Bücher ändern sich selten — Mindestabstand zwischen
// erfolgreichen Läufen (nicht jeder Standby soll das WLAN hochfahren,
// Podcast-Lektion v2.5.9) plus RTC-RAM-Back-off nach Fehlern (wie calendar).
RTC_DATA_ATTR uint32_t s_lastSyncEpoch    = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
constexpr uint32_t kAutoMinIntervalSecs = 6UL * 3600;
constexpr uint32_t kBackoffBaseSecs     = 1800;
constexpr uint32_t kBackoffMaxSecs      = 24UL * 3600;
constexpr uint8_t  kBackoffMaxShift     = 6;

// Zugangsdaten für die Dauer eines Sync-Laufs (Basic-Auth auf jedem Request).
char s_user[64] = "";
char s_pass[65] = "";

// --- Manifest (/.fennek/calibre.tsv: ID<TAB>Dateiname) -------------------------
int loadManifest(uint32_t* ids, int maxN) {
  int n = 0;
  if (!board::sdReady()) return 0;
  spiLock();
  // exists() vor open(): FILE_READ auf fehlende Datei spammt einen VFS-Fehler.
  File f = SD.exists(kManifest) ? SD.open(kManifest, FILE_READ) : File();
  if (f) {
    char line[192];
    while (f.available() && n < maxN) {
      int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[len] = '\0';
      uint32_t id = (uint32_t)strtoul(line, nullptr, 10);
      if (id) ids[n++] = id;
    }
    f.close();
  }
  spiUnlock();
  return n;
}

void appendManifest(uint32_t id, const char* name) {
  spiLock();
  SD.mkdir("/.fennek");
  File f = SD.open(kManifest, FILE_APPEND);
  if (f) { f.printf("%lu\t%s\n", (unsigned long)id, name); f.close(); }
  spiUnlock();
}

// HTTP-GET mit Basic-Auth + manueller Redirect-Auflösung (wie podcast, max 4
// Hops — deckt Reverse-Proxy-Umleitungen http→https ab). Bei Erfolg bleibt
// http offen, Aufrufer ruft http.end().
WiFiClient* httpOpenFollow(HTTPClient& http, WiFiClientSecure& cs, WiFiClient& cp,
                           const char* url, int* code, bool wantDisposition) {
  String cur = url;
  for (int hop = 0; hop < 4; ++hop) {
    bool https = cur.startsWith("https");
    bool begun = https ? (cs.setInsecure(), http.begin(cs, cur)) : http.begin(cp, cur);
    if (!begun) { *code = -1; return nullptr; }
    http.setUserAgent("fennek");
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    if (s_user[0]) http.setAuthorization(s_user, s_pass);
    if (wantDisposition) {
      const char* hdr[] = { "Location", "Content-Disposition" };
      http.collectHeaders(hdr, 2);
    } else {
      const char* hdr[] = { "Location" };
      http.collectHeaders(hdr, 1);
    }
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
  *code = -2;
  return nullptr;
}

// filename="..." aus dem Content-Disposition-Header ziehen; danach für FAT
// säubern (Nicht-ASCII/Sonderzeichen → '_', Länge deckeln, ".epub" sichern).
// Calibre liefert im filename= bereits eine ASCII-Fassung des Titels.
void safeFilename(const String& disp, uint32_t id, char* out, size_t cap) {
  char raw[160] = "";
  const char* f = strstr(disp.c_str(), "filename=\"");
  if (f) {
    f += 10;
    size_t o = 0;
    while (*f && *f != '"' && o < sizeof(raw) - 1) raw[o++] = *f++;
    raw[o] = '\0';
  }
  size_t o = 0;
  for (const char* p = raw; *p && o + 1 < cap && o < 100; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c > 0x7E || strchr("\\/:*?\"<>|", (char)c)) c = '_';
    if (o == 0 && (c == '.' || c == ' ')) continue;   // keine versteckten Dateien
    out[o++] = (char)c;
  }
  out[o] = '\0';
  if (!out[0]) { snprintf(out, cap, "calibre-%lu.epub", (unsigned long)id); return; }
  size_t l = strlen(out);
  if (l < 5 || strcasecmp(out + l - 5, ".epub") != 0) {
    if (l + 5 >= cap) l = cap - 6;
    snprintf(out + l, cap - l, ".epub");
  }
}

// GET → Puffer (Basic-Auth + Redirects). Rückgabe Bytes, -401 Auth-Fehler, -1 sonst.
int fetchBody(const char* url, char* body, size_t cap) {
  // HTTPClient + WiFiClientSecure auf den Heap (Stack-Budget des loopTask, wie podcast).
  HTTPClient*       http = new HTTPClient();
  WiFiClientSecure* cs   = new WiFiClientSecure();
  WiFiClient*       cp   = new WiFiClient();
  int code = 0;
  WiFiClient* st = httpOpenFollow(*http, *cs, *cp, url, &code, false);
  if (!st) {
    Serial.printf("[CALIBRE] Feed HTTP %d (%s)\n", code, url);
    delete http; delete cs; delete cp;
    return (code == 401) ? -401 : -1;
  }

  size_t total = 0;
  uint32_t last = millis();
  while (total < cap - 1) {
    int av = st->available();
    if (av > 0) {
      int want = ((size_t)av > cap - 1 - total) ? (int)(cap - 1 - total) : av;
      int r = st->readBytes((uint8_t*)body + total, want);
      if (r > 0) { total += r; last = millis(); }
    } else {
      if (!http->connected() && st->available() == 0) break;
      if (millis() - last > kStallMs) break;
      delay(5);
    }
  }
  body[total] = '\0';
  http->end();
  delete http; delete cs; delete cp;
  return (int)total;
}

// Relative OPDS-hrefs (führendes '/') gegen die Basis-URL auflösen.
void absUrl(const char* base, const char* href, char* out, size_t cap) {
  if (strncmp(href, "http", 4) == 0) snprintf(out, cap, "%s", href);
  else                               snprintf(out, cap, "%s%s%s", base, href[0] == '/' ? "" : "/", href);
}

// Regal-Feed-URL bestimmen: reine Ziffern in cbshelf = direkte Shelf-ID, sonst
// den Namen im /opds/shelfindex-Feed suchen (Titel-Vergleich, case-insensitiv).
// Rückgabe: 0 = gefunden (out gesetzt), sonst Fehlercode von fetchBody bzw. -404.
int resolveShelfUrl(const char* base, const char* shelf, char* body, size_t cap,
                    char* out, size_t outCap) {
  bool numeric = shelf[0] != '\0';
  for (const char* p = shelf; *p; p++) if (*p < '0' || *p > '9') { numeric = false; break; }
  if (numeric) { snprintf(out, outCap, "%s/opds/shelf/%s", base, shelf); return 0; }

  static char url[200];
  snprintf(url, sizeof(url), "%s/opds/shelfindex", base);
  int n = fetchBody(url, body, cap);
  if (n < 0) return n;

  const size_t want = strlen(shelf);
  const char* p = body;
  while ((p = strstr(p, "<entry")) != nullptr) {
    const char* entryEnd = strstr(p, "</entry>");
    if (!entryEnd) break;
    const char* t = strstr(p, "<title");
    if (t && t < entryEnd) t = strchr(t, '>');
    const char* te = t ? strstr(t, "</title") : nullptr;
    if (t && te && te < entryEnd) {
      t++;
      while (t < te && (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n')) t++;
      const char* tf = te;
      while (tf > t && (tf[-1] == ' ' || tf[-1] == '\t' || tf[-1] == '\r' || tf[-1] == '\n')) tf--;
      // Exakter Name ODER Name + " (" — öffentliche Regale rendern als
      // "<Name> (Public)" (lokalisiert), private als "<Name>".
      const size_t tlen = (size_t)(tf - t);
      bool match = tlen >= want && strncasecmp(t, shelf, want) == 0 &&
                   (tlen == want || (tlen >= want + 2 && t[want] == ' ' && t[want + 1] == '('));
      if (match) {
        const char* h = strstr(p, "/opds/shelf/");
        if (h && h < entryEnd) {
          unsigned long sid = strtoul(h + 12, nullptr, 10);
          snprintf(out, outCap, "%s/opds/shelf/%lu", base, sid);
          return 0;
        }
      }
    }
    p = entryEnd + 8;
  }
  return -404;   // Regal nicht gefunden
}

// Buch-IDs aus einem OPDS-Feed sammeln: jeder EPUB-Acquisition-Link enthält
// "/opds/download/<id>/epub". Rückgabe: neue Gesamtzahl in ids.
int collectIds(const char* body, uint32_t* ids, int maxN, int n) {
  const char* p = body;
  while (n < maxN && (p = strstr(p, "/opds/download/")) != nullptr) {
    p += 15;
    char* endp = nullptr;
    unsigned long v = strtoul(p, &endp, 10);
    if (endp != p && strncmp(endp, "/epub", 5) == 0) {
      bool dup = false;
      for (int k = 0; k < n; k++) if (ids[k] == (uint32_t)v) { dup = true; break; }
      if (!dup) ids[n++] = (uint32_t)v;
    }
    p = (endp != p) ? endp : p + 1;
  }
  return n;
}

// href des <link rel="next" ...>-Tags (OPDS-Pagination); Attribut-Reihenfolge variiert.
bool findNextHref(const char* body, char* out, size_t cap) {
  const char* p = body;
  while ((p = strstr(p, "<link")) != nullptr) {
    const char* end = strchr(p, '>');
    if (!end) return false;
    const char* rel = strstr(p, "rel=\"next\"");
    if (rel && rel < end) {
      const char* h = strstr(p, "href=\"");
      if (h && h < end) {
        h += 6;
        size_t o = 0;
        while (*h && *h != '"' && o + 1 < cap) out[o++] = *h++;
        out[o] = '\0';
        return o > 0;
      }
    }
    p = end;
  }
  return false;
}

// --- Download: {base}/opds/download/<id>/epub/ → /books/<name> -------------------
// Netz-I/O lock-frei, SD-Writes unter spiLock (webfm-Regel). *noEpub bei 404
// (Format zwischenzeitlich entfernt → dauerhaft überspringen). true = Datei
// liegt in /books (frisch geladen oder war schon da, z. B. manuell kopiert).
bool downloadOne(const char* base, uint32_t id,
                 char* nameOut, size_t nameCap, bool* noEpub) {
  *noEpub = false;
  static char url[200];
  snprintf(url, sizeof(url), "%s/opds/download/%lu/epub/", base, (unsigned long)id);

  HTTPClient*       http = new HTTPClient();
  WiFiClientSecure* cs   = new WiFiClientSecure();
  WiFiClient*       cp   = new WiFiClient();
  int code = 0;
  WiFiClient* st = httpOpenFollow(*http, *cs, *cp, url, &code, true);
  if (!st) {
    if (code == 404) *noEpub = true;
    Serial.printf("[CALIBRE] Buch %lu: HTTP %d\n", (unsigned long)id, code);
    delete http; delete cs; delete cp;
    return false;
  }

  safeFilename(http->header("Content-Disposition"), id, nameOut, nameCap);
  char dest[160];
  snprintf(dest, sizeof(dest), "%s/%s", kBooksDir, nameOut);

  spiLock();
  bool exists = SD.exists(dest);
  spiUnlock();
  if (exists) {
    // Gleicher Name schon in /books (manuell kopiert oder Titel-Doppelung):
    // nichts laden, nur im Manifest vermerken.
    http->end(); delete http; delete cs; delete cp;
    Serial.printf("[CALIBRE] %s existiert schon\n", nameOut);
    return true;
  }

  int contentLen = http->getSize();
  Serial.printf("[CALIBRE] Buch %lu: %d B -> %s\n", (unsigned long)id, contentLen, nameOut);

  spiLock();
  if (!SD.exists(kBooksDir)) SD.mkdir(kBooksDir);
  SD.remove(kTmpFile);
  File f = SD.open(kTmpFile, FILE_WRITE);
  bool open = (bool)f;
  spiUnlock();
  if (!open) {
    http->end(); delete http; delete cs; delete cp;
    Serial.println("[CALIBRE] SD-Datei nicht offen");
    return false;
  }

  int bufSz = 64 * 1024;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { bufSz = 8 * 1024; buf = (uint8_t*)malloc(bufSz); }
  if (!buf) {
    spiLock(); f.close(); spiUnlock();
    http->end(); delete http; delete cs; delete cp;
    return false;
  }

  size_t written = 0;
  uint32_t last = millis();
  bool ok = true;
  for (;;) {
    int av = st->available();
    if (av > 0) {
      int want = av > bufSz ? bufSz : av;
      int r = st->readBytes(buf, want);            // Netz: ohne Lock
      if (r > 0) {
        spiLock();
        size_t w = f.write(buf, r);                // SD: unter Lock
        spiUnlock();
        if (w != (size_t)r) { ok = false; break; }
        if ((written >> 18) != ((written + r) >> 18))   // alle 256 KB Fortschritt
          Serial.printf("[CALIBRE]   %u KB\n", (unsigned)((written + r) >> 10));
        written += r;
        last = millis();
        power::noteActivity();                     // Standby-Timer zurücksetzen
        // Content-Length erreicht → fertig. Nicht auf Verbindungsende warten:
        // Traefik/Calibre-Web halten die Verbindung offen (Keep-Alive), sonst
        // liefe der Stall-Timeout ins Leere.
        if (contentLen > 0 && written >= (size_t)contentLen) break;
      }
    } else {
      if (!http->connected() && st->available() == 0) break;
      if (millis() - last > kStallMs) {
        ok = false;
        Serial.printf("[CALIBRE] Download-Timeout nach %u B (connected=%d)\n",
                      (unsigned)written, http->connected() ? 1 : 0);
        break;
      }
      delay(5);
    }
  }
  free(buf);
  spiLock(); f.close(); spiUnlock();
  http->end();
  delete http; delete cs; delete cp;

  if (ok && contentLen > 0 && written < (size_t)contentLen) {
    Serial.printf("[CALIBRE] %s: unvollstaendig %u/%d\n", nameOut, (unsigned)written, contentLen);
    ok = false;
  }
  if (ok && written == 0) ok = false;

  spiLock();
  if (ok) ok = SD.rename(kTmpFile, dest);
  if (!ok) SD.remove(kTmpFile);
  spiUnlock();
  if (ok) Serial.printf("[CALIBRE] %s (%u KB)\n", nameOut, (unsigned)(written >> 10));
  return ok;
}

// --- Sync-Kern -------------------------------------------------------------------
bool syncCore(bool autoRun, char* log, size_t logN) {
  auto setLog = [&](const char* m) { if (log && logN) { strncpy(log, m, logN - 1); log[logN - 1] = '\0'; } };

  char base[160];
  settings::calibreUrl(base, sizeof(base));
  size_t bl = strlen(base);
  while (bl && base[bl - 1] == '/') base[--bl] = '\0';   // trailing '/' kappen
  char shelf[64];
  settings::calibreShelf(shelf, sizeof(shelf));
  settings::calibreUser(s_user, sizeof(s_user));
  settings::calibrePass(s_pass, sizeof(s_pass));

  if (!base[0])           { setLog("keine Server-URL"); return false; }
  if (settings::wifiCount() == 0) { setLog("kein WLAN konfiguriert"); return false; }
  if (!board::sdReady())  { setLog("keine SD-Karte"); return false; }
  if (webfm::state() != webfm::State::OFF) { setLog("WLAN belegt"); return false; }

  uint32_t nowE = timesync::now();
  if (autoRun) {
    if (s_lastSyncEpoch && nowE >= s_lastSyncEpoch &&
        nowE - s_lastSyncEpoch < kAutoMinIntervalSecs) {
      setLog("Intervall nicht erreicht");
      return true;
    }
    if (s_failCount > 0 && s_lastAttemptEpoch != 0 && nowE >= s_lastAttemptEpoch) {
      uint8_t shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
      uint32_t wait = kBackoffBaseSecs << shift;
      if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
      if (nowE - s_lastAttemptEpoch < wait) { setLog("Back-off aktiv"); return false; }
    }
    s_lastAttemptEpoch = nowE;
  }

  bool connected = wifi::connect(kConnectTimeoutMs);

  int  found = -1, fresh = 0, failed = 0;
  bool authFail = false, more = false, noShelf = false;
  if (connected) {
    uint32_t* ids = (uint32_t*)heap_caps_malloc(sizeof(uint32_t) * kMaxIds * 2, MALLOC_CAP_SPIRAM);
    if (!ids) ids = (uint32_t*)malloc(sizeof(uint32_t) * kMaxIds * 2);
    char* body = (char*)heap_caps_malloc(kFeedCap, MALLOC_CAP_SPIRAM);
    if (ids && body) {
      uint32_t* have = ids + kMaxIds;
      int haveN = loadManifest(have, kMaxIds);

      // Regal auflösen, dann Feed-Seiten einsammeln (rel="next"-Pagination).
      char pageUrl[240];
      int err = resolveShelfUrl(base, shelf, body, kFeedCap, pageUrl, sizeof(pageUrl));
      if (err == 0) {
        found = 0;
        for (int page = 0; page < kMaxFeedPages && pageUrl[0]; page++) {
          int bn = fetchBody(pageUrl, body, kFeedCap);
          if (bn < 0) { if (bn == -401) authFail = true; found = -1; break; }
          found = collectIds(body, ids, kMaxIds, found);
          char next[224];
          if (findNextHref(body, next, sizeof(next))) absUrl(base, next, pageUrl, sizeof(pageUrl));
          else pageUrl[0] = '\0';
        }
      } else if (err == -401) {
        authFail = true;
      } else if (err == -404) {
        noShelf = true;
      }
      int started = 0;
      for (int i = 0; i < found; i++) {
        bool known = false;
        for (int k = 0; k < haveN; k++) if (have[k] == ids[i]) { known = true; break; }
        if (known) continue;
        if (started >= kMaxNewPerSync) { more = true; break; }   // Rest beim nächsten Lauf
        started++;
        char name[128];
        bool noEpub = false;
        if (downloadOne(base, ids[i], name, sizeof(name), &noEpub)) {
          appendManifest(ids[i], name);
          fresh++;
        } else if (noEpub) {
          appendManifest(ids[i], "-");   // kein EPUB-Format: dauerhaft überspringen
        } else {
          failed++;
        }
      }
    }
    if (ids)  free(ids);
    if (body) free(body);
  }

  wifi::disconnect();

  bool ok = connected && found >= 0 && failed == 0;
  if (autoRun) {
    if (ok) {
      s_failCount = 0;
      // Bei gedeckeltem Lauf (more) kein Intervall-Stempel: der nächste
      // Standby setzt den Rest fort statt 6 h zu warten.
      if (!more) s_lastSyncEpoch = nowE;
    } else if (s_failCount < 0xFF) {
      s_failCount++;
    }
  }

  char m[64];
  if (!connected)     snprintf(m, sizeof(m), "WLAN-Fehler");
  else if (authFail)  snprintf(m, sizeof(m), "Auth-Fehler (User/Passwort?)");
  else if (noShelf)   snprintf(m, sizeof(m), "Regal nicht gefunden");
  else if (found < 0) snprintf(m, sizeof(m), "Server-Fehler");
  else snprintf(m, sizeof(m), "%d im Regal, %d neu, %d Fehler%s",
                found, fresh, failed, more ? ", Rest folgt" : "");
  setLog(m);
  Serial.printf("[CALIBRE] %s\n", m);
  return ok;
}

}  // namespace

namespace calibre_books {

bool sync(char* log, size_t logN) { return syncCore(false, log, logN); }

bool flushBeforeStandby() {
  if (!settings::calibreAutoSync()) return true;
  char dummy[8];
  return syncCore(true, dummy, sizeof(dummy));
}

int syncedCount() {
  int n = 0;
  if (!board::sdReady()) return 0;
  spiLock();
  File f = SD.exists(kManifest) ? SD.open(kManifest, FILE_READ) : File();
  if (f) {
    char line[192];
    while (f.available()) {
      int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[len] = '\0';
      if (line[0]) n++;
    }
    f.close();
  }
  spiUnlock();
  return n;
}

}  // namespace calibre_books
