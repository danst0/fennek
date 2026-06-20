// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "podcast_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"
#include "services/audio.h"
#include "services/podcast.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <string.h>

namespace {

using gui::Rect;

constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_Y = TOP + 2;
constexpr int HEADER_H = TOP + 26;
constexpr int ROW_H    = 30;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int VISIBLE  = 7;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;

// Listen-Leiste: Zurück | Sync alle | Hoch | Runter
const Rect kBack    {6,   BAR_Y, 52, 44};
const Rect kSyncAll {62,  BAR_Y, 64, 44};
const Rect kUp      {130, BAR_Y, 50, 44};
const Rect kDown    {184, BAR_Y, 50, 44};
const Rect kRetrySD {20, 150, W - 40, 48};

// Detail-/Player-Screen
constexpr int PROG_Y = 150;
constexpr int PROG_H = 50;
const Rect kBtnBack30 {6,   210, 70, 50};
const Rect kBtnPlay   {85,  210, 70, 50};
const Rect kBtnFwd30  {164, 210, 70, 50};
const Rect kBtnSync   {6,   266, 112, 46};
const Rect kBtnList   {126, 266, 108, 46};

enum Screen { LIST, DETAIL };
Screen s_screen = LIST;

podcast::Feed s_feeds[podcast::kMaxFeeds];
int  s_n = -1;                 // -1 = noch nicht geladen
int  s_sel = 0, s_off = 0;

int  s_openFeed = -1;          // im DETAIL gewählter Feed
podcast::Local s_local;
bool s_haveLocal = false;
uint32_t s_epKey = 0;          // CRC32 des Episodenpfads (Bookmark-Key)

// Blockierender Sync: Anforderung wird erst nach dem "Synchronisiere…"-Frame
// ausgeführt, damit der Nutzer Feedback sieht, bevor die UI blockiert.
int  s_syncReq = -2;           // -2 = keiner, -1 = alle, >=0 = Feed-Index
bool s_syncDrawn = false;
char s_syncFeedName[64] = "";
char s_syncLog[64] = "";

// Bookmark-Pflege
uint32_t s_lastSave = 0;
bool     s_wasPlaying = false;
uint32_t s_lastProg = 0, s_lastShownSec = 999999;
int      s_progCount = 0;
constexpr uint32_t kProgressMs = 3000;

void markDirty() { appmgr::markDirty(); }

void loadFeeds() {
  s_n = 0;
  int c = podcast::feedCount();
  for (int i = 0; i < c && i < podcast::kMaxFeeds; ++i)
    if (podcast::feed(i, &s_feeds[i])) s_n++;
  if (s_sel >= s_n) s_sel = s_n > 0 ? s_n - 1 : 0;
  if (s_off > s_sel) s_off = 0;
}

void refreshLocal() {
  s_haveLocal = (s_openFeed >= 0 && s_openFeed < s_n)
                  ? podcast::localEpisode(s_feeds[s_openFeed], &s_local) : false;
  if (!s_haveLocal) { s_local.title[0] = '\0'; s_local.file[0] = '\0'; }
}

void saveBookmarkNow() {
  if (s_epKey) {
    audio::Status st = audio::status();
    if (st.owner == audio::Owner::Podcast) settings::setBookBookmark(s_epKey, 0, st.posSec);
  }
}

void openEpisode() {
  if (!s_haveLocal || !s_local.file[0]) return;
  saveBookmarkNow();
  s_epKey = settings::crc32(s_local.file);
  uint16_t fi = 0; uint32_t pos = 0;
  settings::bookBookmark(s_epKey, &fi, &pos);
  audio::queueBegin(audio::Owner::Podcast);
  audio::queueAdd(s_local.file);
  audio::setShuffle(false);
  audio::setRepeat(audio::Repeat::Off);
  audio::queueCommit(0, pos > 5 ? pos - 5 : 0);
  s_lastSave = millis();
  markDirty();
}

// --- Zeichnen -----------------------------------------------------------------
void drawSyncing(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, "Podcast", 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
  gui::printAt(g, 10, 120, "Synchronisiere\205", 2);   // \205 = …
  if (s_syncFeedName[0]) gui::printAt(g, 10, 150, s_syncFeedName, 1);
  gui::printAt(g, 10, 180, "WLAN aktiv \227 bitte warten", 1);   // \227 = —
}

void drawList(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, "Podcast", 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::NoSdCard), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::InsertCard), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }
  if (s_n <= 0) {
    gui::printAt(g, 10, 80, "Keine Feeds", 2);
    gui::printAt(g, 10, 110, "/podcasts/feeds.txt", 1);
    gui::printAt(g, 10, 124, "per WebFM bearbeiten", 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }

  for (int r = 0; r < VISIBLE && s_off + r < s_n; ++r) {
    int i = s_off + r;
    podcast::Local loc;
    bool have = podcast::localEpisode(s_feeds[i], &loc);
    char lbl[96];
    snprintf(lbl, sizeof(lbl), "%s%s", have ? "\020 " : "  ", s_feeds[i].name);  // \020 = ►
    gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, false);
  }
  int cr = s_sel - s_off;
  if (cr >= 0 && cr < VISIBLE) {
    int y = LIST_Y + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
  if (s_syncLog[0]) gui::printAt(g, 6, BAR_Y - 14, s_syncLog, 1);

  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
  gui::drawButton(g, kSyncAll, "Sync", false);
  if (s_off > 0)                   gui::drawButton(g, kUp,   i18n::tr(i18n::Str::BtnUp), false);
  if (s_off + VISIBLE < s_n)       gui::drawButton(g, kDown, i18n::tr(i18n::Str::BtnDown), false);
}

