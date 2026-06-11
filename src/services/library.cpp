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
  char     path[192];
  char     artist[48];   // ID3 oder Ordner-Fallback
  char     album[48];
  uint32_t fsize;
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

// --- ID3-Cache (/.meck/id3.bin) ----------------------------------------------
constexpr uint32_t kTagCacheMagic   = 0x33444D49;  // "IMD3"
constexpr uint16_t kTagCacheVersion = 1;

struct TagCacheRec {
  uint32_t pathCrc;
  uint32_t fsize;
  char     title[64];    // "" = Datei hat keine Tags (trotzdem gecacht!)
  char     artist[48];
  char     album[48];
};

// Tag-Scan-Zustand (Cursor über s_tracks; Indizes sind bis zum finalen
// Re-Sort stabil, weil buildIndexes() erst nach Abschluss erneut läuft).
int      s_tagDone  = 0;
int      s_tagTotal = 0;
int      s_tagCursor = 0;
uint32_t s_scanGen  = 0;     // invalidiert laufende Schritte nach Re-Scan

Track*            s_tracks = nullptr;
int               s_count  = 0;
SemaphoreHandle_t s_mutex  = nullptr;
char              s_root[80] = "/";

// Künstler-/Album-Runs
int s_artistStart[MAX_TRACKS + 1];
int s_artistFirstAlbum[MAX_TRACKS + 1];
int s_nArtists = 0;
int s_albumStart[MAX_TRACKS + 1];
int s_albumArtist[MAX_TRACKS + 1];
int s_nAlbums = 0;
int s_albumByName[MAX_TRACKS];
int s_byTitle[MAX_TRACKS];

// Playlists
char s_plPath[MAX_PLAYLISTS][192];
int  s_nPlaylists = 0;
int  s_plTracks[MAX_TRACKS];
int  s_plTrackCount = 0;

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

void addTrack(const char* dir, const char* nm, uint32_t fsize) {
  if (s_count >= MAX_TRACKS) return;
  Track& t = s_tracks[s_count];
  strncpy(t.name, nm, sizeof(t.name) - 1); t.name[sizeof(t.name) - 1] = '\0';
  snprintf(t.path, sizeof(t.path), "%s%s%s",
           dir, (dir[strlen(dir) - 1] == '/' ? "" : "/"), nm);
  deriveArtistAlbum(dir, t.artist, sizeof(t.artist), t.album, sizeof(t.album));
  t.fsize  = fsize;
  t.crc    = crc32str(t.path);
  t.tagged = TAG_NONE;
  s_count++;
}

void addPlaylist(const char* dir, const char* nm) {
  if (s_nPlaylists >= MAX_PLAYLISTS) return;
  snprintf(s_plPath[s_nPlaylists], sizeof(s_plPath[0]), "%s%s%s",
           dir, (dir[strlen(dir) - 1] == '/' ? "" : "/"), nm);
  s_nPlaylists++;
}

// Rekursiver Verzeichnis-Walk; Aufrufer hält spiLock().
void scanDir(const char* dir, int depth) {
  if (depth > 6 || s_count >= MAX_TRACKS) return;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  File f;
  while ((f = d.openNextFile()) && s_count < MAX_TRACKS) {
    const char* nm = f.name();
    if (f.isDirectory()) {
      if (!skipDir(nm)) {
        char sub[192];
        snprintf(sub, sizeof(sub), "%s%s%s",
                 dir, (dir[strlen(dir) - 1] == '/' ? "" : "/"), nm);
        f.close();
        scanDir(sub, depth + 1);
      } else f.close();
    } else {
      if (isAudioFile(nm) && f.size() > 0) addTrack(dir, nm, (uint32_t)f.size());
      else if (isM3u(nm))                  addPlaylist(dir, nm);
      f.close();
    }
  }
  d.close();
}

// Flache m3u-Suche in einem einzelnen Verzeichnis (z. B. /playlists, /).
void scanM3uShallow(const char* dir) {
  if (!SD.exists(dir)) return;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  File f;
  while ((f = d.openNextFile())) {
    if (!f.isDirectory() && isM3u(f.name())) addPlaylist(dir, f.name());
    f.close();
  }
  d.close();
}

// --- Sortierung & Runs ------------------------------------------------------
int cmpTrack(const void* a, const void* b) {
  const Track* x = (const Track*)a;
  const Track* y = (const Track*)b;
  int c = strcasecmp(x->artist, y->artist); if (c) return c;
  c = strcasecmp(x->album, y->album);        if (c) return c;
  return strcasecmp(x->name, y->name);
}
int cmpAlbumByName(const void* a, const void* b) {
  int ra = *(const int*)a, rb = *(const int*)b;
  const Track& ta = s_tracks[s_albumStart[ra]];
  const Track& tb = s_tracks[s_albumStart[rb]];
  int c = strcasecmp(ta.album, tb.album); if (c) return c;
  return strcasecmp(ta.artist, tb.artist);
}
int cmpByTitle(const void* a, const void* b) {
  return strcasecmp(s_tracks[*(const int*)a].name, s_tracks[*(const int*)b].name);
}

