#include "mesh_app.h"
#include "mesh_client.h"
#include "config.h"
#include "core/display.h"
#include "core/gui.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

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

// Kontaktliste
constexpr int ROW_H   = 30;
constexpr int VISIBLE = 7;
constexpr int LIST_Y  = HEADER_H + 4;

enum Screen { CHANNEL, CONTACTS, DM, ERROR_SCREEN };
Screen s_screen = CHANNEL;

bool     s_initTried = false;
int      s_dmContact = -1;           // Kontakt-Index der offenen DM-Ansicht
char     s_dmName[32] = "";
int      s_sel = 0, s_off = 0;       // Kontaktlisten-Cursor
char     s_compose[140] = "";
uint32_t s_seenChanges = 0;

void markDirty() { appmgr::markDirty(); }

// Nachricht in Anzeigezeilen umbrechen und (von unten befüllt) sammeln.
struct LineCollector {
  char lines[MSG_LINES][MSG_COLS * 3 + 4];
  int  count = 0;
};

void wrapInto(LineCollector& lc, const char* text) {
  // Einfacher Zeichenumbruch auf MSG_COLS Codepoints (reicht für Chat).
  const char* p = text;
  while (*p && lc.count < MSG_LINES * 4) {
    char* dst = nullptr;
    char tmp[MSG_COLS * 3 + 4];
    int cols = 0, len = 0;
    while (*p && *p != '\n' && cols < MSG_COLS) {
      uint8_t c = (uint8_t)*p;
      int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
      for (int k = 0; k < n && *p && len < (int)sizeof(tmp) - 1; k++) tmp[len++] = *p++;
      cols++;
    }
    tmp[len] = '\0';
    if (*p == '\n') p++;
    (void)dst;
    if (lc.count < MSG_LINES) {
      // Erstmal sequentiell sammeln; Anzeige nimmt die letzten MSG_LINES.
      strncpy(lc.lines[lc.count], tmp, sizeof(lc.lines[0]) - 1);
      lc.lines[lc.count][sizeof(lc.lines[0]) - 1] = '\0';
      lc.count++;
    } else {
      // Puffer voll: alles eins hochschieben (einfach, MSG_LINES ist klein).
      memmove(lc.lines[0], lc.lines[1], sizeof(lc.lines[0]) * (MSG_LINES - 1));
      strncpy(lc.lines[MSG_LINES - 1], tmp, sizeof(lc.lines[0]) - 1);
      lc.lines[MSG_LINES - 1][sizeof(lc.lines[0]) - 1] = '\0';
    }
  }
}

