#include "settings_app.h"
#include "mesh_client.h"
#include "config.h"
#include "core/battery.h"
#include "core/gui.h"
#include "core/settings.h"

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
// 10 Zeilen + Home-Button müssen unter die Statuszeile passen:
// LIST_Y=54 + 10*22 = 274, Button ab 276.
constexpr int ROW_H    = 22;
constexpr int LIST_Y   = HEADER_H + 4;

const Rect kHome {6, 276, 110, 38};

// --- Zeilen ---------------------------------------------------------------------
enum RowId {
  ROW_PRESET, ROW_FREQ, ROW_BW, ROW_SF, ROW_CR, ROW_TX, ROW_NAME,
  ROW_STANDBY, ROW_INFO,
  ROW_VERSION,
  ROW_COUNT
};

// Auto-Standby-Stufen (Minuten; 0 = Aus).
const uint8_t kStandbySteps[] = {0, 2, 5, 10, 30};
constexpr int kNumStandby = 5;

int  s_sel = 0;
bool s_editName = false;
char s_nameBuf[32] = "";

// Presets (Quelle: MeshCore-Community; Narrow = Standard in DE/NRW,
// identisch zu Daniels Repeater: 869,618 / 62,5 / SF8 / CR4-5).
struct Preset { const char* name; settings::MeshParams p; };
const Preset kPresets[] = {
  {"EU Narrow",    {869.618f, 62.5f,  8, 5, 22}},
  {"EU Klassisch", {869.525f, 250.0f, 11, 5, 22}},
};
constexpr int kNumPresets = 2;

void markDirty() { appmgr::markDirty(); }

// Passt der aktuelle Parametersatz zu einem Preset? (-1 = manuell)
int matchPreset(const settings::MeshParams& p) {
  for (int i = 0; i < kNumPresets; i++) {
    const settings::MeshParams& q = kPresets[i].p;
    if (p.freqMhz == q.freqMhz && p.bwKhz == q.bwKhz && p.sf == q.sf && p.cr == q.cr)
      return i;
  }
  return -1;
}

// Parameter speichern + live anwenden.
void apply(const settings::MeshParams& p) {
  settings::setMeshParams(p);
  mesh_client::applyRadioParams();
  markDirty();
}

// Bandbreiten-Stufen (LoRa-üblich).
float nextBw(float bw, int dir) {
  const float steps[] = {62.5f, 125.0f, 250.0f, 500.0f};
  int idx = 0;
  for (int i = 0; i < 4; i++) if (bw == steps[i]) idx = i;
  idx += dir;
  if (idx < 0) idx = 3;
  if (idx > 3) idx = 0;
  return steps[idx];
}

// Wert der Zeile ändern (dir = -1/+1).
void changeRow(int row, int dir) {
  if (row == ROW_STANDBY) {
    uint8_t cur = settings::standbyMinutes();
    int idx = 0;
    for (int i = 0; i < kNumStandby; i++) if (kStandbySteps[i] == cur) idx = i;
    idx = (idx + dir + kNumStandby) % kNumStandby;
    settings::setStandbyMinutes(kStandbySteps[idx]);
    markDirty();
    return;
  }
  settings::MeshParams p = settings::meshParams();
  switch (row) {
    case ROW_PRESET: {
      int cur = matchPreset(p);
      int next = (cur < 0) ? (dir > 0 ? 0 : kNumPresets - 1)
                           : (cur + dir + kNumPresets) % kNumPresets;
      settings::MeshParams np = kPresets[next].p;
      np.txDbm = p.txDbm;          // Sendeleistung bleibt persönliche Wahl
      apply(np);
      return;
    }
    case ROW_FREQ:
      p.freqMhz += dir * 0.025f;
      if (p.freqMhz < 863.0f) p.freqMhz = 863.0f;
      if (p.freqMhz > 870.0f) p.freqMhz = 870.0f;
      break;
    case ROW_BW:  p.bwKhz = nextBw(p.bwKhz, dir); break;
    case ROW_SF:
      p.sf = (uint8_t)constrain((int)p.sf + dir, 7, 12);
      break;
    case ROW_CR:
      p.cr = (uint8_t)constrain((int)p.cr + dir, 5, 8);
      break;
    case ROW_TX:
      p.txDbm = (uint8_t)constrain((int)p.txDbm + dir, 1, 22);
      break;
    default: return;
  }
  apply(p);
}

