#include "music_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "services/audio.h"
#include "services/library.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace {

using gui::Rect;

// --- Layout (240 x 320, Content ab appmgr::CONTENT_Y) ------------------------
constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;        // 24
constexpr int HEADER_Y = TOP + 2;                  // Titelzeile
constexpr int HEADER_H = TOP + 26;                 // Trennlinie bei y=50
constexpr int ROW_H    = 30;
constexpr int LIST_Y   = HEADER_H + 4;             // 54
constexpr int VISIBLE  = 7;                        // 7 Zeilen -> bis y=264
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;  // 268

// Browse-Leiste unten
const Rect kBack    {6,       BAR_Y, 110, 44};
const Rect kToPlayer{W - 116, BAR_Y, 110, 44};

// Kein-SD-Screen
const Rect kRetrySD {20, 150, W - 40, 48};

// Player-Buttons
const Rect kBtnPrev  {6,   158, 70, 52};
const Rect kBtnPlay  {85,  158, 70, 52};
const Rect kBtnNext  {164, 158, 70, 52};
const Rect kBtnVolDn {6,   222, 64, 46};
const Rect kBtnVolUp {170, 222, 64, 46};
const Rect kBtnList  {6,   276, 76, 38};
const Rect kBtnShuf  {88,  276, 70, 38};
const Rect kBtnRep   {164, 276, 70, 38};

// Region des Fortschritts-Streifens (nur dieser wird partiell aktualisiert).
constexpr int PROG_Y = 92;
constexpr int PROG_H = 50;

// --- Navigation -------------------------------------------------------------
enum Level {
  L_MENU, L_ARTISTS, L_ARTIST_ALBUMS, L_ALL_ALBUMS,
  L_ALBUM_TRACKS, L_TITLES, L_PLAYLISTS, L_PLAYLIST_TRACKS
};
struct Frame { Level level; int ctx; int lo, hi; };

Frame    s_stack[10];
int      s_depth   = 0;
enum Screen { BROWSE, PLAYER };
Screen   s_screen  = BROWSE;

uint32_t s_lastProg     = 0;       // letzter Fortschritts-Refresh (ms)
uint32_t s_lastShownSec = 999999;  // zuletzt angezeigte Position
int      s_progCount    = 0;       // Region-Refreshes -> periodisch voll
int      s_plOpenCount  = 0;       // # Tracks der gerade geöffneten Playlist
int      s_sel          = 0;       // Tastatur-Auswahl-Cursor in der Liste

int      s_scratch[MAX_TRACKS];    // Queue-Aufbau (kein Stack-Druck)

constexpr uint32_t kProgressMs = 3000;

const char* kMenu[3] = {"Künstler", "Album", "Playlist"};

void markDirty() { appmgr::markDirty(); }
Frame& top() { return s_stack[s_depth]; }

// --- Datenzugriff je Level --------------------------------------------------
int levelCount(Level lv, int ctx) {
  switch (lv) {
    case L_MENU:            return 3;
    case L_ARTISTS:         return library::artistCount();
    case L_ARTIST_ALBUMS:   return library::artistAlbumCount(ctx);
    case L_ALL_ALBUMS:      return library::albumCount();
    case L_ALBUM_TRACKS:    return library::albumTrackCount(ctx);
    case L_TITLES:          return library::titleCount();
    case L_PLAYLISTS:       return library::playlistCount();
    case L_PLAYLIST_TRACKS: return s_plOpenCount;
  }
  return 0;
}

void itemLabel(Level lv, int ctx, int i, char* out, size_t n) {
  switch (lv) {
    case L_MENU:            strncpy(out, kMenu[i], n - 1); out[n - 1] = '\0'; break;
    case L_ARTISTS:         library::artistName(i, out, n); break;
    case L_ARTIST_ALBUMS:   library::albumLabel(library::artistAlbumRun(ctx, i), false, out, n); break;
    case L_ALL_ALBUMS:      library::albumLabel(library::albumRunByName(i), true, out, n); break;
    case L_ALBUM_TRACKS:    library::albumTrackName(ctx, i, out, n); break;
    case L_TITLES:          library::titleName(i, out, n); break;
    case L_PLAYLISTS:       library::playlistName(i, out, n); break;
    case L_PLAYLIST_TRACKS: library::playlistTrackName(i, out, n); break;
  }
}

