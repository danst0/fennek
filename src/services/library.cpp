#include "library.h"
#include "core/board.h"
#include "config.h"
#include "services/id3.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// =============================================================================
// Datenmodell
//   - Flache Track-Liste, nach (Künstler, Album, Name) sortiert.
//   - Künstler/Album werden aus dem Pfad abgeleitet: <root>/<Künstler>/<Album>/..
//   - Vorberechnete "Runs" (zusammenhängende Bereiche) für Künstler und Alben.
//   - Zusätzliche Sortier-Indizes: Alben nach Name, Titel nach Name.
//   - .m3u/.m3u8-Playlists: Dateien werden beim Scan gesammelt, beim Öffnen
//     geparst und auf Bibliotheks-Tracks aufgelöst.
// =============================================================================

namespace {

constexpr int MAX_PLAYLISTS = 64;

// Tag-Zustand eines Tracks.
enum : uint8_t { TAG_NONE = 0, TAG_DONE = 1 };   // DONE = geparst/aus Cache

struct Track {
  char     name[64];     // Anzeigename: ID3-Titel oder Dateiname
  char     path[TRACK_PATH_LEN];
  char     artist[48];   // ID3 oder Ordner-Fallback
  char     album[48];
  uint32_t fsize;        // 0 = unbekannt (readdir liefert keine Größe; der
                         // Tag-Scan trägt sie beim Öffnen der Datei nach)
  uint32_t crc;          // CRC32 des Pfads (Cache-Key)
  uint8_t  tagged;
};

// CRC32 (Polynom 0xEDB88320), klein und ausreichend als Cache-Key.
uint32_t crc32str(const char* s) {
  uint32_t crc = 0xFFFFFFFF;
  while (*s) {
    crc ^= (uint8_t)*s++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// --- ID3-Cache (/.fennek/id3.bin) ----------------------------------------------
constexpr uint32_t kTagCacheMagic   = 0x33444D49;  // "IMD3"
constexpr uint16_t kTagCacheVersion = 1;

struct TagCacheRec {
  uint32_t pathCrc;
  uint32_t fsize;
  char     title[64];    // "" = Datei hat keine Tags (trotzdem gecacht!)
  char     artist[48];
  char     album[48];
};

// Tag-Scan-Zustand (Cursor über die sortierte Track-Liste; Indizes sind bis zum finalen
// Re-Sort stabil, weil buildIndexes() erst nach Abschluss erneut läuft).
int      s_tagDone  = 0;
int      s_tagTotal = 0;
int      s_tagCursor = 0;
uint32_t s_scanGen  = 0;     // invalidiert laufende Schritte nach Re-Scan

// Track-Speicher: bedarfsbasiert wachsende PSRAM-Blöcke à TRACK_BLOCK Einträge.
// Tracks liegen in Scan-Reihenfolge fest in ihren Blöcken (nie umkopiert,
// keine große zusammenhängende Allokation); sortiert wird nur die
// Permutation s_sorted — alle nach außen sichtbaren "flachen Indizes" sind
// wie bisher sortierte Positionen.
Track*            s_blocks[TRACKS_HARD_MAX / TRACK_BLOCK] = {nullptr};
int               s_cap    = 0;        // allozierte Track-Plätze
int*              s_sorted = nullptr;  // sortierte Position -> Block-Slot
int               s_count  = 0;
bool              s_limitWarned = false;
SemaphoreHandle_t s_mutex  = nullptr;
char              s_root[80] = "/";

inline Track& slotAt(int slot) { return s_blocks[slot / TRACK_BLOCK][slot % TRACK_BLOCK]; }
inline Track& trk(int i)       { return slotAt(s_sorted[i]); }

// Künstler-/Album-Runs (wachsen mit s_cap; die Start-Arrays haben s_cap+1 Einträge)
int* s_artistStart = nullptr;
int* s_artistFirstAlbum = nullptr;
int  s_nArtists = 0;
int* s_albumStart = nullptr;
int* s_albumArtist = nullptr;
int  s_nAlbums = 0;
int* s_albumByName = nullptr;
int* s_byTitle = nullptr;

// Playlists (Pfad-Tabelle im PSRAM, Allokation in begin())
char (*s_plPath)[TRACK_PATH_LEN] = nullptr;
int  s_nPlaylists = 0;
int* s_plTracks = nullptr;
int  s_plTrackCount = 0;

// PSRAM-bevorzugtes realloc für die kleinen, flachen Index-Arrays.
bool growInt(int*& p, size_t n) {
  int* np = (int*)heap_caps_realloc(p, sizeof(int) * n, MALLOC_CAP_SPIRAM);
  if (!np) np = (int*)realloc(p, sizeof(int) * n);
  if (!np) return false;
  p = np;
  return true;
}

// Kapazität blockweise nachziehen. Läuft ggf. unter spiLock() — reine
// Heap-Operationen, kein SPI-Zugriff. false = Hard-Max oder Speicher voll.
bool ensureCapacity(int need) {
  if (need > TRACKS_HARD_MAX) return false;
  while (s_cap < need) {
    int newCap = s_cap + TRACK_BLOCK;
    if (!growInt(s_sorted, newCap)      || !growInt(s_albumByName, newCap) ||
        !growInt(s_byTitle, newCap)     || !growInt(s_plTracks, newCap) ||
        !growInt(s_artistStart, newCap + 1)  || !growInt(s_artistFirstAlbum, newCap + 1) ||
        !growInt(s_albumStart, newCap + 1)   || !growInt(s_albumArtist, newCap + 1))
      return false;
    Track* blk = (Track*)heap_caps_malloc(sizeof(Track) * TRACK_BLOCK, MALLOC_CAP_SPIRAM);
    if (!blk) blk = (Track*)malloc(sizeof(Track) * TRACK_BLOCK);
    if (!blk) return false;
    s_blocks[s_cap / TRACK_BLOCK] = blk;
    s_cap = newCap;
  }
  return true;
}

void lock()   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void unlock() { if (s_mutex) xSemaphoreGive(s_mutex); }

bool hasExt(const char* name, const char* ext) {
  size_t n = strlen(name), e = strlen(ext);
  return n > e && strcasecmp(name + n - e, ext) == 0;
}
// Vom ESP32-audioI2S unterstützte Formate (MP3, AAC/M4A, FLAC, WAV).
bool isAudioFile(const char* n) {
  return hasExt(n, ".mp3") || hasExt(n, ".m4a") || hasExt(n, ".aac") ||
         hasExt(n, ".flac") || hasExt(n, ".wav");
}
bool isM3u(const char* n)  { return hasExt(n, ".m3u") || hasExt(n, ".m3u8"); }

bool skipDir(const char* nm) {
  return strcasecmp(nm, "ripple")   == 0 ||
         strcasecmp(nm, "tiles-bw") == 0 ||
         strcasecmp(nm, "tiles")    == 0;
}

const char* baseName(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// Künstler/Album aus dem Verzeichnis (relativ zur Scan-Wurzel) ableiten.
void deriveArtistAlbum(const char* dir, char* artist, size_t aLen, char* album, size_t bLen) {
  strncpy(artist, "(unbekannt)", aLen - 1); artist[aLen - 1] = '\0';
  strncpy(album,  "(kein Album)", bLen - 1); album[bLen - 1] = '\0';

  const char* rel = dir;
  size_t rl = strlen(s_root);
  if (strncmp(dir, s_root, rl) == 0) rel = dir + rl;
  while (*rel == '/') rel++;
  if (!*rel) return;

  const char* slash = strchr(rel, '/');
  size_t seg0 = slash ? (size_t)(slash - rel) : strlen(rel);
  if (seg0 >= aLen) seg0 = aLen - 1;
  memcpy(artist, rel, seg0); artist[seg0] = '\0';

  if (slash) {
    const char* a = slash + 1;
    const char* slash2 = strchr(a, '/');
    size_t seg1 = slash2 ? (size_t)(slash2 - a) : strlen(a);
    if (seg1 >= bLen) seg1 = bLen - 1;
    if (seg1 > 0) { memcpy(album, a, seg1); album[seg1] = '\0'; }
  }
}

// Pfad mit "." / ".." normalisieren (für m3u-Auflösung).
void normalizePath(const char* in, char* out, size_t outLen) {
  // Segmente auf einen einfachen Stack legen.
  const char* segs[40]; size_t segl[40]; int n = 0;
  const char* p = in;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char* e = p; while (*e && *e != '/') e++;
    size_t len = e - p;
    if (len == 1 && p[0] == '.') {
      // skip
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      if (n > 0) n--;
    } else if (n < 40) {
      segs[n] = p; segl[n] = len; n++;
    }
    p = e;
  }
  size_t o = 0;
  for (int i = 0; i < n && o + 1 < outLen; i++) {
    if (o + 1 < outLen) out[o++] = '/';
    for (size_t j = 0; j < segl[i] && o + 1 < outLen; j++) out[o++] = segs[i][j];
  }
  if (o == 0 && outLen > 1) out[o++] = '/';
  out[o] = '\0';
}

// --- Hintergrund-Scan ---------------------------------------------------------
// Muster vom Bücher-Scan (reader_app.cpp): getNextFileName() (readdir, kein
// fopen — der Komplett-Scan mit openNextFile dauerte Minuten), max. kScanChunk
// Einträge pro spiLock, das offene Verzeichnis überlebt zwischen den Häppchen.
// Neue Tracks landen erst beim Abschluss in s_count — bis dahin bleibt die
// alte Bibliothek konsistent les-/abspielbar.
constexpr int kDirStackMax = 256;
constexpr int kScanChunk   = 6;

char (*s_dirStack)[TRACK_PATH_LEN] = nullptr;   // PSRAM, Allokation in begin()
int   s_dirStackN   = 0;
bool  s_scanning    = false;
int   s_scanCount   = 0;       // gefundene Tracks (noch nicht publiziert)
int   s_scanMode    = 0;       // 0 = Walk ab s_root, 1 = flache m3u-Suche
File  s_scanDir;               // offenes Verzeichnis zwischen den Häppchen
bool  s_scanDirOpen = false;
int   s_scanDirDepth = 0;
int   s_rootSlashes  = 0;
bool  s_rootFallbackTried = false;

int countSlashes(const char* p) { int n = 0; for (; *p; p++) if (*p == '/') n++; return n; }

void addTrackPath(const char* full) {
  if (strlen(full) >= TRACK_PATH_LEN) {
    Serial.printf("[FENNEK] Pfad zu lang (>%d), übersprungen: %s\n", TRACK_PATH_LEN - 1, full);
    return;
  }
  if (!ensureCapacity(s_scanCount + 1)) {
    if (!s_limitWarned) {
      s_limitWarned = true;
      Serial.printf("[FENNEK] Bibliothek: Limit bei %d Tracks erreicht (%s) — weitere Dateien werden ignoriert\n",
                    s_scanCount, s_scanCount >= TRACKS_HARD_MAX ? "Hard-Max" : "Speicher voll");
    }
    return;
  }
  char dir[TRACK_PATH_LEN];
  const char* sl = strrchr(full, '/');
  size_t dl = sl ? (size_t)(sl - full) : 0;
  memcpy(dir, full, dl); dir[dl] = '\0';
  if (!dir[0]) strcpy(dir, "/");
  const char* nm = sl ? sl + 1 : full;

  Track& t = slotAt(s_scanCount);
  strncpy(t.name, nm, sizeof(t.name) - 1); t.name[sizeof(t.name) - 1] = '\0';
  strcpy(t.path, full);
  deriveArtistAlbum(dir, t.artist, sizeof(t.artist), t.album, sizeof(t.album));
  t.fsize  = 0;                    // trägt der Tag-Scan beim Öffnen nach
  t.crc    = crc32str(t.path);
  t.tagged = TAG_NONE;
  s_scanCount++;
}

void addPlaylistPath(const char* full) {
  if (s_nPlaylists >= MAX_PLAYLISTS || strlen(full) >= TRACK_PATH_LEN) return;
  strcpy(s_plPath[s_nPlaylists], full);
  s_nPlaylists++;
}

// Track-Cache (/.fennek/tracks.bin): komplette Bibliothek inkl. Tags — der Boot
// lädt sie in Sekunden, statt minutenlang die SD zu durchwandern.
constexpr const char* kTrackCache    = "/.fennek/tracks.bin";
constexpr const char* kTrackCacheTmp = "/.fennek/tracks.tmp";
constexpr uint32_t kTrackCacheMagic  = 0x4B52544Du;  // "MTRK"
constexpr uint16_t kTrackCacheVer    = 1;

// Cache schreiben (atomar via .tmp + rename). Aufrufer hält lock();
// SD-Zugriffe gechunkt, damit Eingaben/Audio zwischendurch drankommen.
void writeTrackCache() {
  spiLock();
  if (!SD.exists("/.fennek")) SD.mkdir("/.fennek");
  if (SD.exists(kTrackCacheTmp)) SD.remove(kTrackCacheTmp);
  File f = SD.open(kTrackCacheTmp, FILE_WRITE);
  bool ok = (bool)f;
  if (ok) {
    uint32_t n = (uint32_t)s_count, nPl = (uint32_t)s_nPlaylists;
    uint16_t rec = (uint16_t)sizeof(Track);
    ok = f.write((const uint8_t*)&kTrackCacheMagic, 4) == 4;
    f.write((const uint8_t*)&kTrackCacheVer, 2);
    f.write((const uint8_t*)&rec, 2);
    f.write((const uint8_t*)&n, 4);
    f.write((const uint8_t*)&nPl, 4);
    f.write((const uint8_t*)s_root, sizeof(s_root));
  }
  spiUnlock();
  if (!ok) { if (f) { spiLock(); f.close(); spiUnlock(); } return; }
  int n = 0;
  while (ok && n < s_count) {
    spiLock();
    for (int i = 0; i < 16 && n < s_count; i++, n++)
      if (f.write((const uint8_t*)&trk(n), sizeof(Track)) != sizeof(Track)) { ok = false; break; }
    spiUnlock();
  }
  spiLock();
  for (int i = 0; ok && i < s_nPlaylists; i++)
    f.write((const uint8_t*)s_plPath[i], TRACK_PATH_LEN);
  f.close();
  if (ok) {
    if (SD.exists(kTrackCache)) SD.remove(kTrackCache);
    SD.rename(kTrackCacheTmp, kTrackCache);
  }
  spiUnlock();
}

// --- Sortierung & Runs ------------------------------------------------------
// Sortiert wird die Permutation s_sorted (Einträge = Block-Slots).
int cmpTrack(const void* a, const void* b) {
  const Track& x = slotAt(*(const int*)a);
  const Track& y = slotAt(*(const int*)b);
  int c = strcasecmp(x.artist, y.artist); if (c) return c;
  c = strcasecmp(x.album, y.album);       if (c) return c;
  return strcasecmp(x.name, y.name);
}
int cmpAlbumByName(const void* a, const void* b) {
  int ra = *(const int*)a, rb = *(const int*)b;
  const Track& ta = trk(s_albumStart[ra]);
  const Track& tb = trk(s_albumStart[rb]);
  int c = strcasecmp(ta.album, tb.album); if (c) return c;
  return strcasecmp(ta.artist, tb.artist);
}
int cmpByTitle(const void* a, const void* b) {
  return strcasecmp(trk(*(const int*)a).name, trk(*(const int*)b).name);
}

// Tags eines Cache-Records/Parse-Ergebnisses auf den Track anwenden.
// Leere Felder lassen den Ordner-/Dateinamen-Fallback stehen.
void applyTags(Track& t, const char* title, const char* artist, const char* album) {
  if (title && title[0])   { strncpy(t.name,   title,  sizeof(t.name) - 1);   t.name[sizeof(t.name) - 1] = '\0'; }
  if (artist && artist[0]) { strncpy(t.artist, artist, sizeof(t.artist) - 1); t.artist[sizeof(t.artist) - 1] = '\0'; }
  if (album && album[0])   { strncpy(t.album,  album,  sizeof(t.album) - 1);  t.album[sizeof(t.album) - 1] = '\0'; }
  t.tagged = TAG_DONE;
}

int cmpByCrc(const void* a, const void* b) {
  uint32_t x = trk(*(const int*)a).crc, y = trk(*(const int*)b).crc;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// Cache laden und Treffer anwenden. Aufrufer hält lock(); nimmt spiLock selbst.
// Abgleich über einen CRC-sortierten Index + Binärsuche; Datei-Reads gechunkt
// (kein sekundenlanger spiLock — Eingaben/Power-Knopf bleiben bedienbar).
void applyTagCache() {
  s_tagDone = 0; s_tagCursor = 0;
  s_tagTotal = s_count;
  int* byCrc = nullptr;
  TagCacheRec* recs = nullptr;
  constexpr int kRecChunk = 16;
  if (s_count > 0) {
    byCrc = (int*)heap_caps_malloc(sizeof(int) * s_count, MALLOC_CAP_SPIRAM);
    recs  = (TagCacheRec*)heap_caps_malloc(sizeof(TagCacheRec) * kRecChunk, MALLOC_CAP_SPIRAM);
    if (!byCrc) byCrc = (int*)malloc(sizeof(int) * s_count);
    if (!recs)  recs  = (TagCacheRec*)malloc(sizeof(TagCacheRec) * kRecChunk);
  }
  if (byCrc && recs) {
    for (int i = 0; i < s_count; i++) byCrc[i] = i;
    qsort(byCrc, s_count, sizeof(int), cmpByCrc);

    spiLock();
    File f;
    if (SD.exists("/.fennek/id3.bin")) f = SD.open("/.fennek/id3.bin");
    uint32_t magic = 0, n = 0; uint16_t ver = 0;
    bool ok = f &&
              f.read((uint8_t*)&magic, 4) == 4 && magic == kTagCacheMagic &&
              f.read((uint8_t*)&ver, 2) == 2 && ver == kTagCacheVersion &&
              f.read((uint8_t*)&n, 4) == 4;
    spiUnlock();

    uint32_t done = 0;
    while (ok && done < n) {
      int got = 0;
      spiLock();
      while (got < kRecChunk && done < n) {
        if (f.read((uint8_t*)&recs[got], sizeof(TagCacheRec)) != sizeof(TagCacheRec)) { ok = false; break; }
        got++; done++;
      }
      spiUnlock();
      for (int r = 0; r < got; r++) {
        // Binärsuche nach pathCrc, bei Mehrfachtreffern nach links laufen.
        int lo = 0, hi = s_count - 1, hit = -1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          uint32_t c = trk(byCrc[mid]).crc;
          if (c < recs[r].pathCrc) lo = mid + 1;
          else { if (c == recs[r].pathCrc) hit = mid; hi = mid - 1; }
        }
        for (int k = hit; hit >= 0 && k < s_count && trk(byCrc[k]).crc == recs[r].pathCrc; k++) {
          Track& t = trk(byCrc[k]);
          // fsize 0 = noch unbekannt (readdir-Scan) -> Treffer gilt, Größe übernehmen.
          if (t.tagged == TAG_NONE && (t.fsize == recs[r].fsize || t.fsize == 0)) {
            applyTags(t, recs[r].title, recs[r].artist, recs[r].album);
            if (t.fsize == 0) t.fsize = recs[r].fsize;
            break;
          }
        }
      }
    }
    if (f) { spiLock(); f.close(); spiUnlock(); }
  }
  free(byCrc);
  free(recs);
  for (int t = 0; t < s_count; t++) if (trk(t).tagged == TAG_DONE) s_tagDone++;
}

// Cache komplett neu schreiben (atomar via .tmp + rename). Aufrufer hält lock();
// Writes gechunkt (s. applyTagCache).
void writeTagCache() {
  spiLock();
  if (!SD.exists("/.fennek")) SD.mkdir("/.fennek");
  if (SD.exists("/.fennek/id3.tmp")) SD.remove("/.fennek/id3.tmp");
  File f = SD.open("/.fennek/id3.tmp", FILE_WRITE);
  bool ok = (bool)f;
  if (ok) {
    uint32_t n = (uint32_t)s_count;
    ok = f.write((const uint8_t*)&kTagCacheMagic, 4) == 4;
    f.write((const uint8_t*)&kTagCacheVersion, 2);
    f.write((const uint8_t*)&n, 4);
  }
  spiUnlock();
  if (!ok) { if (f) { spiLock(); f.close(); spiUnlock(); } return; }
  TagCacheRec rec;
  int t = 0;
  while (ok && t < s_count) {
    spiLock();
    for (int i = 0; i < 16 && t < s_count; i++, t++) {
      memset(&rec, 0, sizeof(rec));
      rec.pathCrc = trk(t).crc;
      rec.fsize   = trk(t).fsize;
      // Es werden die angewandten Felder gespeichert; bei Tracks ohne Tags
      // stehen hier die Ordner-Fallbacks — beim Laden identisch angewandt.
      strncpy(rec.title,  trk(t).name,   sizeof(rec.title) - 1);
      strncpy(rec.artist, trk(t).artist, sizeof(rec.artist) - 1);
      strncpy(rec.album,  trk(t).album,  sizeof(rec.album) - 1);
      if (f.write((const uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) { ok = false; break; }
    }
    spiUnlock();
  }
  spiLock();
  f.close();
  if (ok) {
    if (SD.exists("/.fennek/id3.bin")) SD.remove("/.fennek/id3.bin");
    SD.rename("/.fennek/id3.tmp", "/.fennek/id3.bin");
  }
  spiUnlock();
}

void buildIndexes() {
  qsort(s_sorted, s_count, sizeof(int), cmpTrack);

  s_nArtists = 0; s_nAlbums = 0;
  for (int i = 0; i < s_count; i++) {
    bool newArtist = (i == 0) || strcasecmp(trk(i).artist, trk(i - 1).artist) != 0;
    bool newAlbum  = newArtist || strcasecmp(trk(i).album, trk(i - 1).album) != 0;
    if (newArtist) {
      s_artistStart[s_nArtists] = i;
      s_artistFirstAlbum[s_nArtists] = s_nAlbums;
      s_nArtists++;
    }
    if (newAlbum) {
      s_albumStart[s_nAlbums] = i;
      s_albumArtist[s_nAlbums] = s_nArtists - 1;
      s_nAlbums++;
    }
  }
  s_artistStart[s_nArtists] = s_count;
  s_artistFirstAlbum[s_nArtists] = s_nAlbums;
  s_albumStart[s_nAlbums] = s_count;

  for (int r = 0; r < s_nAlbums; r++) s_albumByName[r] = r;
  qsort(s_albumByName, s_nAlbums, sizeof(int), cmpAlbumByName);
  for (int i = 0; i < s_count; i++) s_byTitle[i] = i;
  qsort(s_byTitle, s_count, sizeof(int), cmpByTitle);
}

}  // namespace

namespace library {

void begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  ensureCapacity(TRACK_BLOCK);   // erster Block; weitere wachsen beim Scan
  if (!s_dirStack) {
    s_dirStack = (char(*)[TRACK_PATH_LEN])heap_caps_malloc(TRACK_PATH_LEN * kDirStackMax, MALLOC_CAP_SPIRAM);
    if (!s_dirStack) s_dirStack = (char(*)[TRACK_PATH_LEN])malloc(TRACK_PATH_LEN * kDirStackMax);
  }
  if (!s_plPath) {
    s_plPath = (char(*)[TRACK_PATH_LEN])heap_caps_malloc(TRACK_PATH_LEN * MAX_PLAYLISTS, MALLOC_CAP_SPIRAM);
    if (!s_plPath) s_plPath = (char(*)[TRACK_PATH_LEN])malloc(TRACK_PATH_LEN * MAX_PLAYLISTS);
  }
}

void startScan(const char* dir) {
  if (!board::sdReady() || !s_dirStack || !s_plPath) return;
  lock();
  if (s_scanDirOpen) { spiLock(); s_scanDir.close(); spiUnlock(); s_scanDirOpen = false; }
  strncpy(s_root, dir, sizeof(s_root) - 1); s_root[sizeof(s_root) - 1] = '\0';
  s_nPlaylists = 0;        // Playlist-Liste wird vom Scan neu aufgebaut
  s_plTrackCount = 0;
  s_limitWarned = false;
  s_rootFallbackTried = false;
  s_scanCount = 0;
  s_scanMode  = 0;
  strncpy(s_dirStack[0], dir, TRACK_PATH_LEN - 1); s_dirStack[0][TRACK_PATH_LEN - 1] = '\0';
  s_dirStackN   = 1;
  s_rootSlashes = countSlashes(dir);
  s_scanning    = true;
  unlock();
}

bool scanning() { return s_scanning; }
int  scanFound() { return s_scanning ? s_scanCount : 0; }

// Abschluss: Funde publizieren, ID3-Cache anwenden, Indizes bauen, Track-Cache
// schreiben. Aufrufer hält lock().
static void finishScan() {
  s_scanning = false;
  s_count = s_scanCount;
  for (int i = 0; i < s_count; i++) s_sorted[i] = i;
  applyTagCache();   // Treffer sofort; Rest inkrementell via tagScanStep()
  s_scanGen++;
  buildIndexes();
  writeTrackCache();
  Serial.printf("[FENNEK] Bibliothek: %d Tracks, %d Playlists (%d Blöcke à %d, PSRAM frei %u KB)\n",
                s_count, s_nPlaylists, s_cap / TRACK_BLOCK, TRACK_BLOCK,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Ein Häppchen Hintergrund-Scan: max. maxEntries Verzeichniseinträge unter
// einem spiLock (readdir via getNextFileName, kein fopen). Aufruf aus dem
// appmgr-Loop; Stack-Abbau LIFO = Tiefensuche hält den Stack klein.
void scanStep(int maxEntries) {
  if (!s_scanning) return;
  lock();
  spiLock();
  while (!s_scanDirOpen && s_dirStackN > 0) {
    char dir[TRACK_PATH_LEN];
    strncpy(dir, s_dirStack[--s_dirStackN], TRACK_PATH_LEN - 1); dir[TRACK_PATH_LEN - 1] = '\0';
    s_scanDir = SD.open(dir);
    if (s_scanDir && s_scanDir.isDirectory()) {
      s_scanDirOpen  = true;
      s_scanDirDepth = countSlashes(dir) - s_rootSlashes;
    } else if (s_scanDir) s_scanDir.close();
  }
  if (s_scanDirOpen) {
    for (int i = 0; i < maxEntries; i++) {
      bool isDir = false;
      String entry = s_scanDir.getNextFileName(&isDir);   // readdir, kein fopen
      if (entry.length() == 0) { s_scanDir.close(); s_scanDirOpen = false; break; }
      const char* full = entry.c_str();
      const char* nm = strrchr(full, '/');
      nm = nm ? nm + 1 : full;
      if (nm[0] == '.') continue;
      if (isDir) {
        if (s_scanMode == 0 && !skipDir(nm) && s_scanDirDepth < 6) {
          if (s_dirStackN < kDirStackMax) {
            strncpy(s_dirStack[s_dirStackN], full, TRACK_PATH_LEN - 1);
            s_dirStack[s_dirStackN][TRACK_PATH_LEN - 1] = '\0';
            s_dirStackN++;
          } else Serial.println("[FENNEK] Scan: Verzeichnis-Stack voll, Teilbaum übersprungen");
        }
      } else if (s_scanMode == 0 && isAudioFile(nm)) {
        addTrackPath(full);
      } else if (isM3u(nm)) {
        addPlaylistPath(full);
      }
    }
  }
  bool walkDone = (!s_scanDirOpen && s_dirStackN == 0);
  if (walkDone && s_scanMode == 0 && strcmp(s_root, "/") != 0) {
    // Phase 2: flache m3u-Suche außerhalb des Scan-Wurzelbaums (alte
    // scanM3uShallow-Semantik); bei Root-Scan bereits vom Walk abgedeckt.
    s_scanMode = 1;
    if (SD.exists("/playlists")) { strcpy(s_dirStack[s_dirStackN++], "/playlists"); }
    strcpy(s_dirStack[s_dirStackN++], "/");
    walkDone = false;
  }
  spiUnlock();

  bool fallback = false;
  if (walkDone) {
    finishScan();
    fallback = (s_count == 0 && !s_rootFallbackTried && strcmp(s_root, "/") != 0);
  }
  unlock();
  if (fallback) {
    Serial.println("[FENNEK] Keine Tracks unter " MP3_DIR " — durchsuche die ganze Karte");
    startScan("/");
    s_rootFallbackTried = true;
  }
}

// Bibliothek aus dem Track-Cache laden (Sekunden statt Verzeichnis-Walk).
// false = kein/ungültiger Cache -> Aufrufer startet startScan().
bool loadCache() {
  if (!board::sdReady() || !s_plPath) return false;
  lock();
  spiLock();
  File f;
  if (SD.exists(kTrackCache)) f = SD.open(kTrackCache);
  uint32_t magic = 0, n = 0, nPl = 0; uint16_t ver = 0, rec = 0;
  char root[sizeof(s_root)] = "";
  bool ok = f &&
            f.read((uint8_t*)&magic, 4) == 4 && magic == kTrackCacheMagic &&
            f.read((uint8_t*)&ver, 2) == 2 && ver == kTrackCacheVer &&
            f.read((uint8_t*)&rec, 2) == 2 && rec == (uint16_t)sizeof(Track) &&
            f.read((uint8_t*)&n, 4) == 4 && n <= (uint32_t)TRACKS_HARD_MAX &&
            f.read((uint8_t*)&nPl, 4) == 4 && nPl <= (uint32_t)MAX_PLAYLISTS &&
            f.read((uint8_t*)root, sizeof(root)) == sizeof(root);
  spiUnlock();

  int got = 0;
  while (ok && got < (int)n) {
    if (!ensureCapacity(got + 1)) { ok = false; break; }
    spiLock();
    for (int i = 0; i < 16 && got < (int)n; i++) {
      if (f.read((uint8_t*)&slotAt(got), sizeof(Track)) != sizeof(Track)) { ok = false; break; }
      got++;
    }
    spiUnlock();
  }
  int pl = 0;
  if (ok) {
    spiLock();
    for (; pl < (int)nPl; pl++)
      if (f.read((uint8_t*)s_plPath[pl], TRACK_PATH_LEN) != TRACK_PATH_LEN) { ok = false; break; }
    spiUnlock();
  }
  if (f) { spiLock(); f.close(); spiUnlock(); }
  if (!ok || got == 0) { unlock(); return false; }

  memcpy(s_root, root, sizeof(s_root)); s_root[sizeof(s_root) - 1] = '\0';
  s_nPlaylists = pl;
  s_count = got;
  s_tagDone = 0; s_tagCursor = 0; s_tagTotal = s_count;
  for (int i = 0; i < s_count; i++) {
    slotAt(i).path[TRACK_PATH_LEN - 1] = '\0';   // Robustheit bei Cache-Korruption
    s_sorted[i] = i;
    if (slotAt(i).tagged == TAG_DONE) s_tagDone++;
  }
  s_scanGen++;
  buildIndexes();
  Serial.printf("[FENNEK] Bibliothek aus Cache: %d Tracks, %d Playlists (Tags %d/%d)\n",
                s_count, s_nPlaylists, s_tagDone, s_tagTotal);
  unlock();
  return true;
}

// Blockierender Komplett-Scan (nur noch für Selbsttests; normaler Weg ist
// loadCache() bzw. startScan() + scanStep() aus dem appmgr-Loop).
int scan(const char* dir) {
  startScan(dir);
  while (s_scanning) scanStep(32);
  return count();
}

int count() { lock(); int c = s_count; unlock(); return c; }

bool name(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, trk(i).name, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool path(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, trk(i).path, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool trackArtist(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, trk(i).artist, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool trackAlbum(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, trk(i).album, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}

int indexOfPath(const char* p) {
  if (!p || !p[0]) return -1;
  uint32_t crc = crc32str(p);
  lock();
  int found = -1;
  for (int i = 0; i < s_count; i++) {
    if (trk(i).crc == crc && strcmp(trk(i).path, p) == 0) { found = i; break; }
  }
  unlock();
  return found;
}

// --- ID3-Tag-Scan -------------------------------------------------------------
bool tagScanPending() {
  lock(); bool p = (s_tagDone < s_tagTotal); unlock();
  return p;
}

void tagScanProgress(int* done, int* total) {
  lock();
  if (done)  *done  = s_tagDone;
  if (total) *total = s_tagTotal;
  unlock();
}

void tagScanStep(int maxFiles) {
  if (s_scanning) return;   // erst die Dateiliste, dann die Tags
  for (int n = 0; n < maxFiles; n++) {
    // Nächsten ungetaggten Track suchen (Pfad kopieren, Lock freigeben).
    char p[TRACK_PATH_LEN] = "";
    uint32_t gen;
    lock();
    gen = s_scanGen;
    while (s_tagCursor < s_count && trk(s_tagCursor).tagged != TAG_NONE) s_tagCursor++;
    int idx = s_tagCursor;
    bool have = (idx < s_count);
    if (have) { strncpy(p, trk(idx).path, sizeof(p) - 1); p[sizeof(p) - 1] = '\0'; }
    unlock();

    if (!have) {
      // Alles geparst: einmalig Indizes neu sortieren + Caches schreiben.
      lock();
      if (s_tagTotal > 0 && s_tagDone >= s_tagTotal && gen == s_scanGen) {
        buildIndexes();
        writeTagCache();
        writeTrackCache();   // Tags persistieren -> nächster Boot ohne Tag-Scan
      }
      unlock();
      return;
    }

    // ID3 lesen (nimmt spiLock; Library-Lock dabei NICHT halten).
    id3::Tags tags;
    bool ok = id3::read(p, tags);

    lock();
    if (gen == s_scanGen && idx < s_count && strcmp(trk(idx).path, p) == 0) {
      if (ok) applyTags(trk(idx), tags.title, tags.artist, tags.album);
      else    trk(idx).tagged = TAG_DONE;   // keine Tags -> Fallback bleibt
      if (tags.fsize > 0) trk(idx).fsize = tags.fsize;   // readdir kennt keine Größe
      s_tagDone++;
      // Letzter? -> Indizes + Caches.
      if (s_tagDone >= s_tagTotal) {
        buildIndexes();
        writeTagCache();
        writeTrackCache();
      }
    }
    unlock();
  }
}

// --- Künstler ---------------------------------------------------------------
int artistCount() { lock(); int c = s_nArtists; unlock(); return c; }
void artistName(int a, char* out, size_t n) {
  lock();
  if (a >= 0 && a < s_nArtists) { strncpy(out, trk(s_artistStart[a]).artist, n - 1); out[n - 1] = '\0'; }
  else if (n) out[0] = '\0';
  unlock();
}
int artistAlbumCount(int a) {
  lock(); int c = (a >= 0 && a < s_nArtists) ? (s_artistFirstAlbum[a + 1] - s_artistFirstAlbum[a]) : 0;
  unlock(); return c;
}
int artistAlbumRun(int a, int j) {
  lock(); int r = (a >= 0 && a < s_nArtists) ? (s_artistFirstAlbum[a] + j) : -1;
  unlock(); return r;
}

// --- Alben ------------------------------------------------------------------
int albumCount() { lock(); int c = s_nAlbums; unlock(); return c; }
int albumRunByName(int j) {
  lock(); int r = (j >= 0 && j < s_nAlbums) ? s_albumByName[j] : -1;
  unlock(); return r;
}
void albumLabel(int run, bool withArtist, char* out, size_t n) {
  lock();
  if (run >= 0 && run < s_nAlbums) {
    const Track& t = trk(s_albumStart[run]);
    if (withArtist) snprintf(out, n, "%s - %s", t.album, t.artist);
    else            { strncpy(out, t.album, n - 1); out[n - 1] = '\0'; }
  } else if (n) out[0] = '\0';
  unlock();
}
int albumTrackCount(int run) {
  lock(); int c = (run >= 0 && run < s_nAlbums) ? (s_albumStart[run + 1] - s_albumStart[run]) : 0;
  unlock(); return c;
}
void albumTrackName(int run, int k, char* out, size_t n) {
  lock();
  if (run >= 0 && run < s_nAlbums) {
    int idx = s_albumStart[run] + k;
    if (idx < s_albumStart[run + 1]) { strncpy(out, trk(idx).name, n - 1); out[n - 1] = '\0'; }
  } else if (n) out[0] = '\0';
  unlock();
}
int albumTrackFlatIndex(int run, int k) {
  lock(); int idx = -1;
  if (run >= 0 && run < s_nAlbums && s_albumStart[run] + k < s_albumStart[run + 1])
    idx = s_albumStart[run] + k;
  unlock(); return idx;
}
int albumTrackFlatList(int run, int* out, int maxN) {
  lock(); int c = 0;
  if (run >= 0 && run < s_nAlbums)
    for (int idx = s_albumStart[run]; idx < s_albumStart[run + 1] && c < maxN; idx++) out[c++] = idx;
  unlock(); return c;
}

// --- Titel ------------------------------------------------------------------
int titleCount() { lock(); int c = s_count; unlock(); return c; }
void titleName(int i, char* out, size_t n) {
  lock();
  if (i >= 0 && i < s_count) { strncpy(out, trk(s_byTitle[i]).name, n - 1); out[n - 1] = '\0'; }
  else if (n) out[0] = '\0';
  unlock();
}
int titleFlatIndex(int i) {
  lock(); int idx = (i >= 0 && i < s_count) ? s_byTitle[i] : -1;
  unlock(); return idx;
}

// --- Playlists (.m3u/.m3u8) -------------------------------------------------
int playlistCount() { lock(); int c = s_nPlaylists; unlock(); return c; }
void playlistName(int i, char* out, size_t n) {
  lock();
  if (i >= 0 && i < s_nPlaylists) { strncpy(out, baseName(s_plPath[i]), n - 1); out[n - 1] = '\0'; }
  else if (n) out[0] = '\0';
  unlock();
}

int playlistOpen(int i) {
  lock();
  s_plTrackCount = 0;
  if (i < 0 || i >= s_nPlaylists) { unlock(); return 0; }

  // Playlist-Verzeichnis ermitteln.
  char plDir[TRACK_PATH_LEN];
  strncpy(plDir, s_plPath[i], sizeof(plDir) - 1); plDir[sizeof(plDir) - 1] = '\0';
  char* sl = strrchr(plDir, '/');
  if (sl) *sl = '\0'; else strcpy(plDir, "/");
  if (plDir[0] == '\0') strcpy(plDir, "/");

  spiLock();
  File f = SD.open(s_plPath[i]);
  if (f) {
    while (f.available() && s_plTrackCount < s_cap) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line[0] == '#') continue;
      line.replace('\\', '/');

      char abs[TRACK_PATH_LEN];
      if (line[0] == '/') {
        normalizePath(line.c_str(), abs, sizeof(abs));
      } else {
        char joined[TRACK_PATH_LEN + 64];
        snprintf(joined, sizeof(joined), "%s/%s", plDir, line.c_str());
        normalizePath(joined, abs, sizeof(abs));
      }
      // Auf Bibliotheks-Track auflösen: erst Pfad, sonst Dateiname.
      int found = -1;
      for (int t = 0; t < s_count; t++)
        if (strcasecmp(trk(t).path, abs) == 0) { found = t; break; }
      if (found < 0) {
        const char* bn = baseName(abs);
        for (int t = 0; t < s_count; t++)
          if (strcasecmp(trk(t).name, bn) == 0) { found = t; break; }
      }
      if (found >= 0) s_plTracks[s_plTrackCount++] = found;
    }
    f.close();
  }
  spiUnlock();

  int c = s_plTrackCount;
  unlock();
  return c;
}

void playlistTrackName(int k, char* out, size_t n) {
  lock();
  if (k >= 0 && k < s_plTrackCount) { strncpy(out, trk(s_plTracks[k]).name, n - 1); out[n - 1] = '\0'; }
  else if (n) out[0] = '\0';
  unlock();
}
int playlistTrackFlatIndex(int k) {
  lock(); int idx = (k >= 0 && k < s_plTrackCount) ? s_plTracks[k] : -1;
  unlock(); return idx;
}

}  // namespace library
