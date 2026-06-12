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
uint8_t  s_lang = 0;            // UI-Sprache (0 = Deutsch, s. i18n::Lang)
char     s_lastApp[24] = "";
char     s_lastTrack[192] = "";
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
      char b[192];
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

}  // namespace settings
