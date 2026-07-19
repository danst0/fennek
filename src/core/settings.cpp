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
uint8_t  s_fontScale = 1;       // Schriftgröße Lesen/Notizen (1 = klein, 2 = groß)
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
  s_fontScale = s_prefs.getUChar("fscale", 1);
  if (s_fontScale < 1 || s_fontScale > 2) s_fontScale = 1;
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

uint8_t fontScale() { return s_fontScale; }

void setFontScale(uint8_t s) {
  if (s < 1) s = 1;
  if (s > 2) s = 2;
  if (!s_open || s == s_fontScale) return;
  s_fontScale = s;
  s_prefs.putUChar("fscale", s);
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
double     s_meshLat = 0.0, s_meshLon = 0.0;

void meshEnsure() {
  if (s_meshLoaded || !s_open) return;
  s_meshLoaded = true;
  if (s_prefs.isKey("mfreq")) s_mesh.freqMhz = s_prefs.getFloat("mfreq", kMeshDefaults.freqMhz);
  if (s_prefs.isKey("mbw"))   s_mesh.bwKhz   = s_prefs.getFloat("mbw",   kMeshDefaults.bwKhz);
  s_mesh.sf    = s_prefs.getUChar("msf", kMeshDefaults.sf);
  s_mesh.cr    = s_prefs.getUChar("mcr", kMeshDefaults.cr);
  s_mesh.txDbm = s_prefs.getUChar("mtx", kMeshDefaults.txDbm);
  if (s_prefs.isKey("mname")) s_prefs.getString("mname", s_meshName, sizeof(s_meshName));
  if (s_prefs.isKey("mlat")) s_meshLat = s_prefs.getDouble("mlat", 0.0);
  if (s_prefs.isKey("mlon")) s_meshLon = s_prefs.getDouble("mlon", 0.0);
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

bool meshEco() {
  if (!s_open) return true;
  return s_prefs.getBool("mesheco", true);
}
void setMeshEco(bool on) {
  if (!s_open || on == meshEco()) return;
  s_prefs.putBool("mesheco", on);
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

void meshPos(double* lat, double* lon) {
  meshEnsure();
  if (lat) *lat = s_meshLat;
  if (lon) *lon = s_meshLon;
}

void setMeshPos(double lat, double lon) {
  meshEnsure();
  if (!s_open) return;
  if (lat != s_meshLat) { s_meshLat = lat; s_prefs.putDouble("mlat", lat); }
  if (lon != s_meshLon) { s_meshLon = lon; s_prefs.putDouble("mlon", lon); }
}

// --- WLAN-Profile ---------------------------------------------------------------
// Bis zu kMaxWifiProfiles bekannte Netze. Persistenz als kompakter NVS-Blob
// "wifis" (längenpräfigierte Strings), damit nur echte Daten Platz kosten (das
// NVS-Budget ist knapp). Slot 0 wird bei Bedarf aus dem Alt-Schlüssel wssid/wpass
// migriert und bleibt das "einfache" WLAN (ini-Export/webfm/Konsole).
namespace {

struct WifiProfile { char ssid[33]; char pass[65]; };

bool        s_wifiLoaded = false;
int         s_wifiCount  = 0;
WifiProfile s_wifi[kMaxWifiProfiles];

// Blob-Format: [u8 count] dann je Eintrag [u8 ssidLen][ssid][u8 passLen][pass].
void wifiSave() {
  if (!s_open) return;
  uint8_t buf[1 + kMaxWifiProfiles * (1 + 32 + 1 + 64)];
  size_t o = 0;
  buf[o++] = (uint8_t)s_wifiCount;
  for (int i = 0; i < s_wifiCount; i++) {
    uint8_t sl = (uint8_t)strnlen(s_wifi[i].ssid, sizeof(s_wifi[i].ssid) - 1);
    buf[o++] = sl; memcpy(buf + o, s_wifi[i].ssid, sl); o += sl;
    uint8_t pl = (uint8_t)strnlen(s_wifi[i].pass, sizeof(s_wifi[i].pass) - 1);
    buf[o++] = pl; memcpy(buf + o, s_wifi[i].pass, pl); o += pl;
  }
  s_prefs.putBytes("wifis", buf, o);
}

void wifiEnsure() {
  if (s_wifiLoaded || !s_open) return;
  s_wifiLoaded = true;

  size_t len = s_prefs.getBytesLength("wifis");
  if (len > 0) {
    uint8_t buf[1 + kMaxWifiProfiles * (1 + 32 + 1 + 64)];
    if (len <= sizeof(buf) && s_prefs.getBytes("wifis", buf, len) == len) {
      size_t o = 0;
      int cnt = buf[o++];
      for (int i = 0; i < cnt && i < kMaxWifiProfiles && o < len; i++) {
        uint8_t sl = buf[o++];
        if (sl > sizeof(s_wifi[i].ssid) - 1 || o + sl > len) break;
        memcpy(s_wifi[i].ssid, buf + o, sl); s_wifi[i].ssid[sl] = '\0'; o += sl;
        if (o >= len) break;
        uint8_t pl = buf[o++];
        if (pl > sizeof(s_wifi[i].pass) - 1 || o + pl > len) break;
        memcpy(s_wifi[i].pass, buf + o, pl); s_wifi[i].pass[pl] = '\0'; o += pl;
        s_wifiCount = i + 1;
      }
    }
    return;
  }

  // Migration: Alt-Schlüssel wssid/wpass → Slot 0.
  if (s_prefs.isKey("wssid")) {
    s_prefs.getString("wssid", s_wifi[0].ssid, sizeof(s_wifi[0].ssid));
    if (s_prefs.isKey("wpass")) s_prefs.getString("wpass", s_wifi[0].pass, sizeof(s_wifi[0].pass));
    if (s_wifi[0].ssid[0]) { s_wifiCount = 1; wifiSave(); }
  }
}

}  // namespace

int wifiCount() { wifiEnsure(); return s_wifiCount; }

void wifiSsidAt(int i, char* out, size_t n) {
  wifiEnsure();
  if (i < 0 || i >= s_wifiCount) { if (n) out[0] = '\0'; return; }
  strncpy(out, s_wifi[i].ssid, n - 1);
  out[n - 1] = '\0';
}

void wifiPassAt(int i, char* out, size_t n) {
  wifiEnsure();
  if (i < 0 || i >= s_wifiCount) { if (n) out[0] = '\0'; return; }
  strncpy(out, s_wifi[i].pass, n - 1);
  out[n - 1] = '\0';
}

void wifiRemove(int i) {
  wifiEnsure();
  if (i < 0 || i >= s_wifiCount) return;
  for (int j = i; j < s_wifiCount - 1; j++) s_wifi[j] = s_wifi[j + 1];
  s_wifiCount--;
  wifiSave();
}

bool wifiSet(int i, const char* ssid, const char* pass) {
  wifiEnsure();
  if (!s_open || i < 0 || i > s_wifiCount || i >= kMaxWifiProfiles) return false;
  // Leere SSID = Löschen (nur bei bestehendem Eintrag sinnvoll).
  if (!ssid || !ssid[0]) { if (i < s_wifiCount) { wifiRemove(i); return true; } return false; }
  if (i == s_wifiCount) s_wifiCount++;           // anhängen
  strncpy(s_wifi[i].ssid, ssid, sizeof(s_wifi[i].ssid) - 1);
  s_wifi[i].ssid[sizeof(s_wifi[i].ssid) - 1] = '\0';
  if (pass) {
    strncpy(s_wifi[i].pass, pass, sizeof(s_wifi[i].pass) - 1);
    s_wifi[i].pass[sizeof(s_wifi[i].pass) - 1] = '\0';
  }
  wifiSave();
  return true;
}

// --- Legacy-/Einfach-Zugriff = Slot 0 -------------------------------------------
void wifiSsid(char* out, size_t n) { wifiSsidAt(0, out, n); }

void setWifiSsid(const char* ssid) {
  wifiEnsure();
  if (!s_open || !ssid) return;
  char cur[33]; wifiSsidAt(0, cur, sizeof(cur));
  if (strcmp(ssid, cur) == 0) return;
  char pass[65]; wifiPassAt(0, pass, sizeof(pass));   // Passwort erhalten
  wifiSet(0, ssid, s_wifiCount > 0 ? pass : "");
}

void wifiPass(char* out, size_t n) { wifiPassAt(0, out, n); }

void setWifiPass(const char* pass) {
  wifiEnsure();
  if (!s_open || !pass) return;
  char cur[65]; wifiPassAt(0, cur, sizeof(cur));
  if (strcmp(pass, cur) == 0) return;
  if (s_wifiCount == 0) return;                       // ohne SSID kein Passwort
  char ssid[33]; wifiSsidAt(0, ssid, sizeof(ssid));
  wifiSet(0, ssid, pass);
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

// --- Ollama-KI für Notizen (services/notes_ai) --------------------------------
namespace {

bool s_aiLoaded = false;
bool s_aiOn     = false;
char s_aiUrl[128]  = "";
char s_aiModel[48] = "llama3.2";

void aiEnsure() {
  if (s_aiLoaded || !s_open) return;
  s_aiLoaded = true;
  s_aiOn = s_prefs.getBool("oai", false);
  if (s_prefs.isKey("ourl")) s_prefs.getString("ourl", s_aiUrl,   sizeof(s_aiUrl));
  if (s_prefs.isKey("omod")) s_prefs.getString("omod", s_aiModel, sizeof(s_aiModel));
}

}  // namespace

bool aiEnabled() { aiEnsure(); return s_aiOn; }

void setAiEnabled(bool on) {
  aiEnsure();
  if (!s_open || on == s_aiOn) return;
  s_aiOn = on;
  s_prefs.putBool("oai", on);
}

void aiUrl(char* out, size_t n) {
  aiEnsure();
  strncpy(out, s_aiUrl, n - 1);
  out[n - 1] = '\0';
}

void setAiUrl(const char* url) {
  aiEnsure();
  if (!s_open || !url || strcmp(url, s_aiUrl) == 0) return;
  strncpy(s_aiUrl, url, sizeof(s_aiUrl) - 1);
  s_aiUrl[sizeof(s_aiUrl) - 1] = '\0';
  s_prefs.putString("ourl", s_aiUrl);
}

void aiModel(char* out, size_t n) {
  aiEnsure();
  strncpy(out, s_aiModel, n - 1);
  out[n - 1] = '\0';
}

void setAiModel(const char* model) {
  aiEnsure();
  if (!s_open || !model || !model[0] || strcmp(model, s_aiModel) == 0) return;
  strncpy(s_aiModel, model, sizeof(s_aiModel) - 1);
  s_aiModel[sizeof(s_aiModel) - 1] = '\0';
  s_prefs.putString("omod", s_aiModel);
}

// --- OTA-Firmware-Update (services/ota) ---------------------------------------
namespace {

bool s_otaLoaded = false;
// Default: neuestes Release von danst0/fennek (die GitHub-API dient als Manifest).
char s_otaUrl[160] = "https://api.github.com/repos/danst0/fennek/releases/latest";

void otaEnsure() {
  if (s_otaLoaded || !s_open) return;
  s_otaLoaded = true;
  if (s_prefs.isKey("otau")) s_prefs.getString("otau", s_otaUrl, sizeof(s_otaUrl));
}

}  // namespace

void otaUrl(char* out, size_t n) {
  otaEnsure();
  strncpy(out, s_otaUrl, n - 1);
  out[n - 1] = '\0';
}

void setOtaUrl(const char* url) {
  otaEnsure();
  if (!s_open || !url || strcmp(url, s_otaUrl) == 0) return;
  strncpy(s_otaUrl, url, sizeof(s_otaUrl) - 1);
  s_otaUrl[sizeof(s_otaUrl) - 1] = '\0';
  s_prefs.putString("otau", s_otaUrl);
}

// --- Podcast-Auto-Sync (services/podcast) -------------------------------------
bool podcastAutoSync() {
  if (!s_open) return false;
  return s_prefs.getBool("pcas", false);
}

void setPodcastAutoSync(bool on) {
  if (!s_open || on == podcastAutoSync()) return;
  s_prefs.putBool("pcas", on);
}

// --- Todo/Reinschrift (services/reinschrift) ----------------------------------
namespace {

bool s_todoLoaded = false;
bool s_todoOn      = false;
bool s_todoAuto    = false;
char s_todoUrl[160]  = "";
char s_todoUser[64]  = "";
char s_todoPass[65]  = "";
char s_todoPath[160] = "";

void todoEnsure() {
  if (s_todoLoaded || !s_open) return;
  s_todoLoaded = true;
  s_todoOn   = s_prefs.getBool("todoen", false);
  s_todoAuto = s_prefs.getBool("todoauto", false);
  if (s_prefs.isKey("todourl"))  s_prefs.getString("todourl",  s_todoUrl,  sizeof(s_todoUrl));
  if (s_prefs.isKey("todousr"))  s_prefs.getString("todousr",  s_todoUser, sizeof(s_todoUser));
  if (s_prefs.isKey("todopw"))   s_prefs.getString("todopw",   s_todoPass, sizeof(s_todoPass));
  if (s_prefs.isKey("todopath")) s_prefs.getString("todopath", s_todoPath, sizeof(s_todoPath));
}

}  // namespace

bool todoEnabled() { todoEnsure(); return s_todoOn; }
void setTodoEnabled(bool on) {
  todoEnsure();
  if (!s_open || on == s_todoOn) return;
  s_todoOn = on; s_prefs.putBool("todoen", on);
}
bool todoAutoSync() { todoEnsure(); return s_todoAuto; }
void setTodoAutoSync(bool on) {
  todoEnsure();
  if (!s_open || on == s_todoAuto) return;
  s_todoAuto = on; s_prefs.putBool("todoauto", on);
}
void todoUrl(char* out, size_t n)  { todoEnsure(); strncpy(out, s_todoUrl, n - 1);  out[n - 1] = '\0'; }
void todoUser(char* out, size_t n) { todoEnsure(); strncpy(out, s_todoUser, n - 1); out[n - 1] = '\0'; }
void todoPass(char* out, size_t n) { todoEnsure(); strncpy(out, s_todoPass, n - 1); out[n - 1] = '\0'; }
void todoPath(char* out, size_t n) { todoEnsure(); strncpy(out, s_todoPath, n - 1); out[n - 1] = '\0'; }
void setTodoUrl(const char* url) {
  todoEnsure();
  if (!s_open || !url || strcmp(url, s_todoUrl) == 0) return;
  strncpy(s_todoUrl, url, sizeof(s_todoUrl) - 1); s_todoUrl[sizeof(s_todoUrl) - 1] = '\0';
  s_prefs.putString("todourl", s_todoUrl);
}
void setTodoUser(const char* user) {
  todoEnsure();
  if (!s_open || !user || strcmp(user, s_todoUser) == 0) return;
  strncpy(s_todoUser, user, sizeof(s_todoUser) - 1); s_todoUser[sizeof(s_todoUser) - 1] = '\0';
  s_prefs.putString("todousr", s_todoUser);
}
void setTodoPass(const char* pass) {
  todoEnsure();
  if (!s_open || !pass || strcmp(pass, s_todoPass) == 0) return;
  strncpy(s_todoPass, pass, sizeof(s_todoPass) - 1); s_todoPass[sizeof(s_todoPass) - 1] = '\0';
  s_prefs.putString("todopw", s_todoPass);
}
void setTodoPath(const char* path) {
  todoEnsure();
  if (!s_open || !path || strcmp(path, s_todoPath) == 0) return;
  strncpy(s_todoPath, path, sizeof(s_todoPath) - 1); s_todoPath[sizeof(s_todoPath) - 1] = '\0';
  s_prefs.putString("todopath", s_todoPath);
}

// --- Kalender (services/calendar) ---------------------------------------------
namespace {
bool s_calLoaded = false;
char s_calUser[64] = "";
char s_calPass[65] = "";
void calEnsure() {
  if (s_calLoaded || !s_open) return;
  s_calLoaded = true;
  if (s_prefs.isKey("caldavusr")) s_prefs.getString("caldavusr", s_calUser, sizeof(s_calUser));
  if (s_prefs.isKey("caldavpw"))  s_prefs.getString("caldavpw",  s_calPass, sizeof(s_calPass));
}
}  // namespace

bool calAutoSync() {
  if (!s_open) return false;
  return s_prefs.getBool("calauto", false);
}
void setCalAutoSync(bool on) {
  if (!s_open || on == calAutoSync()) return;
  s_prefs.putBool("calauto", on);
}
void calDavUser(char* out, size_t n) { calEnsure(); strncpy(out, s_calUser, n - 1); out[n - 1] = '\0'; }
void calDavPass(char* out, size_t n) { calEnsure(); strncpy(out, s_calPass, n - 1); out[n - 1] = '\0'; }
void setCalDavUser(const char* user) {
  calEnsure();
  if (!s_open || !user || strcmp(user, s_calUser) == 0) return;
  strncpy(s_calUser, user, sizeof(s_calUser) - 1); s_calUser[sizeof(s_calUser) - 1] = '\0';
  s_prefs.putString("caldavusr", s_calUser);
}
void setCalDavPass(const char* pass) {
  calEnsure();
  if (!s_open || !pass || strcmp(pass, s_calPass) == 0) return;
  strncpy(s_calPass, pass, sizeof(s_calPass) - 1); s_calPass[sizeof(s_calPass) - 1] = '\0';
  s_prefs.putString("caldavpw", s_calPass);
}

// --- Calibre-Buch-Sync (services/calibre_books) --------------------------------
namespace {

bool s_cbLoaded  = false;
bool s_cbAuto    = false;
char s_cbUrl[160]  = "";
char s_cbUser[64]  = "";
char s_cbPass[65]  = "";
char s_cbShelf[64] = "";

void cbEnsure() {
  if (s_cbLoaded || !s_open) return;
  s_cbLoaded = true;
  s_cbAuto = s_prefs.getBool("cbauto", false);
  if (s_prefs.isKey("cburl"))   s_prefs.getString("cburl",   s_cbUrl,   sizeof(s_cbUrl));
  if (s_prefs.isKey("cbusr"))   s_prefs.getString("cbusr",   s_cbUser,  sizeof(s_cbUser));
  if (s_prefs.isKey("cbpw"))    s_prefs.getString("cbpw",    s_cbPass,  sizeof(s_cbPass));
  if (s_prefs.isKey("cbshelf")) s_prefs.getString("cbshelf", s_cbShelf, sizeof(s_cbShelf));
  if (!s_cbShelf[0]) strcpy(s_cbShelf, "Fennek");
}

}  // namespace

void calibreUrl(char* out, size_t n)   { cbEnsure(); strncpy(out, s_cbUrl,   n - 1); out[n - 1] = '\0'; }
void calibreUser(char* out, size_t n)  { cbEnsure(); strncpy(out, s_cbUser,  n - 1); out[n - 1] = '\0'; }
void calibrePass(char* out, size_t n)  { cbEnsure(); strncpy(out, s_cbPass,  n - 1); out[n - 1] = '\0'; }
void calibreShelf(char* out, size_t n) { cbEnsure(); strncpy(out, s_cbShelf, n - 1); out[n - 1] = '\0'; }
void setCalibreUrl(const char* url) {
  cbEnsure();
  if (!s_open || !url || strcmp(url, s_cbUrl) == 0) return;
  strncpy(s_cbUrl, url, sizeof(s_cbUrl) - 1); s_cbUrl[sizeof(s_cbUrl) - 1] = '\0';
  s_prefs.putString("cburl", s_cbUrl);
}
void setCalibreUser(const char* user) {
  cbEnsure();
  if (!s_open || !user || strcmp(user, s_cbUser) == 0) return;
  strncpy(s_cbUser, user, sizeof(s_cbUser) - 1); s_cbUser[sizeof(s_cbUser) - 1] = '\0';
  s_prefs.putString("cbusr", s_cbUser);
}
void setCalibrePass(const char* pass) {
  cbEnsure();
  if (!s_open || !pass || strcmp(pass, s_cbPass) == 0) return;
  strncpy(s_cbPass, pass, sizeof(s_cbPass) - 1); s_cbPass[sizeof(s_cbPass) - 1] = '\0';
  s_prefs.putString("cbpw", s_cbPass);
}
void setCalibreShelf(const char* shelf) {
  cbEnsure();
  if (!s_open || !shelf || strcmp(shelf, s_cbShelf) == 0) return;
  strncpy(s_cbShelf, shelf, sizeof(s_cbShelf) - 1); s_cbShelf[sizeof(s_cbShelf) - 1] = '\0';
  s_prefs.putString("cbshelf", s_cbShelf);
}
bool calibreAutoSync() { cbEnsure(); return s_cbAuto; }
void setCalibreAutoSync(bool on) {
  cbEnsure();
  if (!s_open || on == s_cbAuto) return;
  s_cbAuto = on; s_prefs.putBool("cbauto", on);
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
uint16_t s_sudSolved = 0, s_sudBest = 0;
uint16_t s_mathBest = 0;
// Kopfrechnen: adaptive Stufe je Modus, 2 Bit pro Modus (bis 8 Modi gepackt).
// Default = alle Modi auf Stufe 1 (Mittel) -> 0b01 je Feld = 0x5555.
uint16_t s_mathLevels = 0x5555;

void gamesEnsure() {
  if (s_gamesLoaded || !s_open) return;
  s_gamesLoaded = true;
  s_best2048  = s_prefs.getULong("g2kbest", 0);
  s_minesWins = s_prefs.getUShort("mswins", 0);
  s_minesBest = s_prefs.getUShort("msbest", 0);
  s_chessWins = s_prefs.getUShort("chwins", 0);
  s_tttWins   = s_prefs.getUShort("tttw", 0);
  s_tttDraws  = s_prefs.getUShort("tttd", 0);
  s_sudSolved = s_prefs.getUShort("sudwon", 0);
  s_sudBest   = s_prefs.getUShort("sudbest", 0);
  s_mathBest  = s_prefs.getUShort("mqbest", 0);
  s_mathLevels = s_prefs.getUShort("mqlvl", 0x5555);
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

uint16_t sudokuSolved()  { gamesEnsure(); return s_sudSolved; }
uint16_t sudokuBestSec() { gamesEnsure(); return s_sudBest; }

void setSudokuResult(bool won, uint16_t sec) {
  gamesEnsure();
  if (!s_open || !won) return;
  s_sudSolved++;
  s_prefs.putUShort("sudwon", s_sudSolved);
  if (s_sudBest == 0 || sec < s_sudBest) {
    s_sudBest = sec;
    s_prefs.putUShort("sudbest", sec);
  }
}

uint16_t mathBestStreak() { gamesEnsure(); return s_mathBest; }

void setMathBestStreak(uint16_t streak) {
  gamesEnsure();
  if (!s_open || streak <= s_mathBest) return;
  s_mathBest = streak;
  s_prefs.putUShort("mqbest", streak);
}

uint8_t mathLevel(uint8_t mode) {
  gamesEnsure();
  if (mode > 7) mode = 7;
  uint8_t lv = (s_mathLevels >> (mode * 2)) & 0x3;
  return lv > 2 ? 2 : lv;
}

void setMathLevel(uint8_t mode, uint8_t level) {
  gamesEnsure();
  if (!s_open || mode > 7) return;
  if (level > 2) level = 2;
  uint16_t v = (uint16_t)(s_mathLevels & ~(0x3u << (mode * 2)));
  v |= (uint16_t)level << (mode * 2);
  if (v == s_mathLevels) return;   // nur bei echtem Stufenwechsel schreiben
  s_mathLevels = v;
  s_prefs.putUShort("mqlvl", v);
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
  char nurl[128], nuser[64], ourl[128], omod[48], oturl[160];
  meshName(name, sizeof(name));
  wifiSsid(ssid, sizeof(ssid));
  navUrl(nurl, sizeof(nurl));
  navUser(nuser, sizeof(nuser));
  aiUrl(ourl, sizeof(ourl));
  aiModel(omod, sizeof(omod));
  otaUrl(oturl, sizeof(oturl));
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
    "font_scale = %u\n"         // 1 = klein, 2 = groß (Lesen/Notizen)
    "\n[mesh]\n"
    "freq_mhz = %.3f\n"
    "bw_khz = %.1f\n"
    "sf = %u\n"
    "cr = %u\n"
    "tx_dbm = %u\n"
    "node_name = %s\n"
    "eco = %u\n"                // RX-Duty-Cycle (Radio-Sparmodus)
    "\n[wifi]\n"
    "ssid = %s\n"
    "password =\n"             // leer = nicht exportiert; manuell eintragbar
    "\n[navidrome]\n"
    "enabled = %u\n"
    "url = %s\n"
    "user = %s\n"
    "password =\n"             // leer = nicht exportiert; manuell eintragbar
    "\n[ollama]\n"
    "enabled = %u\n"
    "url = %s\n"
    "model = %s\n"
    "\n[update]\n"
    "url = %s\n"
    "\n[games]\n"
    "best2048 = %lu\n"
    "mines_wins = %u\n"
    "mines_best_sec = %u\n"
    "chess_wins = %u\n"
    "ttt_wins = %u\n"
    "ttt_draws = %u\n"
    "sudoku_solved = %u\n"
    "sudoku_best_sec = %u\n"
    "\n[state]\n"               // nur Backup — Import ueberspringt diese Sektion
    "last_app = %s\n"
    "last_track = %s\n"
    "last_pos = %lu\n"
    "last_book = %s\n"
    "last_time = %lu\n"
    "clock_ppm = %u\n",
    (unsigned)volume(), (unsigned)standbyMinutes(), (unsigned)language(), tz,
    (unsigned)fontScale(),
    m.freqMhz, m.bwKhz, (unsigned)m.sf, (unsigned)m.cr, (unsigned)m.txDbm, name,
    (unsigned)(meshEco() ? 1 : 0),
    ssid,
    (unsigned)(navEnabled() ? 1 : 0), nurl, nuser,
    (unsigned)(aiEnabled() ? 1 : 0), ourl, omod,
    oturl,
    (unsigned long)best2048(), (unsigned)minesWins(), (unsigned)minesBestSec(),
    (unsigned)chessWins(), (unsigned)tttWins(), (unsigned)tttDraws(),
    (unsigned)sudokuSolved(), (unsigned)sudokuBestSec(),
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
      else if (strcmp(key, "font_scale") == 0)      { setFontScale((uint8_t)atoi(val)); applied++; }
    } else if (strcmp(section, "mesh") == 0) {
      MeshParams m = meshParams();
      if      (strcmp(key, "freq_mhz") == 0)  { m.freqMhz = atof(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "bw_khz") == 0)    { m.bwKhz = atof(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "sf") == 0)        { m.sf = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "cr") == 0)        { m.cr = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "tx_dbm") == 0)    { m.txDbm = (uint8_t)atoi(val); setMeshParams(m); applied++; }
      else if (strcmp(key, "node_name") == 0) { setMeshName(val); applied++; }
      else if (strcmp(key, "eco") == 0)       { setMeshEco(atoi(val) != 0); applied++; }
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
    } else if (strcmp(section, "ollama") == 0) {
      if      (strcmp(key, "enabled") == 0) { setAiEnabled(atoi(val) != 0); applied++; }
      else if (strcmp(key, "url") == 0)     { setAiUrl(val); applied++; }
      else if (strcmp(key, "model") == 0)   { setAiModel(val); applied++; }
    } else if (strcmp(section, "update") == 0) {
      if      (strcmp(key, "url") == 0)     { setOtaUrl(val); applied++; }
    } else if (strcmp(section, "games") == 0) {
      if      (strcmp(key, "best2048") == 0)       { restoreGameU32("g2kbest", (uint32_t)strtoul(val, nullptr, 10), s_best2048); applied++; }
      else if (strcmp(key, "mines_wins") == 0)     { restoreGameU16("mswins", (uint16_t)atoi(val), s_minesWins); applied++; }
      else if (strcmp(key, "mines_best_sec") == 0) { restoreGameU16("msbest", (uint16_t)atoi(val), s_minesBest); applied++; }
      else if (strcmp(key, "chess_wins") == 0)     { restoreGameU16("chwins", (uint16_t)atoi(val), s_chessWins); applied++; }
      else if (strcmp(key, "ttt_wins") == 0)       { restoreGameU16("tttw", (uint16_t)atoi(val), s_tttWins); applied++; }
      else if (strcmp(key, "ttt_draws") == 0)      { restoreGameU16("tttd", (uint16_t)atoi(val), s_tttDraws); applied++; }
      else if (strcmp(key, "sudoku_solved") == 0)  { restoreGameU16("sudwon", (uint16_t)atoi(val), s_sudSolved); applied++; }
      else if (strcmp(key, "sudoku_best_sec") == 0){ restoreGameU16("sudbest", (uint16_t)atoi(val), s_sudBest); applied++; }
    }
    // [state] und unbekannte Sektionen: bewusst ignoriert.
  }
  free(buf);
  return applied;
}

}  // namespace settings