// Flat-Index eines Track-Items (für Highlight); -1 wenn kein Track-Level.
int itemFlat(Level lv, int ctx, int i) {
  switch (lv) {
    case L_ALBUM_TRACKS:    return library::albumTrackFlatIndex(ctx, i);
    case L_TITLES:          return library::titleFlatIndex(i);
    case L_PLAYLIST_TRACKS: return library::playlistTrackFlatIndex(i);
    default:                return -1;
  }
}

void titleFor(Frame& f, char* out, size_t n) {
  switch (f.level) {
    case L_MENU:            strncpy(out, "Musik", n - 1); out[n - 1] = '\0'; break;
    case L_ARTISTS:         strncpy(out, "Künstler", n - 1); out[n - 1] = '\0'; break;
    case L_ARTIST_ALBUMS:   library::artistName(f.ctx, out, n); break;
    case L_ALL_ALBUMS:      strncpy(out, "Alben", n - 1); out[n - 1] = '\0'; break;
    case L_ALBUM_TRACKS:    library::albumLabel(f.ctx, false, out, n); break;
    case L_TITLES:          strncpy(out, "Titel", n - 1); out[n - 1] = '\0'; break;
    case L_PLAYLISTS:       strncpy(out, "Playlists", n - 1); out[n - 1] = '\0'; break;
    case L_PLAYLIST_TRACKS: library::playlistName(f.ctx, out, n); break;
  }
}

// --- Bucket-Mathematik ------------------------------------------------------
int bucketCount(int M) {
  int k = (M + VISIBLE - 1) / VISIBLE;
  if (k > VISIBLE) k = VISIBLE;
  if (k < 1) k = 1;
  return k;
}
void bucketRange(int lo, int hi, int b, int K, int& bs, int& be) {
  int M = hi - lo;
  bs = lo + (int)((long)b * M / K);
  be = lo + (int)((long)(b + 1) * M / K);
}

void shortLabel(Level lv, int ctx, int idx, char* out, size_t n) {
  char full[64];
  itemLabel(lv, ctx, idx, full, sizeof(full));
  size_t k = 0;
  for (; k < n - 1 && k < 6 && full[k]; k++) out[k] = full[k];
  out[k] = '\0';
}

// --- Navigation-Aktionen ----------------------------------------------------
void pushFrame(Level lv, int ctx) {
  if (s_depth + 1 >= (int)(sizeof(s_stack) / sizeof(s_stack[0]))) return;
  if (lv == L_PLAYLIST_TRACKS) s_plOpenCount = library::playlistOpen(ctx);
  int cnt = levelCount(lv, ctx);
  s_depth++;
  top() = Frame{lv, ctx, 0, cnt};
  s_sel = 0;
  markDirty();
}
void pushBucket(Level lv, int ctx, int lo, int hi) {
  if (s_depth + 1 >= (int)(sizeof(s_stack) / sizeof(s_stack[0]))) return;
  s_depth++;
  top() = Frame{lv, ctx, lo, hi};
  s_sel = 0;
  markDirty();
}
void goBack() {
  if (s_depth > 0) { s_depth--; s_sel = 0; markDirty(); }
}

// Anzahl sichtbarer Zeilen des aktuellen Frames (Leaf-Items oder Buckets).
int visibleRows() {
  Frame& f = top();
  int M = f.hi - f.lo;
  if (M <= 0) return 0;
  return (M <= VISIBLE) ? M : bucketCount(M);
}

// Flat-Index des aktuell spielenden Tracks (-1 = keiner / nicht in Bibliothek).
int currentFlat() {
  char p[192];
  audio::currentPath(p, sizeof(p));
  return p[0] ? library::indexOfPath(p) : -1;
}

// Flat-Index-Liste als Pfad-Queue an den Audio-Service übergeben.
void playFlatList(const int* flat, int n, int startItem) {
  if (n <= 0) return;
  audio::queueBegin(audio::Owner::Music);
  char p[192];
  for (int i = 0; i < n; i++) {
    if (library::path(flat[i], p, sizeof(p))) audio::queueAdd(p);
  }
  audio::queueCommit(startItem);
  s_screen = PLAYER;
  markDirty();
}