void drawTimeAndBar(Adafruit_GFX& g, const audio::Status& st) {
  char t1[12], t2[12];
  snprintf(t1, sizeof(t1), "%lu:%02lu", (unsigned long)(st.posSec / 60), (unsigned long)(st.posSec % 60));
  snprintf(t2, sizeof(t2), "%lu:%02lu", (unsigned long)(st.durSec / 60), (unsigned long)(st.durSec % 60));
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(2);
  g.setCursor(6, PROG_Y + 8); g.print(t1);
  if (st.durSec > 0) {
    uint16_t bw, bh;
    gui::textBounds(g, t2, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, PROG_Y + 8); g.print(t2);
  }
  int barX = 6, barY = PROG_Y + 34, barW = W - 12, barH = 12;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (st.durSec > 0 && st.posSec <= st.durSec) {
    int fw = (int)((uint64_t)(barW - 2) * st.posSec / st.durSec);
    if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  }
}

void drawProgStrip(Adafruit_GFX& g) {
  g.fillRect(0, PROG_Y, W, PROG_H, GxEPD_WHITE);
  drawTimeAndBar(g, audio::status());
}

// Wort-orientierter Mehrzeilen-Druck (Episodentitel können lang sein).
void drawWrapped(Adafruit_GFX& g, int x, int y, int maxW, const char* text, int size, int maxLines) {
  char line[64]; int ll = 0, lines = 0;
  const char* p = text;
  auto flush = [&](bool force) {
    line[ll] = '\0';
    if (ll > 0 && (lines < maxLines)) { gui::printAt(g, x, y + lines * (size * 8 + 2), line, size); lines++; }
    ll = 0; (void)force;
  };
  while (*p && lines < maxLines) {
    const char* sp = strchr(p, ' ');
    int wlen = sp ? (int)(sp - p) : (int)strlen(p);
    if (wlen > (int)sizeof(line) - 1) wlen = sizeof(line) - 1;
    int wpix = (ll + (ll ? 1 : 0) + wlen) * 6 * size;
    if (wpix > maxW && ll > 0) flush(false);
    if (ll && ll < (int)sizeof(line) - 1) line[ll++] = ' ';
    for (int i = 0; i < wlen && ll < (int)sizeof(line) - 1; ++i) line[ll++] = p[i];
    p += wlen;
    while (*p == ' ') p++;
  }
  flush(true);
}

