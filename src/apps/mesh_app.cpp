// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "mesh_app.h"
#include "mesh_client.h"
#include "config.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/i18n.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>
#include <limits.h>
#include <time.h>

namespace {

using gui::Rect;

// --- Layout -------------------------------------------------------------------
constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_Y = TOP + 2;
constexpr int HEADER_H = TOP + 26;

// Nachrichtenbereich (Textgröße 1, 10 px Zeilenhöhe)
constexpr int MSG_Y     = HEADER_H + 4;
constexpr int MSG_LINEH = 10;
constexpr int MSG_LINES = 19;                       // bis y=244
constexpr int MSG_COLS  = 38;

// Eingabezeile
constexpr int INPUT_Y = MSG_Y + MSG_LINES * MSG_LINEH + 4;   // 248
constexpr int INPUT_H = 22;

// Untere Leiste
constexpr int BAR_Y = INPUT_Y + INPUT_H + 4;       // 274
const Rect kBtn1 {6,   BAR_Y, 72, 42};
const Rect kBtn2 {84,  BAR_Y, 72, 42};
const Rect kBtn3 {162, BAR_Y, 72, 42};

// Listen (Chats / Kontakte)
constexpr int ROW_H       = 30;
constexpr int LIST_Y      = HEADER_H + 4;           // Basis (Kontakte-Liste)
constexpr int STATUS_H    = 14;                     // Statuszeile auf CHATS
constexpr int GUTTER_W    = 28;                     // rechter Touch-Scroll-Streifen der Listen
constexpr int SCROLL_STEP = MSG_LINES - 3;          // Touch-Seitensprung im Verlauf

// CHATS  = scrollbare Liste: alle Kanäle + Kontakte MIT Chat-Verlauf (Einstieg).
// CONTACTS = alle bekannten Knoten (Adverts) zum Starten neuer DMs.
// CHANNEL/DM = geöffnete Konversation. ERROR_SCREEN = Radio nicht verfügbar.
enum Screen { CHATS, CONTACTS, CHANNEL, DM, ERROR_SCREEN };
Screen s_screen = CHATS;

bool     s_initTried = false;
int      s_dmContact = -1;           // Kontakt-Index der offenen DM-Ansicht
char     s_dmName[32] = "";
int      s_channelIdx = 0;           // Kanal-Index der offenen CHANNEL-Ansicht
int      s_sel = 0, s_off = 0;       // Listen-Cursor (CHATS bzw. CONTACTS)
int      s_msgScroll = 0;            // Verlaufs-Scrollback (0 = neueste, unten)
char     s_compose[140] = "";
uint32_t s_seenChanges = 0;

void markDirty() { appmgr::markDirty(); }

// Listen-Geometrie hängt vom Screen ab: CHATS hat eine Statuszeile darüber.
int listTop()     { return (s_screen == CHATS) ? (LIST_Y + STATUS_H) : LIST_Y; }
int visibleRows() { return (s_screen == CHATS) ? 6 : 7; }

// --- Kontakt-Verlauf: gibt es DMs (ein/aus) mit diesem Kontakt? ---------------
bool hasHistory(int contactIdx) {
  int n = mesh_client::msgCount();
  for (int i = 0; i < n; i++) {
    mesh_client::MsgView m;
    if (!mesh_client::msg(i, m)) continue;
    if (m.kind != 0 && (int)m.contactIdx == contactIdx) return true;
  }
  return false;
}

bool hasFailedDm(int contactIdx) {
  int n = mesh_client::msgCount();
  for (int i = 0; i < n; i++) {
    mesh_client::MsgView m;
    if (!mesh_client::msg(i, m)) continue;
    if (m.kind == 2 && m.ackState == 3 && (int)m.contactIdx == contactIdx) return true;
  }
  return false;
}

// --- Chat-Liste: Kanäle (zuerst) + Kontakte MIT Verlauf (als DMs) -------------
struct ChatEntry { uint8_t kind; int idx; };   // kind: 0=Kanal, 1=DM (Kontakt)
constexpr int kMaxChats = 8 + 32;              // MAX_GROUP_CHANNELS + MAX_CONTACTS

int buildChatList(ChatEntry* out, int max) {
  int n = 0;
  int nc = mesh_client::channelCount();
  for (int i = 0; i < nc && n < max; i++) out[n++] = {0, i};
  int nk = mesh_client::contactCount();
  for (int i = 0; i < nk && n < max; i++) if (hasHistory(i)) out[n++] = {1, i};
  return n;
}

int chatCount() {
  int c = mesh_client::channelCount();
  int nk = mesh_client::contactCount();
  for (int i = 0; i < nk; i++) if (hasHistory(i)) c++;
  return c;
}

int screenItemCount() {
  return (s_screen == CHATS) ? chatCount() : mesh_client::contactCount();
}

// --- Zeit-Helfer --------------------------------------------------------------
// Lokale Uhrzeit (Zeitzone via timesync::applyTimezone gesetzt) — die Uhr selbst
// läuft intern in UTC.
void fmtClock(uint32_t ts, char* out, size_t n) {
  if (!ts) { snprintf(out, n, "--:--"); return; }
  time_t    tt = (time_t)ts;
  struct tm lt;
  localtime_r(&tt, &lt);
  snprintf(out, n, "%02u:%02u", (unsigned)lt.tm_hour, (unsigned)lt.tm_min);
}

void fmtAge(uint32_t last, char* out, size_t n) {
  uint32_t now = mesh_client::rtcTime();
  if (!last || !now || now < last) { snprintf(out, n, "?"); return; }
  uint32_t d = now - last;
  char rel[12];
  if (d < 3600)       snprintf(rel, sizeof(rel), "%lum", (unsigned long)(d / 60));
  else if (d < 86400) snprintf(rel, sizeof(rel), "%luh", (unsigned long)(d / 3600));
  else                snprintf(rel, sizeof(rel), "%lud", (unsigned long)(d / 86400));
  snprintf(out, n, i18n::tr(i18n::Str::FmtAge), rel);
}

// --- Verlauf in Anzeigezeilen umbrechen (Fenster-Ansatz für Scrollback) -------
// Nur Zeilen im Sichtfenster [winStart, winStart+MSG_LINES) werden gespeichert;
// gidx zählt ALLE Zeilen (für Gesamtzahl im Zähl-Durchlauf, winStart=INT_MAX).
struct LineWindow {
  char lines[MSG_LINES][MSG_COLS * 3 + 4];
  int  count;
  int  gidx;
  int  winStart;
};

void wrapInto(LineWindow& w, const char* text) {
  const char* p = text;
  while (*p) {
    char tmp[MSG_COLS * 3 + 4];
    int cols = 0, len = 0;
    while (*p && *p != '\n' && cols < MSG_COLS) {
      uint8_t c = (uint8_t)*p;
      int nb = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
      for (int k = 0; k < nb && *p && len < (int)sizeof(tmp) - 1; k++) tmp[len++] = *p++;
      cols++;
    }
    tmp[len] = '\0';
    if (*p == '\n') p++;
    if (w.gidx >= w.winStart && w.count < MSG_LINES) {
      strncpy(w.lines[w.count], tmp, sizeof(w.lines[0]) - 1);
      w.lines[w.count][sizeof(w.lines[0]) - 1] = '\0';
      w.count++;
    }
    w.gidx++;
  }
}

// Eine Nachricht zur Anzeigezeile (HH:MM-Prefix + Absender/ACK) formatieren.
void buildLine(const mesh_client::MsgView& m, char* out, size_t n) {
  char ts[8] = "";
  if (m.timestamp) {
    time_t    tt = (time_t)m.timestamp;
    struct tm lt;
    localtime_r(&tt, &lt);
    snprintf(ts, sizeof(ts), "%02u:%02u ", (unsigned)lt.tm_hour, (unsigned)lt.tm_min);
  }
  if (s_screen == CHANNEL) {
    snprintf(out, n, "%s%s", ts, m.text);
  } else {
    const char* ack = "";
    if (m.kind == 2) ack = (m.ackState == 2) ? " *" : (m.ackState == 3) ? " !" : " ...";
    snprintf(out, n, "%s%s%s: %s%s", ts, (m.kind == 2) ? ">" : "",
             (m.kind == 2) ? i18n::tr(i18n::Str::MeshMe) : m.from, m.text, ack);
  }
}

// Verlauf des aktuellen Screens (Channel oder DM-Kontakt) durchlaufen.
void forEachConvLine(LineWindow& w) {
  int n = mesh_client::msgCount();
  for (int i = 0; i < n; i++) {
    mesh_client::MsgView m;
    if (!mesh_client::msg(i, m)) continue;
    if (s_screen == CHANNEL) {
      if (m.kind != 0 || (int)m.channelIdx != s_channelIdx) continue;
    } else {  // DM
      if (m.kind == 0 || (int)m.contactIdx != s_dmContact) continue;
    }
    char line[224];
    buildLine(m, line, sizeof(line));
    wrapInto(w, line);
  }
}

// --- Zeichnen ----------------------------------------------------------------------
void drawHeaderBar(Adafruit_GFX& g, const char* title) {
  gui::printAt(g, 6, HEADER_Y, title, 2);
  g.setTextSize(1);
  uint16_t bw, bh;
  gui::textBounds(g, mesh_client::nodeName(), &bw, &bh);
  g.setCursor(W - 6 - (int)bw, HEADER_Y + 4);
  gui::print(g, mesh_client::nodeName());
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
}

void drawStatusLine(Adafruit_GFX& g) {
  int nf = 0; uint32_t rx = 0, tx = 0;
  mesh_client::radioStats(&nf, &rx, &tx);
  char clk[8]; fmtClock(mesh_client::rtcTime(), clk, sizeof(clk));
  char s[64];
  snprintf(s, sizeof(s), i18n::tr(i18n::Str::FmtMeshStatus),
           nf, (unsigned long)rx, (unsigned long)tx, mesh_client::contactCount(), clk);
  gui::printAt(g, 6, LIST_Y, s, 1);
}

void drawCompose(Adafruit_GFX& g) {
  g.drawRect(4, INPUT_Y, W - 8, INPUT_H, GxEPD_BLACK);
  char line[150];
  snprintf(line, sizeof(line), "%s_", s_compose);
  // Nur das Ende anzeigen, wenn zu lang.
  int maxCols = MSG_COLS - 1;
  int len = strlen(line);
  const char* show = line;
  if (len > maxCols) show = line + (len - maxCols);
  g.setTextSize(1);
  g.setCursor(8, INPUT_Y + 7);
  gui::print(g, show);
  // Bei leerer Zeile + fehlgeschlagener DM: Hinweis aufs erneute Senden (rechts).
  if (!s_compose[0] && s_screen == DM && hasFailedDm(s_dmContact)) {
    const char* hint = i18n::tr(i18n::Str::MeshResendHint);
    uint16_t bw, bh;
    gui::textBounds(g, hint, &bw, &bh);
    g.setCursor(W - 10 - (int)bw, INPUT_Y + 7);
    gui::print(g, hint);
  }
}

void drawChannelOrDm(Adafruit_GFX& g) {
  if (s_screen == CHANNEL) {
    char cn[32] = "";
    if (!mesh_client::channelName(s_channelIdx, cn, sizeof(cn)))
      strncpy(cn, i18n::tr(i18n::Str::MeshChannel), sizeof(cn) - 1);
    drawHeaderBar(g, cn);
  } else {
    char t[48];
    snprintf(t, sizeof(t), "DM: %s", s_dmName);
    drawHeaderBar(g, t);
  }

  // Durchlauf 1: Gesamt-Zeilenzahl ermitteln (kein Speichern).
  LineWindow w;
  w.count = 0; w.gidx = 0; w.winStart = INT_MAX;
  forEachConvLine(w);
  int total = w.gidx;
  int maxScroll = (total > MSG_LINES) ? (total - MSG_LINES) : 0;
  if (s_msgScroll > maxScroll) s_msgScroll = maxScroll;
  if (s_msgScroll < 0) s_msgScroll = 0;
  int winStart = total - MSG_LINES - s_msgScroll;
  if (winStart < 0) winStart = 0;

  // Durchlauf 2: nur das Sichtfenster sammeln.
  w.count = 0; w.gidx = 0; w.winStart = winStart;
  forEachConvLine(w);

  g.setTextSize(1);
  g.setTextColor(GxEPD_BLACK);
  int y = MSG_Y + (MSG_LINES - w.count) * MSG_LINEH;
  if (y < MSG_Y) y = MSG_Y;
  for (int i = 0; i < w.count; i++) {
    g.setCursor(6, y);
    gui::print(g, w.lines[i]);
    y += MSG_LINEH;
  }
  if (total == 0)
    gui::printAt(g, 10, MSG_Y + 30, i18n::tr(i18n::Str::MeshNoMsgs), 1);

  // Scroll-Marker: ▲ = älterer Verlauf oben, ▼ = neuere Nachrichten unten.
  if (winStart > 0)      gui::printAt(g, W - 12, MSG_Y, "\x1E", 1);
  if (s_msgScroll > 0)   gui::printAt(g, W - 12, INPUT_Y - 10, "\x1F", 1);

  drawCompose(g);

  // In jeder Konversation: zurück zur Chat-Liste, Advert, Home.
  gui::drawButton(g, kBtn1, i18n::tr(i18n::Str::BtnBackShort), false);
  gui::drawButton(g, kBtn2, i18n::tr(i18n::Str::BtnAdvert), false);
  gui::drawButton(g, kBtn3, i18n::tr(i18n::Str::BtnHome), false);
}

// Auswahl-Rahmen (doppeltes Rechteck) um die Cursor-Zeile zeichnen.
void drawSelection(Adafruit_GFX& g, int n) {
  int cr = s_sel - s_off;
  if (n > 0 && cr >= 0 && cr < visibleRows()) {
    int y = listTop() + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
}

// Rechter Scroll-Streifen: ▲/▼-Marker, wenn ober-/unterhalb mehr Einträge sind.
void drawScrollHints(Adafruit_GFX& g, int n) {
  int vis = visibleRows(), top = listTop();
  int gx = W - GUTTER_W / 2 - 3;
  if (s_off > 0)            gui::printAt(g, gx, top + 2, "\x1E", 1);
  if (s_off + vis < n)      gui::printAt(g, gx, top + vis * ROW_H - 10, "\x1F", 1);
}

void drawChats(Adafruit_GFX& g) {
  drawHeaderBar(g, i18n::tr(i18n::Str::MeshChats));
  drawStatusLine(g);

  ChatEntry entries[kMaxChats];
  int n = buildChatList(entries, kMaxChats);

  if (n == 0) {
    int hy = listTop() + 6;
    gui::printAt(g, 10, hy,      i18n::tr(i18n::Str::MeshCtHint1), 1);
    gui::printAt(g, 10, hy + 14, i18n::tr(i18n::Str::MeshCtHint2), 1);
  }

  int top = listTop(), vis = visibleRows();
  for (int r = 0; r < vis && s_off + r < n; r++) {
    ChatEntry& e = entries[s_off + r];
    char raw[40] = "", nm[44];
    if (e.kind == 0) {
      mesh_client::channelName(e.idx, raw, sizeof(raw));
      snprintf(nm, sizeof(nm), "%s%s", (raw[0] == '#') ? "" : "#", raw);
    } else {
      mesh_client::contactName(e.idx, raw, sizeof(raw));
      snprintf(nm, sizeof(nm), "\x10 %s", raw);   // ► DM-Marker
    }
    gui::drawRowText(g, top + r * ROW_H, ROW_H, nm, false);
  }
  drawSelection(g, n);
  drawScrollHints(g, n);

  gui::drawButton(g, kBtn1, i18n::tr(i18n::Str::BtnContacts), false);
  gui::drawButton(g, kBtn2, i18n::tr(i18n::Str::BtnAdvert), false);
  gui::drawButton(g, kBtn3, i18n::tr(i18n::Str::BtnHome), false);
}

void drawContacts(Adafruit_GFX& g) {
  drawHeaderBar(g, i18n::tr(i18n::Str::MeshContacts));

  int n = mesh_client::contactCount();
  if (n == 0)
    gui::printAt(g, 10, LIST_Y + 30, i18n::tr(i18n::Str::MeshNoContacts), 1);

  int top = listTop(), vis = visibleRows();
  for (int r = 0; r < vis && s_off + r < n; r++) {
    int idx = s_off + r;
    char raw[40] = "", nm[44];
    mesh_client::contactName(idx, raw, sizeof(raw));
    snprintf(nm, sizeof(nm), "\x10 %s", raw);
    gui::drawRowText(g, top + r * ROW_H, ROW_H, nm, false);

    // Detail (Hops + zuletzt gesehen) klein rechtsbündig in dieselbe Zeile.
    uint8_t hops = 0xFF; uint32_t last = 0;
    mesh_client::contactDetail(idx, &hops, &last, nullptr);
    char age[20]; fmtAge(last, age, sizeof(age));
    char hs[6];
    if (hops == 0xFF) strcpy(hs, "?");
    else snprintf(hs, sizeof(hs), "%uh", (unsigned)(hops & 0x3F));
    char det[28];
    snprintf(det, sizeof(det), "%s %s", hs, age);
    g.setTextSize(1);
    uint16_t bw, bh;
    gui::textBounds(g, det, &bw, &bh);
    g.setCursor(W - GUTTER_W - 4 - (int)bw, top + r * ROW_H + ROW_H / 2 - 3);
    gui::print(g, det);
  }
  drawSelection(g, n);
  drawScrollHints(g, n);

  gui::drawButton(g, kBtn1, i18n::tr(i18n::Str::BtnBackShort), false);
  gui::drawButton(g, kBtn2, i18n::tr(i18n::Str::BtnAdvert), false);
  gui::drawButton(g, kBtn3, i18n::tr(i18n::Str::BtnHome), false);
}

void drawError(Adafruit_GFX& g) {
  drawHeaderBar(g, "Mesh");
  gui::printAt(g, 10, 90, i18n::tr(i18n::Str::MeshNoRadio), 2);
  gui::printAt(g, 10, 124, i18n::tr(i18n::Str::MeshInitFail), 1);
  gui::printAt(g, 10, 138, i18n::tr(i18n::Str::MeshSeeLog), 1);
  gui::drawButton(g, kBtn2, i18n::tr(i18n::Str::BtnRetry), false);
  gui::drawButton(g, kBtn3, i18n::tr(i18n::Str::BtnHome), false);
}

// --- Interaktion --------------------------------------------------------------------
void sendCompose() {
  if (!s_compose[0]) return;
  bool ok = (s_screen == CHANNEL)
                ? mesh_client::sendChannelIdxMsg(s_channelIdx, s_compose)
                : mesh_client::sendDirectMsg(s_dmContact, s_compose);
  if (ok) s_compose[0] = '\0';
  s_msgScroll = 0;
  markDirty();
}

void backToChats() {
  s_screen = CHATS;
  s_compose[0] = '\0';
  s_msgScroll = 0;
  int n = chatCount();
  if (s_sel >= n) { s_sel = 0; s_off = 0; }
  markDirty();
}

void goContacts() {
  s_screen = CONTACTS;
  s_sel = 0; s_off = 0;
  markDirty();
}

void openDm(int idx) {
  if (idx < 0 || idx >= mesh_client::contactCount()) return;
  s_dmContact = idx;
  mesh_client::contactName(idx, s_dmName, sizeof(s_dmName));
  s_compose[0] = '\0';
  s_msgScroll = 0;
  s_screen = DM;
  markDirty();
}

// Eintrag der Chat-Liste öffnen (Kanal oder DM).
void openChat(int listIdx) {
  ChatEntry entries[kMaxChats];
  int n = buildChatList(entries, kMaxChats);
  if (listIdx < 0 || listIdx >= n) return;
  ChatEntry& e = entries[listIdx];
  if (e.kind == 0) {
    s_channelIdx = e.idx;
    s_compose[0] = '\0';
    s_msgScroll = 0;
    s_screen = CHANNEL;
    markDirty();
  } else {
    openDm(e.idx);
  }
}

void moveSel(int delta) {
  int n = screenItemCount();
  if (n <= 0) return;
  int vis = visibleRows();
  int ns = s_sel + delta;
  if (ns < 0) ns = n - 1;
  if (ns >= n) ns = 0;
  s_sel = ns;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + vis) s_off = s_sel - vis + 1;
  markDirty();
}

// Liste um eine Seite verschieben (Touch-Scroll-Streifen). dir: -1 hoch, +1 runter.
void pageList(int dir) {
  int n = screenItemCount();
  if (n <= 0) return;
  int vis = visibleRows();
  int step = (vis > 1) ? (vis - 1) : 1;
  int maxOff = (n > vis) ? (n - vis) : 0;
  s_off += dir * step;
  if (s_off < 0) s_off = 0;
  if (s_off > maxOff) s_off = maxOff;
  // Auswahl ins sichtbare Fenster ziehen.
  if (s_sel < s_off) s_sel = s_off;
  if (s_sel >= s_off + vis) s_sel = s_off + vis - 1;
  if (s_sel >= n) s_sel = n - 1;
  markDirty();
}

void retryInit() {
  s_initTried = true;
  if (mesh_client::begin()) {
    s_screen = CHATS;
    s_sel = 0; s_off = 0;
    s_seenChanges = mesh_client::changeCounter();
  }
  markDirty();
}

void onTouch(int x, int y) {
  if (s_screen == ERROR_SCREEN) {
    if (kBtn2.hit(x, y)) { retryInit(); return; }
    if (kBtn3.hit(x, y)) appmgr::goHome();
    return;
  }
  if (kBtn3.hit(x, y)) { appmgr::goHome(); return; }

  if (s_screen == CHATS || s_screen == CONTACTS) {
    if (kBtn1.hit(x, y)) { (s_screen == CHATS) ? goContacts() : backToChats(); return; }
    if (kBtn2.hit(x, y)) { mesh_client::sendAdvert(); markDirty(); return; }
    int top = listTop(), vis = visibleRows();
    if (y >= top && y < top + vis * ROW_H) {
      // Rechter Streifen scrollt seitenweise; sonst öffnet die Zeile.
      if (x >= W - GUTTER_W) {
        pageList(y < top + vis * ROW_H / 2 ? -1 : +1);
        return;
      }
      int idx = s_off + (y - top) / ROW_H;
      if (s_screen == CHATS) openChat(idx);
      else if (idx < mesh_client::contactCount()) openDm(idx);
    }
    return;
  }

  // CHANNEL / DM: zurück zur Liste, Advert, Scrollback.
  if (kBtn1.hit(x, y)) { backToChats(); return; }
  if (kBtn2.hit(x, y)) { mesh_client::sendAdvert(); markDirty(); return; }
  if (y >= MSG_Y && y < INPUT_Y) {
    int mid = (MSG_Y + INPUT_Y) / 2;
    if (y < mid) s_msgScroll += SCROLL_STEP;
    else { s_msgScroll -= SCROLL_STEP; if (s_msgScroll < 0) s_msgScroll = 0; }
    markDirty();
  }
}

void onKey(char k) {
  if (s_screen == ERROR_SCREEN) {
    if (k == '\r') retryInit();
    return;
  }

  if (s_screen == CHATS || s_screen == CONTACTS) {
    switch (k) {
      case 'w': case 'W': moveSel(-1); break;
      case 's': case 'S': moveSel(+1); break;
      case '\r':
        if (s_screen == CHATS) openChat(s_sel);
        else if (s_sel < mesh_client::contactCount()) openDm(s_sel);
        break;
      case 'k': case 'K': if (s_screen == CHATS) goContacts(); break;
      case '\b':          if (s_screen == CONTACTS) backToChats(); break;
      case 'q': case 'Q': appmgr::goHome(); break;
      default: break;
    }
    return;
  }

  // CHANNEL/DM: Tastatur tippt in die Eingabezeile.
  if (k == '\r') {
    if (s_compose[0]) sendCompose();
    else if (s_screen == DM && mesh_client::resendDirect(s_dmContact)) { s_msgScroll = 0; markDirty(); }
    return;
  }
  if (k == '\b') {
    int len = strlen(s_compose);
    if (len > 0) { s_compose[len - 1] = '\0'; markDirty(); }
    else { backToChats(); }   // leere Eingabe + Backspace = zurück zur Chat-Liste
    return;
  }
  if (k >= 32 && k < 127) {
    int len = strlen(s_compose);
    if (len < (int)sizeof(s_compose) - 1) {
      s_compose[len] = k;
      s_compose[len + 1] = '\0';
      markDirty();
    }
  }
}

// --- App-Klasse -----------------------------------------------------------------------
class MeshApp : public App {
 public:
  const char* id()   const override { return "Mesh"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppMesh); }