void playFromAlbum(int run, int startItem) {
  int n = library::albumTrackFlatList(run, s_scratch, MAX_TRACKS);
  playFlatList(s_scratch, n, startItem);
}
void playFromTitles(int startItem) {
  int n = library::titleCount();
  if (n > MAX_TRACKS) n = MAX_TRACKS;
  for (int i = 0; i < n; i++) s_scratch[i] = library::titleFlatIndex(i);
  playFlatList(s_scratch, n, startItem);
}
void playFromPlaylist(int startItem) {
  int n = s_plOpenCount;
  if (n > MAX_TRACKS) n = MAX_TRACKS;
  for (int i = 0; i < n; i++) s_scratch[i] = library::playlistTrackFlatIndex(i);
  playFlatList(s_scratch, n, startItem);
}

// Auswahl eines einzelnen Items (Leaf-Liste) an Item-Index idx.
void selectItem(Level lv, int ctx, int idx) {
  switch (lv) {
    case L_MENU:
      if (idx == 0)      pushFrame(L_ARTISTS, -1);
      else if (idx == 1) pushFrame(L_ALL_ALBUMS, -1);
      else               pushFrame(L_PLAYLISTS, -1);
      break;
    case L_ARTISTS:         pushFrame(L_ARTIST_ALBUMS, idx); break;
    case L_ARTIST_ALBUMS:   pushFrame(L_ALBUM_TRACKS, library::artistAlbumRun(ctx, idx)); break;
    case L_ALL_ALBUMS:      pushFrame(L_ALBUM_TRACKS, library::albumRunByName(idx)); break;
    case L_ALBUM_TRACKS:    playFromAlbum(ctx, idx); break;
    case L_TITLES:          playFromTitles(idx); break;
    case L_PLAYLISTS:       pushFrame(L_PLAYLIST_TRACKS, idx); break;
    case L_PLAYLIST_TRACKS: playFromPlaylist(idx); break;
  }
}

// SD erneut mounten und Bibliothek neu scannen ("Erneut suchen").
void retrySD() {
  if (board::initSD()) {
    int n = library::scan(MP3_DIR);
    if (n == 0) library::scan("/");
    s_depth = 0;
    s_stack[0] = Frame{L_MENU, -1, 0, 3};
  }
  markDirty();
}

// --- Zeichnen: Browser ------------------------------------------------------
// Nur der Listenbereich (auch für den Cursor-Region-Refresh).
void drawListArea(Adafruit_GFX& g) {
  Frame& f = top();
  int M = f.hi - f.lo;
  int curFlat = currentFlat();

  if (M <= 0) {
    gui::printAt(g, 10, 80, "(leer)", 2);
  } else if (M <= VISIBLE) {
    // Leaf-Liste: Items einzeln
    for (int r = 0; r < M; r++) {
      int idx = f.lo + r;
      char lbl[64];
      itemLabel(f.level, f.ctx, idx, lbl, sizeof(lbl));
      bool cur = (curFlat >= 0 && itemFlat(f.level, f.ctx, idx) == curFlat);
      gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, cur);
    }
  } else {
    // Bucket-Ansicht: Anfangsbuchstaben-Gruppen
    int K = bucketCount(M);
    for (int b = 0; b < K; b++) {
      int bs, be; bucketRange(f.lo, f.hi, b, K, bs, be);
      char a[8], z[8], lbl[24];
      shortLabel(f.level, f.ctx, bs, a, sizeof(a));
      shortLabel(f.level, f.ctx, be - 1, z, sizeof(z));
      snprintf(lbl, sizeof(lbl), "%s - %s", a, z);
      gui::drawRowText(g, LIST_Y + b * ROW_H, ROW_H, lbl, false);
    }
  }

  // Tastatur-Cursor: Doppelrahmen um die ausgewählte Zeile.
  int rows = visibleRows();
  if (rows > 0 && s_sel >= 0 && s_sel < rows) {
    int y = LIST_Y + s_sel * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
}