void drawDetail(Adafruit_GFX& g) {
  const char* fname = (s_openFeed >= 0 && s_openFeed < s_n) ? s_feeds[s_openFeed].name : "Podcast";
  gui::printAt(g, 6, HEADER_Y, fname, 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  audio::Status st = audio::status();
  bool mine = (st.owner == audio::Owner::Podcast && st.pos >= 0);

  if (s_haveLocal && s_local.title[0]) {
    drawWrapped(g, 6, HEADER_H + 10, W - 12, s_local.title, 2, 3);
  } else {
    gui::printAt(g, 6, HEADER_H + 10, "Keine Folge geladen", 2);
    gui::printAt(g, 6, HEADER_H + 34, "\"Sync\" laedt die neueste.", 1);
  }

  if (s_haveLocal) drawTimeAndBar(g, st);

  gui::drawButton(g, kBtnBack30, "<30", false);
  gui::drawButton(g, kBtnPlay, (st.playing && !st.paused && mine) ? "||" : ">", s_haveLocal);
  gui::drawButton(g, kBtnFwd30, "30>", false);
  gui::drawButton(g, kBtnSync, "Sync", false);
  gui::drawButton(g, kBtnList, "Liste", false);
}

// --- Interaktion --------------------------------------------------------------
void requestSync(int idx) {
  s_syncReq = idx;
  s_syncDrawn = false;
  s_syncLog[0] = '\0';
  if (idx >= 0 && idx < s_n) { strncpy(s_syncFeedName, s_feeds[idx].name, sizeof(s_syncFeedName) - 1); s_syncFeedName[sizeof(s_syncFeedName)-1]='\0'; }
  else strcpy(s_syncFeedName, "alle Feeds");
  markDirty();
}

void retrySD() { if (board::initSD()) loadFeeds(); markDirty(); }

void moveSel(int delta) {
  if (s_n <= 0) return;
  int ns = s_sel + delta;
  if (ns < 0) ns = s_n - 1;
  if (ns >= s_n) ns = 0;
  s_sel = ns;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  markDirty();
}

void enterDetail(int idx) {
  if (idx < 0 || idx >= s_n) return;
  s_openFeed = idx;
  refreshLocal();
  if (s_haveLocal) s_epKey = settings::crc32(s_local.file);
  s_screen = DETAIL;
  markDirty();
}

void togglePlay() {
  audio::Status st = audio::status();
  if (st.owner == audio::Owner::Podcast && st.playing) {
    audio::togglePause();
    if (!st.paused) saveBookmarkNow();
  } else {
    openEpisode();
  }
  markDirty();
}

void onListTouch(int x, int y) {
  if (kBack.hit(x, y)) { appmgr::goHome(); return; }
  if (!board::sdReady() || s_n <= 0) { if (kRetrySD.hit(x, y)) retrySD(); return; }
  if (kSyncAll.hit(x, y)) { requestSync(-1); return; }
  if (kUp.hit(x, y))   { s_off = (s_off > VISIBLE) ? s_off - VISIBLE : 0; markDirty(); return; }
  if (kDown.hit(x, y)) { if (s_off + VISIBLE < s_n) { s_off += VISIBLE; markDirty(); } return; }
  if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
    int r = (y - LIST_Y) / ROW_H;
    if (s_off + r < s_n) { s_sel = s_off + r; enterDetail(s_sel); }
  }
}

void onDetailTouch(int x, int y) {
  if      (kBtnPlay.hit(x, y))   { togglePlay(); return; }
  else if (kBtnBack30.hit(x, y)) audio::seekRel(-30);
  else if (kBtnFwd30.hit(x, y))  audio::seekRel(+30);
  else if (kBtnSync.hit(x, y))   { requestSync(s_openFeed); return; }
  else if (kBtnList.hit(x, y))   { s_screen = LIST; markDirty(); return; }
  else return;
  markDirty();
}