// Nachrichten des aktuellen Screens (Channel oder DM-Kontakt) einsammeln.
void collectMessages(LineCollector& lc) {
  int n = mesh_client::msgCount();
  for (int i = 0; i < n; i++) {
    mesh_client::MsgView m;
    if (!mesh_client::msg(i, m)) continue;
    char line[224];
    if (s_screen == CHANNEL) {
      if (m.kind != 0) continue;
      strncpy(line, m.text, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    } else {  // DM
      if (m.kind == 0 || (int)m.contactIdx != s_dmContact) continue;
      const char* ack = "";
      if (m.kind == 2) ack = (m.ackState == 2) ? " *" : (m.ackState == 3) ? " !" : " ...";
      snprintf(line, sizeof(line), "%s%s: %s%s",
               (m.kind == 2) ? ">" : "", (m.kind == 2) ? "ich" : m.from, m.text, ack);
    }
    wrapInto(lc, line);
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
}

void drawChannelOrDm(Adafruit_GFX& g) {
  if (s_screen == CHANNEL) {
    drawHeaderBar(g, "Mesh-Kanal");
  } else {
    char t[48];
    snprintf(t, sizeof(t), "DM: %s", s_dmName);
    drawHeaderBar(g, t);
  }

  LineCollector lc;
  collectMessages(lc);
  g.setTextSize(1);
  g.setTextColor(GxEPD_BLACK);
  // Unten ausrichten: letzte Zeilen ans Ende des Bereichs.
  int y = MSG_Y + (MSG_LINES - lc.count) * MSG_LINEH;
  if (y < MSG_Y) y = MSG_Y;
  for (int i = 0; i < lc.count; i++) {
    g.setCursor(6, y);
    gui::print(g, lc.lines[i]);
    y += MSG_LINEH;
  }
  if (lc.count == 0) {
    gui::printAt(g, 10, MSG_Y + 30, "(noch keine Nachrichten)", 1);
  }

  drawCompose(g);

  if (s_screen == CHANNEL) {
    gui::drawButton(g, kBtn1, "Kont.", false);
    gui::drawButton(g, kBtn2, "Advert", false);
    gui::drawButton(g, kBtn3, "Home", false);
  } else {
    gui::drawButton(g, kBtn1, "Kanal", false);
    gui::drawButton(g, kBtn2, "Kont.", false);
    gui::drawButton(g, kBtn3, "Home", false);
  }
}

void drawContacts(Adafruit_GFX& g) {
  drawHeaderBar(g, "Kontakte");

  int n = mesh_client::contactCount();
  if (n == 0) {
    gui::printAt(g, 10, 90, "Keine Kontakte", 2);
    gui::printAt(g, 10, 120, "Kontakte erscheinen automatisch,", 1);
    gui::printAt(g, 10, 134, "sobald Adverts empfangen werden.", 1);
  }
  for (int r = 0; r < VISIBLE && s_off + r < n; r++) {
    char nm[40];
    mesh_client::contactName(s_off + r, nm, sizeof(nm));
    gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, nm, false);
  }
  int cr = s_sel - s_off;
  if (n > 0 && cr >= 0 && cr < VISIBLE) {
    int y = LIST_Y + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }

  gui::drawButton(g, kBtn1, "Kanal", false);
  gui::drawButton(g, kBtn2, "Advert", false);
  gui::drawButton(g, kBtn3, "Home", false);
}

void drawError(Adafruit_GFX& g) {
  drawHeaderBar(g, "Mesh");
  gui::printAt(g, 10, 90, "Radio nicht gefunden", 2);
  gui::printAt(g, 10, 124, "SX1262-Init fehlgeschlagen.", 1);
  gui::printAt(g, 10, 138, "Details im Serial-Log.", 1);
  gui::drawButton(g, kBtn3, "Home", false);
}

// --- Interaktion --------------------------------------------------------------------
void sendCompose() {
  if (!s_compose[0]) return;
  bool ok = (s_screen == CHANNEL)
                ? mesh_client::sendChannelMsg(s_compose)
                : mesh_client::sendDirectMsg(s_dmContact, s_compose);
  if (ok) s_compose[0] = '\0';
  markDirty();
}

void openDm(int idx) {
  if (idx < 0 || idx >= mesh_client::contactCount()) return;
  s_dmContact = idx;
  mesh_client::contactName(idx, s_dmName, sizeof(s_dmName));
  s_compose[0] = '\0';
  s_screen = DM;
  markDirty();
}

void moveSel(int delta) {
  int n = mesh_client::contactCount();
  if (n <= 0) return;
  int ns = s_sel + delta;
  if (ns < 0) ns = n - 1;
  if (ns >= n) ns = 0;
  s_sel = ns;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  markDirty();
}

void onTouch(int x, int y) {
  if (s_screen == ERROR_SCREEN) {
    if (kBtn3.hit(x, y)) appmgr::goHome();
    return;
  }
  if (kBtn3.hit(x, y)) { appmgr::goHome(); return; }

  if (s_screen == CHANNEL) {
    if (kBtn1.hit(x, y)) { s_screen = CONTACTS; markDirty(); return; }
    if (kBtn2.hit(x, y)) { mesh_client::sendAdvert(); markDirty(); return; }
  } else if (s_screen == DM) {
    if (kBtn1.hit(x, y)) { s_screen = CHANNEL; s_compose[0] = '\0'; markDirty(); return; }
    if (kBtn2.hit(x, y)) { s_screen = CONTACTS; markDirty(); return; }
  } else if (s_screen == CONTACTS) {
    if (kBtn1.hit(x, y)) { s_screen = CHANNEL; markDirty(); return; }
    if (kBtn2.hit(x, y)) { mesh_client::sendAdvert(); markDirty(); return; }
    if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
      int r = (y - LIST_Y) / ROW_H;
      if (s_off + r < mesh_client::contactCount()) openDm(s_off + r);
    }
  }
}

void onKey(char k) {
  if (s_screen == ERROR_SCREEN) return;

  if (s_screen == CONTACTS) {
    switch (k) {
      case 'w': case 'W': moveSel(-1); break;
      case 's': case 'S': moveSel(+1); break;
      case '\r':          openDm(s_sel); break;
      case '\b':          s_screen = CHANNEL; markDirty(); break;
      case 'q': case 'Q': appmgr::goHome(); break;
      default: break;
    }
    return;
  }

  // CHANNEL/DM: Tastatur tippt in die Eingabezeile.
  if (k == '\r') { sendCompose(); return; }
  if (k == '\b') {
    int len = strlen(s_compose);
    if (len > 0) { s_compose[len - 1] = '\0'; markDirty(); }
    else if (s_screen == DM) { s_screen = CHANNEL; markDirty(); }
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
  const char* name() const override { return "Mesh"; }

  void onEnter() override {
    if (!s_initTried) {
      s_initTried = true;
      if (!mesh_client::begin()) s_screen = ERROR_SCREEN;
    } else if (!mesh_client::ready()) {
      s_screen = ERROR_SCREEN;
    }
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
      case CHANNEL: case DM: drawChannelOrDm(g); break;
      case CONTACTS:         drawContacts(g); break;
      case ERROR_SCREEN:     drawError(g); break;
    }
  }
};

MeshApp s_app;

}  // namespace

namespace mesh_app {

App* get() { return &s_app; }

}  // namespace mesh_app