// Kopfzeile (auch als Region-Refresh während des ID3-Scans).
void drawHeader(Adafruit_GFX& g) {
  g.fillRect(0, TOP, W, HEADER_H - TOP, GxEPD_WHITE);
  Frame& f = top();
  char title[48];
  titleFor(f, title, sizeof(title));
  gui::printAt(g, 6, HEADER_Y, title, 2);
  if (library::tagScanPending()) {
    int d, t; library::tagScanProgress(&d, &t);
    char s[24];
    snprintf(s, sizeof(s), "Tags %d/%d", d, t);
    g.setTextSize(1);
    uint16_t bw, bh;
    gui::textBounds(g, s, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, HEADER_Y + 4);
    gui::print(g, s);
  }
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
}

void drawBrowse(Adafruit_GFX& g) {
  drawHeader(g);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, "Keine SD-Karte", 2);
    gui::printAt(g, 10, 110, "Karte einlegen, dann:", 1);
    gui::drawButton(g, kRetrySD, "Erneut suchen", false);
    return;
  }

  drawListArea(g);

  // Untere Leiste
  audio::Status st = audio::status();
  if (s_depth > 0) gui::drawButton(g, kBack, "Zurück", false);
  if (st.pos >= 0 && st.owner == audio::Owner::Music)
    gui::drawButton(g, kToPlayer, "Player", false);
}

// --- Zeichnen: Player -------------------------------------------------------
void drawTimeAndBar(Adafruit_GFX& g, const audio::Status& st) {
  char t1[12], t2[12];
  snprintf(t1, sizeof(t1), "%lu:%02lu", (unsigned long)(st.posSec / 60), (unsigned long)(st.posSec % 60));
  snprintf(t2, sizeof(t2), "%lu:%02lu", (unsigned long)(st.durSec / 60), (unsigned long)(st.durSec % 60));
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(2);
  g.setCursor(6, PROG_Y + 8); g.print(t1);
  if (st.durSec > 0) {
    int16_t bx, by; uint16_t bw, bh;
    g.getTextBounds(t2, 0, 0, &bx, &by, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, PROG_Y + 8); g.print(t2);
  }
  int barX = 6, barY = PROG_Y + 34, barW = W - 12, barH = 12;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (st.durSec > 0 && st.posSec <= st.durSec) {
    int fw = (int)((uint64_t)(barW - 2) * st.posSec / st.durSec);
    if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  }
}

void drawPlayer(Adafruit_GFX& g) {
  audio::Status st = audio::status();
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 6, HEADER_Y, "Wiedergabe", 2);
  if (st.sleepMin > 0) {
    char sl[16];
    snprintf(sl, sizeof(sl), "Zzz %um", (unsigned)st.sleepMin);
    g.setTextSize(1);
    uint16_t bw, bh;
    gui::textBounds(g, sl, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, HEADER_Y + 4);
    gui::print(g, sl);
  }
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  // Titel (ID3 oder Dateiname) + Künstler.
  char nm[64] = "-", artist[48] = "";
  int flat = currentFlat();
  if (flat >= 0) {
    library::name(flat, nm, sizeof(nm));
    library::trackArtist(flat, artist, sizeof(artist));
  } else {
    char p[192];
    audio::currentPath(p, sizeof(p));
    if (p[0]) {
      const char* base = strrchr(p, '/');
      strncpy(nm, base ? base + 1 : p, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0';
    }
  }
  gui::printAt(g, 6, HEADER_H + 8, nm, 2);
  if (artist[0]) gui::printAt(g, 6, HEADER_H + 28, artist, 1);

  drawTimeAndBar(g, st);

  gui::drawButton(g, kBtnPrev, "|<", false);
  gui::drawButton(g, kBtnPlay, (st.playing && !st.paused) ? "||" : ">", true);
  gui::drawButton(g, kBtnNext, ">|", false);
  gui::drawButton(g, kBtnVolDn, "-", false);
  gui::drawButton(g, kBtnVolUp, "+", false);
  char vol[16];
  snprintf(vol, sizeof(vol), "Vol %d", st.volume);
  int16_t bx, by; uint16_t bw, bh;
  g.setTextSize(2);
  g.getTextBounds(vol, 0, 0, &bx, &by, &bw, &bh);
  g.setCursor((W - (int)bw) / 2, kBtnVolDn.y + (kBtnVolDn.h - (int)bh) / 2);
  g.print(vol);

  gui::drawButton(g, kBtnList, "Liste", false);
  gui::drawButton(g, kBtnShuf, "Mix", st.shuffle);
  const char* rep = (st.repeat == audio::Repeat::One) ? "Wdh 1"
                  : (st.repeat == audio::Repeat::All) ? "Wdh A" : "Wdh";
  gui::drawButton(g, kBtnRep, rep, st.repeat != audio::Repeat::Off);
}

