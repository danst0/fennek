// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "settings.h"

#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

namespace {

Preferences s_prefs;
bool        s_open = false;

// Gecachte Werte: Vergleich vor jedem Write (NVS nur bei echter Änderung).
uint8_t  s_volume = 255;        // 255 = noch nicht geladen
uint8_t  s_standby = 5;         // Auto-Standby-Minuten (0 = aus)
uint8_t  s_lang = 0;            // UI-Sprache (0 = Deutsch, s. i18n::Lang)
char     s_lastApp[24] = "";
char     s_lastTrack[TRACK_PATH_LEN] = "";
uint32_t s_lastPos = 0;

// Einmalige Migration vom alten NVS-Namespace "meck" (Firmware hieß früher
// Meck). Kopiert alle bekannten Keys nach "fennek" und leert das alte
// Namespace, damit Lautstärke/Resume/Funkparameter den Umstieg überleben.
void migrateFromMeck() {
  if (s_prefs.isKey("vol")) return;   // "fennek" schon befüllt -> nichts zu tun
  Preferences old;
  if (!old.begin("meck", true)) return;
  if (old.isKey("vol")) {
    s_prefs.putUChar("vol", old.getUChar("vol", 255));
    if (old.isKey("lastapp")) {
      char b[24];
      old.getString("lastapp", b, sizeof(b));
      s_prefs.putString("lastapp", b);
    }
    if (old.isKey("trkpath")) {
      char b[TRACK_PATH_LEN];
      old.getString("trkpath", b, sizeof(b));
      s_prefs.putString("trkpath", b);
      s_prefs.putULong("trkpos", old.getULong("trkpos", 0));
    }
    if (old.isKey("stdby")) s_prefs.putUChar("stdby", old.getUChar("stdby", 5));
    if (old.isKey("mfreq")) s_prefs.putFloat("mfreq", old.getFloat("mfreq", 0));
    if (old.isKey("mbw"))   s_prefs.putFloat("mbw",   old.getFloat("mbw", 0));
    if (old.isKey("msf"))   s_prefs.putUChar("msf",   old.getUChar("msf", 0));
    if (old.isKey("mcr"))   s_prefs.putUChar("mcr",   old.getUChar("mcr", 0));
    if (old.isKey("mtx"))   s_prefs.putUChar("mtx",   old.getUChar("mtx", 0));
    if (old.isKey("mname")) {
      char b[32];
      old.getString("mname", b, sizeof(b));
      s_prefs.putString("mname", b);
    }
    Serial.println("[FENNEK] NVS-Migration meck -> fennek abgeschlossen");
  }
  old.end();
  Preferences wipe;
  if (wipe.begin("meck", false)) { wipe.clear(); wipe.end(); }
}

}  // namespace