// Tags eines Cache-Records/Parse-Ergebnisses auf den Track anwenden.
// Leere Felder lassen den Ordner-/Dateinamen-Fallback stehen.
void applyTags(Track& t, const char* title, const char* artist, const char* album) {
  if (title && title[0])   { strncpy(t.name,   title,  sizeof(t.name) - 1);   t.name[sizeof(t.name) - 1] = '\0'; }
  if (artist && artist[0]) { strncpy(t.artist, artist, sizeof(t.artist) - 1); t.artist[sizeof(t.artist) - 1] = '\0'; }
  if (album && album[0])   { strncpy(t.album,  album,  sizeof(t.album) - 1);  t.album[sizeof(t.album) - 1] = '\0'; }
  t.tagged = TAG_DONE;
}

// Cache laden und Treffer anwenden. Aufrufer hält lock(); nimmt spiLock selbst.
void applyTagCache() {
  s_tagDone = 0; s_tagCursor = 0;
  spiLock();
  File f;
  if (SD.exists("/.meck/id3.bin")) f = SD.open("/.meck/id3.bin");
  if (f) {
    uint32_t magic = 0; uint16_t ver = 0; uint32_t n = 0;
    if (f.read((uint8_t*)&magic, 4) == 4 && magic == kTagCacheMagic &&
        f.read((uint8_t*)&ver, 2) == 2 && ver == kTagCacheVersion &&
        f.read((uint8_t*)&n, 4) == 4) {
      TagCacheRec rec;
      for (uint32_t i = 0; i < n; i++) {
        if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
        for (int t = 0; t < s_count; t++) {
          if (s_tracks[t].crc == rec.pathCrc && s_tracks[t].fsize == rec.fsize &&
              s_tracks[t].tagged == TAG_NONE) {
            applyTags(s_tracks[t], rec.title, rec.artist, rec.album);
            break;
          }
        }
      }
    }
    f.close();
  }
  spiUnlock();
  for (int t = 0; t < s_count; t++) if (s_tracks[t].tagged == TAG_DONE) s_tagDone++;
  s_tagTotal = s_count;
}

// Cache komplett neu schreiben (atomar via .tmp + rename). Aufrufer hält lock().
void writeTagCache() {
  spiLock();
  if (!SD.exists("/.meck")) SD.mkdir("/.meck");
  if (SD.exists("/.meck/id3.tmp")) SD.remove("/.meck/id3.tmp");
  File f = SD.open("/.meck/id3.tmp", FILE_WRITE);
  if (f) {
    uint32_t n = (uint32_t)s_count;
    f.write((const uint8_t*)&kTagCacheMagic, 4);
    f.write((const uint8_t*)&kTagCacheVersion, 2);
    f.write((const uint8_t*)&n, 4);
    TagCacheRec rec;
    for (int t = 0; t < s_count; t++) {
      memset(&rec, 0, sizeof(rec));
      rec.pathCrc = s_tracks[t].crc;
      rec.fsize   = s_tracks[t].fsize;
      // Es werden die angewandten Felder gespeichert; bei Tracks ohne Tags
      // stehen hier die Ordner-Fallbacks — beim Laden identisch angewandt.
      strncpy(rec.title,  s_tracks[t].name,   sizeof(rec.title) - 1);
      strncpy(rec.artist, s_tracks[t].artist, sizeof(rec.artist) - 1);
      strncpy(rec.album,  s_tracks[t].album,  sizeof(rec.album) - 1);
      f.write((const uint8_t*)&rec, sizeof(rec));
    }
    f.close();
    if (SD.exists("/.meck/id3.bin")) SD.remove("/.meck/id3.bin");
    SD.rename("/.meck/id3.tmp", "/.meck/id3.bin");
  }
  spiUnlock();
}