// Nur der Fortschritts-Streifen (für Region-Refresh).
void drawProgStrip(Adafruit_GFX& g) {
  g.fillRect(0, PROG_Y, W, PROG_H, GxEPD_WHITE);
  drawTimeAndBar(g, audio::status());
}

// --- Touch ------------------------------------------------------------------
void onBrowseTouch(int x, int y) {
  Frame& f = top();
  audio::Status st = audio::status();
  if (s_depth > 0 && kBack.hit(x, y)) { goBack(); return; }
  if (st.pos >= 0 && st.owner == audio::Owner::Music && kToPlayer.hit(x, y)) {
    s_screen = PLAYER; markDirty(); return;
  }
  if (!board::sdReady()) {
    if (kRetrySD.hit(x, y)) retrySD();
    return;
  }

  if (y < LIST_Y || y >= LIST_Y + VISIBLE * ROW_H) return;
  int row = (y - LIST_Y) / ROW_H;
  int M = f.hi - f.lo;
  if (M <= 0) return;

  if (M <= VISIBLE) {
    if (row < M) selectItem(f.level, f.ctx, f.lo + row);
  } else {
    int K = bucketCount(M);
    if (row < K) {
      int bs, be; bucketRange(f.lo, f.hi, row, K, bs, be);
      pushBucket(f.level, f.ctx, bs, be);
    }
  }
}

// Repeat-Modus zyklisch weiterschalten (Aus -> Alle -> Eins).
void cycleRepeat() {
  audio::Repeat r = audio::status().repeat;
  audio::Repeat n = (r == audio::Repeat::Off) ? audio::Repeat::All
                  : (r == audio::Repeat::All) ? audio::Repeat::One
                                              : audio::Repeat::Off;
  audio::setRepeat(n);
}

void onPlayerTouch(int x, int y) {
  if      (kBtnPlay.hit(x, y))  audio::togglePause();
  else if (kBtnNext.hit(x, y))  audio::next();
  else if (kBtnPrev.hit(x, y))  audio::prev();
  else if (kBtnVolUp.hit(x, y)) audio::volumeUp();
  else if (kBtnVolDn.hit(x, y)) audio::volumeDown();
  else if (kBtnList.hit(x, y))  { s_screen = BROWSE; }
  else if (kBtnShuf.hit(x, y))  audio::setShuffle(!audio::status().shuffle);
  else if (kBtnRep.hit(x, y))   cycleRepeat();
  else return;
  markDirty();
}

// Cursor bewegen: nur den Listenbereich partiell neu zeichnen (schnell).
void moveSel(int delta) {
  int rows = visibleRows();
  if (rows <= 0) return;
  int ns = s_sel + delta;
  if (ns < 0) ns = rows - 1;       // Wrap-Around
  if (ns >= rows) ns = 0;
  if (ns == s_sel) return;
  s_sel = ns;
  if (!appmgr::isDirty()) display::renderRegion(drawListArea, LIST_Y, VISIBLE * ROW_H);
}

// Aktuell ausgewählte Zeile aktivieren (Leaf-Item oder Bucket).
void activateSel() {
  Frame& f = top();
  int M = f.hi - f.lo;
  if (M <= 0) return;
  if (M <= VISIBLE) {
    if (s_sel < M) selectItem(f.level, f.ctx, f.lo + s_sel);
  } else {
    int K = bucketCount(M);
    if (s_sel < K) {
      int bs, be; bucketRange(f.lo, f.hi, s_sel, K, bs, be);
      pushBucket(f.level, f.ctx, bs, be);
    }
  }
}