namespace settings {

void begin() {
  // Idempotent: ein zweiter Preferences::begin() gäbe false zurück und würde
  // s_open kippen (Pfad: Timer-Wake-Minimal-Boot fällt in den Voll-Boot durch).
  if (s_open) return;
  s_open = s_prefs.begin("fennek", false);
  if (!s_open) return;
  migrateFromMeck();
  s_volume = s_prefs.getUChar("vol", 255);
  s_standby = s_prefs.getUChar("stdby", 5);
  s_lang = s_prefs.getUChar("lang", 0);
  if (s_prefs.isKey("lastapp")) s_prefs.getString("lastapp", s_lastApp, sizeof(s_lastApp));
  if (s_prefs.isKey("trkpath")) s_prefs.getString("trkpath", s_lastTrack, sizeof(s_lastTrack));
  s_lastPos = s_prefs.getULong("trkpos", 0);
}

uint8_t volume() { return s_volume; }

void setVolume(uint8_t v) {
  if (!s_open || v == s_volume) return;
  s_volume = v;
  s_prefs.putUChar("vol", v);
}

uint8_t standbyMinutes() { return s_standby; }

void setStandbyMinutes(uint8_t m) {
  if (!s_open || m == s_standby) return;
  s_standby = m;
  s_prefs.putUChar("stdby", m);
}

uint8_t language() { return s_lang; }

void setLanguage(uint8_t lang) {
  if (!s_open || lang == s_lang) return;
  s_lang = lang;
  s_prefs.putUChar("lang", lang);
}

void lastApp(char* out, size_t n) {
  strncpy(out, s_lastApp, n - 1);
  out[n - 1] = '\0';
}

void setLastApp(const char* name) {
  if (!s_open || strcmp(name, s_lastApp) == 0) return;
  strncpy(s_lastApp, name, sizeof(s_lastApp) - 1);
  s_lastApp[sizeof(s_lastApp) - 1] = '\0';
  s_prefs.putString("lastapp", s_lastApp);
}

void lastTrack(char* pathOut, size_t n, uint32_t* posSec) {
  strncpy(pathOut, s_lastTrack, n - 1);
  pathOut[n - 1] = '\0';
  if (posSec) *posSec = s_lastPos;
}

void setLastTrack(const char* path, uint32_t posSec) {
  if (!s_open) return;
  // Pfad nur schreiben, wenn er sich ändert; Position nur bei >=20 s Abstand
  // (NVS-Schonung, vgl. Plan: Write-on-Change + Delta).
  if (strcmp(path, s_lastTrack) != 0) {
    strncpy(s_lastTrack, path, sizeof(s_lastTrack) - 1);
    s_lastTrack[sizeof(s_lastTrack) - 1] = '\0';
    s_prefs.putString("trkpath", s_lastTrack);
    s_lastPos = posSec;
    s_prefs.putULong("trkpos", posSec);
    return;
  }
  uint32_t delta = (posSec > s_lastPos) ? posSec - s_lastPos : s_lastPos - posSec;
  if (delta >= 20) {
    s_lastPos = posSec;
    s_prefs.putULong("trkpos", posSec);
  }
}

void clearLastTrack() {
  if (!s_open || s_lastTrack[0] == '\0') return;
  s_lastTrack[0] = '\0';
  s_lastPos = 0;
  s_prefs.remove("trkpath");
  s_prefs.remove("trkpos");
}

// --- Hörbuch-Bookmarks --------------------------------------------------------
namespace {

Preferences s_abook;
bool        s_abookOpen = false;

bool abookEnsure() {
  if (!s_abookOpen) s_abookOpen = s_abook.begin("abook", false);
  return s_abookOpen;
}

void bmKey(uint32_t key, char* out) { snprintf(out, 12, "b%08x", (unsigned)key); }

// Gepackt: fileIdx (16 Bit) | posSec (im oberen Bereich)
uint64_t packBm(uint16_t fileIdx, uint32_t posSec) {
  return ((uint64_t)posSec << 16) | fileIdx;
}

}  // namespace

bool bookBookmark(uint32_t key, uint16_t* fileIdx, uint32_t* posSec) {
  if (!abookEnsure()) return false;
  char k[12]; bmKey(key, k);
  if (!s_abook.isKey(k)) return false;
  uint64_t v = s_abook.getULong64(k, 0);
  if (fileIdx) *fileIdx = (uint16_t)(v & 0xFFFF);
  if (posSec)  *posSec  = (uint32_t)(v >> 16);
  return true;
}

void setBookBookmark(uint32_t key, uint16_t fileIdx, uint32_t posSec) {
  if (!abookEnsure()) return;
  char k[12]; bmKey(key, k);
  // Schreibdisziplin: nur bei Datei-Wechsel oder >=20 s Positions-Delta.
  if (s_abook.isKey(k)) {
    uint64_t old = s_abook.getULong64(k, 0);
    uint16_t oF = (uint16_t)(old & 0xFFFF);
    uint32_t oP = (uint32_t)(old >> 16);
    uint32_t d  = (posSec > oP) ? posSec - oP : oP - posSec;
    if (oF == fileIdx && d < 20) return;
  }
  s_abook.putULong64(k, packBm(fileIdx, posSec));
}

void clearBookBookmark(uint32_t key) {
  if (!abookEnsure()) return;
  char k[12]; bmKey(key, k);
  if (s_abook.isKey(k)) s_abook.remove(k);
}

// --- eBook-Lesepositionen -------------------------------------------------------
namespace {

Preferences s_ebook;
bool        s_ebookOpen = false;

bool ebookEnsure() {
  if (!s_ebookOpen) s_ebookOpen = s_ebook.begin("ebook", false);
  return s_ebookOpen;
}

}  // namespace

bool readPos(uint32_t key, uint32_t* page) {
  if (!ebookEnsure()) return false;
  char k[12]; bmKey(key, k);
  if (!s_ebook.isKey(k)) return false;
  if (page) *page = s_ebook.getULong(k, 0);
  return true;
}

void setReadPos(uint32_t key, uint32_t page) {
  if (!ebookEnsure()) return;
  char k[12]; bmKey(key, k);
  if (s_ebook.isKey(k) && s_ebook.getULong(k, 0) == page) return;
  s_ebook.putULong(k, page);
}

namespace {
char s_lastBook[256] = "";        // Calibre-Pfade sind lang
bool s_lastBookLoaded = false;
}

void lastBook(char* out, size_t n) {
  if (!s_lastBookLoaded && ebookEnsure()) {
    s_lastBookLoaded = true;
    if (s_ebook.isKey("bookpath")) s_ebook.getString("bookpath", s_lastBook, sizeof(s_lastBook));
  }
  strncpy(out, s_lastBook, n - 1);
  out[n - 1] = '\0';
}

void setLastBook(const char* path) {
  if (!ebookEnsure() || strcmp(path, s_lastBook) == 0) return;
  strncpy(s_lastBook, path, sizeof(s_lastBook) - 1);
  s_lastBook[sizeof(s_lastBook) - 1] = '\0';
  s_lastBookLoaded = true;
  s_ebook.putString("bookpath", s_lastBook);
}

// --- Mesh-Funkparameter ----------------------------------------------------------
namespace {

// Defaults: "EU/UK Narrow" (Deutschland/NRW-Standard, wie Daniels Repeater).
constexpr MeshParams kMeshDefaults = {869.618f, 62.5f, 8, 5, 22};

bool       s_meshLoaded = false;
MeshParams s_mesh = kMeshDefaults;
char       s_meshName[32] = "T-Deck";

void meshEnsure() {
  if (s_meshLoaded || !s_open) return;
  s_meshLoaded = true;
  if (s_prefs.isKey("mfreq")) s_mesh.freqMhz = s_prefs.getFloat("mfreq", kMeshDefaults.freqMhz);
  if (s_prefs.isKey("mbw"))   s_mesh.bwKhz   = s_prefs.getFloat("mbw",   kMeshDefaults.bwKhz);
  s_mesh.sf    = s_prefs.getUChar("msf", kMeshDefaults.sf);
  s_mesh.cr    = s_prefs.getUChar("mcr", kMeshDefaults.cr);
  s_mesh.txDbm = s_prefs.getUChar("mtx", kMeshDefaults.txDbm);
  if (s_prefs.isKey("mname")) s_prefs.getString("mname", s_meshName, sizeof(s_meshName));
}

}  // namespace

MeshParams meshParams() {
  meshEnsure();
  return s_mesh;
}

void setMeshParams(const MeshParams& p) {
  meshEnsure();
  if (!s_open) return;
  if (p.freqMhz != s_mesh.freqMhz) s_prefs.putFloat("mfreq", p.freqMhz);
  if (p.bwKhz   != s_mesh.bwKhz)   s_prefs.putFloat("mbw",   p.bwKhz);
  if (p.sf      != s_mesh.sf)      s_prefs.putUChar("msf",   p.sf);
  if (p.cr      != s_mesh.cr)      s_prefs.putUChar("mcr",   p.cr);
  if (p.txDbm   != s_mesh.txDbm)   s_prefs.putUChar("mtx",   p.txDbm);
  s_mesh = p;
}

void meshName(char* out, size_t n) {
  meshEnsure();
  strncpy(out, s_meshName, n - 1);
  out[n - 1] = '\0';
}

void setMeshName(const char* name) {
  meshEnsure();
  if (!s_open || !name || !name[0] || strcmp(name, s_meshName) == 0) return;
  strncpy(s_meshName, name, sizeof(s_meshName) - 1);
  s_meshName[sizeof(s_meshName) - 1] = '\0';
  s_prefs.putString("mname", s_meshName);
}

// --- WLAN-Zugangsdaten ----------------------------------------------------------
namespace {

bool s_wifiLoaded = false;
char s_wifiSsid[33] = "";
char s_wifiPass[65] = "";

void wifiEnsure() {
  if (s_wifiLoaded || !s_open) return;
  s_wifiLoaded = true;
  if (s_prefs.isKey("wssid")) s_prefs.getString("wssid", s_wifiSsid, sizeof(s_wifiSsid));
  if (s_prefs.isKey("wpass")) s_prefs.getString("wpass", s_wifiPass, sizeof(s_wifiPass));
}

}  // namespace

void wifiSsid(char* out, size_t n) {
  wifiEnsure();
  strncpy(out, s_wifiSsid, n - 1);
  out[n - 1] = '\0';
}

void setWifiSsid(const char* ssid) {
  wifiEnsure();
  if (!s_open || !ssid || strcmp(ssid, s_wifiSsid) == 0) return;
  strncpy(s_wifiSsid, ssid, sizeof(s_wifiSsid) - 1);
  s_wifiSsid[sizeof(s_wifiSsid) - 1] = '\0';
  s_prefs.putString("wssid", s_wifiSsid);
}

void wifiPass(char* out, size_t n) {
  wifiEnsure();
  strncpy(out, s_wifiPass, n - 1);
  out[n - 1] = '\0';
}

void setWifiPass(const char* pass) {
  wifiEnsure();
  if (!s_open || !pass || strcmp(pass, s_wifiPass) == 0) return;
  strncpy(s_wifiPass, pass, sizeof(s_wifiPass) - 1);
  s_wifiPass[sizeof(s_wifiPass) - 1] = '\0';
  s_prefs.putString("wpass", s_wifiPass);
}

// --- Navidrome/Subsonic-Zugangsdaten -------------------------------------------
namespace {

bool s_navLoaded = false;
bool s_navOn     = false;
char s_navUrl[128]  = "";
char s_navUser[64]  = "";
char s_navPass[65]  = "";

void navEnsure() {
  if (s_navLoaded || !s_open) return;
  s_navLoaded = true;
  s_navOn = s_prefs.getBool("nscro", false);
  if (s_prefs.isKey("nurl"))  s_prefs.getString("nurl",  s_navUrl,  sizeof(s_navUrl));
  if (s_prefs.isKey("nuser")) s_prefs.getString("nuser", s_navUser, sizeof(s_navUser));
  if (s_prefs.isKey("npass")) s_prefs.getString("npass", s_navPass, sizeof(s_navPass));
}

}  // namespace

bool navEnabled() { navEnsure(); return s_navOn; }

void setNavEnabled(bool on) {
  navEnsure();
  if (!s_open || on == s_navOn) return;
  s_navOn = on;
  s_prefs.putBool("nscro", on);
}

void navUrl(char* out, size_t n) {
  navEnsure();
  strncpy(out, s_navUrl, n - 1);
  out[n - 1] = '\0';
}

void setNavUrl(const char* url) {
  navEnsure();
  if (!s_open || !url || strcmp(url, s_navUrl) == 0) return;
  strncpy(s_navUrl, url, sizeof(s_navUrl) - 1);
  s_navUrl[sizeof(s_navUrl) - 1] = '\0';
  s_prefs.putString("nurl", s_navUrl);
}

void navUser(char* out, size_t n) {
  navEnsure();
  strncpy(out, s_navUser, n - 1);
  out[n - 1] = '\0';
}

void setNavUser(const char* user) {
  navEnsure();
  if (!s_open || !user || strcmp(user, s_navUser) == 0) return;
  strncpy(s_navUser, user, sizeof(s_navUser) - 1);
  s_navUser[sizeof(s_navUser) - 1] = '\0';
  s_prefs.putString("nuser", s_navUser);
}

void navPass(char* out, size_t n) {
  navEnsure();
  strncpy(out, s_navPass, n - 1);
  out[n - 1] = '\0';
}

void setNavPass(const char* pass) {
  navEnsure();
  if (!s_open || !pass || strcmp(pass, s_navPass) == 0) return;
  strncpy(s_navPass, pass, sizeof(s_navPass) - 1);
  s_navPass[sizeof(s_navPass) - 1] = '\0';
  s_prefs.putString("npass", s_navPass);
}

// --- Uhrzeit-Persistenz + Zeitzone (services/timesync) -------------------------
// Nur Kaltstart-Fallback (Stromausfall/Reset); im Normalbetrieb überlebt die
// ESP32-Systemzeit den Deep Sleep selbst. Schreibdrosselung liegt im Aufrufer.
uint32_t lastTime() {
  if (!s_open || !s_prefs.isKey("ltime")) return 0;
  return s_prefs.getUInt("ltime", 0);
}

void setLastTime(uint32_t epoch) {
  if (!s_open) return;
  if (s_prefs.getUInt("ltime", 0) == epoch) return;
  s_prefs.putUInt("ltime", epoch);
}

uint16_t clockPpm() {
  if (!s_open) return 0;
  return s_prefs.getUShort("cppm", 0);
}

void setClockPpm(uint16_t ppm) {
  if (!s_open) return;
  if (s_prefs.getUShort("cppm", 0) == ppm) return;
  s_prefs.putUShort("cppm", ppm);
}

// Zeitzone als POSIX-TZ-String (z. B. "CET-1CEST,M3.5.0,M10.5.0/3" = Europe/Berlin
// inkl. automatischer Sommerzeit). Default Berlin. NTP/Mesh liefern nur UTC.
void tzString(char* out, size_t n) {
  const char* def = "CET-1CEST,M3.5.0,M10.5.0/3";
  if (!s_open || !s_prefs.isKey("tz")) { strncpy(out, def, n - 1); out[n - 1] = '\0'; return; }
  s_prefs.getString("tz", out, n);
  if (!out[0]) { strncpy(out, def, n - 1); out[n - 1] = '\0'; }
}

void setTzString(const char* tz) {
  if (!s_open || !tz || !tz[0]) return;
  char cur[48];
  tzString(cur, sizeof(cur));
  if (strcmp(tz, cur) == 0) return;
  s_prefs.putString("tz", tz);
}

// --- Spiele: Beststände + Schach-Spielstand ------------------------------------
namespace {

bool     s_gamesLoaded = false;
uint32_t s_best2048 = 0;
uint16_t s_minesWins = 0, s_minesBest = 0;
uint16_t s_chessWins = 0;
uint16_t s_tttWins = 0, s_tttDraws = 0;

void gamesEnsure() {
  if (s_gamesLoaded || !s_open) return;
  s_gamesLoaded = true;
  s_best2048  = s_prefs.getULong("g2kbest", 0);
  s_minesWins = s_prefs.getUShort("mswins", 0);
  s_minesBest = s_prefs.getUShort("msbest", 0);
  s_chessWins = s_prefs.getUShort("chwins", 0);
  s_tttWins   = s_prefs.getUShort("tttw", 0);
  s_tttDraws  = s_prefs.getUShort("tttd", 0);
}

}  // namespace

uint32_t best2048() { gamesEnsure(); return s_best2048; }

void setBest2048(uint32_t score) {
  gamesEnsure();
  if (!s_open || score <= s_best2048) return;
  s_best2048 = score;
  s_prefs.putULong("g2kbest", score);
}

uint16_t minesWins()    { gamesEnsure(); return s_minesWins; }
uint16_t minesBestSec() { gamesEnsure(); return s_minesBest; }

void setMinesResult(bool won, uint16_t sec) {
  gamesEnsure();
  if (!s_open || !won) return;   // Niederlagen werden nicht gezählt
  s_minesWins++;
  s_prefs.putUShort("mswins", s_minesWins);
  if (s_minesBest == 0 || sec < s_minesBest) {
    s_minesBest = sec;
    s_prefs.putUShort("msbest", sec);
  }
}

uint16_t chessWins() { gamesEnsure(); return s_chessWins; }

void addChessWin() {
  gamesEnsure();
  if (!s_open) return;
  s_chessWins++;
  s_prefs.putUShort("chwins", s_chessWins);
}

bool chessGame(void* buf, size_t n) {
  if (!s_open || !s_prefs.isKey("chgame")) return false;
  if (s_prefs.getBytesLength("chgame") != n) return false;   // Formatwechsel
  return s_prefs.getBytes("chgame", buf, n) == n;
}

void setChessGame(const void* buf, size_t n) {
  if (!s_open) return;
  if (n == 0) {
    if (s_prefs.isKey("chgame")) s_prefs.remove("chgame");
    return;
  }
  s_prefs.putBytes("chgame", buf, n);
}

uint16_t tttWins()  { gamesEnsure(); return s_tttWins; }
uint16_t tttDraws() { gamesEnsure(); return s_tttDraws; }

void addTttResult(bool win, bool draw) {
  gamesEnsure();
  if (!s_open) return;
  if (win)  { s_tttWins++;  s_prefs.putUShort("tttw", s_tttWins); }
  if (draw) { s_tttDraws++; s_prefs.putUShort("tttd", s_tttDraws); }
}

uint32_t crc32(const char* s) {
  uint32_t crc = 0xFFFFFFFF;
  while (*s) {
    crc ^= (uint8_t)*s++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// --- INI-Export/Import ---------------------------------------------------------
// Serialisierung in einen RAM-Puffer (kein SD-/SPI-Zugriff hier — den macht
// services/settingsfile unter spiLock). Format: [sektion] + key = value.
size_t exportIni(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  MeshParams m = meshParams();
  char name[32], ssid[33], tz[48], lapp[24], ltrk[TRACK_PATH_LEN], lbook[256];
  char nurl[128], nuser[64];
  meshName(name, sizeof(name));
  wifiSsid(ssid, sizeof(ssid));
  navUrl(nurl, sizeof(nurl));
  navUser(nuser, sizeof(nuser));
  // WLAN-Passwort wird BEWUSST NICHT exportiert (Klartext auf entnehmbarer SD).
  // Das Feld bleibt leer; ein leeres Feld lässt beim Import das NVS-Passwort
  // unangetastet — nur ein manuell eingetragenes Passwort wird übernommen.
  tzString(tz, sizeof(tz));
  lastApp(lapp, sizeof(lapp));
  uint32_t lpos = 0;
  lastTrack(ltrk, sizeof(ltrk), &lpos);
  lastBook(lbook, sizeof(lbook));

  int len = snprintf(out, cap,
    "# Fennek-Einstellungen — bearbeitbar, beim Boot eingelesen.\n"
    "# Pro-Buch-Lesepositionen/Hoerbuch-Bookmarks sind hier NICHT enthalten\n"
    "# (intern CRC32-verschluesselt). [state] wird beim Import ignoriert.\n"
    "\n[general]\n"
    "volume = %u\n"             // 0..21
    "standby_minutes = %u\n"    // 0 = aus
    "language = %u\n"           // 0 = Deutsch
    "timezone = %s\n"
    "\n[mesh]\n"
    "freq_mhz = %.3f\n"
    "bw_khz = %.1f\n"
    "sf = %u\n"
    "cr = %u\n"
    "tx_dbm = %u\n"
    "node_name = %s\n"
    "\n[wifi]\n"
    "ssid = %s\n"
    "password =\n"             // leer = nicht exportiert; manuell eintragbar
    "\n[navidrome]\n"
    "enabled = %u\n"
    "url = %s\n"
    "user = %s\n"
    "password =\n"             // leer = nicht exportiert; manuell eintragbar
    "\n[games]\n"
    "best2048 = %lu\n"
    "mines_wins = %u\n"
    "mines_best_sec = %u\n"
    "chess_wins = %u\n"
    "ttt_wins = %u\n"
    "ttt_draws = %u\n"
    "\n[state]\n"               // nur Backup — Import ueberspringt diese Sektion
    "last_app = %s\n"
    "last_track = %s\n"
    "last_pos = %lu\n"
    "last_book = %s\n"
    "last_time = %lu\n"
    "clock_ppm = %u\n",
    (unsigned)volume(), (unsigned)standbyMinutes(), (unsigned)language(), tz,
    m.freqMhz, m.bwKhz, (unsigned)m.sf, (unsigned)m.cr, (unsigned)m.txDbm, name,
    ssid,
    (unsigned)(navEnabled() ? 1 : 0), nurl, nuser,
    (unsigned long)best2048(), (unsigned)minesWins(), (unsigned)minesBestSec(),
    (unsigned)chessWins(), (unsigned)tttWins(), (unsigned)tttDraws(),
    lapp, ltrk, (unsigned long)lpos, lbook,
    (unsigned long)lastTime(), (unsigned)clockPpm());

  if (len < 0) return 0;
  return (size_t)len >= cap ? cap - 1 : (size_t)len;
}

namespace {

// Beidseitig trimmen (in-place): führende/abschließende Whitespaces entfernen.
char* trim(char* s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  char* e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
  return s;
}

// Direkter NVS-Write für Spiele-Bestände beim Restore: die öffentlichen Setter
// sind monoton/inkrementell (setBest2048 nur Rekorde, addChessWin zählt hoch),
// ungeeignet zum Wiederherstellen eines exakten Werts.
void restoreGameU32(const char* nvsKey, uint32_t v, uint32_t& cache) {
  if (!s_open) return;
  gamesEnsure();
  cache = v;
  s_prefs.putULong(nvsKey, v);
}
void restoreGameU16(const char* nvsKey, uint16_t v, uint16_t& cache) {
  if (!s_open) return;
  gamesEnsure();
  cache = v;
  s_prefs.putUShort(nvsKey, v);
}

}  // namespace

int importIni(const char* text) {
  if (!text) return -1;
  // Auf einer modifizierbaren Kopie arbeiten (zeilenweises In-place-Trimmen).
  size_t n = strlen(text);
  char* buf = (char*)malloc(n + 1);
  if (!buf) return -1;
  memcpy(buf, text, n + 1);

  int applied = 0;
  char section[16] = "";
  char* save = nullptr;
  for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
    char* t = trim(line);
    if (!t[0] || t[0] == '#' || t[0] == ';') continue;
    if (t[0] == '[') {
      char* close = strchr(t, ']');
      if (close) {
        *close = '\0';
        strncpy(section, t + 1, sizeof(section) - 1);
        section[sizeof(section) - 1] = '\0';
      }
      continue;
    }
    char* eq = strchr(t, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = trim(t);
    char* val = trim(eq + 1);

    if (strcmp(section, "general") == 0) {
      if      (strcmp(key, "volume") == 0)          { setVolume((uint8_t)atoi(val)); applied++; }
      else if (strcmp(key, "standby_minutes") == 0) { setStandbyMinutes((uint8_t)atoi(val)); applied++; }
      else if (strcmp(key, "language") == 0)        { setLanguage((uint8_t)atoi(val)); applied++; }
      else if (strcmp(key, "timezone") == 0)        { setTzString(val); applied++; }
    } else if (strcmp(section, "mesh") == 0) {
      MeshParams m = meshParams();
      if      (strcmp(key, "freq_mhz") == 0)  { m.freqMhz = atof(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "bw_khz") == 0)    { m.bwKhz = atof(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "sf") == 0)        { m.sf = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "cr") == 0)        { m.cr = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "tx_dbm") == 0)    { m.txDbm = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "node_name") == 0) { setMeshName(val); applied++; }
    } else if (strcmp(section, "wifi") == 0) {
      if      (strcmp(key, "ssid") == 0)     { setWifiSsid(val); applied++; }
      // Leeres Passwort-Feld überspringen: bewahrt das im NVS gespeicherte
      // Passwort (Export schreibt es nie). Nur manuell Eingetragenes wird gesetzt.
      else if (strcmp(key, "password") == 0 && val[0]) { setWifiPass(val); applied++; }
    } else if (strcmp(section, "navidrome") == 0) {
      if      (strcmp(key, "enabled") == 0) { setNavEnabled(atoi(val) != 0); applied++; }
      else if (strcmp(key, "url") == 0)     { setNavUrl(val); applied++; }
      else if (strcmp(key, "user") == 0)    { setNavUser(val); applied++; }
      // Leeres Passwort-Feld überspringen (wie WLAN) — bewahrt NVS-Passwort.
      else if (strcmp(key, "password") == 0 && val[0]) { setNavPass(val); applied++; }
    } else if (strcmp(section, "games") == 0) {
      if      (strcmp(key, "best2048") == 0)       { restoreGameU32("g2kbest", (uint32_t)strtoul(val, nullptr, 10), s_best2048); applied++; }
      else if (strcmp(key, "mines_wins") == 0)     { restoreGameU16("mswins", (uint16_t)atoi(val), s_minesWins); applied++; }
      else if (strcmp(key, "mines_best_sec") == 0) { restoreGameU16("msbest", (uint16_t)atoi(val), s_minesBest); applied++; }
      else if (strcmp(key, "chess_wins") == 0)     { restoreGameU16("chwins", (uint16_t)atoi(val), s_chessWins); applied++; }
      else if (strcmp(key, "ttt_wins") == 0)       { restoreGameU16("tttw", (uint16_t)atoi(val), s_tttWins); applied++; }
      else if (strcmp(key, "ttt_draws") == 0)      { restoreGameU16("tttd", (uint16_t)atoi(val), s_tttDraws); applied++; }
    }
    // [state] und unbekannte Sektionen: bewusst ignoriert.
  }
  free(buf);
  return applied;
}

}  // namespace settings