void buildIndexes() {
  qsort(s_tracks, s_count, sizeof(Track), cmpTrack);

  s_nArtists = 0; s_nAlbums = 0;
  for (int i = 0; i < s_count; i++) {
    bool newArtist = (i == 0) || strcasecmp(s_tracks[i].artist, s_tracks[i - 1].artist) != 0;
    bool newAlbum  = newArtist || strcasecmp(s_tracks[i].album, s_tracks[i - 1].album) != 0;
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
  if (!s_tracks) {
    s_tracks = (Track*)heap_caps_malloc(sizeof(Track) * MAX_TRACKS, MALLOC_CAP_SPIRAM);
    if (!s_tracks) s_tracks = (Track*)malloc(sizeof(Track) * MAX_TRACKS);
  }
}

int scan(const char* dir) {
  if (!s_tracks) return 0;
  lock();
  s_count = 0; s_nPlaylists = 0;
  strncpy(s_root, dir, sizeof(s_root) - 1); s_root[sizeof(s_root) - 1] = '\0';

  spiLock();
  scanDir(dir, 0);
  scanM3uShallow("/playlists");
  scanM3uShallow("/");
  spiUnlock();

  // ID3-Cache anwenden (Treffer sofort), Rest inkrementell via tagScanStep().
  applyTagCache();
  s_scanGen++;

  buildIndexes();
  int c = s_count;
  unlock();
  return c;
}

int count() { lock(); int c = s_count; unlock(); return c; }

bool name(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, s_tracks[i].name, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool path(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, s_tracks[i].path, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool trackArtist(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, s_tracks[i].artist, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}
bool trackAlbum(int i, char* out, size_t n) {
  lock(); bool ok = (i >= 0 && i < s_count);
  if (ok) { strncpy(out, s_tracks[i].album, n - 1); out[n - 1] = '\0'; }
  unlock(); return ok;
}

int indexOfPath(const char* p) {
  if (!p || !p[0]) return -1;
  uint32_t crc = crc32str(p);
  lock();
  int found = -1;
  for (int i = 0; i < s_count; i++) {
    if (s_tracks[i].crc == crc && strcmp(s_tracks[i].path, p) == 0) { found = i; break; }
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
  for (int n = 0; n < maxFiles; n++) {
    // Nächsten ungetaggten Track suchen (Pfad kopieren, Lock freigeben).
    char p[192] = "";
    uint32_t gen;
    lock();
    gen = s_scanGen;
    while (s_tagCursor < s_count && s_tracks[s_tagCursor].tagged != TAG_NONE) s_tagCursor++;
    int idx = s_tagCursor;
    bool have = (idx < s_count);
    if (have) { strncpy(p, s_tracks[idx].path, sizeof(p) - 1); p[sizeof(p) - 1] = '\0'; }
    unlock();

    if (!have) {
      // Alles geparst: einmalig Indizes neu sortieren + Cache schreiben.
      lock();
      if (s_tagTotal > 0 && s_tagDone >= s_tagTotal && gen == s_scanGen) {
        buildIndexes();
        writeTagCache();
      }
      unlock();
      return;
    }

    // ID3 lesen (nimmt spiLock; Library-Lock dabei NICHT halten).
    id3::Tags tags;
    bool ok = id3::read(p, tags);

    lock();
    if (gen == s_scanGen && idx < s_count && strcmp(s_tracks[idx].path, p) == 0) {
      if (ok) applyTags(s_tracks[idx], tags.title, tags.artist, tags.album);
      else    s_tracks[idx].tagged = TAG_DONE;   // keine Tags -> Fallback bleibt
      s_tagDone++;
      // Letzter? -> Indizes + Cache.
      if (s_tagDone >= s_tagTotal) {
        buildIndexes();
        writeTagCache();
      }
    }
    unlock();
  }
}

// --- Künstler ---------------------------------------------------------------
int artistCount() { lock(); int c = s_nArtists; unlock(); return c; }
void artistName(int a, char* out, size_t n) {
  lock();
  if (a >= 0 && a < s_nArtists) { strncpy(out, s_tracks[s_artistStart[a]].artist, n - 1); out[n - 1] = '\0'; }
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
    const Track& t = s_tracks[s_albumStart[run]];
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
    if (idx < s_albumStart[run + 1]) { strncpy(out, s_tracks[idx].name, n - 1); out[n - 1] = '\0'; }
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
  if (i >= 0 && i < s_count) { strncpy(out, s_tracks[s_byTitle[i]].name, n - 1); out[n - 1] = '\0'; }
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
  char plDir[192];
  strncpy(plDir, s_plPath[i], sizeof(plDir) - 1); plDir[sizeof(plDir) - 1] = '\0';
  char* sl = strrchr(plDir, '/');
  if (sl) *sl = '\0'; else strcpy(plDir, "/");
  if (plDir[0] == '\0') strcpy(plDir, "/");

  spiLock();
  File f = SD.open(s_plPath[i]);
  if (f) {
    while (f.available() && s_plTrackCount < MAX_TRACKS) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line[0] == '#') continue;
      line.replace('\\', '/');

      char abs[256];
      if (line[0] == '/') {
        normalizePath(line.c_str(), abs, sizeof(abs));
      } else {
        char joined[320];
        snprintf(joined, sizeof(joined), "%s/%s", plDir, line.c_str());
        normalizePath(joined, abs, sizeof(abs));
      }
      // Auf Bibliotheks-Track auflösen: erst Pfad, sonst Dateiname.
      int found = -1;
      for (int t = 0; t < s_count; t++)
        if (strcasecmp(s_tracks[t].path, abs) == 0) { found = t; break; }
      if (found < 0) {
        const char* bn = baseName(abs);
        for (int t = 0; t < s_count; t++)
          if (strcasecmp(s_tracks[t].name, bn) == 0) { found = t; break; }
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
  if (k >= 0 && k < s_plTrackCount) { strncpy(out, s_tracks[s_plTracks[k]].name, n - 1); out[n - 1] = '\0'; }
  else if (n) out[0] = '\0';
  unlock();
}
int playlistTrackFlatIndex(int k) {
  lock(); int idx = (k >= 0 && k < s_plTrackCount) ? s_plTracks[k] : -1;
  unlock(); return idx;
}

}  // namespace library