void onBrowseKey(char k) {
  audio::Status st = audio::status();
  switch (k) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(+1); break;
    case '\r':          activateSel(); break;
    case '\b':          if (s_depth > 0) goBack(); else appmgr::goHome(); break;
    case 'q': case 'Q': appmgr::goHome(); break;
    case 'p': case 'P':
      if (st.pos >= 0 && st.owner == audio::Owner::Music) { s_screen = PLAYER; markDirty(); }
      break;
    case ' ':           if (st.playing) { audio::togglePause(); } break;
    default: break;
  }
}

// Sleep-Timer zyklisch: Aus -> 15 -> 30 -> 45 -> 60 -> Aus.
void cycleSleep() {
  uint16_t m = audio::status().sleepMin;
  uint16_t n = (m == 0) ? 15 : (m <= 15) ? 30 : (m <= 30) ? 45 : (m <= 45) ? 60 : 0;
  audio::setSleepTimer(n);
}

void onPlayerKey(char k) {
  switch (k) {
    case ' ': case '\r': audio::togglePause(); markDirty(); break;
    case 'a': case 'A':  audio::prev(); markDirty(); break;
    case 'd': case 'D':  audio::next(); markDirty(); break;
    case 'w': case 'W':  audio::volumeUp(); markDirty(); break;
    case 's': case 'S':  audio::volumeDown(); markDirty(); break;
    case 'x': case 'X':  audio::setShuffle(!audio::status().shuffle); markDirty(); break;
    case 'r': case 'R':  cycleRepeat(); markDirty(); break;
    case 'z': case 'Z':  cycleSleep(); markDirty(); break;
    case '\b':           s_screen = BROWSE; markDirty(); break;
    case 'q': case 'Q':  appmgr::goHome(); break;
    default: break;
  }
}

// --- App-Klasse ---------------------------------------------------------------
class MusicApp : public App {
 public:
  const char* name() const override { return "Musik"; }

  void onEnter() override {
    // Beim ersten Betreten (oder nach SD-Wechsel) auf dem Hauptmenü starten.
    if (s_depth == 0 && s_stack[0].hi == 0) s_stack[0] = Frame{L_MENU, -1, 0, 3};
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      if (s_screen == BROWSE) onBrowseTouch(e.x, e.y);
      else                    onPlayerTouch(e.x, e.y);
    } else {
      if (s_screen == BROWSE) onBrowseKey(e.key);
      else                    onPlayerKey(e.key);
    }
  }

  void tick() override {
    if (appmgr::isDirty()) return;

    // ID3-Scan-Fortschritt in der Kopfzeile (nur Region-Refresh, alle 5 s).
    if (s_screen == BROWSE && library::tagScanPending()) {
      static uint32_t s_lastTagHdr = 0;
      if (millis() - s_lastTagHdr >= 5000) {
        s_lastTagHdr = millis();
        display::renderRegion(drawHeader, TOP, HEADER_H - TOP + 1);
      }
      return;
    }
    if (s_screen != PLAYER) return;

    // Fortschritt im Player nur als Streifen aktualisieren (kein Voll-Refresh).
    audio::Status st = audio::status();
    if (st.playing && !st.paused && st.posSec != s_lastShownSec &&
        millis() - s_lastProg >= kProgressMs) {
      s_lastProg = millis();
      s_lastShownSec = st.posSec;
      // periodisch voll neu zeichnen (Ghosting im Streifen beseitigen)
      if (++s_progCount % 10 == 0) appmgr::markDirty();
      else display::renderRegion(drawProgStrip, PROG_Y, PROG_H);
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    s_lastShownSec = 999999;
    if (s_screen == BROWSE) drawBrowse(g);
    else                    drawPlayer(g);
  }
};

MusicApp s_app;

}  // namespace

namespace music_app {

App* get() {
  // Frame-Stack initialisieren (einmalig).
  static bool s_init = false;
  if (!s_init) {
    s_init = true;
    s_stack[0] = Frame{L_MENU, -1, 0, 3};
  }
  return &s_app;
}

}  // namespace music_app