void rowLabel(int row, char* lbl, size_t n) {
  settings::MeshParams p = settings::meshParams();
  switch (row) {
    case ROW_PRESET: {
      int m = matchPreset(p);
      snprintf(lbl, n, "Preset: %s", m >= 0 ? kPresets[m].name : "Manuell");
      break;
    }
    case ROW_FREQ: snprintf(lbl, n, "Frequenz: %.3f MHz", (double)p.freqMhz); break;
    case ROW_BW:   snprintf(lbl, n, "Bandbreite: %.1f kHz", (double)p.bwKhz); break;
    case ROW_SF:   snprintf(lbl, n, "Spreading: SF%u", p.sf); break;
    case ROW_CR:   snprintf(lbl, n, "Coding-Rate: 4/%u", p.cr); break;
    case ROW_TX:   snprintf(lbl, n, "Sendeleistung: %u dBm", p.txDbm); break;
    case ROW_NAME: {
      char nm[32];
      settings::meshName(nm, sizeof(nm));
      if (s_editName) snprintf(lbl, n, "Name: %s_", s_nameBuf);
      else            snprintf(lbl, n, "Name: %s", nm);
      break;
    }
    case ROW_STANDBY: {
      uint8_t m = settings::standbyMinutes();
      if (m == 0) snprintf(lbl, n, "Standby: Aus");
      else        snprintf(lbl, n, "Standby: %u min", m);
      break;
    }
    case ROW_INFO:
      snprintf(lbl, n, "Akku: %u mV (%u%%%s)", battery::milliVolts(),
               battery::percent(), battery::charging() ? ", lädt" : "");
      break;
    case ROW_VERSION:
      snprintf(lbl, n, "Firmware: Fennek %s", FENNEK_VERSION);
      break;
  }
}

void startEditName() {
  settings::meshName(s_nameBuf, sizeof(s_nameBuf));
  s_editName = true;
  markDirty();
}

void finishEditName(bool save) {
  if (save && s_nameBuf[0]) mesh_client::setNodeName(s_nameBuf);
  s_editName = false;
  markDirty();
}

void onKey(char k) {
  // Name-Editiermodus: Tasten gehen in den Namen.
  if (s_editName) {
    if (k == '\r')      { finishEditName(true); return; }
    if (k == 0x02)      { finishEditName(false); return; }   // Mic = abbrechen
    if (k == '\b') {
      int len = strlen(s_nameBuf);
      if (len > 0) { s_nameBuf[len - 1] = '\0'; markDirty(); }
      else finishEditName(false);
      return;
    }
    if (k >= 32 && k < 127) {
      int len = strlen(s_nameBuf);
      if (len < (int)sizeof(s_nameBuf) - 1) {
        s_nameBuf[len] = k;
        s_nameBuf[len + 1] = '\0';
        markDirty();
      }
    }
    return;
  }

  switch (k) {
    case 'w': case 'W': s_sel = (s_sel + ROW_COUNT - 1) % ROW_COUNT; markDirty(); break;
    case 's': case 'S': s_sel = (s_sel + 1) % ROW_COUNT; markDirty(); break;
    case 'a': case 'A': changeRow(s_sel, -1); break;
    case 'd': case 'D': changeRow(s_sel, +1); break;
    case '\r':
      if (s_sel == ROW_NAME) startEditName();
      else changeRow(s_sel, +1);
      break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onTouch(int x, int y) {
  if (kHome.hit(x, y)) {
    if (s_editName) finishEditName(true);
    appmgr::goHome();
    return;
  }
  if (y < LIST_Y || y >= LIST_Y + ROW_COUNT * ROW_H) return;
  int row = (y - LIST_Y) / ROW_H;
  s_sel = row;
  if (row == ROW_NAME) { startEditName(); return; }
  if (row == ROW_INFO) { markDirty(); return; }
  // Linke Hälfte = runter, rechte Hälfte = rauf.
  changeRow(row, (x >= W / 2) ? +1 : -1);
}

class SettingsApp : public App {
 public:
  const char* name() const override { return "Einstellungen"; }

  void onLeave() override {
    if (s_editName) finishEditName(true);
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) onTouch(e.x, e.y);
    else                           onKey(e.key);
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    gui::printAt(g, 6, HEADER_Y, "Einstellungen", 2);
    g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

    for (int r = 0; r < ROW_COUNT; r++) {
      char lbl[64];
      rowLabel(r, lbl, sizeof(lbl));
      gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, false);
    }
    // Cursor
    int y = LIST_Y + s_sel * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);

    gui::drawButton(g, kHome, "Home", false);
    g.setTextSize(1);
    g.setCursor(124, 284);
    gui::print(g, s_editName ? "Enter speichert" : "A/D bzw. Tap ändert");
    g.setCursor(124, 298);
    gui::print(g, "Wirkt sofort aufs Funkmodul");
  }
};

SettingsApp s_app;

}  // namespace

namespace settings_app {

App* get() { return &s_app; }

}  // namespace settings_app