void onListKey(char k) {
  switch (k) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(+1); break;
    case '\r':          if (s_n > 0) enterDetail(s_sel); break;
    case 'y': case 'Y': requestSync(-1); break;
    case '\b': case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onDetailKey(char k) {
  switch (k) {
    case ' ': case '\r': togglePlay(); break;
    case 'a': case 'A':  audio::seekRel(-30); markDirty(); break;
    case 'd': case 'D':  audio::seekRel(+30); markDirty(); break;
    case 'w': case 'W':  audio::volumeUp(); markDirty(); break;
    case 's': case 'S':  audio::volumeDown(); markDirty(); break;
    case 'y': case 'Y':  requestSync(s_openFeed); break;
    case '\b':           s_screen = LIST; markDirty(); break;
    case 'q': case 'Q':  appmgr::goHome(); break;
    default: break;
  }
}

class PodcastApp : public App {
 public:
  const char* id()   const override { return "Podcast"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppPodcast); }

  void onEnter() override {
    if (s_n < 0 && board::sdReady()) loadFeeds();
    audio::Status st = audio::status();
    if (st.owner == audio::Owner::Podcast && st.playing && s_openFeed >= 0) s_screen = DETAIL;
  }

  void onLeave() override { saveBookmarkNow(); }

  void handleInput(const InputEvent& e) override {
    if (s_syncReq != -2) return;   // während des Syncs keine Eingaben
    if (e.type == InputEvent::TAP) {
      if (s_screen == LIST) onListTouch(e.x, e.y);
      else                  onDetailTouch(e.x, e.y);
    } else {
      if (s_screen == LIST) onListKey(e.key);
      else                  onDetailKey(e.key);
    }
  }

  void background() override {
    audio::Status st = audio::status();
    bool mine = (st.owner == audio::Owner::Podcast && st.playing);
    if (mine && s_epKey) {
      uint32_t now = millis();
      if (now - s_lastSave >= 30000) { s_lastSave = now; settings::setBookBookmark(s_epKey, 0, st.posSec); }
    } else if (s_wasPlaying) {
      saveBookmarkNow();
    }
    s_wasPlaying = mine;
  }

  void tick() override {
    // Aufgeschobener, blockierender Sync: erst nach dem "Synchronisiere…"-Frame.
    if (s_syncReq != -2) {
      if (!s_syncDrawn) {
        if (!appmgr::isDirty()) s_syncDrawn = true;   // Frame steht
        return;
      }
      int req = s_syncReq;
      if (req == -1) podcast::syncAll(s_syncLog, sizeof(s_syncLog), nullptr);
      else           podcast::syncOne(req, s_syncLog, sizeof(s_syncLog), nullptr);
      s_syncReq = -2;
      loadFeeds();
      if (s_screen == DETAIL) refreshLocal();
      markDirty();
      return;
    }
    // Fortschrittsstreifen im Player (Region-Refresh, kein Vollbild).
    if (appmgr::isDirty() || s_screen != DETAIL || !s_haveLocal) return;
    audio::Status st = audio::status();
    if (st.owner == audio::Owner::Podcast && st.playing && !st.paused &&
        st.posSec != s_lastShownSec && millis() - s_lastProg >= kProgressMs) {
      s_lastProg = millis();
      s_lastShownSec = st.posSec;
      if (++s_progCount % 20 == 0) appmgr::markDirty();
      else display::renderRegion(drawProgStrip, PROG_Y, PROG_H);
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    s_lastShownSec = 999999;
    if (s_syncReq != -2)        drawSyncing(g);
    else if (s_screen == LIST)  drawList(g);
    else                        drawDetail(g);
  }
};

PodcastApp s_app;

}  // namespace

namespace podcast_app {
App* get() { return &s_app; }
}
