#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

Preferences s_prefs;
bool        s_open = false;

// Gecachte Werte: Vergleich vor jedem Write (NVS nur bei echter Änderung).
uint8_t  s_volume = 255;        // 255 = noch nicht geladen
uint8_t  s_standby = 5;         // Auto-Standby-Minuten (0 = aus)
char     s_lastApp[24] = "";
char     s_lastTrack[192] = "";
uint32_t s_lastPos = 0;

}  // namespace

namespace settings {

void begin() {
  s_open = s_prefs.begin("meck", false);
  if (!s_open) return;
  s_volume = s_prefs.getUChar("vol", 255);
  s_standby = s_prefs.getUChar("stdby", 5);
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

uint32_t crc32(const char* s) {
  uint32_t crc = 0xFFFFFFFF;
  while (*s) {
    crc ^= (uint8_t)*s++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

}  // namespace settings
