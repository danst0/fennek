// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "notes_ai.h"
#include "config.h"
#include "core/board.h"
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
#include <mbedtls/sha256.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

namespace {

constexpr const char* kNotesDir  = "/notes";
constexpr const char* kStateFile = "/.fennek/notes_ai.done";  // "Dateiname<TAB>SHA256"/Zeile
constexpr int kFileLen   = 16;     // "YYYY-MM-DD.md" + Reserve
constexpr int kHashLen   = 65;     // SHA256 als Hex (64) + NUL
constexpr int kMaxDone   = 400;    // gut ein Jahr Tagesnotizen
constexpr int kMaxPerRun = 20;     // je Standby hoechstens so viele (WLAN kurz halten)
constexpr int kNoteCap   = 8192;   // RAM-Puffer pro Notiz (wie notes_app)
constexpr int kBodyCap   = 49152;  // JSON-Request (Notiz escaped + Wrapper)
constexpr int kRespCap   = 49152;  // JSON-Response von Ollama

constexpr uint32_t kConnectTimeoutMs = 15000;
// HTTPClient::setTimeout() nimmt ein uint16_t (max. 65535 ms) — 120000 lief
// still auf 54464 ms über. Das ist die Wartezeit auf die Antwort-Header, also
// das Zeitfenster, in dem Ollama (stream:false) generieren darf, bevor das
// erste Byte kommt. Wir schöpfen den uint16_t-Bereich knapp aus; der Body-Read
// danach ist ohnehin separat über kReadTimeoutMs abgesichert.
constexpr uint16_t kGenTimeoutMs     = 65000;   // LLM-Generierung kann dauern
constexpr uint32_t kReadTimeoutMs    = 30000;

// System-Prompt: so WENIG wie moeglich aendern. Der Weg zu Ollama ist reines
// UTF-8/HTTP (keine CP437-Wandlung wie beim Display) — daher echte Umlaute.
// Ziel: nur Rechtschreibung/Umlaute/Zeichensetzung korrigieren, NICHT umdichten.
const char* kSystem =
  "Du bist ein behutsamer Korrekturleser. Korrigiere in der folgenden Notiz nur "
  "Rechtschreibung (besonders fehlende Umlaute wie ä/ö/ü/ß), Tippfehler und "
  "Zeichensetzung. Behalte Wortwahl, Satzbau, Reihenfolge und den Stil der "
  "Notiz so weit wie möglich bei — formuliere NICHT um und gliedere nicht neu. "
  "Nur wenn ein Satz sonst unverständlich bliebe, ergänze minimal das Nötigste. "
  "Erfinde keine Inhalte und lasse nichts weg. Antworte ausschließlich mit dem "
  "korrigierten Notiztext, ohne Einleitung, Erklärungen oder Anführungszeichen.";

// Back-off fuer den Pre-Standby-Lauf (RTC-RAM ueberlebt Deep Sleep), analog zu
// services/scrobble.cpp.
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;
constexpr uint32_t kBackoffBaseSecs = 1800;        // 30 min nach 1. Fehlschlag
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600; // Deckel 24 h
constexpr uint8_t  kBackoffMaxShift = 6;

// --- kleine Helfer ------------------------------------------------------------
bool hasExt(const char* n, const char* e) {
  size_t ln = strlen(n), le = strlen(e);
  return ln > le && strcasecmp(n + ln - le, e) == 0;
}

// Heutige Tagesnotiz (lokale Zeit ueber timesync) — wird ausgelassen.
void todayFile(char* out, size_t n) {
  time_t t = (time_t)timesync::now();
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d.md", &tmv);
}

// Basis-URL ohne abschliessenden '/'.
void baseUrl(char* out, size_t n) {
  settings::aiUrl(out, n);
  size_t l = strlen(out);
  while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
}

// JSON-String-Escape (Notiz/Modell in den Request). UTF-8-Bytes (>=0x80)
// laufen unveraendert durch (gueltig in JSON). Gibt false bei Pufferende.
bool jsonAppendEscaped(char* dst, size_t cap, size_t* len, const char* src) {
  static const char* hx = "0123456789abcdef";
  size_t o = *len;
  for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
    char esc[8]; int el;
    unsigned char c = *p;
    switch (c) {
      case '"':  esc[0]='\\'; esc[1]='"';  el=2; break;
      case '\\': esc[0]='\\'; esc[1]='\\'; el=2; break;
      case '\n': esc[0]='\\'; esc[1]='n';  el=2; break;
      case '\r': esc[0]='\\'; esc[1]='r';  el=2; break;
      case '\t': esc[0]='\\'; esc[1]='t';  el=2; break;
      default:
        if (c < 0x20) {
          esc[0]='\\'; esc[1]='u'; esc[2]='0'; esc[3]='0';
          esc[4]=hx[(c>>4)&0xF]; esc[5]=hx[c&0xF]; el=6;
        } else { esc[0]=(char)c; el=1; }
    }
    if (o + el >= cap) { dst[o] = '\0'; *len = o; return false; }
    memcpy(dst + o, esc, el); o += el;
  }
  dst[o] = '\0'; *len = o;
  return true;
}