  void onEnter() override {
    if (!s_initTried) {
      s_initTried = true;
      s_screen = mesh_client::begin() ? CHATS : ERROR_SCREEN;
    } else if (!mesh_client::ready()) {
      s_screen = ERROR_SCREEN;
    } else {
      s_screen = CHATS;   // Wiedereintritt zeigt immer die Chat-Übersicht
    }
    s_msgScroll = 0;
    s_seenChanges = mesh_client::changeCounter();
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) onTouch(e.x, e.y);
    else                           onKey(e.key);
  }

  // Mesh-Pumpe läuft IMMER (auch wenn andere Apps im Vordergrund sind).
  void background() override {
    if (mesh_client::ready()) mesh_client::loop();
  }

  void tick() override {
    if (appmgr::isDirty()) return;
    // Neue Nachrichten/Kontakte -> neu zeichnen (koalesziert über Dirty-Flag).
    uint32_t ch = mesh_client::changeCounter();
    if (ch != s_seenChanges) {
      s_seenChanges = ch;
      markDirty();
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    switch (s_screen) {
      case CHATS:            drawChats(g); break;
      case CONTACTS:         drawContacts(g); break;
      case CHANNEL: case DM: drawChannelOrDm(g); break;
      case ERROR_SCREEN:     drawError(g); break;
    }
  }
};

MeshApp s_app;

}  // namespace

namespace mesh_app {

App* get() { return &s_app; }

}  // namespace mesh_app