// Codepoint als UTF-8 anhaengen (fuer \uXXXX in der Antwort).
void appendUtf8(char* dst, size_t cap, size_t* len, uint32_t cp) {
  size_t o = *len;
  auto put = [&](char c){ if (o + 1 < cap) dst[o++] = c; };
  if (cp < 0x80) put((char)cp);
  else if (cp < 0x800) { put((char)(0xC0|(cp>>6))); put((char)(0x80|(cp&0x3F))); }
  else if (cp < 0x10000) { put((char)(0xE0|(cp>>12))); put((char)(0x80|((cp>>6)&0x3F))); put((char)(0x80|(cp&0x3F))); }
  else { put((char)(0xF0|(cp>>18))); put((char)(0x80|((cp>>12)&0x3F))); put((char)(0x80|((cp>>6)&0x3F))); put((char)(0x80|(cp&0x3F))); }
  *len = o;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Wert des JSON-String-Schluessels "key" aus body extrahieren + entescapen.
// Minimal-Parser (kein ArduinoJson). false = Schluessel/String nicht gefunden.
bool jsonExtractString(const char* body, const char* key, char* out, size_t cap) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(body, needle);
  if (!p) return false;
  p += strlen(needle);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return false;
  p++;                                   // hinter das oeffnende "
  size_t o = 0;
  while (*p && o + 4 < cap) {
    char c = *p++;
    if (c == '"') break;                 // unescaptes " = Stringende
    if (c == '\\') {
      char e = *p++;
      switch (e) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case '/': out[o++] = '/';  break;
        case '"': out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case 'u': {
          int h0=hexVal(p[0]), h1=hexVal(p[1]), h2=hexVal(p[2]), h3=hexVal(p[3]);
          if (h0<0||h1<0||h2<0||h3<0) { out[o]='\0'; return o>0; }
          uint32_t cp = (h0<<12)|(h1<<8)|(h2<<4)|h3; p += 4;
          // Surrogat-Paar (UTF-16) zusammensetzen.
          if (cp >= 0xD800 && cp <= 0xDBFF && p[0]=='\\' && p[1]=='u') {
            int g0=hexVal(p[2]),g1=hexVal(p[3]),g2=hexVal(p[4]),g3=hexVal(p[5]);
            if (g0>=0&&g1>=0&&g2>=0&&g3>=0) {
              uint32_t lo = (g0<<12)|(g1<<8)|(g2<<4)|g3; p += 6;
              cp = 0x10000 + ((cp-0xD800)<<10) + (lo-0xDC00);
            }
          }
          appendUtf8(out, cap, &o, cp);
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

// --- Done-Liste (/.fennek/notes_ai.done) --------------------------------------
// Pro polierter Notiz: Dateiname + SHA256 (Hex) der polierten Fassung. Weicht
// der aktuelle Inhalts-Hash davon ab, wurde die Notiz seither editiert → erneut
// polieren. (Genauer Aenderungsindikator — auch laengengleiche Edits triggern.)
struct Done { char file[kFileLen]; char hash[kHashLen]; };

int loadDone(Done* done, int cap) {
  int n = 0;
  if (!board::sdReady()) return 0;
  spiLock();
  File f = SD.open(kStateFile, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return 0;
  char line[kFileLen + kHashLen + 4];
  for (;;) {
    spiLock();
    int len = f.available() ? f.readBytesUntil('\n', line, sizeof(line) - 1) : -1;
    spiUnlock();
    if (len < 0) break;
    line[len] = '\0';
    char* cr = strchr(line, '\r'); if (cr) *cr = '\0';
    if (!line[0]) continue;
    if (n >= cap) break;
    char* tab = strchr(line, '\t');             // "name\thash"; alt = nur Name
    done[n].hash[0] = '\0';
    if (tab) { *tab = '\0'; strncpy(done[n].hash, tab + 1, kHashLen - 1); done[n].hash[kHashLen-1]='\0'; }
    strncpy(done[n].file, line, kFileLen - 1); done[n].file[kFileLen - 1] = '\0';
    n++;
  }
  spiLock(); f.close(); spiUnlock();
  return n;
}

void saveDone(Done* done, int n) {
  if (!board::sdReady()) return;
  spiLock();
  SD.mkdir("/.fennek");
  File f = SD.open(kStateFile, FILE_WRITE);
  if (f) {
    for (int i = 0; i < n; i++) f.printf("%s\t%s\n", done[i].file, done[i].hash);
    f.close();
  }
  spiUnlock();
}

// Index in der Done-Liste (-1 = nicht vorhanden).
int findDone(Done* done, int n, const char* name) {
  for (int i = 0; i < n; i++) if (strcmp(done[i].file, name) == 0) return i;
  return -1;
}

// 32-Byte-Digest → Hex.
void hexDigest(const uint8_t* dig, char out[kHashLen]) {
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { out[2*i] = hx[dig[i] >> 4]; out[2*i+1] = hx[dig[i] & 0xF]; }
  out[64] = '\0';
}

// SHA256 eines Speicherpuffers (polierte Fassung — kein Re-Read noetig).
void hashBuf(const char* data, int len, char out[kHashLen]) {
  uint8_t dig[32];
  mbedtls_sha256_ret((const unsigned char*)data, (size_t)len, dig, 0);  // 0 = SHA-256
  hexDigest(dig, out);
}

// SHA256 des aktuellen Notiz-Inhalts (gechunkt unter spiLock). out[0]='\0' bei
// fehlender Datei.
bool noteHash(const char* file, char out[kHashLen]) {
  out[0] = '\0';
  char path[40]; snprintf(path, sizeof(path), "%s/%s", kNotesDir, file);
  spiLock();
  File f = SD.open(path, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  uint8_t buf[512];
  for (;;) {
    spiLock();
    int rd = f.available() ? f.read(buf, sizeof(buf)) : 0;
    spiUnlock();
    if (rd <= 0) break;
    mbedtls_sha256_update_ret(&ctx, buf, (size_t)rd);
  }
  spiLock(); f.close(); spiUnlock();
  uint8_t dig[32];
  mbedtls_sha256_finish_ret(&ctx, dig);
  mbedtls_sha256_free(&ctx);
  hexDigest(dig, out);
  return true;
}

// --- Notiz lesen/schreiben (gechunkt unter spiLock) ---------------------------
void notePath(const char* file, char* out, size_t n) {
  snprintf(out, n, "%s/%s", kNotesDir, file);
}

int readNote(const char* file, char* buf, int cap) {
  char path[40]; notePath(file, path, sizeof(path));
  int len = 0; buf[0] = '\0';
  spiLock();
  File f = SD.open(path, FILE_READ);
  bool open = (bool)f;
  spiUnlock();
  if (!open) return 0;
  spiLock();
  while (len < cap - 1) {
    int want = cap - 1 - len; if (want > 512) want = 512;
    int rd = f.read((uint8_t*)(buf + len), want);
    if (rd <= 0) break;
    len += rd;
  }
  buf[len] = '\0';
  f.close();
  spiUnlock();
  return len;
}

bool writeNote(const char* file, const char* buf, int len) {
  char path[40]; notePath(file, path, sizeof(path));
  spiLock();
  if (!SD.exists(kNotesDir)) SD.mkdir(kNotesDir);
  File f = SD.open(path, FILE_WRITE);
  bool ok = (bool)f;
  spiUnlock();
  for (int i = 0; ok && i < len; ) {
    int n = (len - i > 512) ? 512 : len - i;
    spiLock();
    ok = f.write((const uint8_t*)(buf + i), n) == (size_t)n;
    spiUnlock();
    i += n;
  }
  spiLock(); if (f) f.close(); spiUnlock();
  return ok;
}

// --- WLAN auf/zu (zentraler Helfer: Scan → stärkstes bekanntes Netz) ------------
bool wifiUp(const char* ssid, const char* pass) {
  (void)ssid; (void)pass;
  return wifi::connect(kConnectTimeoutMs);
}

void wifiDown() { wifi::disconnect(); }

// --- Ollama-Aufruf ------------------------------------------------------------
// POST {url}/api/generate mit {model,system,prompt,stream:false}. Antwort-Body
// nach resp (gechunkt aus dem Stream gelesen). true = HTTP 200 + Body.
bool httpPostJson(const String& url, const char* body, char* resp, size_t respCap) {
  bool https = url.startsWith("https");
  HTTPClient http;
  WiFiClientSecure cs; WiFiClient cp;
  bool begun;
  if (https) { cs.setInsecure(); begun = http.begin(cs, url); }
  else       { begun = http.begin(cp, url); }
  if (!begun) return false;
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kGenTimeoutMs);
  int code = http.POST((uint8_t*)body, strlen(body));
  if (code != 200) { Serial.printf("[NOTESAI] HTTP %d\n", code); http.end(); return false; }

  WiFiClient* st = http.getStreamPtr();
  size_t total = 0; uint32_t last = millis();
  while (total < respCap - 1) {
    int av = st->available();
    if (av > 0) {
      int want = (size_t)av < respCap - 1 - total ? av : (int)(respCap - 1 - total);
      int r = st->readBytes((uint8_t*)resp + total, want);
      if (r > 0) { total += r; last = millis(); }
    } else {
      if (!http.connected() && st->available() == 0) break;
      if (millis() - last > kReadTimeoutMs) break;
      delay(10);
    }
  }
  resp[total] = '\0';
  http.end();
  return total > 0;
}

// Entfernt inline eingebettete <think>...</think>-Bloecke (manche Reasoning-
// Modelle schreiben den Denkprozess in die Antwort statt in Ollamas separates
// 'thinking'-Feld). Damit landet das Denken nie in der Notiz. No-op bei sauberer
// Antwort. Auch <thinking>...</thinking> wird erfasst (case-insensitive).
void stripThink(char* s) {
  for (;;) {
    char* open = nullptr;
    for (char* p = s; *p; p++)
      if (p[0] == '<' && strncasecmp(p, "<think", 6) == 0) { open = p; break; }
    if (!open) break;
    char* gt = strchr(open, '>');
    if (!gt) break;
    char* close = nullptr;
    for (char* p = gt + 1; *p; p++)
      if (p[0] == '<' && strncasecmp(p, "</think", 7) == 0) { close = p; break; }
    if (!close) break;                       // unvollstaendig -> stehen lassen
    char* after = strchr(close, '>');
    if (!after) break;
    after++;
    memmove(open, after, strlen(after) + 1); // Block herausschneiden
  }
}

bool ollamaGenerate(const char* base, const char* model, const char* note,
                    char* body, size_t bodyCap, char* resp, size_t respCap,
                    char* out, size_t outCap) {
  // Request-Body bauen.
  size_t bl = 0;
  body[0] = '\0';
  auto lit = [&](const char* s){ size_t l=strlen(s); if (bl+l<bodyCap){ memcpy(body+bl,s,l); bl+=l; body[bl]='\0'; } };
  lit("{\"model\":\"");           jsonAppendEscaped(body, bodyCap, &bl, model);
  lit("\",\"system\":\"");        jsonAppendEscaped(body, bodyCap, &bl, kSystem);
  lit("\",\"prompt\":\"");
  bool full = jsonAppendEscaped(body, bodyCap, &bl, note);
  lit("\",\"stream\":false,\"options\":{\"temperature\":0.1}}");
  if (!full) Serial.println("[NOTESAI] Warnung: Notiz fuer Request gekuerzt");

  String url = String(base) + "/api/generate";
  if (!httpPostJson(url, body, resp, respCap)) return false;
  if (!jsonExtractString(resp, "response", out, outCap)) {
    Serial.println("[NOTESAI] Antwort ohne 'response'-Feld");
    return false;
  }
  stripThink(out);   // falls das Modell den Denkteil inline einbettet
  // Fuehrende/abschliessende Leerzeichen/Zeilen trimmen.
  int n = (int)strlen(out);
  int a = 0; while (a < n && (out[a]=='\n'||out[a]=='\r'||out[a]==' '||out[a]=='\t')) a++;
  int b = n; while (b > a && (out[b-1]=='\n'||out[b-1]=='\r'||out[b-1]==' '||out[b-1]=='\t')) b--;
  if (a > 0) memmove(out, out + a, b - a);
  out[b - a] = '\0';
  return out[0] != '\0';
}

// --- Eligible-Scan ------------------------------------------------------------
// Sammelt bis zu kMaxPerRun zu polierende Dateinamen in cand[]: .md, nicht
// heute, und entweder noch nicht in der Done-Liste ODER seither geaendert
// (aktueller SHA256 != gespeicherter). *total = Gesamtzahl offener Notizen
// (ungekappt). Rueckgabe = Anzahl in cand.
int collectPending(Done* done, int doneN, char (*cand)[kFileLen], int* total) {
  *total = 0;
  int nc = 0;
  if (!board::sdReady()) return 0;
  char today[kFileLen]; todayFile(today, sizeof(today));

  spiLock();
  if (!SD.exists(kNotesDir)) SD.mkdir(kNotesDir);
  File dir = SD.open(kNotesDir);
  bool ok = dir && dir.isDirectory();
  spiUnlock();
  if (!ok) { if (dir) { spiLock(); dir.close(); spiUnlock(); } return 0; }

  for (;;) {
    spiLock();
    bool isDir = false;
    String entry = dir.getNextFileName(&isDir);
    spiUnlock();
    if (entry.length() == 0) break;
    const char* full = entry.c_str();
    const char* nm = strrchr(full, '/'); nm = nm ? nm + 1 : full;
    if (isDir || nm[0] == '.') continue;
    if (!hasExt(nm, ".md")) continue;
    if (strcmp(nm, today) == 0) continue;          // heutige Notiz auslassen
    int idx = findDone(done, doneN, nm);
    if (idx >= 0) {                                 // bekannt: nur bei geaendertem Inhalt
      char h[kHashLen];
      if (noteHash(nm, h) && strcmp(done[idx].hash, h) == 0) continue;  // unveraendert
    }
    (*total)++;
    if (nc < kMaxPerRun) {
      strncpy(cand[nc], nm, kFileLen - 1); cand[nc][kFileLen - 1] = '\0';
      nc++;
    }
  }
  spiLock(); dir.close(); spiUnlock();
  return nc;
}

// Gemeinsamer Kern. useBackoff nur fuer den Pre-Standby-Pfad.
bool flushCore(bool useBackoff, char* log, size_t logN) {
  auto setLog = [&](const char* m){ if (log && logN) { strncpy(log, m, logN-1); log[logN-1]='\0'; } };

  if (!settings::aiEnabled()) { setLog("KI aus"); return true; }

  char base[128], model[48], ssid[33], wpass[65];
  baseUrl(base, sizeof(base));
  settings::aiModel(model, sizeof(model));
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(wpass, sizeof(wpass));
  if (!base[0])  { setLog("Ollama nicht konfiguriert"); return false; }
  if (!ssid[0])  { setLog("kein WLAN konfiguriert"); return false; }
  if (!board::sdReady()) { setLog("keine SD-Karte"); return false; }
  if (webfm::state() != webfm::State::OFF) { setLog("WLAN belegt"); return false; }

  Done* done = (Done*)heap_caps_malloc(sizeof(Done) * kMaxDone, MALLOC_CAP_SPIRAM);
  if (!done) done = (Done*)malloc(sizeof(Done) * kMaxDone);
  if (!done) { setLog("kein Speicher"); return false; }
  int doneN = loadDone(done, kMaxDone);

  char cand[kMaxPerRun][kFileLen];
  int total = 0;
  int nc = collectPending(done, doneN, cand, &total);
  if (total == 0) { free(done); setLog("nichts offen"); return true; }

  // Back-off-Gate erst, wenn wirklich Arbeit ansteht.
  if (useBackoff) {
    uint32_t cur = timesync::now();
    if (s_failCount > 0 && s_lastAttemptEpoch != 0 && cur >= s_lastAttemptEpoch) {
      uint8_t  shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
      uint32_t wait  = kBackoffBaseSecs << shift;
      if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
      if (cur - s_lastAttemptEpoch < wait) {
        Serial.printf("[NOTESAI] Pre-Standby uebersprungen (Back-off: %u Fehlschlaege, "
                      "warte %lus)\n", (unsigned)s_failCount, (unsigned long)wait);
        free(done); setLog("Back-off aktiv"); return false;
      }
    }
    s_lastAttemptEpoch = cur;
  }

  if (nc < total)
    Serial.printf("[NOTESAI] %d von %d offenen Notizen in diesem Lauf (Rest folgt)\n", nc, total);

  if (!wifiUp(ssid, wpass)) {
    wifiDown();
    free(done);
    if (useBackoff && s_failCount < 0xFF) s_failCount++;
    setLog("WLAN-Fehler");
    return false;
  }

  char* body = (char*)heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM);
  char* resp = (char*)heap_caps_malloc(kRespCap, MALLOC_CAP_SPIRAM);
  char* note = (char*)heap_caps_malloc(kNoteCap, MALLOC_CAP_SPIRAM);
  int polished = 0, failed = 0;
  if (body && resp && note) {
    Serial.printf("[NOTESAI] WLAN verbunden, poliere %d Notiz(en) ...\n", nc);
    for (int i = 0; i < nc; i++) {
      int len = readNote(cand[i], note, kNoteCap);
      if (len == 0) continue;                       // leere/fehlende Notiz uebergehen
      int outLen = 0;
      if (ollamaGenerate(base, model, note, body, kBodyCap, resp, kRespCap, note, kNoteCap)
          && (outLen = (int)strlen(note), writeNote(cand[i], note, outLen))) {
        polished++;
        // Done-Liste mit dem SHA256 der polierten Fassung fortschreiben
        // (vorhandenen Eintrag aktualisieren bzw. neu anlegen).
        int di = findDone(done, doneN, cand[i]);
        if (di < 0 && doneN < kMaxDone) {
          di = doneN++;
          strncpy(done[di].file, cand[i], kFileLen - 1); done[di].file[kFileLen-1]='\0';
        }
        if (di >= 0) hashBuf(note, outLen, done[di].hash);
        Serial.printf("[NOTESAI] %s: poliert (%d B)\n", cand[i], outLen);
      } else {
        failed++;
        Serial.printf("[NOTESAI] %s: Fehler\n", cand[i]);
      }
    }
  } else {
    failed = nc;
    Serial.println("[NOTESAI] kein PSRAM fuer Puffer");
  }

  wifiDown();
  if (body) free(body);
  if (resp) free(resp);
  if (note) free(note);

  saveDone(done, doneN);
  free(done);

  if (useBackoff) {
    if (failed == 0) s_failCount = 0;
    else if (s_failCount < 0xFF) s_failCount++;
  }

  char m[64];
  snprintf(m, sizeof(m), "%d poliert, %d Fehler (%d offen)", polished, failed, total - polished);
  setLog(m);
  Serial.printf("[NOTESAI] %s\n", m);
  return failed == 0;
}

}  // namespace

namespace notes_ai {

int pendingCount() {
  if (!board::sdReady() || !settings::aiEnabled()) return 0;
  Done* done = (Done*)heap_caps_malloc(sizeof(Done) * kMaxDone, MALLOC_CAP_SPIRAM);
  if (!done) done = (Done*)malloc(sizeof(Done) * kMaxDone);
  if (!done) return 0;
  int doneN = loadDone(done, kMaxDone);
  char cand[kMaxPerRun][kFileLen];
  int total = 0;
  collectPending(done, doneN, cand, &total);
  free(done);
  return total;
}

bool flushBeforeStandby() {
  char dummy[8];
  return flushCore(true, dummy, sizeof(dummy));
}

bool flushNow(char* log, size_t logN) {
  return flushCore(false, log, logN);
}

bool ping(char* msg, size_t msgN) {
  auto setMsg = [&](const char* m){ if (msg && msgN) { strncpy(msg, m, msgN-1); msg[msgN-1]='\0'; } };
  char base[128], ssid[33], wpass[65];
  baseUrl(base, sizeof(base));
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(wpass, sizeof(wpass));
  if (!base[0]) { setMsg("Ollama nicht konfiguriert"); return false; }
  if (!ssid[0]) { setMsg("kein WLAN konfiguriert"); return false; }
  if (webfm::state() != webfm::State::OFF) { setMsg("WLAN belegt"); return false; }

  if (!wifiUp(ssid, wpass)) { wifiDown(); setMsg("WLAN-Fehler"); return false; }

  String url = String(base) + "/api/tags";
  bool https = url.startsWith("https");
  HTTPClient http;
  WiFiClientSecure cs; WiFiClient cp;
  bool begun = https ? (cs.setInsecure(), http.begin(cs, url)) : http.begin(cp, url);
  bool ok = false;
  if (begun) {
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kConnectTimeoutMs);
    int code = http.GET();
    ok = (code == 200);
    setMsg(ok ? "Verbindung OK" : "Server-Fehler (URL/Port?)");
    http.end();
  } else {
    setMsg("Server-Fehler (URL/Port?)");
  }
  wifiDown();
  return ok;
}

}  // namespace notes_ai
